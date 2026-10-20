#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

// Plane a*x + b*y + c*z + d = 0, with (a, b, c) the normal pointing to the inside of the frustum.
class Plane
{
public:
    Plane() = default;
    Plane(const Vec3f& normal, float distance) : m_normal(normal), m_distance(distance) {}

    const Vec3f& GetNormal() const { return m_normal; }
    float        GetDistance() const { return m_distance; }

    // True when the box lies entirely on the outer side of the plane.
    bool IsOutside(const BoundingBoxf3& box) const;

private:
    Vec3f m_normal   = Vec3f::Zero();
    float m_distance = 0.f;
};

// The six clip planes of a view-projection matrix, in the space the matrix maps from (world space for
// projection * view). Used to skip drawing volumes that cannot reach the screen.
class Frustum
{
public:
    Frustum() = default;
    explicit Frustum(const Matrix4d& view_projection);

    // Conservative: false only when the box is certainly outside, so a culled volume would not have
    // produced any fragment.
    bool Intersects(const BoundingBoxf3& box) const;

    Plane planes[6];
};

} // namespace Slic3r
