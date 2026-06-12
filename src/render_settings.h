// render_settings.h - the exact, spec-mandated render option tables and the
// RenderSettings struct shared across the UI and renderer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class Backend { Auto, HardwareRT, Compute };

struct ResolutionOption {
    const char* label;
    uint32_t width;
    uint32_t height;
};

// All resolutions are 16:9, exactly as specified.
static const ResolutionOption kResolutions[] = {
    { "1080p", 1920,  1080  },
    { "2K",    2560,  1440  },
    { "4K",    3840,  2160  },
    { "6K",    5760,  3240  },
    { "8K",    7680,  4320  },
    { "10K",   9600,  5400  },
    { "15K",   14400, 8100  },
    { "20K",   19200, 10800 },
    { "40K",   38400, 21600 },
    { "60K",   57600, 32400 },
    { "80K",   76800, 43200 },
};
static const int kResolutionCount = sizeof(kResolutions) / sizeof(kResolutions[0]);

static const int kSppOptions[]     = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
static const int kSppOptionCount   = sizeof(kSppOptions) / sizeof(kSppOptions[0]);

static const int kBounceOptions[]  = { 16, 32, 48, 64 };
static const int kBounceOptionCount = sizeof(kBounceOptions) / sizeof(kBounceOptions[0]);

static const int kTileSizeOptions[] = { 128, 256, 512 };
static const int kTileSizeOptionCount = sizeof(kTileSizeOptions) / sizeof(kTileSizeOptions[0]);

// JPEG cannot exceed this on either axis (libjpeg dimension field is 16-bit).
static const uint32_t kJpegMaxSide = 65500u;

struct RenderSettings {
    int      resolutionIndex = 0;          // index into kResolutions (1080p)
    int      spp             = 16;
    int      maxBounces      = 16;
    float    exposure        = 1.0f;
    bool     fireflyClamp    = true;
    float    fireflyClampValue = 64.0f;
    bool     skyEnabled      = true;
    float    skyIntensity    = 1.0f;
    Backend  backend         = Backend::Auto;
    int      tileSize        = 256;        // 128 / 256 / 512
    std::wstring scratchFolder;            // empty => use system temp

    uint32_t width()  const { return kResolutions[resolutionIndex].width; }
    uint32_t height() const { return kResolutions[resolutionIndex].height; }
};
