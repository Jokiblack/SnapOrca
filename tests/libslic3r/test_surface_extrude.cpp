// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// Align & Construct: the picked flat face, its outline and the solid extruded from it.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/NeotkoSurfaceExtrude.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {

// 20 x 10 plate, 2 high, with a 4 x 4 hole through it.
indexed_triangle_set plate_with_hole()
{
    ExPolygon ex;
    ex.contour.points = { Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                          Point::new_scale(20., 10.), Point::new_scale(0., 10.) };
    Polygon hole;
    hole.points = { Point::new_scale(3., 3.), Point::new_scale(3., 7.),
                    Point::new_scale(7., 7.), Point::new_scale(7., 3.) };
    ex.holes.emplace_back(hole);
    return slices_to_mesh(std::vector<ExPolygons>{ { ex } }, 0., 2., 2.);
}

int top_facet(const indexed_triangle_set &its, const std::vector<Vec3f> &normals)
{
    for (int f = 0; f < (int)its.indices.size(); ++f)
        if (normals[f].normalized().z() > 0.99f)
            return f;
    return -1;
}

struct TopFace
{
    indexed_triangle_set its = plate_with_hole();
    ExPolygons           outline;
    double               z    = -1.;
    double               area = 0.; // mm2
};

TopFace top_face()
{
    TopFace tf;
    const std::vector<Vec3f>   normals   = its_face_normals(tf.its);
    const std::vector<Vec3i32> neighbors = its_face_neighbors(tf.its);
    const std::vector<int>     region    = SurfaceExtrude::coplanar_region(normals, neighbors, top_facet(tf.its, normals), 0.9994f);
    tf.outline = SurfaceExtrude::region_outline(tf.its, region, Transform3d::Identity(), &tf.z);
    if (!tf.outline.empty())
        tf.area = tf.outline.front().area() * SCALING_FACTOR * SCALING_FACTOR;
    return tf;
}

} // namespace

TEST_CASE("The coplanar region of a flat face holds only that face", "[SurfaceExtrude]")
{
    const indexed_triangle_set its       = plate_with_hole();
    const std::vector<Vec3f>   normals   = its_face_normals(its);
    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);
    const int                  seed      = top_facet(its, normals);
    REQUIRE(seed >= 0);

    const std::vector<int> region = SurfaceExtrude::coplanar_region(normals, neighbors, seed, 0.9994f);
    REQUIRE(!region.empty());
    for (int f : region)
        CHECK(normals[f].normalized().z() > 0.99f);
}

TEST_CASE("The outline of a face keeps its holes and its height", "[SurfaceExtrude]")
{
    const TopFace tf = top_face();
    REQUIRE(tf.outline.size() == 1);
    CHECK(tf.outline.front().holes.size() == 1);
    CHECK_THAT(tf.z, WithinRel(2., 1e-6));
    CHECK_THAT(tf.area, WithinRel(20. * 10. - 4. * 4., 0.01));
    CHECK(SurfaceExtrude::outline_holes(tf.outline).size() == 1);
}

TEST_CASE("Extruding an outline gives a closed solid of its area times the height", "[SurfaceExtrude]")
{
    const TopFace tf = top_face();
    REQUIRE(!tf.outline.empty());

    const indexed_triangle_set sheet = SurfaceExtrude::extrude_outline(tf.outline, 0., tf.z, 0.05);
    REQUIRE(!sheet.indices.empty());
    CHECK(its_num_open_edges(sheet) == 0);
    CHECK_THAT(its_volume(sheet), WithinRel(tf.area * 0.05, 0.01));

    // Shrinking by 1 mm: outer 18 x 8, the hole grows to 6 x 6.
    const indexed_triangle_set shrunk = SurfaceExtrude::extrude_outline(tf.outline, -1., tf.z, 0.05);
    CHECK_THAT(its_volume(shrunk), WithinRel((18. * 8. - 6. * 6.) * 0.05, 0.02));

    // An offset that eats the whole shape gives nothing.
    CHECK(SurfaceExtrude::extrude_outline(tf.outline, -20., tf.z, 0.05).indices.empty());
}

TEST_CASE("A curved extrusion follows its curve and stays closed", "[SurfaceExtrude]")
{
    const TopFace tf = top_face();
    REQUIRE(!tf.outline.empty());

    // A straight curve gives the plain solid.
    std::vector<SurfaceExtrude::CurveNode> curve(2);
    curve[0].t = 0.; curve[0].ht = 0.5;
    curve[1].t = 1.; curve[1].ht = 1.;
    const indexed_triangle_set straight = SurfaceExtrude::extrude_outline_curved(tf.outline, 0., tf.z, 2., curve, false);
    CHECK_THAT(its_volume(straight), WithinRel(tf.area * 2., 0.01));

    // Pulling the top in by 1 mm gives less volume, but more than the fully shrunk prism.
    curve[0].he = -0.5;
    curve[1].e  = -1.;
    const indexed_triangle_set tapered = SurfaceExtrude::extrude_outline_curved(tf.outline, 0., tf.z, 2., curve, false);
    CHECK(its_num_open_edges(tapered) == 0);
    CHECK(its_volume(tapered) < its_volume(straight));
    CHECK(its_volume(tapered) > (18. * 8. - 6. * 6.) * 2.);
}
