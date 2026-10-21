// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// The pure pieces of the Sandwich engine: tool sequences, positional fields, pass stacks and the
// profile library. The slicing side lives in tests/fff_print/test_sandwich.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ColorStitch.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/SurfacePassKind.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

struct RawLine {
    Polyline pl;
    float    width = 0.4f;
};

// A fill line along X at height y_mm, from x0_mm to x1_mm.
RawLine horizontal_line(double y_mm, double x0_mm = 0., double x1_mm = 10.)
{
    RawLine l;
    l.pl.points = { Point(scaled<coord_t>(x0_mm), scaled<coord_t>(y_mm)), Point(scaled<coord_t>(x1_mm), scaled<coord_t>(y_mm)) };
    return l;
}

size_t count_of(const std::vector<int> &v, int tool) { return size_t(std::count(v.begin(), v.end(), tool)); }

std::vector<double> linear_t(int n)
{
    std::vector<double> t(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        t[size_t(i)] = n > 1 ? double(i) / double(n - 1) : 0.5;
    return t;
}

} // namespace

TEST_CASE("The dithered tool sequences hold the exact share of every tool", "[ColorStitch]")
{
    const std::vector<int> two = ColorStitch::build_dithered_tools_2color(10, 0, 1, 30);
    REQUIRE(two.size() == 10);
    CHECK(count_of(two, 0) == 3);
    CHECK(count_of(two, 1) == 7);
    // Sampled at the line positions, the positional variant is the counting one.
    CHECK(ColorStitch::build_dithered_tools_2color_at(linear_t(10), 0, 1, 30) == two);

    const std::vector<int> three = ColorStitch::build_dithered_tools_3color(12, 0, 1, 2, 25, 50);
    REQUIRE(three.size() == 12);
    CHECK(count_of(three, 0) + count_of(three, 1) + count_of(three, 2) == 12);
    CHECK(ColorStitch::build_dithered_tools_3color_at(linear_t(12), 0, 1, 2, 25, 50) == three);

    CHECK(ColorStitch::build_dithered_tools_2color(5, 0, 1, 100) == std::vector<int>(5, 0));
    CHECK(ColorStitch::build_dithered_tools_2color(5, 0, 1, 0) == std::vector<int>(5, 1));
    CHECK(ColorStitch::build_dithered_tools_2color(0, 0, 1, 50).empty());
}

TEST_CASE("Custom bands repeat their counts and skip empty bands", "[ColorStitch]")
{
    CHECK(ColorStitch::build_custom_bands(7, 0, 2, 1, 1, -1, 3, 2, 0) == std::vector<int>{ 0, 0, 1, 0, 0, 1, 0 });
    // No band at all: every line keeps tool A.
    CHECK(ColorStitch::build_custom_bands(3, 2, 0, 1, 0, -1, 0, -1, 0) == std::vector<int>(3, 2));
}

TEST_CASE("Millimetre bands follow the position of each line, not its index", "[ColorStitch]")
{
    // Lines along X, so the axis across them is Y; bands of 1 mm repeat every 2 mm from the origin.
    const std::vector<RawLine> lines = { horizontal_line(0.5), horizontal_line(1.5), horizontal_line(2.5), horizontal_line(3.5) };
    CHECK(compute_slot_per_line_band_mm(lines, { 1.0, 1.0 }, 0, 0.0, 0.0) == std::vector<int>{ 0, 1, 0, 1 });
    // Moving the anchor by one band swaps them.
    CHECK(compute_slot_per_line_band_mm(lines, { 1.0, 1.0 }, 0, 0.0, 1.0) == std::vector<int>{ 1, 0, 1, 0 });
}

