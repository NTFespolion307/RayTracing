#include "ui.h"
#include "obj_loader.h"
#include "theme.h"

#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <windows.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
namespace {
void messageBox(const char* text, const char* title, UINT icon) {
    MessageBoxA(nullptr, text, title, MB_OK | icon);
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

PFN_vkVoidFunction imguiLoader(const char* name, void* user) {
    return vkGetInstanceProcAddr((VkInstance)user, name);
}

// Native open dialog for OBJ; returns empty on cancel.
std::wstring openObjDialog() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) return result;
    COMDLG_FILTERSPEC types[] = { { L"Wavefront OBJ", L"*.obj" }, { L"All files", L"*.*" } };
    dlg->SetFileTypes(2, types);
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring saveFileDialog(const wchar_t* ext, const wchar_t* desc, const wchar_t* pattern) {
    std::wstring result;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) return result;
    COMDLG_FILTERSPEC type[] = { { desc, pattern } };
    dlg->SetFileTypes(1, type);
    dlg->SetDefaultExtension(ext);
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring pickFolderDialog() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}
} // namespace

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
bool App::initWindow() {
    if (!glfwInit()) { messageBox("Failed to initialize GLFW.", "VkGigaTracer", MB_ICONERROR); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (headless_) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(1440, 900, "VkGigaTracer", nullptr, nullptr);
    if (!window_) { messageBox("Failed to create a window.", "VkGigaTracer", MB_ICONERROR); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan + swapchain
// ---------------------------------------------------------------------------
bool App::initVulkan() {
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true;
#endif
    if (!ctx_.initInstance(validation)) {
        messageBox("A Vulkan-capable GPU driver is required to run VkGigaTracer.\n"
                   "vulkan-1.dll could not be loaded or no Vulkan instance could be created.",
                   "VkGigaTracer", MB_ICONERROR);
        return false;
    }
    vkLog("initVulkan: instance ok");
    glfwInitVulkanLoader((PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr);

    if (glfwCreateWindowSurface(ctx_.instance(), window_, nullptr, &surface_) != VK_SUCCESS) {
        messageBox("Failed to create a window surface.", "VkGigaTracer", MB_ICONERROR);
        return false;
    }
    vkLog("initVulkan: surface ok");
    if (!ctx_.createDevice(surface_)) {
        messageBox("No suitable Vulkan device was found (needs present + compute).",
                   "VkGigaTracer", MB_ICONERROR);
        return false;
    }
    vkLog("initVulkan: device ok");

    Backend b = ctx_.resolveBackend(Backend::Auto);
    backendLabel_ = (b == Backend::HardwareRT) ? "Hardware Ray Query" : "Compute";

    // Pick a surface format ourselves via volk. (ImGui's *_SelectSurfaceFormat
    // helper cannot be used yet: ImGui's Vulkan function pointers are not loaded
    // until ImGui_ImplVulkan_LoadFunctions runs in initImGui.)
    {
        uint32_t fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical(), surface_, &fc, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fc ? fc : 1);
        if (fc) vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical(), surface_, &fc, formats.data());
        scFormat_ = (fc && formats[0].format != VK_FORMAT_UNDEFINED) ? formats[0].format
                                                                     : VK_FORMAT_B8G8R8A8_UNORM;
        for (const auto& f : formats) {
            if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                scFormat_ = f.format;
                break;
            }
        }
    }

    VkAttachmentDescription color{};
    color.format = scFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &color;
    rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (!vkCheck(vkCreateRenderPass(ctx_.device(), &rpci, nullptr, &renderPass_), "createRenderPass"))
        return false;

    if (!createSwapchain()) return false;

    // Per-frame sync + command buffers.
    framePool_ = ctx_.createCommandPool(ctx_.graphicsFamily());
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = framePool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(ctx_.device(), &ai, frameCmd_);
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kFramesInFlight; ++i) {
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &imgAvail_[i]);
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &renderDone_[i]);
        vkCreateFence(ctx_.device(), &fci, nullptr, &inFlight_[i]);
    }

    renderer_.init(&ctx_);
    viewer_.init(&ctx_);
    return true;
}

