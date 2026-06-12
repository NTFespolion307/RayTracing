#include "savers.h"
#include "render_settings.h"
#include <cstdio>
#include <vector>
#include <csetjmp>

#include <png.h>
extern "C" {
#include <jpeglib.h>
}

namespace {

FILE* wfopenWrite(const std::wstring& path) {
    return _wfopen(path.c_str(), L"wb");
}

// Convert one RGBA row (from the mapping) to a tightly packed RGB row.
inline void rgbaToRgb(const uint8_t* src, uint8_t* dst, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        dst[x * 3 + 0] = src[x * 4 + 0];
        dst[x * 3 + 1] = src[x * 4 + 1];
        dst[x * 3 + 2] = src[x * 4 + 2];
    }
}

void deletePartial(const std::wstring& path) { _wremove(path.c_str()); }

} // namespace

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------
bool savePNG(const MmapImage& img, const std::wstring& path, SaveProgress& p) {
    FILE* fp = wfopenWrite(path);
    if (!fp) { p.message = "Could not create the PNG file."; return false; }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); return false; }

    std::vector<uint8_t> row(img.width() * 3);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        deletePartial(path);
        p.message = "PNG encoding failed.";
        return false;
    }

    png_init_io(png, fp);
    png_set_compression_level(png, 4); // balance speed vs size for huge images
    png_set_IHDR(png, info, img.width(), img.height(), 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (uint32_t y = 0; y < img.height(); ++y) {
        if (p.cancel.load()) {
            png_destroy_write_struct(&png, &info);
            fclose(fp);
            deletePartial(path);
            p.message = "Save cancelled.";
            return false;
        }
        rgbaToRgb(img.rowPtr(y), row.data(), img.width());
        png_write_row(png, row.data());
        if ((y & 1023) == 0) p.progress.store(float(y) / float(img.height()));
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    p.progress.store(1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// JPEG (libjpeg-turbo, streaming scanlines)
// ---------------------------------------------------------------------------
namespace {
struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf jmp;
};
void jpegErrorExit(j_common_ptr cinfo) {
    JpegError* err = reinterpret_cast<JpegError*>(cinfo->err);
    longjmp(err->jmp, 1);
}
} // namespace

bool saveJPEG(const MmapImage& img, const std::wstring& path, int quality, SaveProgress& p) {
    if (img.width() > kJpegMaxSide || img.height() > kJpegMaxSide) {
        p.message = "JPEG cannot exceed 65500 px per side at this resolution.";
        return false;
    }
    FILE* fp = wfopenWrite(path);
    if (!fp) { p.message = "Could not create the JPEG file."; return false; }

    jpeg_compress_struct cinfo;
    JpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpegErrorExit;

    std::vector<uint8_t> row(img.width() * 3);
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        deletePartial(path);
        p.message = "JPEG encoding failed.";
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = img.width();
    cinfo.image_height = img.height();
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality < 1 ? 1 : (quality > 100 ? 100 : quality), TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        if (p.cancel.load()) {
            jpeg_destroy_compress(&cinfo);
            fclose(fp);
            deletePartial(path);
            p.message = "Save cancelled.";
            return false;
        }
        uint32_t y = cinfo.next_scanline;
        rgbaToRgb(img.rowPtr(y), row.data(), img.width());
        JSAMPROW rowPtr = row.data();
        jpeg_write_scanlines(&cinfo, &rowPtr, 1);
        if ((y & 1023) == 0) p.progress.store(float(y) / float(img.height()));
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    p.progress.store(1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// PPM (P6 binary) - GIMP opens this natively with no import dialog.
// ---------------------------------------------------------------------------
bool savePPM(const MmapImage& img, const std::wstring& path, SaveProgress& p) {
    FILE* fp = wfopenWrite(path);
    if (!fp) { p.message = "Could not create the PPM file."; return false; }

    std::fprintf(fp, "P6\n%u %u\n255\n", img.width(), img.height());

    std::vector<uint8_t> row(img.width() * 3);
    for (uint32_t y = 0; y < img.height(); ++y) {
        if (p.cancel.load()) {
            fclose(fp);
            deletePartial(path);
            p.message = "Save cancelled.";
            return false;
        }
        rgbaToRgb(img.rowPtr(y), row.data(), img.width());
        if (std::fwrite(row.data(), 1, row.size(), fp) != row.size()) {
            fclose(fp);
            deletePartial(path);
            p.message = "Disk write failed.";
            return false;
        }
        if ((y & 1023) == 0) p.progress.store(float(y) / float(img.height()));
    }

    fclose(fp);
    p.progress.store(1.0f);
    return true;
}

bool saveImage(const MmapImage& img, const std::wstring& path,
               SaveFormat fmt, int jpegQuality, SaveProgress& p) {
    bool ok = false;
    switch (fmt) {
        case SaveFormat::PNG:  ok = savePNG(img, path, p); break;
        case SaveFormat::JPEG: ok = saveJPEG(img, path, jpegQuality, p); break;
        case SaveFormat::PPM:  ok = savePPM(img, path, p); break;
    }
    p.success.store(ok);
    p.done.store(true);
    return ok;
}