TEST_CASE("The two pieces of a line cut by a hole share one lane", "[ColorStitch]")
{
    const std::vector<RawLine> lines = { horizontal_line(1.0, 0., 2.), horizontal_line(1.0, 5., 7.), horizontal_line(2.0) };
    const FieldGroups          groups = compute_field_groups(lines, 0, 0.0, 1.0, 0.0, 0.0, false);
    REQUIRE(groups.t_of_group.size() == 2);
    CHECK(groups.group_of_line == std::vector<int>{ 0, 0, 1 });
    CHECK_THAT(groups.t_of_group[0], WithinAbs(0.0, 1e-9));
    CHECK_THAT(groups.t_of_group[1], WithinAbs(1.0, 1e-9));

    const FieldGroups inverted = compute_field_groups(lines, 0, 0.0, 1.0, 0.0, 0.0, true);
    CHECK(inverted.group_of_line == std::vector<int>{ 1, 1, 0 });
    CHECK(std::is_sorted(inverted.t_of_group.begin(), inverted.t_of_group.end()));
}

TEST_CASE("A pass stack keeps its passes through JSON and tells no effect from not authored", "[ColorStitch][Sandwich]")
{
    SurfacePassStack st;
    st.enabled            = true;
    st.perimeter_override = true;
    SurfacePass solid;
    solid.kind       = SurfacePassKind::Solid;
    solid.ratio      = 0.4;
    solid.solid_tool = 2;
    solid.speed_pct  = 80;
    solid.gcode_start = "M117 pass";
    SurfacePass stitch;
    stitch.kind                = SurfacePassKind::ColorStitch;
    stitch.ratio               = 0.6;
    stitch.colorstitch.present = true;
    stitch.colorstitch.kv["interlayer_colormix_pattern_top"] = "12";
    st.passes = { solid, stitch };

    const SurfacePassStack back = SurfacePassStack::from_json(st.to_json());
    REQUIRE(back.passes.size() == 2);
    CHECK(back.enabled);
    CHECK(back.perimeter_override);
    CHECK(back.any_effect());
    CHECK(back.passes[0].kind == SurfacePassKind::Solid);
    CHECK(back.passes[0].solid_tool == 2);
    CHECK(back.passes[0].speed_pct == 80);
    CHECK(back.passes[0].gcode_start == "M117 pass");
    CHECK_THAT(back.passes[0].ratio, WithinAbs(0.4, 1e-9));
    CHECK(back.passes[1].kind == SurfacePassKind::ColorStitch);
    CHECK(back.passes[1].colorstitch == stitch.colorstitch);

    // Unauthored: nothing stored. Authored without effect: a single None pass.
    CHECK(SurfacePassStack().to_json().empty());
    SurfacePassStack off = st;
    off.enabled          = false;
    const SurfacePassStack none = SurfacePassStack::from_json(off.to_json());
    REQUIRE(none.passes.size() == 1);
    CHECK(none.passes.front().kind == SurfacePassKind::None);
    CHECK_FALSE(none.any_effect());

    CHECK(SurfacePassStack::from_json("not json").empty());
}

TEST_CASE("A default region asks for no Sandwich", "[ColorStitch][Sandwich]")
{
    const PrintRegionConfig cfg;
    CHECK_FALSE(SurfacePassStack::resolve(cfg, erTopSolidInfill).any_effect());
    CHECK_FALSE(SurfacePassStack::resolve(cfg, erPenultimateInfill).any_effect());
    CHECK_FALSE(SurfacePassStack::resolve_for_zone(DynamicPrintConfig::full_print_config(), false).any_effect());
}

TEST_CASE("The legacy MultiPass keys become a stack of solid passes", "[ColorStitch][Sandwich]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "multipass_enabled", "1" },
                                 { "multipass_num_passes", "2" },
                                 { "multipass_tool_1", "0" },
                                 { "multipass_tool_2", "1" },
                                 { "multipass_width_ratio_1", "0.5" },
                                 { "multipass_width_ratio_2", "0.5" } });
    const SurfacePassStack st = SurfacePassStack::resolve_for_zone(cfg, false);
    REQUIRE(st.passes.size() == 2);
    CHECK(st.any_effect());
    CHECK(st.all_solid());
    CHECK(st.passes[0].solid_tool == 0);
    CHECK(st.passes[1].solid_tool == 1);
    // MultiPass of the top surface leaves the penultimate one alone.
    CHECK_FALSE(SurfacePassStack::resolve_for_zone(cfg, true).any_effect());
}