bool App::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical(), surface_, &caps);

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    if (fbw == 0 || fbh == 0) return false; // minimized

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::clamp((uint32_t)fbw, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp((uint32_t)fbh, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    scExtent_ = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    minImageCount_ = caps.minImageCount < 2 ? 2 : caps.minImageCount;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = scFormat_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;
    if (!vkCheck(vkCreateSwapchainKHR(ctx_.device(), &ci, nullptr, &swapchain_), "createSwapchain"))
        return false;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, nullptr);
    scImages_.resize(n);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, scImages_.data());

    scViews_.resize(n);
    scFramebuffers_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = scImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = scFormat_;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(ctx_.device(), &vci, nullptr, &scViews_[i]);

        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = renderPass_;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &scViews_[i];
        fbci.width = extent.width;
        fbci.height = extent.height;
        fbci.layers = 1;
        vkCreateFramebuffer(ctx_.device(), &fbci, nullptr, &scFramebuffers_[i]);
    }
    return true;
}

void App::destroySwapchain() {
    for (auto fb : scFramebuffers_) vkDestroyFramebuffer(ctx_.device(), fb, nullptr);
    for (auto v : scViews_) vkDestroyImageView(ctx_.device(), v, nullptr);
    scFramebuffers_.clear();
    scViews_.clear();
    if (swapchain_) { vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

void App::recreateSwapchain() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) { glfwWaitEvents(); glfwGetFramebufferSize(window_, &w, &h); }
    vkDeviceWaitIdle(ctx_.device());
    destroySwapchain();
    createSwapchain();
    ImGui_ImplVulkan_SetMinImageCount(minImageCount_);
    swapchainDirty_ = false;
}

// ---------------------------------------------------------------------------
// ImGui
// ---------------------------------------------------------------------------
bool App::initImGui() {
    VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = sizes;
    if (!vkCheck(vkCreateDescriptorPool(ctx_.device(), &dpci, nullptr, &imguiPool_), "imguiPool")) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // keyboard accessibility
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    float dpiScale = xscale > 0 ? xscale : 1.0f;
    // DPI is baked into the font sizes and the style metrics (not just the
    // font), so leave FontGlobalScale at 1 to avoid double-scaling.
    theme::loadFonts(io, dpiScale);
    theme::apply(dpiScale, theme::Mode::Light);

    ImGui_ImplGlfw_InitForVulkan(window_, true);
    ImGui_ImplVulkan_LoadFunctions(imguiLoader, ctx_.instance());

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = ctx_.instance();
    info.PhysicalDevice = ctx_.physical();
    info.Device = ctx_.device();
    info.QueueFamily = ctx_.graphicsFamily();
    info.Queue = ctx_.graphicsQueue();
    info.DescriptorPool = imguiPool_;
    info.RenderPass = renderPass_;
    info.MinImageCount = minImageCount_;
    info.ImageCount = (uint32_t)scImages_.size();
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) {
        messageBox("Failed to initialize the ImGui Vulkan backend.", "VkGigaTracer", MB_ICONERROR);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------
void App::loadDemoScene() {
    scene_ = buildDemoScene();
    if (renderer_.prepareScene(scene_, settings_.backend)) {
        sceneReady_ = true;
        Backend b = renderer_.activeBackend();
        backendLabel_ = (b == Backend::HardwareRT) ? "Hardware Ray Query" : "Compute";
        renderer_.setPreviewCamera(scene_.camera);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Demo scene loaded (%zu triangles).", scene_.triangleCount());
        statusLine_ = buf;
    } else {
        statusLine_ = "Failed to prepare the demo scene.";
    }
}

void App::loadObjDialog() {
    std::wstring path = openObjDialog();
    if (path.empty()) return;
    ObjLoadResult res = loadObj(path);
    if (!res.ok) {
        statusLine_ = "OBJ load failed: " + res.message;
        return;
    }
    scene_ = std::move(res.scene);
    if (renderer_.prepareScene(scene_, settings_.backend)) {
        sceneReady_ = true;
        Backend b = renderer_.activeBackend();
        backendLabel_ = (b == Backend::HardwareRT) ? "Hardware Ray Query" : "Compute";
        renderer_.setPreviewCamera(scene_.camera);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Loaded %s (%zu triangles).",
                      scene_.name.c_str(), scene_.triangleCount());
        statusLine_ = buf;
    } else {
        statusLine_ = "Failed to prepare the loaded scene.";
    }
}

