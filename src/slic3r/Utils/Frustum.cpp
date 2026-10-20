#include "Frustum.hpp"

#include "libslic3r/libslic3r.h"

#include <Eigen/Core>

#include <cmath>

namespace Slic3r {

bool Plane::IsOutside(const BoundingBoxf3& box) const
{
    const Vec3f box_min   = box.min.cast<float>();
    const Vec3f box_max   = box.max.cast<float>();
    const Vec3f center    = (box_min + box_max) * 0.5f;
    const Vec3f half_size = box_max - center;
    // Radius of the box projected on the plane normal.
    const float radius = half_size.x() * std::abs(m_normal.x()) + half_size.y() * std::abs(m_normal.y()) +
                         half_size.z() * std::abs(m_normal.z());
    return center.dot(m_normal) + m_distance < -radius;
}

Frustum::Frustum(const Matrix4d& view_projection)
{
    // Gribb-Hartmann extraction: a point p is inside when -w <= x, y, z <= w for (x, y, z, w) = M * p,
    // so each plane is row 3 plus or minus row 0 (left/right), 1 (bottom/top) or 2 (near/far).
    const Eigen::RowVector4d w = view_projection.row(3);
    for (int i = 0; i < 6; ++i) {
        const Eigen::RowVector4d row   = view_projection.row(i / 2);
        const Eigen::RowVector4d coeff = (i % 2 == 0) ? Eigen::RowVector4d(w + row) : Eigen::RowVector4d(w - row);
        const double             norm  = coeff.head<3>().norm();
        // A degenerate plane would cull everything or nothing at random; leave it accepting all.
        if (norm > EPSILON)
            planes[i] = Plane((coeff.head<3>() / norm).transpose().cast<float>(), float(coeff(3) / norm));
        else
            planes[i] = Plane(Vec3f::Zero(), 1.f);
    }
}

bool Frustum::Intersects(const BoundingBoxf3& box) const
{
    for (const Plane& plane : planes)
        if (plane.IsOutside(box))
            return false;
    return true;
}

} // namespace Slic3r