TEST_CASE("A PathBlend pass keeps its geometry through its blob", "[ColorStitch][PathBlend]")
{
    PathBlendPassConfig pb;
    pb.mode        = PathBlendPassConfig::Mode::Half;
    pb.floor_mm    = 0.03f;
    pb.mid_end_mm  = 0.12f;
    pb.tool_bottom = 2;
    pb.tool_top    = -1;
    pb.ease_mode   = 3;
    pb.fill_angle  = 45;
    pb.in_t        = 0.1f;
    pb.out_t       = 0.9f;
    pb.span_mm     = 8.f;
    const PathBlendPassConfig back = PathBlendPassConfig::from_blob_json(pb.to_blob_json());
    CHECK(back.mode == pb.mode);
    CHECK_THAT(back.floor_mm, WithinAbs(pb.floor_mm, 1e-6));
    CHECK_THAT(back.mid_end_mm, WithinAbs(pb.mid_end_mm, 1e-6));
    CHECK(back.tool_bottom == 2);
    CHECK(back.tool_top == -1);
    CHECK(back.ease_mode == 3);
    CHECK(back.fill_angle == 45);
    CHECK_THAT(back.in_t, WithinAbs(0.1, 1e-6));
    CHECK_THAT(back.out_t, WithinAbs(0.9, 1e-6));
    CHECK_THAT(back.span_mm, WithinAbs(8.0, 1e-6));

    // First blob schema: ratios of a 0.2 mm layer.
    const PathBlendPassConfig legacy = PathBlendPassConfig::from_blob_json(R"({"num_passes":1,"tool":[3],"min_ratio":0.25,"max_ratio":0.75})");
    CHECK(legacy.mode == PathBlendPassConfig::Mode::Half);
    CHECK(legacy.tool_bottom == 3);
    CHECK(legacy.tool_top == -1);
    CHECK_THAT(legacy.floor_mm, WithinAbs(0.05, 1e-6));
    CHECK_THAT(legacy.mid_end_mm, WithinAbs(0.15, 1e-6));
}

TEST_CASE("PathBlend bands climb the ramp and the cap fills the layer above it", "[ColorStitch][PathBlend]")
{
    PathBlendPassConfig pb;
    pb.mode       = PathBlendPassConfig::Mode::Full;
    pb.floor_mm   = 0.04f;
    pb.mid_end_mm = 0.16f;
    const double bottom_z = 1.0, H = 0.2;
    const std::vector<PBBand> bands = compute_pb_bands(pb, bottom_z, H, 0.03, true, 3, 0.035);
    REQUIRE(bands.size() == 6);
    for (int k = 0; k < 3; ++k) {
        const PBBand &ramp = bands[size_t(k)];
        const PBBand &cap  = bands[size_t(k + 3)];
        CHECK_FALSE(ramp.is_cap);
        CHECK(cap.is_cap);
        CHECK_THAT(ramp.z_top, WithinAbs(bottom_z + 0.04 + 0.04 * (k + 1), 1e-5));
        CHECK_THAT(cap.z_top, WithinAbs(bottom_z + H, 1e-5));
        // The ramp under the middle of the band plus the cap is the layer height.
        const double ramp_mid = 0.04 + double(ramp.t_mid) * 0.12;
        CHECK_THAT(ramp_mid + cap.h_step, WithinAbs(H, 1e-5));
        CHECK(cap.h_step >= 0.035 - 1e-6);
    }
    for (size_t k = 1; k < 3; ++k)
        CHECK(bands[k].z_top > bands[k - 1].z_top);

    // No room between the floor and the top of the ramp: a degenerate pass.
    pb.floor_mm   = 0.17f;
    pb.mid_end_mm = 0.18f;
    CHECK(compute_pb_bands(pb, bottom_z, H, 0.03, true, 0, 0.035).empty());
}