void App::startFinalRender() {
    if (!sceneReady_ || renderer_.finalRunning()) return;
    std::wstring err;
    uint64_t required = 0;
    if (!renderer_.startFinalRender(scene_, settings_, err, required)) {
        double gb = required / (1024.0 * 1024.0 * 1024.0);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "This render needs about %.1f GB of free scratch space.\n"
            "Free up space or choose a different scratch drive in Advanced settings.", gb);
        messageBox(buf, "VkGigaTracer - Disk Space", MB_ICONWARNING);
        statusLine_ = "Render not started: insufficient scratch disk space.";
        return;
    }
    viewer_.resetView();
    requestTab_ = 1;   // switch to the result tab once; user can switch back
    statusLine_ = "Rendering...";
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
void App::startSave(SaveFormat fmt) {
    const wchar_t* ext; const wchar_t* desc; const wchar_t* pat;
    switch (fmt) {
        case SaveFormat::PNG:  ext = L"png"; desc = L"PNG image"; pat = L"*.png"; break;
        case SaveFormat::JPEG: ext = L"jpg"; desc = L"JPEG image"; pat = L"*.jpg"; break;
        default:               ext = L"ppm"; desc = L"Portable Pixmap"; pat = L"*.ppm"; break;
    }
    std::wstring path = saveFileDialog(ext, desc, pat);
    if (path.empty()) return;

    if (saveThread_.joinable()) saveThread_.join();
    saveProgress_.progress.store(0.0f);
    saveProgress_.cancel.store(false);
    saveProgress_.done.store(false);
    saveProgress_.success.store(false);
    saveProgress_.message.clear();
    pendingSavePath_ = path;
    pendingSaveFormat_ = fmt;
    saving_ = true;
    saveModalOpen_ = true;
    int q = jpegQuality_;
    saveThread_ = std::thread([this, fmt, path, q]() {
        saveImage(renderer_.finalImage(), path, fmt, q, saveProgress_);
    });
}

// ---------------------------------------------------------------------------
// UI panels
// ---------------------------------------------------------------------------
// A combo whose currently-selected option text is `preview`; rows of `count`
// entries built by `entry(i, char* buf, size_t)`. Returns the picked index, or
// -1 if nothing was clicked. Laid out as a label-left / control-right row.
namespace {
template <class EntryFn>
int comboRow(const char* label, const char* id, const char* preview,
             int count, int current, EntryFn entry) {
    int picked = -1;
    theme::RowLabel(label);
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < count; ++i) {
            char buf[64];
            entry(i, buf, sizeof(buf));
            if (ImGui::Selectable(buf, current == i)) picked = i;
        }
        ImGui::EndCombo();
    }
    return picked;
}
} // namespace

