#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PerHeadProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("An enum vector index past the end of the vector reads as undefined", "[ConfigValueFormatter][Regression]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, ConfigOptionEnumsGenericNullable::nil_value()));

    CHECK(get_string_value(key + "#1", config) == "Undefined");
}

TEST_CASE("An enum vector index inside the vector reads as its label", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, def->enum_keys_map->at("Top Only")));

    CHECK(get_string_value(key + "#0", config).utf8_string() == "Top Only");
}

TEST_CASE("An enum value outside the label list reads as undefined", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, int(def->enum_labels.size())));

    CHECK(get_string_value(key + "#0", config) == "Undefined");
}

// Snapmaker Orca: the dialogs read the old value of a head column of a process preset with values
// per tool head from the saved preset laid out like the edited one.
TEST_CASE("Formatter shows a head column's old value from the saved preset laid out like the edited one", "[ConfigValueFormatter][PerHeadOverride]")
{
    const auto text_of = [](const wxString& value) { return std::string(value.utf8_str()); };

    DynamicPrintConfig saved;
    saved.option<ConfigOptionInts>("print_extruder_id", true)->values          = { 1, 1 };
    saved.option<ConfigOptionStrings>("print_extruder_variant", true)->values  = { "Direct Drive Standard", "Direct Drive High Flow" };
    saved.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = { 200., 500. };

    DynamicPrintConfig printer;
    printer.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::vector<double>(4, 0.4);
    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (int head = 1; head <= 4; ++head)
        for (const char *variant : { "Direct Drive Standard", "Direct Drive High Flow" }) {
            ids.emplace_back(head);
            variants.emplace_back(variant);
        }
    printer.option<ConfigOptionInts>("printer_extruder_id", true)->values         = ids;
    printer.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = variants;

    DynamicPrintConfig edited = saved;
    PerHeadProcess::widen(edited, printer);
    REQUIRE(edited.option<ConfigOptionInts>("print_extruder_id")->values.size() == 10);
    edited.option<ConfigOptionFloatsNullable>("outer_wall_speed")->values[6] = 90.;
    PerHeadProcess::set_head_value(edited, 2, "outer_wall_speed", 6);

    // The saved list has two columns: index 6 read directly is past its end.
    CHECK(text_of(get_string_value("outer_wall_speed#6", saved)) == "Undefined");
    DynamicPrintConfig        storage;
    const DynamicPrintConfig& reference = PerHeadProcess::reference_in_layout_of(edited, saved, storage);
    CHECK(text_of(get_string_value("outer_wall_speed#6", reference)) == "200");
    CHECK(text_of(get_string_value("outer_wall_speed#7", reference)) == "500");
    CHECK(text_of(get_string_value("outer_wall_speed#6", edited)) == "90");
}
