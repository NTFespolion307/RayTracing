// viewer.h - image viewer for the result tab. Always letterbox-fits the full
// image to the window; supports wheel zoom and drag pan. Uses a low-res
// preview texture for overview zooms and streams the visible full-resolution
// region out of the mapped file into a detail texture when zoomed in.
#pragma once
#include "vk_context.h"
#include "mmap_image.h"
#include <imgui.h>
#include <vector>
#include <cstdint>

struct ViewerTexture {
    Image image;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet imguiId = VK_NULL_HANDLE; // usable as ImTextureID
    uint32_t w = 0, h = 0;
    bool valid() const { return image.view != VK_NULL_HANDLE; }
};

class Viewer {
public:
    void init(VkContext* ctx);
    void shutdown();

    // Replace the overview/preview texture (RGBA8). Safe to call from UI thread.
    void setPreview(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h);

    // Draw an interactive, fitted view of the full image. `full` may be null
    // (then only the preview is shown). fullW/fullH are the true dimensions.
    void drawResult(uint32_t fullW, uint32_t fullH, const MmapImage* full);

    // Draw a simple fitted image that is re-uploaded every call (used by the
    // live preview tab).
    void drawFitted(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h);

    void resetView() { zoom_ = 1.0f; panX_ = panY_ = 0.0f; fitted_ = true; }

private:
    bool uploadTexture(ViewerTexture& tex, const std::vector<uint8_t>& rgba,
                       uint32_t w, uint32_t h);
    void destroyTexture(ViewerTexture& tex);
    void streamDetail(uint32_t fullW, uint32_t fullH, const MmapImage* full,
                      float imgX, float imgY, float dispW, float dispH,
                      const ImVec2& region);

    VkContext* ctx_ = nullptr;
    ViewerTexture preview_;
    ViewerTexture detail_;
    ViewerTexture fittedTex_;
    std::vector<uint8_t> detailBuf_;

    float zoom_ = 1.0f;
    float panX_ = 0.0f, panY_ = 0.0f;  // pan in screen pixels
    bool  fitted_ = true;
    int   detailThrottle_ = 0;
    uint32_t lastDetailW_ = 0, lastDetailH_ = 0;
};
