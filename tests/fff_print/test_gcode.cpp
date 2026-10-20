#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace Slic3r;

TEST_CASE("Klipper object labels name each copy without the characters Klipper cannot parse", "[GCode]")
{
    const auto [name, label] = GENERATE(table<std::string, std::string>({
        {"my part (2)", "my_part_2"},
        {"(cube)", "cube"},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"exclude_object", "1"}});
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config, nullptr, false, 2);
    model.objects.front()->name = name;
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    for (const char *copy : {"0", "1"}) {
        const std::string instance_label = label + "_id_0_copy_" + copy;
        INFO(instance_label);
        CHECK(gcode.find("EXCLUDE_OBJECT_DEFINE NAME=" + instance_label + " ") != std::string::npos);
        CHECK(gcode.find("EXCLUDE_OBJECT_START NAME=" + instance_label + "\n") != std::string::npos);
    }
}

// Firmwares read the compatibility header lines (Anycubic's GoKlipper heats the bed with
// first_layer_bed_temperature), so they must describe the filament the plate prints, not filament 1.
TEST_CASE("The G-code header names the first layer temperatures of the filament the plate prints", "[GCode]")
{
    const std::string formula = GENERATE("by_first_filament", "by_highest_temp");
    DYNAMIC_SECTION("bed_temperature_formula " << formula) {
        // Filament 1: bed 60, nozzle 200. Filament 2: bed 100, nozzle 250. Everything prints with filament 2.
        DynamicPrintConfig config = Test::multifilament_config(2, {
            {"bed_temperature_formula", formula},
            // Authored 1..n as PresetBundle writes it; the all-1s default collapses every per-filament value
            // to filament 1.
            {"filament_self_index", "1,2"},
            {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
            {"enable_prime_tower", 0},
            {"nozzle_temperature_initial_layer", "200,250"},
            {"nozzle_temperature", "200,250"},
            {"supertack_plate_temp_initial_layer", "60,100"},
            {"cool_plate_temp_initial_layer", "60,100"},
            {"textured_cool_plate_temp_initial_layer", "60,100"},
            {"eng_plate_temp_initial_layer", "60,100"},
            {"hot_plate_temp_initial_layer", "60,100"},
            {"textured_plate_temp_initial_layer", "60,100"},
            {"sparse_infill_filament_id", 2},
            {"internal_solid_filament_id", 2},
            {"top_surface_filament_id", 2},
            {"bottom_surface_filament_id", 2},
            {"outer_wall_filament_id", 2},
            {"inner_wall_filament_id", 2},
            {"skirt_loops", 0},
            {"brim_type", "no_brim"},
        });
        const std::string gcode = Test::slice({Test::cube(20)}, config);

        const auto header_value = [&gcode](const std::string& key) {
            const std::string token = "; " + key + " = ";
            const size_t pos = gcode.find(token);
            REQUIRE(pos != std::string::npos);
            return std::stoi(gcode.substr(pos + token.size()));
        };
        CHECK(header_value("first_layer_bed_temperature") == 100);
        CHECK(header_value("first_layer_temperature") == 250);
    }
}

// Printing by object reassigns the initial filament to each object's first filament while it emits the objects, so
// the header must still name the filament the print starts with: the firmware heats the bed for the first object.
TEST_CASE("The G-code header names the first object's filament when printing by object", "[GCode]")
{
    // Filament 1: bed 60, nozzle 200. Filament 2: bed 100, nozzle 250. Object 1 prints with filament 2, object 2
    // with filament 1.
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"print_sequence", "by object"},
        {"bed_temperature_formula", "by_first_filament"},
        {"filament_self_index", "1,2"},
        {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"enable_prime_tower", 0},
        {"nozzle_temperature_initial_layer", "200,250"},
        {"nozzle_temperature", "200,250"},
        {"supertack_plate_temp_initial_layer", "60,100"},
        {"cool_plate_temp_initial_layer", "60,100"},
        {"textured_cool_plate_temp_initial_layer", "60,100"},
        {"eng_plate_temp_initial_layer", "60,100"},
        {"hot_plate_temp_initial_layer", "60,100"},
        {"textured_plate_temp_initial_layer", "60,100"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    const std::string gcode = Test::slice_with_object_overrides({Test::cube(20), Test::cube(20)}, config,
                                                                {{{"extruder", "2"}}, {{"extruder", "1"}}});

    // Filament 2 starts and filament 1 follows, so the last object's filament differs from the first one's.
    const size_t first_t1 = gcode.find("\nT1");
    const size_t first_t0 = gcode.find("\nT0");
    REQUIRE(first_t1 != std::string::npos);
    REQUIRE(first_t0 != std::string::npos);
    REQUIRE(first_t1 < first_t0);

    const auto header_value = [&gcode](const std::string& key) {
        const std::string token = "; " + key + " = ";
        const size_t pos = gcode.find(token);
        REQUIRE(pos != std::string::npos);
        return std::stoi(gcode.substr(pos + token.size()));
    };
    CHECK(header_value("first_layer_bed_temperature") == 100);
    CHECK(header_value("first_layer_temperature") == 250);
}

TEST_CASE("Object label comments number each object and copy with or without exclude object", "[GCode]")
{
    // repetier has no exclude-object commands, so set_object_info() writes nothing for it.
    // BBL printers write M624/M625 skip blocks whatever exclude_object is set to.
    const auto [flavor, exclude_object, bbl] = GENERATE(table<std::string, std::string, bool>({
        {"marlin2", "0", false},
        {"klipper", "0", false},
        {"klipper", "1", false},
        {"repetier", "1", false},
        {"marlin2", "0", true},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", flavor}, {"exclude_object", exclude_object}, {"gcode_label_objects", "1"}});
    Print print;
    print.is_BBL_printer() = bbl;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.), Test::cube(20.)}, print, model, config, nullptr, false, 2);
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    INFO(flavor << ", exclude_object " << exclude_object << ", BBL " << bbl);
    for (const char *label : {"id:0 copy 0", "id:0 copy 1", "id:1 copy 0", "id:1 copy 1"}) {
        INFO(label);
        CHECK(gcode.find("; printing object object.stl " + std::string(label) + "\n") != std::string::npos);
    }
    if (bbl)
        for (const PrintObject *object : print.objects())
            for (const PrintInstance &instance : object->instances()) {
                const std::string label_id = std::to_string(instance.model_instance->get_labeled_id());
                INFO("label id " << label_id);
                CHECK(gcode.find("; start printing object, unique label id: " + label_id + "\nM624 ") != std::string::npos);
                CHECK(gcode.find("; stop printing object, unique label id: " + label_id + "\nM625\n") != std::string::npos);
            }
}

TEST_CASE("An exported G-code's settings load back as the settings it was sliced with", "[GCode]")
{
    // Two filaments, so the config block carries the flush matrix already multiplied by flush_multiplier.
    const DynamicPrintConfig config = Test::multifilament_config(2, {
        { "flush_multiplier", "0.5" },
        { "printable_area", "0x0,250x0,250x210,0x210" },
        { "printable_height", "180" },
    });
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ Test::cube(20.) }, print, model, config, nullptr, false);
    ScopedTemporaryFile file(".gcode");
    {
        std::ofstream os(file.string(), std::ios::binary);
        os << Test::gcode(print);
        REQUIRE(os.good());
    }

    PresetBundle bundle;
    // The config block leaves prime_volume_mode out, so the receiver's choice has to survive the load.
    bundle.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value = PrimeVolumeMode::pvmFast;
    ConfigSubstitutions substitutions;
    bundle.load_config_model(file.string(), bundle.config_from_gcode(file.string(), ForwardCompatibilitySubstitutionRule::Enable, substitutions));

    const DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    CHECK(printer.opt_serialize("printable_area") == config.opt_serialize("printable_area"));
    CHECK_THAT(printer.opt_float("printable_height"), Catch::Matchers::WithinAbs(180., 1e-6));
    // The purge between the two filaments is what the slice used: 280 at a 0.5 multiplier.
    const std::vector<double> &matrix     = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    const std::vector<double> &multiplier = bundle.project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
    REQUIRE(matrix.size() == 4);
    REQUIRE(multiplier.size() == 1);
    CHECK_THAT(matrix[1] * multiplier[0], Catch::Matchers::WithinAbs(140., 1e-6));
    CHECK(bundle.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == PrimeVolumeMode::pvmFast);
}

