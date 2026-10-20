#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include <libslic3r/Format/STL.hpp>
#include <libslic3r/MeshDiagnostics.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// Turns the detection on for one test and restores the default (off) afterwards.
struct ScopedReversedFacesDetection
{
    explicit ScopedReversedFacesDetection(bool enabled) { set_reversed_faces_detection(enabled); }
    ~ScopedReversedFacesDetection() { set_reversed_faces_detection(false); }
};

indexed_triangle_set inverted(indexed_triangle_set its)
{
    its_flip_triangles(its);
    return its;
}

// Every face normal points away from the centroid of a convex solid when the winding is outward.
bool normals_point_outward(const indexed_triangle_set &its)
{
    Vec3f centroid = Vec3f::Zero();
    for (const stl_vertex &v : its.vertices)
        centroid += v;
    centroid /= float(its.vertices.size());
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3f &a = its.vertices[f[0]];
        const Vec3f  n = (its.vertices[f[1]] - a).cross(its.vertices[f[2]] - a);
        if (n.dot(a - centroid) <= 0.f)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("A cube with every face wound inwards is reported as reversed, an outward cube is not", "[MeshDiagnostics]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    REQUIRE(normals_point_outward(cube));

    const MeshDiagnosticStats good = its_quick_diagnostics(cube);
    CHECK(good.open_edges == 0);
    CHECK(good.non_manifold_edges == 0);
    CHECK(good.same_direction_edges == 0);
    CHECK_FALSE(good.has_reversed_faces);

    const MeshDiagnosticStats bad = its_quick_diagnostics(inverted(cube));
    // A consistent inward winding is still a manifold: only the ray test can tell.
    CHECK(bad.same_direction_edges == 0);
    CHECK(bad.has_reversed_faces);
}

TEST_CASE("A single face wound against its neighbours is reported without a ray test", "[MeshDiagnostics]")
{
    indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    std::swap(cube.indices.front()[1], cube.indices.front()[2]);

    const MeshDiagnosticStats stats = its_edge_diagnostics(cube);
    CHECK(stats.same_direction_edges == 3);
    CHECK(stats.open_edges == 0);

    MeshDiagnosticStats detected = stats;
    its_detect_reversed_faces(cube, detected);
    CHECK(detected.has_reversed_faces);
}

TEST_CASE("An open mesh and an inward cavity are not reported as reversed", "[MeshDiagnostics]")
{
    SECTION("open mesh: the ray test needs a watertight shell") {
        indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        cube.indices.pop_back();
        const MeshDiagnosticStats stats = its_quick_diagnostics(cube);
        CHECK(stats.open_edges == 3);
        CHECK_FALSE(stats.has_reversed_faces);
    }
    SECTION("a cavity is wound inwards by design") {
        indexed_triangle_set solid = its_make_cube(24., 24., 24.);
        TriangleMesh         cavity(inverted(its_make_cube(12., 12., 12.)));
        cavity.translate(6.f, 6.f, 6.f);
        its_merge(solid, cavity.its);
        const MeshDiagnosticStats stats = its_quick_diagnostics(solid);
        CHECK(stats.open_edges == 0);
        CHECK_FALSE(stats.has_reversed_faces);
    }
}

TEST_CASE("TriangleMesh statistics carry the flag only while the detection is enabled", "[MeshDiagnostics]")
{
    const indexed_triangle_set inside_out = inverted(its_make_cube(20., 20., 20.));

    SECTION("off by default") {
        const TriangleMesh mesh(inside_out);
        CHECK(mesh.stats().volume < 0.f);
        CHECK_FALSE(mesh.stats().has_reversed_faces);
    }
    SECTION("enabled: flagged by the constructor, cleared by the flip the importers apply on a negative volume") {
        ScopedReversedFacesDetection detection(true);
        TriangleMesh                 mesh(inside_out);
        CHECK(mesh.stats().has_reversed_faces);
        REQUIRE(mesh.stats().volume < 0.f);
        mesh.flip_triangles();
        CHECK(mesh.stats().volume > 0.f);
        CHECK_FALSE(mesh.stats().has_reversed_faces);
        CHECK(normals_point_outward(mesh.its));
    }
}

TEST_CASE("Importing an STL cube with every face inverted yields a positive volume with outward normals", "[MeshDiagnostics][stl]")
{
    ScopedReversedFacesDetection detection(true);
    ScopedTemporaryFile          stl(".stl");
    REQUIRE(its_write_stl_binary(stl.string().c_str(), "inverted", inverted(its_make_cube(20., 20., 20.))));

    Model model;
    REQUIRE(load_stl(stl.string().c_str(), &model));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const TriangleMesh &mesh = model.objects.front()->volumes.front()->mesh();
    CHECK(mesh.stats().volume > 0.f);
    CHECK(normals_point_outward(mesh.its));
    CHECK_FALSE(mesh.stats().has_reversed_faces);
    CHECK_FALSE(model.objects.front()->get_object_stl_stats().has_reversed_faces);
}