TEST_CASE("Pattern digits 5 to 9 name the live mixed filaments after the physical ones", "[ColorStitch][MixedFilament]")
{
    const size_t num_physical = GENERATE(size_t(2), size_t(4));
    INFO("physical filaments " << num_physical);
    const std::vector<std::string> colours(num_physical, "#FF0000");

    MixedFilamentManager mgr;
    for (const int pct : { 25, 50, 75 })
        mgr.add_custom_filament(1, 2, pct, colours);
    std::vector<MixedFilament> &rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 3);
    // A deleted row has no virtual id: digit 5 is the second row, digit 6 the third.
    rows[0].deleted        = true;
    rows[1].manual_pattern = "1122";
    rows[2].component_a    = unsigned(num_physical - 1);
    rows[2].component_b    = unsigned(num_physical);
    rows[2].manual_pattern = "12";

    PrintRegionConfig cfg;
    cfg.interlayer_colormix_use_virtual.value = true;
    CHECK(ColorStitch::build_tool_list_from_pattern("5", cfg, &mgr, num_physical) == std::vector<int>{ 0, 0, 1, 1 });
    CHECK(ColorStitch::build_tool_list_from_pattern("6", cfg, &mgr, num_physical) ==
          std::vector<int>{ int(num_physical) - 2, int(num_physical) - 1 });
    CHECK(ColorStitch::build_tool_list_from_pattern("16", cfg, &mgr, num_physical) ==
          std::vector<int>{ 0, int(num_physical) - 2, int(num_physical) - 1 });

    // Without use_virtual the digits past 4 are ignored and the pattern falls back to the region tools.
    cfg.interlayer_colormix_use_virtual.value = false;
    CHECK(ColorStitch::build_tool_list_from_pattern("12", cfg, &mgr, num_physical) == std::vector<int>{ 0, 1 });
    CHECK(ColorStitch::build_tool_list_from_pattern("33", cfg, &mgr, num_physical) == std::vector<int>{ 2, 2 });
}

TEST_CASE("The profile library keeps ids and content through JSON", "[ColorStitch][SurfaceEffectProfile]")
{
    SurfaceEffectProfileManager lib;
    SurfaceEffectProfile        a;
    a.name                = "Stripes";
    a.preview_argb        = 0xFF00FF00u;
    a.stack_top_json      = R"({"v":1,"enabled":true,"passes":[{"kind":2,"ratio":1.0}]})";
    a.colorstitch.present = true;
    a.colorstitch.kv["interlayer_colormix_pattern_top"] = "1212";
    const int id_a = lib.add(a);
    SurfaceEffectProfile b;
    b.name           = "Auto";
    b.auto_generated = true;
    const int id_b   = lib.add(b);
    CHECK(id_a == 1);
    CHECK(id_b == 2);
    REQUIRE(lib.remove(id_a));
    // Ids are never reused: paint may still refer to a removed profile.
    CHECK(lib.add(SurfaceEffectProfile()) == 3);
    CHECK(lib.rename(id_b, "Renamed"));

    SurfaceEffectProfileManager back;
    REQUIRE(back.from_json(lib.to_json()));
    CHECK(back == lib);
    CHECK(back.find(id_a) == nullptr);
    REQUIRE(back.find(id_b) != nullptr);
    CHECK(back.find(id_b)->name == "Renamed");
    CHECK(back.find(id_b)->auto_generated);

    SurfaceEffectProfile clash;
    clash.id = id_b;
    CHECK_FALSE(back.adopt(clash));
    CHECK_FALSE(back.from_json("{broken"));
}
