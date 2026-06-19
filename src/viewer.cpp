#include "viewer.h"
#include "mathlib.h"
#include "theme.h"
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>

void Viewer::init(VkContext* ctx) { ctx_ = ctx; }

void Viewer::shutdown() {
    if (!ctx_) return;
    vkDeviceWaitIdle(ctx_->device());
    destroyTexture(preview_);
    destroyTexture(detail_);
    destroyTexture(fittedTex_);
}

void Viewer::destroyTexture(ViewerTexture& tex) {
    if (tex.imguiId) { ImGui_ImplVulkan_RemoveTexture(tex.imguiId); tex.imguiId = VK_NULL_HANDLE; }
    if (tex.sampler) { vkDestroySampler(ctx_->device(), tex.sampler, nullptr); tex.sampler = VK_NULL_HANDLE; }
    ctx_->destroyImage(tex.image);
    tex.w = tex.h = 0;
}

bool Viewer::uploadTexture(ViewerTexture& tex, const std::vector<uint8_t>& rgba,
                           uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || rgba.size() < (size_t)w * h * 4) return false;
    VkDevice dev = ctx_->device();

    bool recreate = !tex.valid() || tex.w != w || tex.h != h;
    if (recreate) {
        destroyTexture(tex);
        if (!ctx_->createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, tex.image))
            return false;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 1.0f;
        vkCreateSampler(dev, &sci, nullptr, &tex.sampler);
        tex.w = w; tex.h = h;
    }

    Buffer staging;
    if (!ctx_->createBuffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, true))
        return false;
    std::memcpy(staging.mapped, rgba.data(), (size_t)w * h * 4);

    VkCommandPool pool = ctx_->createCommandPool(ctx_->graphicsFamily());
    VkCommandBuffer cmd = ctx_->beginOneShot(pool);

    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = from; b.newLayout = to;
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        b.image = tex.image.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    ctx_->endOneShotAndWait(pool, cmd, ctx_->graphicsQueue());
    vkDestroyCommandPool(dev, pool, nullptr);
    ctx_->destroyBuffer(staging);

    if (recreate || !tex.imguiId) {
        if (tex.imguiId) ImGui_ImplVulkan_RemoveTexture(tex.imguiId);
        tex.imguiId = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.image.view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

void Viewer::setPreview(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    uploadTexture(preview_, rgba, w, h);
}

void Viewer::drawFitted(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    if (!rgba.empty()) uploadTexture(fittedTex_, rgba, w, h);
    if (!fittedTex_.valid()) { ImGui::TextUnformatted("Rendering preview..."); return; }

    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x < 16 || region.y < 16) return;
    float scale = std::min(region.x / fittedTex_.w, region.y / fittedTex_.h);
    float dw = fittedTex_.w * scale, dh = fittedTex_.h * scale;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.x += (region.x - dw) * 0.5f;
    p0.y += (region.y - dh) * 0.5f;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)fittedTex_.imguiId, p0,
                                         ImVec2(p0.x + dw, p0.y + dh));
}

void Viewer::streamDetail(uint32_t fullW, uint32_t fullH, const MmapImage* full,
                          float imgX, float imgY, float dispW, float dispH,
                          const ImVec2& region) {
    // Visible rectangle of the image in full-resolution pixel coordinates.
    ImVec2 rmin = ImGui::GetItemRectMin();
    ImVec2 rmax = ImGui::GetItemRectMax();
    float vx0 = std::max(rmin.x, imgX), vy0 = std::max(rmin.y, imgY);
    float vx1 = std::min(rmax.x, imgX + dispW), vy1 = std::min(rmax.y, imgY + dispH);
    if (vx1 <= vx0 || vy1 <= vy0) return;

    float u0 = (vx0 - imgX) / dispW * fullW;
    float u1 = (vx1 - imgX) / dispW * fullW;
    float v0 = (vy0 - imgY) / dispH * fullH;
    float v1 = (vy1 - imgY) / dispH * fullH;

    uint32_t dw = std::min<uint32_t>(2048, (uint32_t)std::ceil(vx1 - vx0));
    uint32_t dh = std::min<uint32_t>(2048, (uint32_t)std::ceil(vy1 - vy0));
    if (dw == 0 || dh == 0) return;

    detailBuf_.assign((size_t)dw * dh * 4, 0);
    for (uint32_t y = 0; y < dh; ++y) {
        float fy = v0 + (y + 0.5f) / dh * (v1 - v0);
        uint32_t sy = std::min<uint32_t>(fullH - 1, (uint32_t)fy);
        const uint8_t* srcRow = full->rowPtr(sy);
        uint8_t* dstRow = detailBuf_.data() + (size_t)y * dw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            float fx = u0 + (x + 0.5f) / dw * (u1 - u0);
            uint32_t sx = std::min<uint32_t>(fullW - 1, (uint32_t)fx);
            const uint8_t* s = srcRow + (size_t)sx * 4;
            uint8_t* d = dstRow + (size_t)x * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }
    uploadTexture(detail_, detailBuf_, dw, dh);

    if (detail_.valid())
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)detail_.imguiId,
            ImVec2(vx0, vy0), ImVec2(vx1, vy1));
}

