// savers.h - streaming PNG / JPEG / PPM writers. Each reads RGBA rows directly
// from the memory-mapped image and writes RGB rows, so peak extra RAM stays a
// few row buffers regardless of output resolution.
#pragma once
#include "mmap_image.h"
#include <atomic>
#include <string>

enum class SaveFormat { PNG, JPEG, PPM };

struct SaveProgress {
    std::atomic<float> progress{ 0.0f };  // 0..1
    std::atomic<bool>  cancel{ false };
    std::atomic<bool>  done{ false };
    std::atomic<bool>  success{ false };
    std::string        message;
};

// All synchronous; call from a worker thread. Honor progress.cancel between
// rows. Wide path so non-ASCII destinations work.
bool savePNG (const MmapImage& img, const std::wstring& path, SaveProgress& p);
bool saveJPEG(const MmapImage& img, const std::wstring& path, int quality, SaveProgress& p);
bool savePPM (const MmapImage& img, const std::wstring& path, SaveProgress& p);

// Dispatches by format; quality only used for JPEG.
bool saveImage(const MmapImage& img, const std::wstring& path,
               SaveFormat fmt, int jpegQuality, SaveProgress& p);
