// Single translation unit that compiles the Vulkan Memory Allocator
// implementation. We use volk, so VMA must fetch entry points dynamically
// through the vkGetInstanceProcAddr / vkGetDeviceProcAddr we hand it.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <volk.h>
#include <vk_mem_alloc.h>
