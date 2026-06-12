#include "accel.h"
#include <cstring>

namespace {
constexpr VkBufferUsageFlags kAsInput =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr VkBufferUsageFlags kAsStorage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags kScratch =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
} // namespace

bool Accel::build(VkContext& ctx,
                  const std::vector<GPUVertex>& verts,
                  const std::vector<GPUTriangle>& tris) {
    if (!ctx.rayQuerySupported() || verts.empty() || tris.empty()) return false;

    // Required scratch alignment.
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx.physical(), &p2);
    VkDeviceSize scratchAlign = asProps.minAccelerationStructureScratchOffsetAlignment;
    if (scratchAlign == 0) scratchAlign = 256;
    auto alignUp = [](VkDeviceAddress a, VkDeviceSize al) {
        return (a + al - 1) & ~(al - 1);
    };

    // Packed positions (vec3) and indices, in triangle order.
    std::vector<float> pos(verts.size() * 3);
    for (size_t i = 0; i < verts.size(); ++i) {
        pos[i * 3 + 0] = verts[i].pos.x;
        pos[i * 3 + 1] = verts[i].pos.y;
        pos[i * 3 + 2] = verts[i].pos.z;
    }
    std::vector<uint32_t> idx(tris.size() * 3);
    for (size_t i = 0; i < tris.size(); ++i) {
        idx[i * 3 + 0] = tris[i].v0;
        idx[i * 3 + 1] = tris[i].v1;
        idx[i * 3 + 2] = tris[i].v2;
    }

    if (!ctx.uploadBuffer(pos.data(), pos.size() * sizeof(float), kAsInput, positions, true)) return false;
    if (!ctx.uploadBuffer(idx.data(), idx.size() * sizeof(uint32_t), kAsInput, indices, true)) return false;

    // ---- BLAS ----
    VkAccelerationStructureGeometryKHR geo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = positions.address;
    geo.geometry.triangles.vertexStride = sizeof(float) * 3;
    geo.geometry.triangles.maxVertex = (uint32_t)verts.size() - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo.geometry.triangles.indexData.deviceAddress = indices.address;

    VkAccelerationStructureBuildGeometryInfoKHR bgi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geo;

    uint32_t primCount = (uint32_t)tris.size();
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(
        ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &primCount, &sizes);

    if (!ctx.createBuffer(sizes.accelerationStructureSize, kAsStorage, blasBuffer, false, true)) return false;

    VkAccelerationStructureCreateInfoKHR aci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    aci.buffer = blasBuffer.buffer;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (!vkCheck(vkCreateAccelerationStructureKHR(ctx.device(), &aci, nullptr, &blas),
                 "vkCreateAccelerationStructureKHR(BLAS)")) return false;

    Buffer blasScratch;
    if (!ctx.createBuffer(sizes.buildScratchSize + scratchAlign, kScratch, blasScratch, false, true)) return false;

    bgi.dstAccelerationStructure = blas;
    bgi.scratchData.deviceAddress = alignUp(blasScratch.address, scratchAlign);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

    VkCommandPool pool = ctx.createCommandPool(ctx.graphicsFamily());
    VkCommandBuffer cmd = ctx.beginOneShot(pool);
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &pRange);
    ctx.endOneShotAndWait(pool, cmd, ctx.graphicsQueue());

    VkAccelerationStructureDeviceAddressInfoKHR dai{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    dai.accelerationStructure = blas;
    VkDeviceAddress blasAddr = vkGetAccelerationStructureDeviceAddressKHR(ctx.device(), &dai);

    // ---- TLAS ----
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = 1.0f;
    inst.transform.matrix[1][1] = 1.0f;
    inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = blasAddr;

    if (!ctx.uploadBuffer(&inst, sizeof(inst), kAsInput, instanceBuffer, true)) {
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        ctx.destroyBuffer(blasScratch);
        return false;
    }

    VkAccelerationStructureGeometryKHR tgeo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.arrayOfPointers = VK_FALSE;
    tgeo.geometry.instances.data.deviceAddress = instanceBuffer.address;

    VkAccelerationStructureBuildGeometryInfoKHR tbgi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tbgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbgi.geometryCount = 1;
    tbgi.pGeometries = &tgeo;

    uint32_t instCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(
        ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbgi, &instCount, &tsizes);

    if (!ctx.createBuffer(tsizes.accelerationStructureSize, kAsStorage, tlasBuffer, false, true)) {
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        ctx.destroyBuffer(blasScratch);
        return false;
    }

    VkAccelerationStructureCreateInfoKHR taci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    taci.buffer = tlasBuffer.buffer;
    taci.size = tsizes.accelerationStructureSize;
    taci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (!vkCheck(vkCreateAccelerationStructureKHR(ctx.device(), &taci, nullptr, &tlas),
                 "vkCreateAccelerationStructureKHR(TLAS)")) {
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        ctx.destroyBuffer(blasScratch);
        return false;
    }

    Buffer tlasScratch;
    if (!ctx.createBuffer(tsizes.buildScratchSize + scratchAlign, kScratch, tlasScratch, false, true)) {
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        ctx.destroyBuffer(blasScratch);
        return false;
    }

    tbgi.dstAccelerationStructure = tlas;
    tbgi.scratchData.deviceAddress = alignUp(tlasScratch.address, scratchAlign);

    VkAccelerationStructureBuildRangeInfoKHR trange{};
    trange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* pTRange = &trange;

    cmd = ctx.beginOneShot(pool);
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tbgi, &pTRange);
    ctx.endOneShotAndWait(pool, cmd, ctx.graphicsQueue());

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    ctx.destroyBuffer(blasScratch);
    ctx.destroyBuffer(tlasScratch);
    vkLog("Acceleration structure built.");
    return true;
}

void Accel::destroy(VkContext& ctx) {
    if (tlas) { vkDestroyAccelerationStructureKHR(ctx.device(), tlas, nullptr); tlas = VK_NULL_HANDLE; }
    if (blas) { vkDestroyAccelerationStructureKHR(ctx.device(), blas, nullptr); blas = VK_NULL_HANDLE; }
    ctx.destroyBuffer(blasBuffer);
    ctx.destroyBuffer(tlasBuffer);
    ctx.destroyBuffer(instanceBuffer);
    ctx.destroyBuffer(positions);
    ctx.destroyBuffer(indices);
}
