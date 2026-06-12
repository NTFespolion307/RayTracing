#include "vk_context.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <array>
#include <ctime>

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
namespace {
std::mutex g_logMutex;
std::wstring logPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    if (s != std::wstring::npos) p = p.substr(0, s + 1);
    return p + L"gigatracer.log";
}
} // namespace

void vkLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = _wfopen(logPath().c_str(), L"a");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    char ts[32] = "??:??:??";
    if (localtime_s(&tmv, &t) == 0)
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    std::fprintf(f, "[%s] %s\n", ts, msg.c_str());
    std::fclose(f);
}

const char* vkResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "VK_ERROR_UNKNOWN";
    }
}

bool vkCheck(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s -> %s", what, vkResultString(r));
    vkLog(buf);
    return false;
}

// ---------------------------------------------------------------------------
// Debug messenger
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        vkLog(std::string("[validation] ") + (data->pMessage ? data->pMessage : ""));
    return VK_FALSE;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
bool VkContext::initInstance(bool enableValidation) {
    if (!vkCheck(volkInitialize(), "volkInitialize")) return false;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        "VK_KHR_win32_surface",
    };
    std::vector<const char*> layers;

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> available(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, available.data());
    bool hasValidation = false;
    for (auto& l : available)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) hasValidation = true;

    if (enableValidation && hasValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        validationEnabled_ = true;
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkGigaTracer";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();

    if (!vkCheck(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance")) {
        // Retry without the optional debug layer/extension.
        if (validationEnabled_) {
            validationEnabled_ = false;
            ci.enabledLayerCount = 0;
            ci.enabledExtensionCount = 2; // surface + win32 only
            if (!vkCheck(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance(retry)"))
                return false;
        } else {
            return false;
        }
    }
    volkLoadInstance(instance_);

    if (validationEnabled_) {
        VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        if (vkCreateDebugUtilsMessengerEXT)
            vkCreateDebugUtilsMessengerEXT(instance_, &dci, nullptr, &debugMessenger_);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Physical device selection
// ---------------------------------------------------------------------------
namespace {
bool hasExtension(VkPhysicalDevice dev, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, props.data());
    for (auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}
} // namespace

bool VkContext::pickPhysicalDevice(VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { vkLog("No Vulkan physical devices found."); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (auto dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qprops.data());

        int gfx = -1, comp = -1;
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
            bool g = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            bool c = (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (g && c && present && gfx < 0) gfx = (int)i;
        }
        // Prefer a dedicated compute queue (compute without graphics).
        for (uint32_t i = 0; i < qn; ++i) {
            bool g = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            bool c = (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (c && !g) { comp = (int)i; break; }
        }
        if (gfx < 0) continue;          // can't present + render
        if (comp < 0) comp = gfx;

        bool rq = props.apiVersion >= VK_API_VERSION_1_2 &&
                  hasExtension(dev, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                  hasExtension(dev, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                  hasExtension(dev, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        if (rq) {
            VkPhysicalDeviceRayQueryFeaturesKHR rqf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
            asf.pNext = &rqf;
            VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            f2.pNext = &asf;
            vkGetPhysicalDeviceFeatures2(dev, &f2);
            rq = rqf.rayQuery && asf.accelerationStructure;
        }

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 200;
        if (rq) score += 100;

        if (score > bestScore) {
            bestScore = score;
            physical_ = dev;
            props_ = props;
            graphicsFamily_ = (uint32_t)gfx;
            computeFamily_ = (uint32_t)comp;
            rayQuerySupported_ = rq;
        }
    }

    if (physical_ == VK_NULL_HANDLE) { vkLog("No suitable Vulkan device (present + compute)."); return false; }
    vkLog(std::string("Selected GPU: ") + props_.deviceName +
          (rayQuerySupported_ ? " [ray query]" : " [compute]"));
    return true;
}

// ---------------------------------------------------------------------------
// Logical device
// ---------------------------------------------------------------------------
bool VkContext::createLogicalDevice() {
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    VkDeviceQueueCreateInfo q0{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q0.queueFamilyIndex = graphicsFamily_;
    q0.queueCount = 1;
    q0.pQueuePriorities = &prio;
    qcis.push_back(q0);
    if (computeFamily_ != graphicsFamily_) {
        VkDeviceQueueCreateInfo q1 = q0;
        q1.queueFamilyIndex = computeFamily_;
        qcis.push_back(q1);
    }

    std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };

    void* pNext = nullptr;
    if (rayQuerySupported_) {
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        v12.bufferDeviceAddress = VK_TRUE;
        asf.accelerationStructure = VK_TRUE;
        rqf.rayQuery = VK_TRUE;
        v12.pNext = &asf;
        asf.pNext = &rqf;
        features2.pNext = &v12;
        pNext = &features2;
        bufferDeviceAddress_ = true;
    }

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = pNext;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)extensions.size();
    dci.ppEnabledExtensionNames = extensions.data();

    VkResult r = vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS && rayQuerySupported_) {
        // Fall back to a plain compute device if RT device creation fails.
        vkLog("RT device creation failed; falling back to compute-only device.");
        rayQuerySupported_ = false;
        bufferDeviceAddress_ = false;
        extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci2{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci2.queueCreateInfoCount = (uint32_t)qcis.size();
        dci2.pQueueCreateInfos = qcis.data();
        dci2.enabledExtensionCount = (uint32_t)extensions.size();
        dci2.ppEnabledExtensionNames = extensions.data();
        r = vkCreateDevice(physical_, &dci2, nullptr, &device_);
    }
    if (!vkCheck(r, "vkCreateDevice")) return false;

    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, computeFamily_, 0, &computeQueue_);
    return true;
}

bool VkContext::createAllocator() {
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo aci{};
    aci.instance = instance_;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.vulkanApiVersion = VK_API_VERSION_1_2;
    aci.pVulkanFunctions = &vf;
    if (bufferDeviceAddress_)
        aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    return vkCheck(vmaCreateAllocator(&aci, &allocator_), "vmaCreateAllocator");
}

bool VkContext::createDevice(VkSurfaceKHR surface) {
    surface_ = surface;
    if (!pickPhysicalDevice(surface)) return false;
    if (!createLogicalDevice()) return false;
    if (!createAllocator()) return false;
    return true;
}

bool VkContext::recreateDevice() {
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = VK_NULL_HANDLE; }
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    // Keep the previously chosen physical device + families.
    if (!createLogicalDevice()) return false;
    if (!createAllocator()) return false;
    return true;
}

void VkContext::shutdown() {
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = VK_NULL_HANDLE; }
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (debugMessenger_ && vkDestroyDebugUtilsMessengerEXT)
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    debugMessenger_ = VK_NULL_HANDLE;
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------
bool VkContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                             Buffer& out, bool hostVisible, bool deviceAddress) {
    if (size == 0) size = 4;
    if (deviceAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    if (hostVisible) {
        // RANDOM => host-cached so the CPU can both write (uploads, params) and
        // read back (tile accumulation) correctly. SEQUENTIAL_WRITE would be
        // write-combined and unreadable for the resolve step.
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo info{};
    if (!vkCheck(vmaCreateBuffer(allocator_, &bci, &aci, &out.buffer, &out.alloc, &info),
                 "vmaCreateBuffer"))
        return false;
    out.size = size;
    out.mapped = hostVisible ? info.pMappedData : nullptr;

    if (deviceAddress) {
        VkBufferDeviceAddressInfo dai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        dai.buffer = out.buffer;
        out.address = vkGetBufferDeviceAddress(device_, &dai);
    }
    return true;
}

void VkContext::destroyBuffer(Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(allocator_, b.buffer, b.alloc);
    b = Buffer{};
}

bool VkContext::uploadBuffer(const void* data, VkDeviceSize size,
                             VkBufferUsageFlags usage, Buffer& out, bool deviceAddress) {
    if (size == 0) { return createBuffer(4, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, out, false, deviceAddress); }

    Buffer staging;
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, true)) return false;
    std::memcpy(staging.mapped, data, (size_t)size);

    if (!createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, out, false, deviceAddress)) {
        destroyBuffer(staging);
        return false;
    }

    VkCommandPool pool = createCommandPool(graphicsFamily_);
    VkCommandBuffer cmd = beginOneShot(pool);
    VkBufferCopy copy{ 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, out.buffer, 1, &copy);
    endOneShotAndWait(pool, cmd, graphicsQueue_);
    vkDestroyCommandPool(device_, pool, nullptr);
    destroyBuffer(staging);
    return true;
}

bool VkContext::createImage(uint32_t w, uint32_t h, VkFormat fmt,
                            VkImageUsageFlags usage, Image& out) {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (!vkCheck(vmaCreateImage(allocator_, &ici, &aci, &out.image, &out.alloc, nullptr),
                 "vmaCreateImage"))
        return false;

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (!vkCheck(vkCreateImageView(device_, &vci, nullptr, &out.view), "vkCreateImageView")) {
        vmaDestroyImage(allocator_, out.image, out.alloc);
        out = Image{};
        return false;
    }
    out.width = w; out.height = h; out.format = fmt;
    return true;
}

void VkContext::destroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vmaDestroyImage(allocator_, img.image, img.alloc);
    img = Image{};
}

VkCommandPool VkContext::createCommandPool(uint32_t family) {
    VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device_, &ci, nullptr, &pool);
    return pool;
}

VkCommandBuffer VkContext::beginOneShot(VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VkContext::endOneShotAndWait(VkCommandPool pool, VkCommandBuffer cmd, VkQueue queue) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device_, &fci, nullptr, &fence);
    {
        std::lock_guard<std::mutex> lock(submitMutex_);
        vkQueueSubmit(queue, 1, &si, fence);
    }
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, pool, 1, &cmd);
}

Backend VkContext::resolveBackend(Backend requested) const {
    switch (requested) {
        case Backend::HardwareRT: return rayQuerySupported_ ? Backend::HardwareRT : Backend::Compute;
        case Backend::Compute:    return Backend::Compute;
        case Backend::Auto:
        default:                  return rayQuerySupported_ ? Backend::HardwareRT : Backend::Compute;
    }
}
