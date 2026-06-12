// mmap_image.h - disk-backed RGBA8 image accessed through a single 64-bit
// MapViewOfFile mapping. This is the backing store for the full-resolution
// render; its size is bounded only by free disk space, never by RAM or VRAM.
#pragma once
#include <cstdint>
#include <string>
#include <windows.h>

class MmapImage {
public:
    MmapImage() = default;
    ~MmapImage();

    MmapImage(const MmapImage&) = delete;
    MmapImage& operator=(const MmapImage&) = delete;

    // Creates a width*height*4 byte file in `folder` and maps it.
    // Returns false on failure (caller surfaces disk-space messaging itself).
    bool create(const std::wstring& folder, uint32_t width, uint32_t height);
    void destroy();

    bool valid() const { return base_ != nullptr; }
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t sizeBytes() const { return (uint64_t)width_ * height_ * 4ull; }

    // Pointer to the first byte of the given row (RGBA8).
    uint8_t* rowPtr(uint32_t y) { return base_ + (uint64_t)y * width_ * 4ull; }
    const uint8_t* rowPtr(uint32_t y) const { return base_ + (uint64_t)y * width_ * 4ull; }
    uint8_t* data() { return base_; }

    const std::wstring& path() const { return path_; }

    // Required free bytes for a given resolution.
    static uint64_t requiredBytes(uint32_t w, uint32_t h) { return (uint64_t)w * h * 4ull; }

    // Queries free bytes available on the volume that would hold `folder`.
    static bool freeSpace(const std::wstring& folder, uint64_t& freeBytes);
    // Returns the effective scratch folder (system temp if `folder` empty).
    static std::wstring resolveFolder(const std::wstring& folder);

private:
    HANDLE   file_    = INVALID_HANDLE_VALUE;
    HANDLE   mapping_ = nullptr;
    uint8_t* base_    = nullptr;
    uint32_t width_   = 0;
    uint32_t height_  = 0;
    std::wstring path_;
};
