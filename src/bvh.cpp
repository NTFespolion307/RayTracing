#include "bvh.h"
#include <cstring>
#include <cstdint>

namespace {

inline float asFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct Prim {
    AABB box;
    vec3 centroid;
    uint32_t tri;
};

inline vec3 triVert(const std::vector<GPUVertex>& v, uint32_t i) {
    return { v[i].pos.x, v[i].pos.y, v[i].pos.z };
}

constexpr int   kBins = 12;
constexpr int   kLeafSize = 2;

} // namespace

BVHResult buildBVH(const std::vector<GPUVertex>& vertices,
                   const std::vector<GPUTriangle>& triangles) {
    BVHResult result;
    const size_t triCount = triangles.size();
    if (triCount == 0) {
        // A single empty leaf so the shader never dereferences a null root.
        GPUBVHNode n{};
        n.lo = vec4(0, 0, 0, asFloat(0));
        n.hi = vec4(0, 0, 0, asFloat(0));
        result.nodes.push_back(n);
        return result;
    }

    std::vector<Prim> prims(triCount);
    for (size_t i = 0; i < triCount; ++i) {
        const GPUTriangle& t = triangles[i];
        vec3 a = triVert(vertices, t.v0);
        vec3 b = triVert(vertices, t.v1);
        vec3 c = triVert(vertices, t.v2);
        AABB box;
        box.expand(a); box.expand(b); box.expand(c);
        prims[i].box = box;
        prims[i].centroid = (a + b + c) * (1.0f / 3.0f);
        prims[i].tri = (uint32_t)i;
    }

    // Worst case node count for a binary BVH over N leaves is 2N-1.
    result.nodes.reserve(triCount * 2);

    struct Task { uint32_t start, count, nodeIndex; };
    std::vector<Task> stack;

    auto makeNode = [&]() -> uint32_t {
        result.nodes.push_back(GPUBVHNode{});
        return (uint32_t)result.nodes.size() - 1;
    };

    auto computeBounds = [&](uint32_t start, uint32_t count, AABB& bounds, AABB& centroidBounds) {
        bounds = AABB{};
        centroidBounds = AABB{};
        for (uint32_t i = 0; i < count; ++i) {
            bounds.expand(prims[start + i].box);
            centroidBounds.expand(prims[start + i].centroid);
        }
    };

    uint32_t root = makeNode();
    stack.push_back({ 0, (uint32_t)triCount, root });

    while (!stack.empty()) {
        Task task = stack.back();
        stack.pop_back();

        AABB bounds, centroidBounds;
        computeBounds(task.start, task.count, bounds, centroidBounds);

        auto writeLeaf = [&]() {
            GPUBVHNode& node = result.nodes[task.nodeIndex];
            node.lo = vec4(bounds.lo, asFloat(task.start));
            node.hi = vec4(bounds.hi, asFloat(task.count));
        };

        if (task.count <= kLeafSize) { writeLeaf(); continue; }

        // Split along the axis with the largest centroid extent.
        vec3 ext = centroidBounds.extent();
        int axis = 0;
        if (ext.y > ext.x) axis = 1;
        if (ext.z > ext[axis]) axis = 2;
        float lo = centroidBounds.lo[axis];
        float hi = centroidBounds.hi[axis];
        if (hi - lo < 1e-12f) { writeLeaf(); continue; }

        // Bin primitives.
        AABB binBounds[kBins];
        int  binCount[kBins] = { 0 };
        float scale = kBins / (hi - lo);
        for (uint32_t i = 0; i < task.count; ++i) {
            const Prim& p = prims[task.start + i];
            int b = (int)((p.centroid[axis] - lo) * scale);
            if (b < 0) b = 0;
            if (b >= kBins) b = kBins - 1;
            binCount[b]++;
            binBounds[b].expand(p.box);
        }

        // SAH cost of splitting after each bin boundary.
        float leftArea[kBins - 1], rightArea[kBins - 1];
        int   leftCount[kBins - 1], rightCount[kBins - 1];
        AABB acc; int cnt = 0;
        for (int i = 0; i < kBins - 1; ++i) {
            acc.expand(binBounds[i]); cnt += binCount[i];
            leftArea[i] = acc.surfaceArea(); leftCount[i] = cnt;
        }
        acc = AABB{}; cnt = 0;
        for (int i = kBins - 1; i > 0; --i) {
            acc.expand(binBounds[i]); cnt += binCount[i];
            rightArea[i - 1] = acc.surfaceArea(); rightCount[i - 1] = cnt;
        }

        float bestCost = 1e30f;
        int   bestSplit = -1;
        for (int i = 0; i < kBins - 1; ++i) {
            if (leftCount[i] == 0 || rightCount[i] == 0) continue;
            float cost = leftArea[i] * leftCount[i] + rightArea[i] * rightCount[i];
            if (cost < bestCost) { bestCost = cost; bestSplit = i; }
        }

        if (bestSplit < 0) { writeLeaf(); continue; }

        // Partition prims around the chosen bin boundary.
        float splitPos = lo + (bestSplit + 1) / scale;
        uint32_t mid = task.start;
        for (uint32_t i = task.start; i < task.start + task.count; ++i) {
            if (prims[i].centroid[axis] < splitPos) {
                std::swap(prims[i], prims[mid]);
                ++mid;
            }
        }
        uint32_t leftCnt = mid - task.start;
        if (leftCnt == 0 || leftCnt == task.count) {
            // Degenerate partition: fall back to a median split.
            mid = task.start + task.count / 2;
            leftCnt = mid - task.start;
        }

        uint32_t leftIdx = makeNode();
        uint32_t rightIdx = makeNode();
        {
            GPUBVHNode& node = result.nodes[task.nodeIndex];
            node.lo = vec4(bounds.lo, asFloat(leftIdx));
            node.hi = vec4(bounds.hi, asFloat(0)); // interior: count == 0
        }
        stack.push_back({ task.start, leftCnt, leftIdx });
        stack.push_back({ mid, task.count - leftCnt, rightIdx });
    }

    // Emit triangles in primitive order so leaf ranges are contiguous.
    result.orderedTriangles.resize(triCount);
    for (size_t i = 0; i < triCount; ++i)
        result.orderedTriangles[i] = triangles[prims[i].tri];

    return result;
}
