#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinRel;

static constexpr double CubeSize = 10.;

// Sum of the facet areas of `its`.
static double its_area_sum(const indexed_triangle_set &its)
{
    double area = 0.;
    for (const Vec3i32 &face : its.indices) {
        const Vec3f &a = its.vertices[face(0)];
        const Vec3f &b = its.vertices[face(1)];
        const Vec3f &c = its.vertices[face(2)];
        area += 0.5 * double((b - a).cross(c - a).norm());
    }
    return area;
}

// Paints the top of a cube (the facets whose normal points up) with Extruder3 and every other
// facet with Extruder2, so the painting carries two colors that a horizontal cut splits
// unevenly: the top ends up on the upper half only, the sides on both.
static void paint_top_and_sides(ModelVolume *volume)
{
    const indexed_triangle_set &its = volume->mesh().its;
    TriangleSelector            selector(volume->mesh());
    for (size_t facet_idx = 0; facet_idx < its.indices.size(); ++facet_idx) {
        const Vec3i32 &face   = its.indices[facet_idx];
        const Vec3f    normal = (its.vertices[face(1)] - its.vertices[face(0)]).cross(its.vertices[face(2)] - its.vertices[face(0)]);
        selector.set_facet(int(facet_idx), normal.z() > 0.f ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::Extruder2);
    }
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
}

// A painted cube of CubeSize standing on the bed, with the only instance at the origin.
static ModelObject *add_painted_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    ModelVolume *volume = object->add_volume(make_cube(CubeSize, CubeSize, CubeSize));
    volume->name        = "cube";
    object->add_instance();
    paint_top_and_sides(volume);
    return object;
}

// A horizontal cut plane through the middle of the cube, in the gizmo's convention: the cut
// matrix is relative to the instance offset, which is zero here.
static Transform3d mid_cut_matrix() { return Geometry::translation_transform(Vec3d(0., 0., 0.5 * CubeSize)); }

// Two solid parts next to each other on X, each with its own filament and name, so a cut that
// collapses them into one volume would lose both.
static ModelObject *add_two_part_object(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "pair";
    ModelVolume *red    = object->add_volume(make_cube(CubeSize, CubeSize, CubeSize));
    red->name           = "red";
    red->config.set("extruder", 2);
    TriangleMesh blue_mesh = make_cube(CubeSize, CubeSize, CubeSize);
    blue_mesh.translate(float(2. * CubeSize), 0.f, 0.f);
    ModelVolume *blue = object->add_volume(blue_mesh);
    blue->name        = "blue";
    blue->config.set("extruder", 3);
    object->add_instance();
    return object;
}

static std::set<int> part_extruders(const ModelObject *object)
{
    std::set<int> extruders;
    for (const ModelVolume *volume : object->volumes)
        if (volume->is_model_part() && volume->config.has("extruder"))
            extruders.insert(volume->config.extruder());
    return extruders;
}

static std::set<std::string> part_names(const ModelObject *object)
{
    std::set<std::string> names;
    for (const ModelVolume *volume : object->volumes)
        if (volume->is_model_part())
            names.insert(volume->name);
    return names;
}

static double parts_volume(const ModelObject *object)
{
    double volume_sum = 0.;
    for (const ModelVolume *volume : object->volumes)
        if (volume->is_model_part())
            volume_sum += double(its_volume(volume->mesh().its));
    return volume_sum;
}