namespace {

// What the object's first extrusions see after the raft, printed with filament 2, hands over to
// filament 1 in a spiral vase print.
struct SpiralRaftToolChange
{
    double first_model_extrusion_z { -1. };
    double highest_z_before_model  { 0. };
    double nominal_model_layer_z   { 0. };
    double lowest_model_extrusion_z { std::numeric_limits<double>::max() };
};

SpiralRaftToolChange spiral_raft_tool_change(const std::string &change_filament_gcode)
{
    // The raft is filament 2's only job; the object prints every feature with filament 1, so the
    // one tool change of the print sits between the raft and the first object layer. Spiral mode
    // keeps its usual single wall and no infill; one bottom shell layer keeps that first object
    // layer a normal (non-spiral) layer, so its Z is the layer's nominal print_z.
    DynamicPrintConfig config = Test::multifilament_config(2, {
        { "spiral_mode",                 true },
        { "wall_loops",                  1 },
        { "top_shell_layers",            0 },
        { "bottom_shell_layers",         1 },
        { "sparse_infill_density",       "0%" },
        { "enable_support",              true },
        { "raft_layers",                 2 },
        { "support_filament",            2 },
        { "support_interface_filament",  2 },
        { "outer_wall_filament_id",      1 },
        { "inner_wall_filament_id",      1 },
        { "sparse_infill_filament_id",   1 },
        { "internal_solid_filament_id",  1 },
        { "bottom_surface_filament_id",  1 },
        { "enable_prime_tower",          false },
        { "skirt_loops",                 0 },
        { "brim_type",                   "no_brim" },
        { "change_filament_gcode",       change_filament_gcode },
    });

    Print print;
    Model model;
    Test::init_print({ Test::TestMesh::cube_20x20x20 }, print, model, config);
    std::string gcode = Test::gcode(print);
    // The settings footer echoes change_filament_gcode, which must not be read as moves.
    const size_t footer_start = gcode.find("; CONFIG_BLOCK_START");
    if (footer_start != std::string::npos)
        gcode.erase(footer_start);

    SpiralRaftToolChange result;
    result.nominal_model_layer_z = print.objects().front()->layers().front()->print_z;
    int  current_tool   = -1;
    bool raft_tool_seen = false;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&result, &current_tool, &raft_tool_seen](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view cmd = line.cmd();
        if (cmd.size() > 1 && cmd.front() == 'T') {
            current_tool = std::atoi(std::string(cmd.substr(1)).c_str());
            raft_tool_seen |= current_tool == 1;
        }
        const bool model_extrusion = raft_tool_seen && current_tool == 0 && line.extruding(self) && line.dist_XY(self) > 0.;
        if (result.first_model_extrusion_z < 0.) {
            result.highest_z_before_model = std::max(result.highest_z_before_model, double(self.z()));
            if (model_extrusion)
                result.first_model_extrusion_z = self.z();
        }
        if (model_extrusion)
            result.lowest_model_extrusion_z = std::min(result.lowest_model_extrusion_z, double(self.z()));
    });
    return result;
}

} // namespace

