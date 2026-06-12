// renderer.h - the out-of-core tiled path-tracing engine plus a shared
// interactive-preview path. Owns all GPU scene resources and the compute
// pipeline; runs the final render on a worker thread with TDR-safe batching
// and device-lost recovery.
#pragma once
#include "vk_context.h"
#include "scene.h"
#include "mmap_image.h"
#include "accel.h"
#include "bvh.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>

class Renderer {
public:
    void init(VkContext* ctx);
    void shutdown();

    // Upload a scene and build all GPU resources for `backend`. Safe to call
    // again to switch scenes; tears down previous resources first.
    bool prepareScene(const Scene& scene, Backend requested);
    Backend activeBackend() const { return backend_; }

    // ---- interactive preview ----
    // Renders/accumulates a few spp of the scene at (w,h) into an internal
    // RGBA8 buffer. Resets accumulation when the camera/size changes. Returns
    // true if a fresh frame is available in copyPreview().
    void setPreviewCamera(const Camera& cam);
    void requestPreviewReset();
    bool renderPreviewFrame(uint32_t w, uint32_t h, const RenderSettings& s);
    void copyPreviewPixels(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);

    // ---- final render ----
    // Allocates the scratch mmap and starts the worker. Returns false (with
    // errMsg + requiredBytes) only for insufficient disk space.
    bool startFinalRender(const Scene& scene, const RenderSettings& s,
                          std::wstring& errMsg, uint64_t& requiredBytes);
    void cancelFinal();
    bool finalRunning() const { return finalRunning_.load(); }
    MmapImage& finalImage() { return image_; }

    struct Progress {
        uint64_t tilesDone = 0, tilesTotal = 0;
        uint32_t curTileSpp = 0, targetSpp = 0;
        double   elapsed = 0, eta = 0, mrays = 0;
        Backend  backend = Backend::Compute;
        bool     finished = false;
    };
    Progress progress() const;

    // Preview (box-downsampled) of the final image for the result viewer.
    void finalPreviewSize(uint32_t& w, uint32_t& h) const;
    void copyFinalPreview(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);

private:
    // GPU resource management
    bool createPipeline();
    void destroyPipeline();
    bool uploadSceneBuffers(const Scene& scene);
    void destroySceneBuffers();
    bool rebuildDeviceResources();           // after device-lost
    void updateAccumBinding(const Buffer& accum);
    void writeParams(const Camera& cam, uint32_t w, uint32_t h, const RenderSettings& s);

    // worker
    void finalWorker();
    bool renderTile(uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
                    uint32_t targetSpp, uint32_t& outBatch);
    void resolveTileToImage(uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
                            uint32_t spp, const RenderSettings& s);

    VkContext* ctx_ = nullptr;
    Backend    backend_ = Backend::Compute;

    VkDescriptorSetLayout dsl_  = VK_NULL_HANDLE;
    VkPipelineLayout      plyt_ = VK_NULL_HANDLE;
    VkPipeline            pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool      dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet       dset_ = VK_NULL_HANDLE;

    Buffer vbuf_, tbuf_, mbuf_, lbuf_, bvhBuf_, paramsUbo_;
    Accel  accel_;
    uint32_t vertCount_ = 0, triCount_ = 0, lightCount_ = 0;
    Scene  scene_;                 // retained for params + device recovery
    BVHResult bvh_;                // retained for device recovery

    VkCommandPool   workPool_ = VK_NULL_HANDLE;
    VkCommandBuffer workCmd_  = VK_NULL_HANDLE;
    VkFence         workFence_ = VK_NULL_HANDLE;

    // preview state
    Buffer previewAccum_;
    uint32_t previewW_ = 0, previewH_ = 0;
    uint32_t previewSpp_ = 0;
    uint32_t previewMaxSide_ = 512;   // adapts to hold an interactive frame rate
    std::atomic<bool> previewReset_{ true };
    std::mutex previewMutex_;
    std::vector<uint8_t> previewPixels_;   // resolved RGBA8

    // final state
    MmapImage image_;
    RenderSettings settings_;
    std::thread worker_;
    std::atomic<bool> finalRunning_{ false };
    std::atomic<bool> cancelFlag_{ false };
    Buffer accumBuf_;              // 1 tile RGBA32F, host visible
    uint32_t accumCapacity_ = 0;   // pixels

    // final preview accumulation (box downsample), guarded by finalPrevMutex_
    mutable std::mutex finalPrevMutex_;
    std::vector<float>    fpSum_;     // rgb per cell
    std::vector<uint32_t> fpCount_;
    uint32_t fpW_ = 0, fpH_ = 0, fpScale_ = 1;

    // progress, guarded by progMutex_
    mutable std::mutex progMutex_;
    Progress prog_;
    uint32_t curBatchSpp_ = 1;
};
