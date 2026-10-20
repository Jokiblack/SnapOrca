#include "MeshDiagnostics.hpp"
#include "MeshRaycast.hpp"
#include "Point.hpp"

#include <admesh/stl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>
#include <tbb/parallel_sort.h>

namespace Slic3r {

namespace {

std::atomic<bool> s_reversed_faces_detection{false};

bool face_vertices_valid(const indexed_triangle_set &its, const stl_triangle_vertex_indices &idx)
{
    const size_t n = its.vertices.size();
    return idx[0] >= 0 && idx[1] >= 0 && idx[2] >= 0
        && static_cast<size_t>(idx[0]) < n
        && static_cast<size_t>(idx[1]) < n
        && static_cast<size_t>(idx[2]) < n;
}

// A hit whose smallest barycentric coordinate is this close to 0 sits on a triangle edge or vertex,
// where neighbouring faces answer the same query differently. AABBTreeIndirect may also place a
// near-edge crossing slightly inside one triangle. 0.02 stays below the interior barycentric of the
// reversed-face samples the threshold was tuned on (~0.06) while covering those near-edge hits.
constexpr double k_bary_eps = 0.02;

// RayMeshHit uses P = (1-u-v)*v0 + u*v1 + v*v2.
bool hit_on_edge_or_vertex(const RayMeshHit &hit, double bary_eps)
{
    const double w = 1.0 - hit.u - hit.v;
    return std::min(hit.u, std::min(hit.v, w)) <= bary_eps;
}

// Visible back faces from outside the AABB. The caller passes a watertight mesh (no open edges, no
// non-manifold edges). Per ray: all hits. A closed mesh produces an even hit count (enter/leave
// pairs), an odd count is discarded as a leak. The whole ray is also discarded if the two smallest t
// differ by less than k_coincident_t, or if its outermost hit is a sliver, lands on an edge or vertex,
// or is grazing. Only the outermost surviving hit is tested; one back-facing hit sets the flag.
bool detect_visible_backfaces(const indexed_triangle_set &its)
{
    if (its.indices.empty() || its.vertices.empty())
        return false;

    // Later face lookups index the vertices without a bounds check.
    for (const stl_triangle_vertex_indices &idx : its.indices)
        if (!face_vertices_valid(its, idx)) {
            BOOST_LOG_TRIVIAL(debug) << "reversed-faces: skip, invalid vertex index";
            return false;
        }

    stl_vertex bmin_f = its.vertices.front();
    stl_vertex bmax_f = bmin_f;
    for (const stl_vertex &v : its.vertices) {
        bmin_f = bmin_f.cwiseMin(v);
        bmax_f = bmax_f.cwiseMax(v);
    }

    const Vec3d  bmin   = bmin_f.cast<double>();
    const Vec3d  bmax   = bmax_f.cast<double>();
    const Vec3d  center = 0.5 * (bmin + bmax);
    const Vec3d  extent = bmax - bmin;
    const double radius = 0.5 * extent.norm();
    if (radius <= 0.)
        return false;
    const double max_ext  = extent.maxCoeff();
    const double standoff = radius + std::max(max_ext * 0.05, 1.0);

    const TriangleRaycaster caster(its);

    // 26 axis and diagonal directions plus 20 on a Fibonacci sphere.
    std::vector<Vec3d> dirs;
    dirs.reserve(46);
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && y == 0 && z == 0)
                    continue;
                dirs.emplace_back(double(x), double(y), double(z));
            }
    constexpr int    extra  = 20;
    constexpr double golden = 2.399963229728653;
    for (int i = 0; i < extra; ++i) {
        const double y   = 1.0 - 2.0 * (i + 0.5) / extra;
        const double r   = std::sqrt(std::max(0.0, 1.0 - y * y));
        const double phi = i * golden;
        dirs.emplace_back(r * std::cos(phi), y, r * std::sin(phi));
    }

    constexpr double graze    = 0.08;
    constexpr double backface = 1e-4;
    // Two crossings closer than this are coincident faces. Relative to the AABB so a 2 mm part and a
    // 300 mm part do not share one absolute threshold; a 100 mm part lands near 0.01 mm.
    const double k_coincident_t = radius * 1e-4;
    // nlen = ||(b-a)x(c-a)|| = 2 * area. Slivers with a tiny area relative to the AABB have unreliable normals.
    const double min_nlen = 2.0 * radius * radius * 1e-12;

    int n_ray = 0, n_miss = 0, n_odd = 0, n_coincident = 0, n_boundary = 0;
    int n_graze = 0, n_sliver = 0, n_front = 0, n_back = 0;