// After the raft's tool change the head must come back down to the first object layer before it
// extrudes, whatever Z change_filament_gcode left the head at. Bambu Studio 77b9dd94d restored the
// layer Z in the spiral-vase travel branch; SnapOrca routes every travel through travel_to_xyz
// with the layer's nominal Z, which restores it on the way to the first point.
TEST_CASE("Spiral vase restores the object layer Z after a raft tool change", "[GCode][SpiralVase][Regression]")
{
    SECTION("tool change ends at a clearance Z above the whole print") {
        const SpiralRaftToolChange result = spiral_raft_tool_change("G1 X5 F12000\nG1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n");
        REQUIRE(result.first_model_extrusion_z >= 0.);
        CHECK(result.highest_z_before_model >= result.nominal_model_layer_z + 2.);
        CHECK_THAT(result.first_model_extrusion_z, Catch::Matchers::WithinAbs(result.nominal_model_layer_z, 1e-3));
        CHECK(result.lowest_model_extrusion_z >= result.nominal_model_layer_z - 1e-3);
    }
    SECTION("tool change leaves the XY position unknown at a clearance Z") {
        const SpiralRaftToolChange result = spiral_raft_tool_change("G1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n");
        REQUIRE(result.first_model_extrusion_z >= 0.);
        CHECK(result.highest_z_before_model >= result.nominal_model_layer_z + 2.);
        CHECK_THAT(result.first_model_extrusion_z, Catch::Matchers::WithinAbs(result.nominal_model_layer_z, 1e-3));
        CHECK(result.lowest_model_extrusion_z >= result.nominal_model_layer_z - 1e-3);
    }
    SECTION("tool change does not alter Z") {
        const SpiralRaftToolChange result = spiral_raft_tool_change("T[next_extruder]\n");
        REQUIRE(result.first_model_extrusion_z >= 0.);
        CHECK_THAT(result.first_model_extrusion_z, Catch::Matchers::WithinAbs(result.nominal_model_layer_z, 1e-3));
        CHECK(result.lowest_model_extrusion_z >= result.nominal_model_layer_z - 1e-3);
    }
}