void Viewer::drawResult(uint32_t fullW, uint32_t fullH, const MmapImage* full) {
    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x < 16 || region.y < 16) { return; }
    ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("viewcanvas", region,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    float fitScale = std::min(region.x / float(fullW), region.y / float(fullH));
    if (fitScale <= 0) fitScale = 1.0f;

    float dispW = fullW * fitScale * zoom_;
    float dispH = fullH * fitScale * zoom_;
    float imgX = origin.x + (region.x - dispW) * 0.5f + panX_;
    float imgY = origin.y + (region.y - dispH) * 0.5f + panY_;

    // Wheel zoom anchored at the cursor.
    if (hovered && io.MouseWheel != 0.0f) {
        float fx = (io.MousePos.x - imgX) / dispW;
        float fy = (io.MousePos.y - imgY) / dispH;
        float newZoom = clampf(zoom_ * std::pow(1.15f, io.MouseWheel), 1.0f, 64.0f);
        float ndW = fullW * fitScale * newZoom;
        float ndH = fullH * fitScale * newZoom;
        panX_ += (dispW - ndW) * fx;
        panY_ += (dispH - ndH) * fy;
        zoom_ = newZoom;
        fitted_ = false;
        dispW = ndW; dispH = ndH;
        imgX = origin.x + (region.x - dispW) * 0.5f + panX_;
        imgY = origin.y + (region.y - dispH) * 0.5f + panY_;
    }
    // Drag pan (either button).
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        panX_ += io.MouseDelta.x;
        panY_ += io.MouseDelta.y;
        fitted_ = false;
        imgX = origin.x + (region.x - dispW) * 0.5f + panX_;
        imgY = origin.y + (region.y - dispH) * 0.5f + panY_;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);

    // Letterbox matte: theme background, which is slightly darker than the
    // surrounding surface in both light and dark modes, so the image bounds
    // read clearly without a hardcoded colour that breaks the dark theme.
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y),
                      ImGui::GetColorU32(theme::colors().bg));

    bool useDetail = full && full->valid() && dispW > preview_.w * 1.05f && fullW > preview_.w;
    if (preview_.valid())
        dl->AddImage((ImTextureID)preview_.imguiId, ImVec2(imgX, imgY),
                     ImVec2(imgX + dispW, imgY + dispH));

    if (useDetail) {
        if (--detailThrottle_ <= 0) {
            streamDetail(fullW, fullH, full, imgX, imgY, dispW, dispH, region);
            detailThrottle_ = 3;
            lastDetailW_ = detail_.w; lastDetailH_ = detail_.h;
        } else if (detail_.valid()) {
            // Re-draw the last detail texture over the visible rect.
            ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
            float vx0 = std::max(rmin.x, imgX), vy0 = std::max(rmin.y, imgY);
            float vx1 = std::min(rmax.x, imgX + dispW), vy1 = std::min(rmax.y, imgY + dispH);
            if (vx1 > vx0 && vy1 > vy0)
                dl->AddImage((ImTextureID)detail_.imguiId, ImVec2(vx0, vy0), ImVec2(vx1, vy1));
        }
    }

    dl->PopClipRect();
}