void App::drawSidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colors().surface);
    ImGui::BeginChild("sidebar", ImVec2(theme::scale(340), 0), ImGuiChildFlags_Border);

    theme::SectionHeader("RENDER SETTINGS");

    // --- Output -----------------------------------------------------------
    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
        char prev[64];
        std::snprintf(prev, sizeof(prev), "%s  (%u x %u)",
                      kResolutions[settings_.resolutionIndex].label,
                      kResolutions[settings_.resolutionIndex].width,
                      kResolutions[settings_.resolutionIndex].height);
        int r = comboRow("Resolution", "##res", prev, kResolutionCount,
                         settings_.resolutionIndex,
                         [](int i, char* b, size_t n) {
            std::snprintf(b, n, "%s  (%u x %u)", kResolutions[i].label,
                          kResolutions[i].width, kResolutions[i].height);
        });
        if (r >= 0) settings_.resolutionIndex = r;
    }

    // --- Quality ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        char prev[16];
        std::snprintf(prev, sizeof(prev), "%d", settings_.spp);
        int sIdx = -1;
        for (int i = 0; i < kSppOptionCount; ++i) if (kSppOptions[i] == settings_.spp) sIdx = i;
        int s = comboRow("Samples", "##spp", prev, kSppOptionCount, sIdx,
                         [](int i, char* b, size_t n) { std::snprintf(b, n, "%d", kSppOptions[i]); });
        if (s >= 0) settings_.spp = kSppOptions[s];

        std::snprintf(prev, sizeof(prev), "%d", settings_.maxBounces);
        int bIdx = -1;
        for (int i = 0; i < kBounceOptionCount; ++i) if (kBounceOptions[i] == settings_.maxBounces) bIdx = i;
        int bb = comboRow("Max bounces", "##bounce", prev, kBounceOptionCount, bIdx,
                          [](int i, char* b, size_t n) { std::snprintf(b, n, "%d", kBounceOptions[i]); });
        if (bb >= 0) settings_.maxBounces = kBounceOptions[bb];

        theme::RowLabel("Exposure");
        ImGui::SliderFloat("##exposure", &settings_.exposure, 0.1f, 8.0f, "%.2f");

        ImGui::Checkbox("Firefly clamp", &settings_.fireflyClamp);
        if (settings_.fireflyClamp) {
            theme::RowLabel("Clamp value");
            ImGui::SliderFloat("##clamp", &settings_.fireflyClampValue, 1.0f, 256.0f, "%.0f");
        }
    }

    // --- Lighting ---------------------------------------------------------
    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Sky", &settings_.skyEnabled);
        if (settings_.skyEnabled) {
            theme::RowLabel("Sky intensity");
            ImGui::SliderFloat("##skyint", &settings_.skyIntensity, 0.0f, 4.0f, "%.2f");
        }
    }

    // --- Performance / hardware ------------------------------------------
    if (ImGui::CollapsingHeader("Performance")) {
        const char* backends[] = { "Auto", "Hardware RT", "Compute" };
        theme::RowLabel("Backend");
        int bidx = (int)settings_.backend;
        if (ImGui::Combo("##backend", &bidx, backends, 3)) {
            settings_.backend = (Backend)bidx;
            if (sceneReady_ && !renderer_.finalRunning()) {
                renderer_.prepareScene(scene_, settings_.backend);
                Backend b = renderer_.activeBackend();
                backendLabel_ = (b == Backend::HardwareRT) ? "Hardware Ray Query" : "Compute";
                renderer_.setPreviewCamera(scene_.camera);
            }
        }

        char prev[16];
        std::snprintf(prev, sizeof(prev), "%d", settings_.tileSize);
        int tIdx = -1;
        for (int i = 0; i < kTileSizeOptionCount; ++i) if (kTileSizeOptions[i] == settings_.tileSize) tIdx = i;
        int t = comboRow("Tile size", "##tile", prev, kTileSizeOptionCount, tIdx,
                         [](int i, char* b, size_t n) { std::snprintf(b, n, "%d", kTileSizeOptions[i]); });
        if (t >= 0) settings_.tileSize = kTileSizeOptions[t];

        std::string sf = settings_.scratchFolder.empty() ? "(system temp)" : wideToUtf8(settings_.scratchFolder);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
        ImGui::TextWrapped("Scratch: %s", sf.c_str());
        ImGui::PopStyleColor();
        if (theme::SecondaryButton("Choose scratch folder...", ImVec2(-FLT_MIN, 0))) {
            std::wstring f = pickFolderDialog();
            if (!f.empty()) settings_.scratchFolder = f;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawPreview() {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x > 16 && region.y > 16 && sceneReady_ && !renderer_.finalRunning()) {
        ImGui::InvisibleButton("previewInput", region,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();
        bool changed = false;
        Camera& cam = scene_.camera;
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            cam.yaw -= io.MouseDelta.x * 0.01f;
            cam.pitch = clampf(cam.pitch + io.MouseDelta.y * 0.01f, -1.55f, 1.55f);
            changed = true;
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            vec3 fwd = normalize(cam.target - cam.position);
            vec3 right = normalize(cross(fwd, cam.up));
            vec3 up = cross(right, fwd);
            float k = cam.distance * 0.0015f;
            cam.pivot -= right * (io.MouseDelta.x * k);
            cam.pivot += up * (io.MouseDelta.y * k);
            changed = true;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            cam.distance = clampf(cam.distance * std::pow(0.9f, io.MouseWheel), 0.05f, 1000.0f);
            changed = true;
        }
        if (changed) { cam.updateFromOrbit(); renderer_.setPreviewCamera(cam); }

        renderer_.renderPreviewFrame((uint32_t)region.x, (uint32_t)region.y, settings_);
        std::vector<uint8_t> px; uint32_t pw = 0, ph = 0;
        renderer_.copyPreviewPixels(px, pw, ph);
        ImGui::SetCursorScreenPos(p0);
        viewer_.drawFitted(px, pw, ph);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
        if (!sceneReady_)
            ImGui::TextUnformatted("Load the demo scene or an OBJ to begin.");
        else if (renderer_.finalRunning())
            ImGui::TextUnformatted("A final render is running - see the Result view.");
        ImGui::PopStyleColor();
    }
}

void App::drawResultView() {
    Renderer::Progress pr = renderer_.progress();
    bool running = renderer_.finalRunning();

    // Progress bar (full width) shown while running or once any tile is done.
    if (running || pr.tilesDone > 0) {
        float frac = pr.tilesTotal ? float(pr.tilesDone) / float(pr.tilesTotal) : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, theme::scale(8)), "");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
        ImGui::Text("Tiles %llu / %llu   \xc2\xb7   spp %u / %u   \xc2\xb7   %.2f MRays/s   \xc2\xb7   %.1fs elapsed   \xc2\xb7   ETA %.1fs",
                    (unsigned long long)pr.tilesDone, (unsigned long long)pr.tilesTotal,
                    pr.curTileSpp, pr.targetSpp, pr.mrays, pr.elapsed, pr.eta);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
        ImGui::TextUnformatted("No render yet. Press Render to start.");
        ImGui::PopStyleColor();
    }

    // Update the final-image preview texture periodically.
    if (++finalPreviewThrottle_ >= 8 || (!running && finalPreviewThrottle_ > 0)) {
        finalPreviewThrottle_ = 0;
        std::vector<uint8_t> px; uint32_t pw = 0, ph = 0;
        renderer_.copyFinalPreview(px, pw, ph);
        if (pw > 0 && ph > 0) viewer_.setPreview(px, pw, ph);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colors().surfaceAlt);
    ImGui::BeginChild("resultview", ImVec2(0, 0), ImGuiChildFlags_Border);
    viewer_.drawResult(settings_.width(), settings_.height(), &renderer_.finalImage());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawViewport() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colors().surface);
    ImGui::BeginChild("viewport", ImVec2(0, 0), ImGuiChildFlags_Border);

    // A programmatic switch (e.g. pressing Render) wins for this frame.
    if (requestTab_ >= 0) { activeTab_ = requestTab_; requestTab_ = -1; }

    const char* modes[] = { "Preview", "Result" };
    theme::SegmentedControl("vpmode", modes, 2, &activeTab_);

    // Right-aligned contextual action for the Result view.
    if (activeTab_ == 1) {
        float bw = theme::scale(140);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        if (renderer_.finalRunning()) {
            if (theme::DangerButton("Cancel", ImVec2(bw, 0))) {
                renderer_.cancelFinal();
                statusLine_ = "Cancelling...";
            }
        } else {
            bool any = renderer_.progress().tilesDone > 0;
            if (!any) ImGui::BeginDisabled();
            // Defer OpenPopup to drawSaveModal so it shares the popup's ID scope.
            if (theme::PrimaryButton("Save Image...", ImVec2(bw, 0))) openSavePopup_ = true;
            if (!any) ImGui::EndDisabled();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
        ImGui::SameLine(0, theme::space().lg);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("drag: orbit   \xc2\xb7   right-drag: pan   \xc2\xb7   wheel: zoom");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (activeTab_ == 0) drawPreview();
    else                 drawResultView();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    drawSaveModal();
}

void App::drawHeaderBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::space().md, theme::space().sm));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colors().surface);
    ImGui::BeginChild("header", ImVec2(0, theme::scale(56)), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar);

    if (theme::fontHeader()) ImGui::PushFont(theme::fontHeader());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("VkGigaTracer");
    if (theme::fontHeader()) ImGui::PopFont();

    ImGui::SameLine(0, theme::space().lg);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().textMuted);
    ImGui::Text("GPU: %s   \xc2\xb7   Backend: %s", ctx_.deviceName(), backendLabel_.c_str());
    ImGui::PopStyleColor();

    // Light/dark theme toggle (sits left of the action cluster, which still
    // right-aligns itself, so its layout is unaffected).
    bool dark = theme::mode() == theme::Mode::Dark;
    ImGui::SameLine(0, theme::space().lg);
    if (theme::SecondaryButton(dark ? "Light mode" : "Dark mode", ImVec2(theme::scale(110), 0)))
        theme::setMode(dark ? theme::Mode::Light : theme::Mode::Dark);

    // Right-aligned action cluster: Demo | Load OBJ | Render.
    float bw1 = theme::scale(110), bw2 = theme::scale(120), bw3 = theme::scale(130);
    float gap = theme::space().sm;
    float cluster = bw1 + bw2 + bw3 + gap * 2;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cluster);
    if (theme::SecondaryButton("Demo Scene", ImVec2(bw1, 0))) loadDemoScene();
    ImGui::SameLine(0, gap);
    if (theme::SecondaryButton("Load OBJ...", ImVec2(bw2, 0))) loadObjDialog();
    ImGui::SameLine(0, gap);
    bool canRender = sceneReady_ && !renderer_.finalRunning();
    if (!canRender) ImGui::BeginDisabled();
    if (theme::PrimaryButton("Render", ImVec2(bw3, 0))) startFinalRender();
    if (!canRender) ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::drawStatusBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::space().md, theme::space().sm));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colors().surface);
    ImGui::BeginChild("statusbar", ImVec2(0, theme::scale(36)), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar);
    uint64_t bytes = MmapImage::requiredBytes(settings_.width(), settings_.height());
    char right[128];
    std::snprintf(right, sizeof(right), "Output %u x %u   \xc2\xb7   Scratch %.1f GB",
                  settings_.width(), settings_.height(), bytes / (1024.0 * 1024.0 * 1024.0));
    theme::StatusBar(statusLine_.empty() ? "Ready." : statusLine_.c_str(), right);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::drawSaveModal() {
    bool jpegAllowed = settings_.width() <= kJpegMaxSide && settings_.height() <= kJpegMaxSide;

    if (openSavePopup_) { ImGui::OpenPopup("SaveOptions"); openSavePopup_ = false; }
    if (ImGui::BeginPopup("SaveOptions")) {
        theme::SectionHeader("SAVE IMAGE");
        if (theme::SecondaryButton("PNG (lossless)", ImVec2(theme::scale(220), 0))) {
            ImGui::CloseCurrentPopup(); startSave(SaveFormat::PNG);
        }
        if (!jpegAllowed) ImGui::BeginDisabled();
        if (theme::SecondaryButton("JPEG", ImVec2(theme::scale(220), 0))) {
            ImGui::CloseCurrentPopup(); startSave(SaveFormat::JPEG);
        }
        if (!jpegAllowed) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("JPEG is limited to 65500 px per side.");
        }
        theme::RowLabel("JPEG quality");
        ImGui::SliderInt("##jpegq", &jpegQuality_, 1, 100);
        if (theme::SecondaryButton("PPM (P6 raw)", ImVec2(theme::scale(220), 0))) {
            ImGui::CloseCurrentPopup(); startSave(SaveFormat::PPM);
        }
        ImGui::EndPopup();
    }

    if (saveModalOpen_) {
        ImGui::OpenPopup("Saving");
        saveModalOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Saving", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Saving image...");
        ImGui::ProgressBar(saveProgress_.progress.load(), ImVec2(theme::scale(320), 0));
        if (saveProgress_.done.load()) {
            if (saveThread_.joinable()) saveThread_.join();
            saving_ = false;
            bool ok = saveProgress_.success.load();
            statusLine_ = ok ? "Image saved." : ("Save failed: " + saveProgress_.message);
            ImGui::CloseCurrentPopup();
        } else {
            if (theme::DangerButton("Cancel")) saveProgress_.cancel.store(true);
        }
        ImGui::EndPopup();
    }
}

