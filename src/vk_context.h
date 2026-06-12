// vk_context.h - Vulkan instance/device/allocator ownership, backend
// capability detection, and small allocation/submit helpers. All entry points
// come from volk; all allocations go through VMA.
#pragma once
#include <volk.h>
#include <vk_mem_alloc.h>
#include "render_settings.h"
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

// One logging sink for the whole app: gigatracer.log next to the exe.
void vkLog(const std::string& msg);
const char* vkResultString(VkResult r);
// Returns true on VK_SUCCESS; logs context and the result string otherwise.
bool vkCheck(VkResult r, const char* what);

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VmaAllocation  alloc  = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage        image  = VK_NULL_HANDLE;
    VmaAllocation  alloc  = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    uint32_t       width = 0, height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;
};

class VkContext {
public:
    bool initInstance(bool enableValidation);
    // Picks a physical device able to present to `surface`, then creates the
    // logical device + VMA allocator. `surface` is retained for recreation.
    bool createDevice(VkSurfaceKHR surface);
    // Tears down and recreates device + allocator after VK_ERROR_DEVICE_LOST.
    bool recreateDevice();
    void shutdown();

    VkInstance        instance()      const { return instance_; }
    VkPhysicalDevice  physical()      const { return physical_; }
    VkDevice          device()        const { return device_; }
    VmaAllocator      allocator()     const { return allocator_; }
    VkQueue           graphicsQueue() const { return graphicsQueue_; }
    VkQueue           computeQueue()  const { return computeQueue_; }
    uint32_t          graphicsFamily() const { return graphicsFamily_; }
    uint32_t          computeFamily()  const { return computeFamily_; }

    bool   rayQuerySupported() const { return rayQuerySupported_; }
    bool   validationEnabled() const { return validationEnabled_; }
    const char* deviceName()   const { return props_.deviceName; }
    const VkPhysicalDeviceProperties& props() const { return props_; }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      Buffer& out, bool hostVisible = false,
                      bool deviceAddress = false);
    void destroyBuffer(Buffer& b);

    // Upload host data into a device-local buffer via a temporary staging copy.
    bool uploadBuffer(const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage, Buffer& out,
                      bool deviceAddress = false);

    bool createImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageUsageFlags usage, Image& out);
    void destroyImage(Image& img);

    VkCommandPool   createCommandPool(uint32_t family);
    VkCommandBuffer beginOneShot(VkCommandPool pool);
    void endOneShotAndWait(VkCommandPool pool, VkCommandBuffer cmd, VkQueue queue);

    Backend resolveBackend(Backend requested) const;

    // Guards vkQueueSubmit / vkQueuePresentKHR when the render worker and the
    // UI thread share a queue. Hold only around the submit call, never the
    // fence wait, so the UI stays responsive.
    std::mutex& submitMutex() { return submitMutex_; }

private:
    bool pickPhysicalDevice(VkSurfaceKHR surface);
    bool createLogicalDevice();
    bool createAllocator();

    VkInstance       instance_  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_  = VK_NULL_HANDLE;
    VkDevice         device_    = VK_NULL_HANDLE;
    VmaAllocator     allocator_ = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_   = VK_NULL_HANDLE;  // not owned

    VkQueue   graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue   computeQueue_  = VK_NULL_HANDLE;
    uint32_t  graphicsFamily_ = 0;
    uint32_t  computeFamily_  = 0;

    VkPhysicalDeviceProperties props_{};
    bool rayQuerySupported_ = false;
    bool validationEnabled_ = false;
    bool bufferDeviceAddress_ = false;
    std::mutex submitMutex_;
};
