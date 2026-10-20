#pragma once

#include "Point.hpp"

#include <admesh/stl.h>

#include <memory>
#include <vector>

namespace Slic3r {

struct RayMeshHit {
    int    face = -1;
    double t    = 0.;
    double u    = 0.; // P = (1-u-v)*v0 + u*v1 + v*v2
    double v    = 0.;
};

// Ray / triangle-set intersection on top of AABBTreeIndirect. The tree is built once per mesh
// with an epsilon proportional to the mesh size, so axis-aligned rays grazing a shared edge of
// the bounding boxes still reach the leaves behind it.
class TriangleRaycaster {
    struct Impl;
    std::unique_ptr<Impl> m_impl;
public:
    explicit TriangleRaycaster(const indexed_triangle_set &its);
    ~TriangleRaycaster();
    TriangleRaycaster(const TriangleRaycaster &) = delete;
    TriangleRaycaster &operator=(const TriangleRaycaster &) = delete;
    // Hits sorted by the ray parameter t. A ray coplanar with a triangle yields no hit for it.
    std::vector<RayMeshHit> all_hits(const Vec3d &origin, const Vec3d &dir) const;
};

} // namespace Slic3r
