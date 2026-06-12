// bvh.h - binned-SAH BVH built on the CPU and flattened for GPU traversal.
// The flattened nodes are uploaded as an SSBO and walked by the compute
// fallback shader with a fixed-size stack.
#pragma once
#include "scene.h"
#include <vector>

struct BVHResult {
    std::vector<GPUBVHNode>  nodes;             // flattened tree
    std::vector<GPUTriangle> orderedTriangles;  // triangles reordered to match leaves
};

// Builds a BVH over the given triangles. Leaf nodes reference a contiguous
// range [first, first+count) into orderedTriangles. Interior nodes store the
// left child index (right child = left+1) and a triangle count of 0.
BVHResult buildBVH(const std::vector<GPUVertex>& vertices,
                   const std::vector<GPUTriangle>& triangles);