TEST_CASE("A planar cut keeps the multi-color painting on both halves when asked to", "[CutUtils]")
{
    Model        model;
    ModelObject *object = add_painted_cube(model);

    Cut cut(object, 0, mid_cut_matrix(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &halves = cut.perform_with_plane();
    REQUIRE(halves.size() == 2);

    const double side_area = CubeSize * CubeSize; // one face of the cube
    const double half_side = 0.5 * side_area;     // one face of a half

    // The upper half keeps the top (Extruder3) and the upper half of the four sides (Extruder2).
    {
        const ModelObject *upper = halves[0];
        REQUIRE(upper->volumes.size() == 1);
        const ModelVolume *volume = upper->volumes.front();
        REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder2));
        REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder3));
        CHECK_THAT(its_area_sum(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::Extruder3)), WithinRel(side_area, 0.03));
        CHECK_THAT(its_area_sum(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::Extruder2)), WithinRel(4. * half_side, 0.03));
    }

    // The lower half keeps the bottom and the lower half of the sides (Extruder2) and never sees
    // the top color: the cap the cut created stays unpainted.
    {
        const ModelObject *lower = halves[1];
        REQUIRE(lower->volumes.size() == 1);
        const ModelVolume *volume = lower->volumes.front();
        REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder2));
        CHECK_FALSE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder3));
        CHECK_THAT(its_area_sum(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::Extruder2)), WithinRel(side_area + 4. * half_side, 0.03));
    }
}

TEST_CASE("A planar cut drops the painting unless asked to keep it", "[CutUtils]")
{
    Model        model;
    ModelObject *object = add_painted_cube(model);

    Cut cut(object, 0, mid_cut_matrix(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs &halves = cut.perform_with_plane();
    REQUIRE(halves.size() == 2);
    for (const ModelObject *half : halves) {
        REQUIRE(half->volumes.size() == 1);
        CHECK(half->volumes.front()->mmu_segmentation_facets.empty());
    }
}

TEST_CASE("A contour cut keeps each part's filament and name on the half it goes to", "[CutUtils]")
{
    Model        model;
    ModelObject *object = add_two_part_object(model);

    // The first part is selected (goes to the upper object), the second is not.
    std::vector<Cut::Part> parts{Cut::Part{true, false}, Cut::Part{false, false}};

    Cut cut(object, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs &halves = cut.perform_by_contour(object, parts, 0);
    REQUIRE(halves.size() == 2);

    const ModelObject *upper = halves[0];
    REQUIRE(upper->volumes.size() == 1);
    REQUIRE(upper->volumes.front()->config.has("extruder"));
    CHECK(upper->volumes.front()->config.extruder() == 2);
    CHECK(upper->volumes.front()->name == "red");

    const ModelObject *lower = halves[1];
    REQUIRE(lower->volumes.size() == 1);
    REQUIRE(lower->volumes.front()->config.has("extruder"));
    CHECK(lower->volumes.front()->config.extruder() == 3);
    CHECK(lower->volumes.front()->name == "blue");
}

TEST_CASE("A groove cut merges the pieces of each part back into that part", "[CutUtils]")
{
    Model        model;
    ModelObject *object = add_two_part_object(model);

    Cut::Groove groove;
    groove.depth       = 2.f;
    groove.width       = 4.f;
    groove.flaps_angle = float(PI) / 3.f;
    groove.angle       = 0.f;

    // One dovetail along the horizontal plane through the middle of both parts.
    Cut cut(object, 0, mid_cut_matrix(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs &halves = cut.perform_with_groove(groove, Transform3d::Identity(), 1, 0.f, float(3. * CubeSize));
    REQUIRE(halves.size() == 2);

    // Every half holds exactly one volume per source part, each carrying that part's filament
    // and name, instead of a single volume with neither.
    for (const ModelObject *half : halves) {
        std::vector<const ModelVolume *> solid_parts;
        for (const ModelVolume *volume : half->volumes)
            if (volume->is_model_part())
                solid_parts.push_back(volume);
        CHECK(solid_parts.size() == 2);
        CHECK(part_extruders(half) == std::set<int>{2, 3});
        CHECK(part_names(half) == std::set<std::string>{"red", "blue"});
    }

    // The pieces of both halves add up to the two cubes, less the tolerance the groove removes.
    const double total_volume = parts_volume(halves[0]) + parts_volume(halves[1]);
    CHECK(total_volume > 0.95 * 2. * CubeSize * CubeSize * CubeSize);
    CHECK(total_volume <= 1.001 * 2. * CubeSize * CubeSize * CubeSize);
}
