#include "mmap_image.h"
#include <chrono>

MmapImage::~MmapImage() { destroy(); }

std::wstring MmapImage::resolveFolder(const std::wstring& folder) {
    if (!folder.empty()) return folder;
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n > MAX_PATH) return L".";
    return std::wstring(buf);
}

bool MmapImage::freeSpace(const std::wstring& folder, uint64_t& freeBytes) {
    std::wstring dir = resolveFolder(folder);
    ULARGE_INTEGER freeForCaller{}, total{}, totalFree{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &freeForCaller, &total, &totalFree))
        return false;
    freeBytes = freeForCaller.QuadPart;
    return true;
}

bool MmapImage::create(const std::wstring& folder, uint32_t width, uint32_t height) {
    destroy();
    width_ = width;
    height_ = height;

    std::wstring dir = resolveFolder(folder);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        dir += L'\\';

    // Unique scratch filename so concurrent runs don't collide.
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    wchar_t name[64];
    swprintf(name, 64, L"VkGigaTracer_%08X_%08X.scratch",
             (unsigned)(now & 0xFFFFFFFF), GetCurrentProcessId());
    path_ = dir + name;

    file_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                        nullptr);
    if (file_ == INVALID_HANDLE_VALUE) { width_ = height_ = 0; return false; }

    uint64_t size = sizeBytes();
    DWORD hi = (DWORD)(size >> 32);
    DWORD lo = (DWORD)(size & 0xFFFFFFFF);

    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    if (!mapping_) { destroy(); return false; }

    base_ = (uint8_t*)MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base_) { destroy(); return false; }
    return true;
}

void MmapImage::destroy() {
    if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    // FILE_FLAG_DELETE_ON_CLOSE reclaims the scratch file automatically.
    width_ = height_ = 0;
    path_.clear();
}