void App::buildUI() {
    // Detect when a final render stops so the status line doesn't get stuck on
    // "Rendering...". A full tile count means it completed; otherwise it was
    // cancelled (cancelFinal already wrote "Cancelling..." in that case).
    {
        bool running = renderer_.finalRunning();
        if (wasRendering_ && !running) {
            Renderer::Progress pr = renderer_.progress();
            if (pr.tilesTotal > 0 && pr.tilesDone >= pr.tilesTotal) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "Render complete in %.1f s.", pr.elapsed);
                statusLine_ = buf;
            } else {
                statusLine_ = "Render cancelled.";
            }
        }
        wasRendering_ = running;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::colors().bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::space().md, theme::space().md));
    ImGui::Begin("VkGigaTracer", nullptr, flags);

    // Shell: header bar, then a body that splits into sidebar + viewport, then
    // a status bar pinned to the bottom.
    drawHeaderBar();
    ImGui::Spacing();

    float footer = theme::scale(36) + ImGui::GetStyle().ItemSpacing.y;
    float bodyH = ImGui::GetContentRegionAvail().y - footer;
    if (bodyH < theme::scale(120)) bodyH = theme::scale(120);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("body", ImVec2(0, bodyH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(); // nested children use the normal padding again
    drawSidebar();
    drawViewport();   // does its own SameLine()
    ImGui::EndChild();

    ImGui::Spacing();
    drawStatusBar();

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void App::drawFrame() {
    if (swapchainDirty_) recreateSwapchain();

    VkDevice dev = ctx_.device();
    vkWaitForFences(dev, 1, &inFlight_[frameIndex_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX,
                                         imgAvail_[frameIndex_], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    vkResetFences(dev, 1, &inFlight_[frameIndex_]);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    buildUI();
    ImGui::Render();

    VkCommandBuffer cmd = frameCmd_[frameIndex_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    const ImVec4 bg = theme::colors().bg;                // match the active (light/dark) theme bg
    clear.color = { { bg.x, bg.y, bg.z, 1.0f } };
    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = renderPass_;
    rbi.framebuffer = scFramebuffers_[imageIndex];
    rbi.renderArea.extent = scExtent_;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imgAvail_[frameIndex_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[frameIndex_];

    VkResult pres;
    {
        std::lock_guard<std::mutex> lock(ctx_.submitMutex());
        vkQueueSubmit(ctx_.graphicsQueue(), 1, &si, inFlight_[frameIndex_]);

        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone_[frameIndex_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &imageIndex;
        pres = vkQueuePresentKHR(ctx_.graphicsQueue(), &pi);
    }
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void App::mainLoop() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w == 0 || h == 0) { glfwWaitEvents(); continue; }
        drawFrame();
    }
}

void App::cleanup() {
    renderer_.cancelFinal();
    if (saveThread_.joinable()) { saveProgress_.cancel.store(true); saveThread_.join(); }
    if (ctx_.device()) vkDeviceWaitIdle(ctx_.device());

    renderer_.shutdown();
    viewer_.shutdown();

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (imguiPool_) vkDestroyDescriptorPool(ctx_.device(), imguiPool_, nullptr);

    for (int i = 0; i < kFramesInFlight; ++i) {
        if (imgAvail_[i]) vkDestroySemaphore(ctx_.device(), imgAvail_[i], nullptr);
        if (renderDone_[i]) vkDestroySemaphore(ctx_.device(), renderDone_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(ctx_.device(), inFlight_[i], nullptr);
    }
    if (framePool_) vkDestroyCommandPool(ctx_.device(), framePool_, nullptr);
    destroySwapchain();
    if (renderPass_) vkDestroyRenderPass(ctx_.device(), renderPass_, nullptr);
    if (surface_) vkDestroySurfaceKHR(ctx_.instance(), surface_, nullptr);
    ctx_.shutdown();

    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

int App::runSelfTest() {
    vkLog("selftest: start");
    headless_ = true;
    if (!initWindow()) return 1;
    if (!initVulkan()) return 1;
    loadDemoScene();
    if (!sceneReady_) { vkLog("selftest: scene not ready"); return 1; }

    settings_.resolutionIndex = 2; // 1080p
    settings_.spp = 64;            // exercises the adaptive batching path
    settings_.maxBounces = 16;
    settings_.tileSize = 256;

    std::wstring err; uint64_t req = 0;
    if (!renderer_.startFinalRender(scene_, settings_, err, req)) {
        vkLog("selftest: startFinalRender failed");
        return 1;
    }
    while (renderer_.finalRunning()) Sleep(100);

    Renderer::Progress pr = renderer_.progress();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "selftest: render done tiles=%llu/%llu elapsed=%.2fs",
                  (unsigned long long)pr.tilesDone, (unsigned long long)pr.tilesTotal, pr.elapsed);
    vkLog(buf);

    SaveProgress sp;
    bool ok = saveImage(renderer_.finalImage(), L"selftest_out.ppm", SaveFormat::PPM, 92, sp);
    vkLog(ok ? "selftest: PPM saved" : "selftest: PPM save FAILED");

    cleanup();
    vkLog("selftest: done");
    return ok ? 0 : 1;
}

int App::run() {
    vkLog("run: start");
    if (!initWindow()) return 1;
    vkLog("run: window ok");
    if (!initVulkan()) { if (window_) { glfwDestroyWindow(window_); glfwTerminate(); } return 1; }
    vkLog("run: vulkan ok");
    if (!initImGui()) { cleanup(); return 1; }
    vkLog("run: imgui ok");
    loadDemoScene();
    vkLog("run: demo scene ok");
    mainLoop();
    cleanup();
    return 0;
}