    for (Vec3d dir : dirs) {
        dir.normalize();
        ++n_ray;
        const Vec3d origin  = center + dir * standoff;
        const Vec3d ray_dir = -dir;

        std::vector<RayMeshHit> hits = caster.all_hits(origin, ray_dir);
        hits.erase(std::remove_if(hits.begin(), hits.end(), [](const RayMeshHit &h) { return h.t < 1e-6; }), hits.end());
        if (hits.empty()) {
            ++n_miss;
            continue;
        }
        // Closed meshes enter and leave in pairs; an odd hit count is a leak.
        if (hits.size() % 2 == 1) {
            ++n_odd;
            continue;
        }
        if (hits[1].t - hits[0].t < k_coincident_t) {
            ++n_coincident;
            continue;
        }

        const RayMeshHit &hit = hits.front();
        if (hit.face < 0 || static_cast<size_t>(hit.face) >= its.indices.size()) {
            ++n_miss;
            continue;
        }

        const stl_triangle_vertex_indices &idx = its.indices[hit.face];
        const Vec3d a = its.vertices[idx[0]].cast<double>();
        const Vec3d b = its.vertices[idx[1]].cast<double>();
        const Vec3d c = its.vertices[idx[2]].cast<double>();
        Vec3d        N    = (b - a).cross(c - a);
        const double nlen = N.norm();
        if (nlen < min_nlen) {
            ++n_sliver;
            continue;
        }
        N /= nlen;

        if (hit_on_edge_or_vertex(hit, k_bary_eps)) {
            ++n_boundary;
            continue;
        }

        const double n_ray_dot = N.dot(ray_dir);
        if (std::abs(n_ray_dot) < graze) {
            ++n_graze;
            continue;
        }

        const Vec3d  hit_pt = origin + ray_dir * hit.t;
        const double ndot   = N.dot(origin - hit_pt);
        if (ndot < -backface)
            ++n_back;
        else
            ++n_front;
    }

    BOOST_LOG_TRIVIAL(debug)
        << "reversed-faces: rays=" << n_ray << " miss=" << n_miss << " odd=" << n_odd
        << " coincident=" << n_coincident << " sliver=" << n_sliver << " boundary=" << n_boundary
        << " graze=" << n_graze << " front=" << n_front << " back=" << n_back;

    return n_back > 0;
}

} // namespace

void its_detect_reversed_faces(const indexed_triangle_set &its, MeshDiagnosticStats &stats)
{
    if (stats.non_manifold_edges != 0 || stats.same_direction_edges != 0) {
        stats.has_reversed_faces = true;
        return;
    }
    if (stats.open_edges != 0)
        return;
    stats.has_reversed_faces = detect_visible_backfaces(its);
}

MeshDiagnosticStats its_quick_diagnostics(const indexed_triangle_set &its)
{
    MeshDiagnosticStats stats = its_edge_diagnostics(its);
    its_detect_reversed_faces(its, stats);
    return stats;
}

MeshDiagnosticStats its_edge_diagnostics(const indexed_triangle_set &its)
{
    MeshDiagnosticStats result;
    const size_t        num_vertices = its.vertices.size();
    const size_t        num_faces    = its.indices.size();

    if (num_faces == 0)
        return result;

    struct HalfEdge {
        uint64_t key     = 0;
        uint32_t face    = 0;
        uint8_t  forward = 0;
        bool     operator<(const HalfEdge &rhs) const
        {
            if (key != rhs.key)
                return key < rhs.key;
            if (face != rhs.face)
                return face < rhs.face;
            return forward < rhs.forward;
        }
    };

    std::vector<HalfEdge> edges;
    edges.reserve(num_faces * 3);

    for (size_t fid = 0; fid < num_faces; ++fid) {
        const stl_triangle_vertex_indices &face = its.indices[fid];
        // Degenerate faces (repeated vertex indices) carry no usable edges.
        if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0])
            continue;
        const size_t v[3] = { static_cast<size_t>(face[0]), static_cast<size_t>(face[1]), static_cast<size_t>(face[2]) };
        for (int i = 0; i < 3; ++i) {
            size_t va = v[i], vb = v[(i + 1) % 3];
            if (va >= num_vertices || vb >= num_vertices)
                continue;
            const uint8_t forward = va < vb ? 1 : 0;
            if (!forward)
                std::swap(va, vb);
            edges.push_back({ (uint64_t(va) << 32) | uint64_t(vb), static_cast<uint32_t>(fid), forward });
        }
    }

    tbb::parallel_sort(edges.begin(), edges.end());

    for (size_t i = 0; i < edges.size();) {
        size_t j = i + 1;
        while (j < edges.size() && edges[j].key == edges[i].key)
            ++j;
        const size_t count = j - i;
        size_t       plus = 0, minus = 0;
        for (size_t k = i; k < j; ++k)
            if (edges[k].forward)
                ++plus;
            else
                ++minus;
        if (plus >= 2 || minus >= 2)
            ++result.same_direction_edges;
        if (count == 1)
            ++result.open_edges;
        else if (count > 2)
            ++result.non_manifold_edges;
        i = j;
    }

    return result;
}

void set_reversed_faces_detection(bool enabled)
{
    s_reversed_faces_detection.store(enabled, std::memory_order_relaxed);
}

bool reversed_faces_detection_enabled()
{
    return s_reversed_faces_detection.load(std::memory_order_relaxed);
}

} // namespace Slic3r
