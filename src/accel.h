// accel.h - BLAS/TLAS construction for the hardware ray-query backend.
// Builds a single bottom-level AS over all triangles and a one-instance
// top-level AS. Primitive indices match the triangle upload order so the
// shader can fetch shading data from the same triangle SSBO.
#pragma once
#include "vk_context.h"
#include "scene.h"
#include <vector>

struct Accel {
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    Buffer blasBuffer, tlasBuffer, instanceBuffer, positions, indices;

    bool valid() const { return tlas != VK_NULL_HANDLE; }

    // Returns false on any failure (caller silently falls back to compute).
    bool build(VkContext& ctx,
               const std::vector<GPUVertex>& verts,
               const std::vector<GPUTriangle>& tris);
    void destroy(VkContext& ctx);
};
