#include "renderer.h"
#include <chrono>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>

#include "pathtrace_compute.h"
#include "pathtrace_rayquery.h"

using clock_type = std::chrono::steady_clock;
static double nowSeconds() {
    return std::chrono::duration<double>(clock_type::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Tonemapping (CPU resolve)
// ---------------------------------------------------------------------------
namespace {
vec3 acesFitted(vec3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    vec3 num = x * (x * a + vec3(b));
    vec3 den = x * (x * c + vec3(d)) + vec3(e);
    return { clampf(num.x / den.x, 0, 1), clampf(num.y / den.y, 0, 1), clampf(num.z / den.z, 0, 1) };
}
float linToSrgb(float c) {
    c = clampf(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
uint8_t toU8(float c) { return (uint8_t)clampf(linToSrgb(c) * 255.0f + 0.5f, 0.0f, 255.0f); }

constexpr uint32_t kMinTile = 64;
constexpr double   kBudgetSec = 0.04;    // target submit time (short = smoother display)
constexpr double   kMaxSubmitSec = 0.5;  // hard ceiling before subdividing
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Renderer::init(VkContext* ctx) { ctx_ = ctx; }

void Renderer::shutdown() {
    cancelFinal();
    if (worker_.joinable()) worker_.join();
    if (ctx_ && ctx_->device()) vkDeviceWaitIdle(ctx_->device());
    destroyPipeline();
    destroySceneBuffers();
    if (ctx_) {
        ctx_->destroyBuffer(accumBuf_);
        ctx_->destroyBuffer(previewAccum_);
        if (workFence_) { vkDestroyFence(ctx_->device(), workFence_, nullptr); workFence_ = VK_NULL_HANDLE; }
        if (workPool_) { vkDestroyCommandPool(ctx_->device(), workPool_, nullptr); workPool_ = VK_NULL_HANDLE; }
    }
    image_.destroy();
}

// ---------------------------------------------------------------------------
// Pipeline + descriptors
// ---------------------------------------------------------------------------
bool Renderer::createPipeline() {
    destroyPipeline();
    VkDevice dev = ctx_->device();

    VkDescriptorType binding6 = (backend_ == Backend::HardwareRT)
        ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    VkDescriptorSetLayoutBinding b[7]{};
    auto set = [&](int i, VkDescriptorType t) {
        b[i].binding = i; b[i].descriptorType = t; b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    };
    set(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    set(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    set(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    set(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    set(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    set(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    set(6, binding6);

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 7;
    lci.pBindings = b;
    if (!vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl_), "createDSL")) return false;

    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GPUPushConstants) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (!vkCheck(vkCreatePipelineLayout(dev, &plci, nullptr, &plyt_), "createPL")) return false;

    const unsigned char* spv = (backend_ == Backend::HardwareRT) ? pathtrace_rayquery_spv : pathtrace_compute_spv;
    size_t spvLen = (backend_ == Backend::HardwareRT) ? pathtrace_rayquery_spv_len : pathtrace_compute_spv_len;

    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = spvLen;
    smci.pCode = reinterpret_cast<const uint32_t*>(spv);
    VkShaderModule module = VK_NULL_HANDLE;
    if (!vkCheck(vkCreateShaderModule(dev, &smci, nullptr, &module), "createShaderModule")) return false;

    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = plyt_;
    bool ok = vkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe_),
                      "createComputePipeline");
    vkDestroyShaderModule(dev, module, nullptr);
    if (!ok) return false;

    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (!vkCheck(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool_), "createDescriptorPool")) return false;

    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = dpool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl_;
    if (!vkCheck(vkAllocateDescriptorSets(dev, &dsai, &dset_), "allocDescriptorSet")) return false;
    return true;
}

void Renderer::destroyPipeline() {
    if (!ctx_ || !ctx_->device()) { dsl_ = VK_NULL_HANDLE; plyt_ = VK_NULL_HANDLE; pipe_ = VK_NULL_HANDLE; dpool_ = VK_NULL_HANDLE; dset_ = VK_NULL_HANDLE; return; }
    VkDevice dev = ctx_->device();
    if (dpool_) { vkDestroyDescriptorPool(dev, dpool_, nullptr); dpool_ = VK_NULL_HANDLE; }
    if (pipe_) { vkDestroyPipeline(dev, pipe_, nullptr); pipe_ = VK_NULL_HANDLE; }
    if (plyt_) { vkDestroyPipelineLayout(dev, plyt_, nullptr); plyt_ = VK_NULL_HANDLE; }
    if (dsl_) { vkDestroyDescriptorSetLayout(dev, dsl_, nullptr); dsl_ = VK_NULL_HANDLE; }
    dset_ = VK_NULL_HANDLE;
}

void Renderer::updateAccumBinding(const Buffer& accum) {
    VkDescriptorBufferInfo bi{ accum.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = dset_; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Scene upload
// ---------------------------------------------------------------------------
static bool uploadStorage(VkContext& ctx, const void* data, VkDeviceSize bytes, Buffer& out) {
    if (bytes == 0) {
        // Allocate a tiny non-empty buffer so the SSBO is valid.
        return ctx.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, out);
    }
    return ctx.uploadBuffer(data, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out);
}

bool Renderer::uploadSceneBuffers(const Scene& scene) {
    destroySceneBuffers();
    VkContext& c = *ctx_;

    vertCount_ = (uint32_t)scene.vertices.size();
    triCount_ = (uint32_t)scene.triangles.size();
    lightCount_ = (uint32_t)scene.lights.size();

    if (!uploadStorage(c, scene.vertices.data(), scene.vertices.size() * sizeof(GPUVertex), vbuf_)) return false;
    if (!uploadStorage(c, scene.materials.data(), scene.materials.size() * sizeof(GPUMaterial), mbuf_)) return false;

    if (scene.lights.empty()) {
        GPULight dummy{};
        if (!uploadStorage(c, &dummy, sizeof(GPULight), lbuf_)) return false;
    } else {
        if (!uploadStorage(c, scene.lights.data(), scene.lights.size() * sizeof(GPULight), lbuf_)) return false;
    }

    if (backend_ == Backend::HardwareRT) {
        if (!uploadStorage(c, scene.triangles.data(), scene.triangles.size() * sizeof(GPUTriangle), tbuf_)) return false;
        // A non-empty placeholder for binding 6's slot is not needed (it is the AS).
        if (!c.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bvhBuf_)) return false;
    } else {
        bvh_ = buildBVH(scene.vertices, scene.triangles);
        if (!uploadStorage(c, bvh_.orderedTriangles.data(), bvh_.orderedTriangles.size() * sizeof(GPUTriangle), tbuf_)) return false;
        if (!uploadStorage(c, bvh_.nodes.data(), bvh_.nodes.size() * sizeof(GPUBVHNode), bvhBuf_)) return false;
    }

    if (!c.createBuffer(sizeof(GPUParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, paramsUbo_, true)) return false;
    return true;
}

void Renderer::destroySceneBuffers() {
    if (!ctx_) return;
    ctx_->destroyBuffer(vbuf_);
    ctx_->destroyBuffer(tbuf_);
    ctx_->destroyBuffer(mbuf_);
    ctx_->destroyBuffer(lbuf_);
    ctx_->destroyBuffer(bvhBuf_);
    ctx_->destroyBuffer(paramsUbo_);
    accel_.destroy(*ctx_);
}

// Writes bindings 1..6 (binding 0/accum is set per dispatch).
static void writeSceneDescriptors(VkContext& ctx, VkDescriptorSet ds, Backend backend,
                                  const Buffer& params, const Buffer& verts, const Buffer& tris,
                                  const Buffer& mats, const Buffer& lights, const Buffer& bvh,
                                  const Accel& accel) {
    VkDescriptorBufferInfo pInfo{ params.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo vInfo{ verts.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo tInfo{ tris.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo mInfo{ mats.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lInfo{ lights.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo bInfo{ bvh.buffer, 0, VK_WHOLE_SIZE };

    VkWriteDescriptorSet w[6]{};
    auto mk = [&](int i, uint32_t binding, VkDescriptorType t, VkDescriptorBufferInfo* info) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = ds; w[i].dstBinding = binding; w[i].descriptorCount = 1;
        w[i].descriptorType = t; w[i].pBufferInfo = info;
    };
    mk(0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &pInfo);
    mk(1, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vInfo);
    mk(2, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &tInfo);
    mk(3, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &mInfo);
    mk(4, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lInfo);

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    if (backend == Backend::HardwareRT) {
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &accel.tlas;
        mk(5, 6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr);
        w[5].pNext = &asWrite;
    } else {
        mk(5, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bInfo);
    }
    vkUpdateDescriptorSets(ctx.device(), 6, w, 0, nullptr);
}

// ---------------------------------------------------------------------------
// prepareScene
// ---------------------------------------------------------------------------
bool Renderer::prepareScene(const Scene& scene, Backend requested) {
    if (!ctx_) return false;
    vkDeviceWaitIdle(ctx_->device());
    scene_ = scene;
    settings_.backend = requested;

    backend_ = ctx_->resolveBackend(requested);

    if (!uploadSceneBuffers(scene)) { vkLog("Scene upload failed."); return false; }

    if (backend_ == Backend::HardwareRT) {
        if (!accel_.build(*ctx_, scene.vertices, scene.triangles)) {
            // Silent fallback to the compute path.
            vkLog("AS build failed; falling back to compute backend.");
            backend_ = Backend::Compute;
            destroySceneBuffers();
            if (!uploadSceneBuffers(scene)) return false;
        }
    }

    if (!createPipeline()) return false;
    writeSceneDescriptors(*ctx_, dset_, backend_, paramsUbo_, vbuf_, tbuf_, mbuf_, lbuf_, bvhBuf_, accel_);

    // Worker command resources.
    if (!workPool_) workPool_ = ctx_->createCommandPool(ctx_->computeFamily());
    if (!workCmd_) {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = workPool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx_->device(), &ai, &workCmd_);
    }
    if (!workFence_) {
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(ctx_->device(), &fci, nullptr, &workFence_);
    }

    // 512^2 RGBA32F accumulation tile (host visible, ~4 MiB).
    accumCapacity_ = 512 * 512;
    ctx_->destroyBuffer(accumBuf_);
    if (!ctx_->createBuffer((VkDeviceSize)accumCapacity_ * sizeof(float) * 4,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, accumBuf_, true)) return false;

    previewReset_.store(true);
    previewSpp_ = 0;
    return true;
}

bool Renderer::rebuildDeviceResources() {
    // Called after VK_ERROR_DEVICE_LOST: device + all handles are invalid.
    destroyPipeline();
    // Buffers/AS belong to the dead device; drop handles without destroying.
    vbuf_ = tbuf_ = mbuf_ = lbuf_ = bvhBuf_ = paramsUbo_ = Buffer{};
    accumBuf_ = previewAccum_ = Buffer{};
    accel_ = Accel{};
    workPool_ = VK_NULL_HANDLE; workCmd_ = VK_NULL_HANDLE; workFence_ = VK_NULL_HANDLE;

    if (!ctx_->recreateDevice()) { vkLog("Device recreation failed."); return false; }
    return prepareScene(scene_, settings_.backend);
}

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------
void Renderer::writeParams(const Camera& cam, uint32_t w, uint32_t h, const RenderSettings& s) {
    GPUParams p{};
    vec3 fwd = normalize(cam.target - cam.position);
    vec3 right = normalize(cross(fwd, cam.up));
    vec3 up = cross(right, fwd);
    float aspect = float(w) / float(h);
    float tanH = std::tan(radians(cam.fovYDegrees * 0.5f));
    p.camOrigin = vec4(cam.position, 0);
    p.camU = vec4(right * (tanH * aspect), 0);
    p.camV = vec4(up * tanH, 0);
    p.camW = vec4(fwd, 0);
    bool sky = s.skyEnabled || lightCount_ == 0; // never render fully black
    p.sky0 = vec4(s.skyIntensity, sky ? 1.0f : 0.0f, s.exposure, s.fireflyClampValue);
    p.sky1 = vec4(s.fireflyClamp ? 1.0f : 0.0f, 0, 0, 0);
    p.numLights = lightCount_;
    p.maxBounces = (uint32_t)s.maxBounces;
    p.imageWidth = w;
    p.imageHeight = h;
    std::memcpy(paramsUbo_.mapped, &p, sizeof(p));
}

// ---------------------------------------------------------------------------
// Dispatch helper: one batch into the currently-bound accum buffer.
// Returns submit time in seconds, or -1 on device lost.
// ---------------------------------------------------------------------------
static double dispatchBatch(VkContext& ctx, VkCommandBuffer cmd, VkFence fence,
                            VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet ds,
                            const Buffer& accum, const GPUPushConstants& pc,
                            bool clearFirst, bool& deviceLost) {
    deviceLost = false;
    VkDevice dev = ctx.device();
    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (clearFirst) {
        vkCmdFillBuffer(cmd, accum.buffer, 0, VK_WHOLE_SIZE, 0);
        VkBufferMemoryBarrier fb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fb.buffer = accum.buffer; fb.size = VK_WHOLE_SIZE;
        fb.srcQueueFamilyIndex = fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &fb, 0, nullptr);
    } else {
        VkBufferMemoryBarrier rb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        rb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        rb.buffer = accum.buffer; rb.size = VK_WHOLE_SIZE;
        rb.srcQueueFamilyIndex = rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &rb, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    uint32_t gx = (pc.tileW + 7) / 8;
    uint32_t gy = (pc.tileH + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Make writes visible to the host for the CPU resolve.
    VkBufferMemoryBarrier hb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.buffer = accum.buffer; hb.size = VK_WHOLE_SIZE;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &hb, 0, nullptr);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    double t0 = nowSeconds();
    VkResult r;
    {
        std::lock_guard<std::mutex> lock(ctx.submitMutex());
        r = vkQueueSubmit(ctx.computeQueue(), 1, &si, fence);
    }
    if (r == VK_ERROR_DEVICE_LOST) { deviceLost = true; return -1.0; }
    r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    if (r == VK_ERROR_DEVICE_LOST) { deviceLost = true; return -1.0; }
    double dt = nowSeconds() - t0;

    vmaInvalidateAllocation(ctx.allocator(), accum.alloc, 0, VK_WHOLE_SIZE);
    return dt;
}

// ---------------------------------------------------------------------------
// Final render: tile scheduler
// ---------------------------------------------------------------------------
bool Renderer::startFinalRender(const Scene& scene, const RenderSettings& s,
                                std::wstring& errMsg, uint64_t& requiredBytes) {
    if (finalRunning_.load()) return true;
    if (worker_.joinable()) worker_.join();

    settings_ = s;
    uint32_t w = s.width(), h = s.height();
    requiredBytes = MmapImage::requiredBytes(w, h);

    uint64_t freeBytes = 0;
    if (MmapImage::freeSpace(s.scratchFolder, freeBytes) && freeBytes < requiredBytes) {
        errMsg = L"Insufficient scratch disk space.";
        return false;
    }
    if (!image_.create(s.scratchFolder, w, h)) {
        errMsg = L"Could not create the scratch file (disk full or path invalid).";
        return false;
    }
    // Initialize the file to black so unrendered tiles are black.
    std::memset(image_.data(), 0, (size_t)image_.sizeBytes());

    // Final-preview (box-downsample) buffers.
    uint32_t scale = 1;
    while ((std::max(w, h) + scale - 1) / scale > 4096) ++scale;
    fpScale_ = scale;
    fpW_ = (w + scale - 1) / scale;
    fpH_ = (h + scale - 1) / scale;
    {
        std::lock_guard<std::mutex> lk(finalPrevMutex_);
        fpSum_.assign((size_t)fpW_ * fpH_ * 3, 0.0f);
        fpCount_.assign((size_t)fpW_ * fpH_, 0);
    }

    {
        std::lock_guard<std::mutex> lk(progMutex_);
        prog_ = Progress{};
        uint32_t ts = (uint32_t)s.tileSize;
        prog_.tilesTotal = (uint64_t)((w + ts - 1) / ts) * ((h + ts - 1) / ts);
        prog_.targetSpp = (uint32_t)s.spp;
        prog_.backend = backend_;
    }

    cancelFlag_.store(false);
    finalRunning_.store(true);
    curBatchSpp_ = 1;
    worker_ = std::thread(&Renderer::finalWorker, this);
    return true;
}

void Renderer::cancelFinal() { cancelFlag_.store(true); }

// Renders one region fully into accumBuf_ then resolves to the image.
// Returns: 0 ok, 1 too slow (subdivide), 2 device lost.
bool Renderer::renderTile(uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
                          uint32_t targetSpp, uint32_t& statusOut) {
    statusOut = 0;
    uint32_t accumulated = 0;
    bool first = true;
    while (accumulated < targetSpp) {
        if (cancelFlag_.load()) break;
        uint32_t n = std::min(curBatchSpp_, targetSpp - accumulated);
        GPUPushConstants pc{};
        pc.tileX = tx; pc.tileY = ty; pc.tileW = tw; pc.tileH = th;
        pc.sampleBase = accumulated; pc.sampleCount = n; pc.frameSeed = 0x9E3779B9u;

        bool deviceLost = false;
        double dt = dispatchBatch(*ctx_, workCmd_, workFence_, pipe_, plyt_, dset_,
                                  accumBuf_, pc, first, deviceLost);
        if (deviceLost) { statusOut = 2; return false; }
        first = false;
        accumulated += n;

        {
            std::lock_guard<std::mutex> lk(progMutex_);
            prog_.curTileSpp = accumulated;
            prog_.mrays = dt > 0 ? (double(tw) * th * n) / dt / 1e6 : 0.0;
        }

        // Too slow even at 1 spp on a sizable tile -> ask to subdivide.
        if (n == 1 && dt > kMaxSubmitSec && tw > kMinTile && th > kMinTile && accumulated == 1) {
            statusOut = 1;
            return false;
        }
        // Adapt batch size toward the time budget. The ceiling is kept modest
        // so a single dispatch stays well under any driver watchdog.
        const uint32_t kMaxBatch = 64u;
        if (dt > 0) {
            if (dt < kBudgetSec * 0.6 && curBatchSpp_ < kMaxBatch) curBatchSpp_ = std::min(curBatchSpp_ * 2u, kMaxBatch);
            else if (dt > kBudgetSec * 1.5 && curBatchSpp_ > 1) curBatchSpp_ = std::max(curBatchSpp_ / 2u, 1u);
        }

        // Leave the GPU idle for a slice proportional to the work just done, so
        // that on a shared (display) GPU the compositor gets time to refresh and
        // the screen does not flash. On a fast GPU dt is tiny, so this rounds to
        // zero and the render runs at full speed.
        int yieldMs = (int)(dt * 1000.0 * 0.3);
        if (yieldMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(yieldMs, 20)));
    }
    resolveTileToImage(tx, ty, tw, th, accumulated, settings_);
    return true;
}

void Renderer::resolveTileToImage(uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
                                  uint32_t spp, const RenderSettings& s) {
    const vec4* accum = reinterpret_cast<const vec4*>(accumBuf_.mapped);
    float exposure = s.exposure;

    std::lock_guard<std::mutex> lk(finalPrevMutex_);
    for (uint32_t y = 0; y < th; ++y) {
        uint8_t* dst = image_.rowPtr(ty + y) + (uint64_t)tx * 4;
        for (uint32_t x = 0; x < tw; ++x) {
            vec4 a = accum[y * tw + x];
            float cnt = a.w > 0 ? a.w : 1.0f;
            vec3 col{ a.x / cnt, a.y / cnt, a.z / cnt };
            col = acesFitted(col * exposure);
            uint8_t r = toU8(col.x), g = toU8(col.y), b = toU8(col.z);
            dst[x * 4 + 0] = r; dst[x * 4 + 1] = g; dst[x * 4 + 2] = b; dst[x * 4 + 3] = 255;

            uint32_t cx = (tx + x) / fpScale_, cy = (ty + y) / fpScale_;
            if (cx < fpW_ && cy < fpH_) {
                size_t cell = (size_t)cy * fpW_ + cx;
                fpSum_[cell * 3 + 0] += r / 255.0f;
                fpSum_[cell * 3 + 1] += g / 255.0f;
                fpSum_[cell * 3 + 2] += b / 255.0f;
                fpCount_[cell]++;
            }
        }
    }
}

void Renderer::finalWorker() {
    double start = nowSeconds();
    const uint32_t ts = (uint32_t)settings_.tileSize;
    const uint32_t W = image_.width(), H = image_.height();
    const uint32_t targetSpp = (uint32_t)settings_.spp;

    // Bind the tile accumulation buffer to descriptor slot 0 and upload the
    // camera/scene parameters for the full image (the preview path writes its
    // own params, so the final render must set them here).
    updateAccumBinding(accumBuf_);
    writeParams(scene_.camera, W, H, settings_);

    int recoveries = 0;
    const int kMaxRecoveries = 6;
    uint64_t done = 0;
    for (uint32_t ty = 0; ty < H && !cancelFlag_.load(); ty += ts) {
        for (uint32_t tx = 0; tx < W && !cancelFlag_.load(); tx += ts) {
            uint32_t tw = std::min(ts, W - tx);
            uint32_t th = std::min(ts, H - ty);

            // Restart the adaptive batch from 1 spp for every grid tile so an
            // expensive tile can never inherit a large batch grown on cheap
            // tiles (a big single dispatch can exceed the driver watchdog and
            // crash some drivers instead of returning VK_ERROR_DEVICE_LOST).
            curBatchSpp_ = 1;

            // Process this grid tile, subdividing for TDR safety as needed.
            std::vector<std::array<uint32_t, 4>> stack;
            stack.push_back({ tx, ty, tw, th });
            while (!stack.empty()) {
                if (cancelFlag_.load()) break;
                auto r = stack.back(); stack.pop_back();
                uint32_t status = 0;
                bool ok = renderTile(r[0], r[1], r[2], r[3], targetSpp, status);
                if (!ok && status == 2) {
                    // Device lost: recover and restart this grid tile. Cap the
                    // number of attempts so a persistently failing GPU can never
                    // be hammered in a tight reset loop.
                    if (++recoveries > kMaxRecoveries) {
                        vkLog("Too many device-lost events; stopping. Partial image kept.");
                        std::lock_guard<std::mutex> lk(progMutex_);
                        prog_.finished = true;
                        finalRunning_.store(false);
                        return;
                    }
                    vkLog("VK_ERROR_DEVICE_LOST - recovering.");
                    curBatchSpp_ = 1;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (!rebuildDeviceResources()) {
                        std::lock_guard<std::mutex> lk(progMutex_);
                        prog_.finished = true;
                        finalRunning_.store(false);
                        return;
                    }
                    updateAccumBinding(accumBuf_); // descriptor set was recreated
                    writeParams(scene_.camera, W, H, settings_);
                    stack.clear();
                    stack.push_back({ tx, ty, tw, th });
                    continue;
                }
                if (!ok && status == 1) {
                    // Too slow: split into quadrants (down to kMinTile).
                    uint32_t hw = std::max(r[2] / 2, kMinTile);
                    uint32_t hh = std::max(r[3] / 2, kMinTile);
                    for (uint32_t sy = r[1]; sy < r[1] + r[3]; sy += hh)
                        for (uint32_t sx = r[0]; sx < r[0] + r[2]; sx += hw)
                            stack.push_back({ sx, sy, std::min(hw, r[0] + r[2] - sx),
                                              std::min(hh, r[1] + r[3] - sy) });
                    curBatchSpp_ = 1;
                    continue;
                }
            }

            ++done;
            double elapsed = nowSeconds() - start;
            std::lock_guard<std::mutex> lk(progMutex_);
            prog_.tilesDone = done;
            prog_.elapsed = elapsed;
            double per = done > 0 ? elapsed / double(done) : 0;
            prog_.eta = per * double(prog_.tilesTotal - done);
        }
    }

    std::lock_guard<std::mutex> lk(progMutex_);
    prog_.finished = true;
    prog_.elapsed = nowSeconds() - start;
    if (cancelFlag_.load()) prog_.eta = 0;
    finalRunning_.store(false);
}

Renderer::Progress Renderer::progress() const {
    std::lock_guard<std::mutex> lk(progMutex_);
    return prog_;
}

void Renderer::finalPreviewSize(uint32_t& w, uint32_t& h) const {
    std::lock_guard<std::mutex> lk(finalPrevMutex_);
    w = fpW_; h = fpH_;
}

void Renderer::copyFinalPreview(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    std::lock_guard<std::mutex> lk(finalPrevMutex_);
    w = fpW_; h = fpH_;
    rgba.assign((size_t)fpW_ * fpH_ * 4, 0);
    for (size_t i = 0; i < (size_t)fpW_ * fpH_; ++i) {
        uint32_t c = fpCount_[i];
        if (c == 0) { rgba[i * 4 + 3] = 255; continue; }
        rgba[i * 4 + 0] = (uint8_t)clampf(fpSum_[i * 3 + 0] / c * 255.0f, 0, 255);
        rgba[i * 4 + 1] = (uint8_t)clampf(fpSum_[i * 3 + 1] / c * 255.0f, 0, 255);
        rgba[i * 4 + 2] = (uint8_t)clampf(fpSum_[i * 3 + 2] / c * 255.0f, 0, 255);
        rgba[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------
// Interactive preview
// ---------------------------------------------------------------------------
void Renderer::setPreviewCamera(const Camera& cam) {
    scene_.camera = cam;
    previewReset_.store(true);
}
void Renderer::requestPreviewReset() { previewReset_.store(true); }

bool Renderer::renderPreviewFrame(uint32_t w, uint32_t h, const RenderSettings& s) {
    if (!pipe_ || w == 0 || h == 0) return false;

    // Cap internal preview resolution so a frame stays interactive (adaptive).
    uint32_t maxSide = previewMaxSide_;
    uint32_t pw = w, ph = h;
    uint32_t longSide = std::max(pw, ph);
    if (longSide > maxSide) {
        float k = float(maxSide) / float(longSide);
        pw = std::max(1u, (uint32_t)(pw * k));
        ph = std::max(1u, (uint32_t)(ph * k));
    }

    if (pw != previewW_ || ph != previewH_) {
        ctx_->destroyBuffer(previewAccum_);
        if (!ctx_->createBuffer((VkDeviceSize)pw * ph * sizeof(float) * 4,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, previewAccum_, true))
            return false;
        previewW_ = pw; previewH_ = ph;
        previewReset_.store(true);
    }

    bool reset = previewReset_.exchange(false);
    if (reset) previewSpp_ = 0;

    writeParams(scene_.camera, pw, ph, s);
    updateAccumBinding(previewAccum_);

    GPUPushConstants pc{};
    pc.tileX = 0; pc.tileY = 0; pc.tileW = pw; pc.tileH = ph;
    pc.sampleBase = previewSpp_; pc.sampleCount = 1; pc.frameSeed = 0x1234567u;

    bool deviceLost = false;
    double dt = dispatchBatch(*ctx_, workCmd_ ? workCmd_ : VK_NULL_HANDLE, workFence_,
                              pipe_, plyt_, dset_, previewAccum_, pc, reset, deviceLost);
    if (deviceLost) { rebuildDeviceResources(); return false; }
    previewSpp_ += 1;

    // Adapt the preview resolution to keep the frame (and the shared display)
    // responsive: shrink if a frame is slow, grow back when there is headroom.
    if (dt > 0.06 && previewMaxSide_ > 160)
        previewMaxSide_ = std::max(160u, (uint32_t)(previewMaxSide_ * 0.8f));
    else if (dt > 0 && dt < 0.02 && previewMaxSide_ < 1024)
        previewMaxSide_ = std::min(1024u, previewMaxSide_ + 64u);

    // Resolve to RGBA8.
    const vec4* accum = reinterpret_cast<const vec4*>(previewAccum_.mapped);
    std::lock_guard<std::mutex> lk(previewMutex_);
    previewPixels_.assign((size_t)pw * ph * 4, 0);
    for (uint32_t i = 0; i < pw * ph; ++i) {
        vec4 a = accum[i];
        float cnt = a.w > 0 ? a.w : 1.0f;
        vec3 col = acesFitted(vec3{ a.x / cnt, a.y / cnt, a.z / cnt } * s.exposure);
        previewPixels_[i * 4 + 0] = toU8(col.x);
        previewPixels_[i * 4 + 1] = toU8(col.y);
        previewPixels_[i * 4 + 2] = toU8(col.z);
        previewPixels_[i * 4 + 3] = 255;
    }
    return true;
}

void Renderer::copyPreviewPixels(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    std::lock_guard<std::mutex> lk(previewMutex_);
    w = previewW_; h = previewH_;
    rgba = previewPixels_;
}
