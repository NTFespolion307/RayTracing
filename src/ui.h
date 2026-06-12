// ui.h - the application shell: GLFW window, Vulkan swapchain, Dear ImGui,
// the two-tab interface (Setup & Preview / Render Result), native file
// dialogs, and the background save worker.
#pragma once
#include "vk_context.h"
#include "renderer.h"
#include "viewer.h"
#include "savers.h"
#include "scene.h"
#include "render_settings.h"

#include <thread>
#include <vector>
#include <string>

struct GLFWwindow;

class App {
public:
    int run();
    // Headless validation: render the demo scene to the mmap and save a PPM,
    // without opening the UI. Used by the --selftest command-line flag.
    int runSelfTest();

private:
    bool initWindow();
    bool initVulkan();
    bool initImGui();
    bool createSwapchain();
    void destroySwapchain();
    void recreateSwapchain();
    void cleanup();

    void mainLoop();
    void drawFrame();
    void buildUI();
    void drawSetupTab();
    void drawResultTab();
    void drawSaveModal();

    void loadDemoScene();
    void loadObjDialog();
    void openSaveDialog();
    void startSave(SaveFormat fmt);
    void startFinalRender();
    void handlePreviewCamera();

    GLFWwindow* window_ = nullptr;
    VkContext   ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    Renderer    renderer_;
    Viewer      viewer_;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat       scFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D     scExtent_{};
    std::vector<VkImage>       scImages_;
    std::vector<VkImageView>   scViews_;
    std::vector<VkFramebuffer> scFramebuffers_;
    VkRenderPass   renderPass_ = VK_NULL_HANDLE;
    uint32_t       minImageCount_ = 2;

    static constexpr int kFramesInFlight = 2;
    VkCommandPool   framePool_ = VK_NULL_HANDLE;
    VkCommandBuffer frameCmd_[kFramesInFlight]{};
    VkSemaphore     imgAvail_[kFramesInFlight]{};
    VkSemaphore     renderDone_[kFramesInFlight]{};
    VkFence         inFlight_[kFramesInFlight]{};
    uint32_t        frameIndex_ = 0;
    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    bool            swapchainDirty_ = false;

    // App state
    Scene           scene_;
    RenderSettings  settings_;
    bool            sceneReady_ = false;
    int             activeTab_ = 0;            // 0 = setup, 1 = result (actual)
    int             requestTab_ = -1;          // one-shot programmatic switch
    std::string     statusLine_;
    std::string     backendLabel_;
    int             jpegQuality_ = 92;

    // Camera interaction (orbit) for the preview.
    bool   draggingOrbit_ = false, draggingPan_ = false;
    double lastMouseX_ = 0, lastMouseY_ = 0;
    int    previewFrameThrottle_ = 0;
    int    finalPreviewThrottle_ = 0;

    bool         headless_ = false;   // hidden window for --selftest

    // Save worker
    std::thread  saveThread_;
    SaveProgress saveProgress_;
    bool         saving_ = false;
    bool         openSavePopup_ = false;
    bool         saveModalOpen_ = false;
    std::wstring pendingSavePath_;
    SaveFormat   pendingSaveFormat_ = SaveFormat::PNG;
};
