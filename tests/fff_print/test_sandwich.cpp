// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// The Sandwich engine through the slicer: which layers get passes, at which heights, with which tools.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ColorStitch.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/SurfacePassKind.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// A 20 mm cube in 0.4 mm layers, first layer included: 50 layers, the top one from 19.6 to 20.0.
constexpr double kCube = 20., kLayer = 0.4;

DynamicPrintConfig sandwich_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra, unsigned int filaments = 1)
{
    DynamicPrintConfig config = filaments > 1 ? multifilament_config(filaments) : DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.4" },
                                    { "initial_layer_print_height", "0.4" },
                                    { "enable_prime_tower", "0" },
                                    { "enable_support", "0" },
                                    { "top_shell_layers", "3" },
                                    { "bottom_shell_layers", "3" } });
    config.set_deserialize_strict(extra);
    return config;
}

// Indices of the layers that carry passes.
std::vector<size_t> layers_with_passes(const PrintObject &object)
{
    std::vector<size_t> out;
    const auto         &sub = object.multipass_sublayers();
    for (size_t i = 0; i < sub.size(); ++i)
        if (!sub[i].empty())
            out.push_back(i);
    return out;
}

// Every Z at which the G-code extrudes.
std::set<double> extrusion_heights(const std::string &gcode)
{
    std::set<double> z;
    GCodeReader      parser;
    parser.parse_buffer(gcode, [&z](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self))
            z.insert(std::round(double(self.z()) * 1000.) / 1000.);
    });
    return z;
}

bool extrudes_at(const std::set<double> &heights, double z)
{
    return std::any_of(heights.begin(), heights.end(), [z](double h) { return std::abs(h - z) < 0.002; });
}

// Paints every upward facing triangle of `volume` with ColorStitch slot `slot`.
void paint_top_faces(ModelVolume &volume, int slot)
{
    TriangleSelector            selector(volume.mesh());
    const indexed_triangle_set &its = volume.mesh().its;
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3f &a = its.vertices[its.indices[f](0)], &b = its.vertices[its.indices[f](1)], &c = its.vertices[its.indices[f](2)];
        if ((b - a).cross(c - a).normalized().z() > 0.9f)
            selector.set_facet(int(f), EnforcerBlockerType(slot));
    }
    REQUIRE(volume.colorstitch_paint_facets.set(selector));
}

} // namespace

TEST_CASE("A print with the Sandwich keys at their defaults has no passes", "[Sandwich]")
{
    Print print;
    init_and_process_print({ cube(kCube) }, print, sandwich_config({}));
    for (const PrintObject *object : print.objects()) {
        CHECK_FALSE(object->has_multipass_sublayers());
        CHECK(object->colorstitch_paint_cache() == nullptr);
    }
}

TEST_CASE("A two-pass MultiPass prints the top surface in two half-height passes", "[Sandwich][MultiPass]")
{
    const DynamicPrintConfig config = sandwich_config({ { "multipass_enabled", "1" },
                                                        { "multipass_surface", "1" },
                                                        { "multipass_num_passes", "2" },
                                                        { "multipass_tool_1", "0" },
                                                        { "multipass_tool_2", "0" },
                                                        { "multipass_width_ratio_1", "0.5" },
                                                        { "multipass_width_ratio_2", "0.5" } });
    Print print;
    Model model;
    init_print({ cube(kCube) }, print, model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == size_t(kCube / kLayer));

    // Only the top layer has a top surface.
    const std::vector<size_t> with_passes = layers_with_passes(object);
    REQUIRE(with_passes == std::vector<size_t>{ object.layers().size() - 1 });
    std::vector<MultiPassSubLayer> passes = object.multipass_sublayers()[with_passes.front()];
    REQUIRE(passes.size() == 2);
    std::sort(passes.begin(), passes.end(), [](const auto &l, const auto &r) { return l.print_z < r.print_z; });
    const double top = kCube, bottom = kCube - kLayer;
    CHECK_THAT(passes[0].print_z, WithinAbs(bottom + 0.5 * kLayer, 0.01));
    CHECK_THAT(passes[1].print_z, WithinAbs(top, 0.01));
    for (const MultiPassSubLayer &p : passes) {
        CHECK(p.effect == SurfacePassKind::Solid);
        CHECK(p.tool_id == 0);
        CHECK_FALSE(p.fills.empty());
    }

    const std::set<double> heights = extrusion_heights(Test::gcode(print));
    CHECK(extrudes_at(heights, bottom + 0.5 * kLayer));
    CHECK(extrudes_at(heights, top));
}

TEST_CASE("A two-colour ColorStitch splits the top surface into one pass per filament", "[Sandwich][ColorStitch]")
{
    const DynamicPrintConfig config = sandwich_config({ { "interlayer_colormix_enabled", "1" },
                                                        { "interlayer_colormix_surface", "1" },
                                                        { "interlayer_colormix_mode", "0" },
                                                        { "interlayer_colormix_pattern_top", "12" },
                                                        { "top_surface_pattern", "monotonicline" } },
                                                      2);
    Print print;
    Model model;
    init_print({ cube(kCube) }, print, model, config);
    print.process();
    const PrintObject &object = *print.objects().front();

    const std::vector<size_t> with_passes = layers_with_passes(object);
    REQUIRE(with_passes == std::vector<size_t>{ object.layers().size() - 1 });
    const std::vector<MultiPassSubLayer> &passes = object.multipass_sublayers()[with_passes.front()];
    std::set<int> tools;
    for (const MultiPassSubLayer &p : passes) {
        CHECK(p.effect == SurfacePassKind::ColorStitch);
        tools.insert(p.tool_id);
    }
    CHECK(tools == std::set<int>{ 0, 1 });
}

TEST_CASE("A painted profile gives passes only where its paint is", "[Sandwich][ColorStitch]")
{
    const DynamicPrintConfig config = sandwich_config({});
    Print print;
    Model model;
    init_print({ cube(kCube) }, print, model, config);

    SurfacePassStack stack;
    stack.enabled = true;
    for (int i = 0; i < 2; ++i) {
        SurfacePass p;
        p.kind       = SurfacePassKind::Solid;
        p.ratio      = 0.5;
        p.solid_tool = 0;
        stack.passes.push_back(p);
    }
    SurfaceEffectProfile profile;
    profile.name           = "Two halves";
    profile.stack_top_json = stack.to_json();
    const int id           = model.surface_effect_profiles.add(profile);

    ModelVolume &volume = *model.objects.front()->volumes.front();
    paint_top_faces(volume, 1);
    volume.colorstitch_slot_to_profile_id[1] = id;

    // The paint and the library reach the print through apply(), as they do from the plater.
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(config);
    full.set_key_value("gcode_comments", new ConfigOptionBool(true));
    print.apply(model, full);
    print.process();

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.colorstitch_paint_cache() != nullptr);
    const std::vector<size_t> with_passes = layers_with_passes(object);
    REQUIRE(with_passes == std::vector<size_t>{ object.layers().size() - 1 });
    CHECK(object.multipass_sublayers()[with_passes.front()].size() == 2);

    // Unmapping the slot removes the effect.
    volume.colorstitch_slot_to_profile_id[1] = 0;
    print.apply(model, full);
    print.process();
    CHECK_FALSE(print.objects().front()->has_multipass_sublayers());
}
