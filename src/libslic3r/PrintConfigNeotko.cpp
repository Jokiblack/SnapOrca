// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
//
// Options of the NeotkoCM features ported to SnapOrca, registered up front so that the feature
// batches do not have to touch PrintConfig.*, Preset.cpp or the invalidation lists. Key names,
// types, limits and defaults are Neotko's, so NeotkoCM projects keep their values when opened here.
// Every default leaves the G-code unchanged: each feature only acts once its own option is set.
//
// All of them are comDevelop and none is placed in a tab yet. The batch that implements a feature
// puts its options in Tab.cpp and gives them the mode they had in Neotko, noted in each block.
// Typographic Spacing (A3), the Precision ALH core (A6) and Align & Stack (B3) have no options.
#include "PrintConfig.hpp"

#include "Config.hpp"
#include "I18N.hpp"

#include <set>
#include <string>

namespace Slic3r {

void PrintConfigDef::init_neotko_params()
{
    std::set<std::string> keys_before;
    for (const auto &[key, opt] : this->options)
        keys_before.insert(key);
    ConfigOptionDef *def;

    // ---- A1 Bridging infill extra expansion ----
    // Batch A1. PrintRegionConfig, read by LayerRegion::process_external_surfaces(); invalidates posPrepareInfill.
    // 0 = unchanged. comAdvanced, in the Quality tab, Bridging group (as in Neotko).

    def = this->add("bridge_expansion_extra", coFloat);
    def->label = L("Bridging infill extra expansion");
    def->category = L("Quality");
    def->tooltip = L("Extra distance the bridging infill grows into the surrounding solid area of the same part, on "
                     "top of the automatic expansion. The bridge then starts over solid material, so it is already "
                     "anchored when it begins to span. A large value lets the bridge take over the whole surrounding "
                     "region.\n"
                     "0 = automatic expansion only.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 999;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.));

    // ---- A2 Overhang Shadow ----
    // Batch A2. PrintRegionConfig, read only by GCode::_extrude() (estimate_extrusion_quality), so they only
    // invalidate psGCodeExport. overhang_shadow_inner_wall is the switch: while it is off the other two do nothing.
    // comAdvanced, as in Neotko; Speed tab, Overhang speed group.

    def = this->add("overhang_shadow_inner_wall", coBool);
    def->label = L("Slow down inner walls next to overhangs");
    def->category = L("Speed");
    def->tooltip = L("Give the inner wall a share of the slowdown its overhanging neighbour gets, so the bead that "
                     "holds up the overhanging outer wall is printed as carefully as the overhang itself. Requires "
                     "\"Slow down for overhangs\" and has no effect when the outer wall is printed first.");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("overhang_shadow_speed_ratio", coPercent);
    def->label = L("Inner wall slowdown");
    def->category = L("Speed");
    def->tooltip = L("How much of the neighbouring wall's overhang treatment the inner wall borrows. 100 grades the "
                     "inner wall as if it were the overhanging wall itself, overhang fan included; 0 disables the "
                     "effect.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 100;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionPercent(30));

    def = this->add("overhang_shadow_distance", coPercent);
    def->label = L("Inner wall reach");
    def->category = L("Speed");
    def->tooltip = L("How far outwards to look for the overhanging wall, as a percentage of the line width. 100 is "
                     "the wall right next to this one; larger values reach deeper into the walls.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 1000;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionPercent(200));

    // ---- A4 Expert G-code Reprocessor ----
    // Batch A4. PrintConfig, project option (s_project_options in PresetBundle.cpp). JSON list of the rules edited
    // from the preview panel, applied by BackgroundSlicingProcess::finalize_gcode() to a copy of the G-code, so it is
    // in steps_ignore of Print::invalidate_state_by_config_options(). Empty = unchanged. comDevelop in Neotko: never in a tab.

    def = this->add("expert_gcode_reprocessor_rules", coString);
    def->label = L("Expert G-code Reprocessor rules");
    def->tooltip = L("Internal JSON list of the Expert G-code Reprocessor rules. Edited from its panel in the "
                     "preview; not meant to be edited directly.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    // ---- A5 PerObject Support ----
    // Batch A5. PrintObjectConfig, read by TreeSupport, TreeModelVolumes and SupportMaterial; invalidates
    // posSupportMaterial. Neotko mode: comAdvanced (Support tab).

    def = this->add("support_cross_object_avoidance", coBool);
    def->label = L("PerObject Support");
    def->category = L("Support");
    def->tooltip = L("Support avoids the other objects on the plate, keeping the support/object XY distance from "
                     "them, instead of being generated through them. Useful when separate objects touch or overlap. "
                     "Only works when all objects are printed at once (by layer).");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionBool(false));

    // ---- B1 Height Adaptive Effects ----
    // Batch B1. PrintObjectConfig. Curves "z:value[:tension]|..." written only by the gizmo; empty = effect off.
    // The line width curves invalidate posPerimeters (the wall ones also posPrepareInfill), the fuzzy skin ones
    // posPerimeters and posSupportMaterial, the XY compensation ones posSlice. comDevelop in Neotko: never in a tab.

    def = this->add("neotko_hae_xy_contour", coString);
    def->label = L("Height adaptive XY contour compensation curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_xy_hole", coString);
    def->label = L("Height adaptive XY hole compensation curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_infill_width", coString);
    def->label = L("Height adaptive sparse infill line width curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_outer_wall_width", coString);
    def->label = L("Height adaptive outer wall line width curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_inner_wall_width", coString);
    def->label = L("Height adaptive inner wall line width curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_fuzzy_thickness", coString);
    def->label = L("Height adaptive fuzzy skin thickness curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_fuzzy_point_distance", coString);
    def->label = L("Height adaptive fuzzy skin point distance curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_fuzzy_scale", coString);
    def->label = L("Height adaptive fuzzy skin noise scale curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_fuzzy_octaves", coString);
    def->label = L("Height adaptive fuzzy skin noise octaves curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_hae_fuzzy_persistence", coString);
    def->label = L("Height adaptive fuzzy skin noise persistence curve");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    // ---- B2 Painter Pro Mode ----
    // Batch B2. PrintObjectConfig, read by generate_print_object_regions() (PrintApply.cpp), MultiMaterialSegmentation
    // and PrintObject (surface depth); they invalidate posSlice. 0 / empty list = unchanged. Neotko mode: comAdvanced
    // (Advanced category); the *_per_color ones are written by the painting gizmo.

    def = this->add("mmu_segmented_region_extra_walls", coInt);
    def->label = L("Extra walls on segmented regions");
    def->category = L("Advanced");
    def->tooltip = L("Adds this many walls to the painted (multi-material) regions only, on top of the object's wall "
                     "count. Keeps a small painted mark solid without thickening every wall of the object. 0 disables "
                     "it.");
    def->min = 0;
    def->max = 8;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("mmu_segmented_region_surface_depth", coInt);
    def->label = L("Surface depth of painted regions");
    def->category = L("Advanced");
    def->tooltip = L("Extends a painted top or bottom surface this many layers into the object as solid infill of the "
                     "painted color, following the painted outline. 0 disables it.");
    def->min = 0;
    def->max = 20;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("mmu_segmented_region_extra_walls_per_color", coInts);
    def->label = L("Extra walls per painted color");
    def->category = L("Advanced");
    def->tooltip = L("Per-color override of \"Extra walls on segmented regions\", indexed by paint color slot. 0 uses "
                     "the global value.");
    def->min = 0;
    def->max = 8;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts());

    def = this->add("mmu_segmented_region_surface_depth_per_color", coInts);
    def->label = L("Surface depth per painted color");
    def->category = L("Advanced");
    def->tooltip = L("Per-color override of \"Surface depth of painted regions\", indexed by paint color slot. 0 uses "
                     "the global value.");
    def->min = 0;
    def->max = 20;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts());

    // ---- B4 Support Zones ----
    // Batch B4. PrintRegionConfig: stored in the config of each zone's modifier volume and written only by the gizmo.
    // neotko_support_zone_gesture is descriptive (the engine never reads it) and invalidates nothing; the others
    // invalidate posSupportMaterial. The roof with its own filament uses the existing support_filament and
    // support_interface_filament keys. comDevelop in Neotko: never in a tab.

    def = this->add("neotko_support_zone_gesture", coString);
    def->label = L("Support zone gesture");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_zone_lean_deg", coFloat);
    def->label = L("Support zone lean angle");
    def->sidetext = u8"°";
    def->min = 0;
    def->max = 89;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("neotko_zone_roof_only", coBool);
    def->label = L("Support zone: only under unsupported surface");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("neotko_zone_solid", coBool);
    def->label = L("Support zone: print the whole block");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("neotko_zone_land_only", coBool);
    def->label = L("Support zone: only land at the end");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("neotko_support_cut", coBool);
    def->label = L("Support cut");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    // ---- S1 Penultimate Top Layers ----
    // Batch S1. PrintObjectConfig. With penultimate_top_layers = 0 no penultimate surface is classified and the other
    // three do nothing. Neotko mode: comSimple (Strength tab). As in Neotko, penultimate_solid_infill_pattern invalidates
    // posInfill and the others fall through to invalidate_all_steps().

    def = this->add("penultimate_top_layers", coInt);
    def->label = L("Penultimate top layers");
    def->category = L("Strength");
    def->tooltip = L("Number of solid infill layers right below the top surface layers that get their own pattern, "
                     "density and speed. 0 disables them.");
    def->min = 0;
    def->max = 20;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("penultimate_solid_infill_density", coPercent);
    def->label = L("Penultimate solid infill density");
    def->category = L("Strength");
    def->tooltip = L("Density of the penultimate solid infill layers, for a smoother transition between sparse infill "
                     "and the top surface.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionPercent(100));

    def = this->add("penultimate_solid_infill_pattern", coEnum);
    def->label = L("Penultimate solid infill pattern");
    def->category = L("Strength");
    def->tooltip = L("Line pattern of the penultimate solid infill layers. Keep Monotonic line when using PathBlend "
                     "or ColorStitch on them.");
    def->enum_keys_map = &ConfigOptionEnum<InfillPattern>::get_enum_values();
    def->enum_values = this->get("top_surface_pattern")->enum_values;
    def->enum_labels = this->get("top_surface_pattern")->enum_labels;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipMonotonicLine));

    def = this->add("penultimate_infill_speed", coFloat);
    def->label = L("Penultimate infill speed");
    def->category = L("Speed");
    def->tooltip = L("Speed of the penultimate solid infill layers. 0 uses the top surface speed.");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(50.));

    // ---- S2-S4 Sandwich: ColorStitch, MultiPass and PathBlend ----
    // Batches S2 (engine), S3 (multi-tool emission and tower) and S4 (GUI). PrintRegionConfig. Nothing happens unless
    // interlayer_colormix_enabled, multipass_enabled, penultimate_multipass_enabled or multipass_path_gradient is on, or a
    // painted profile asks for it. The *_tool_* keys are 0-based physical extruder indices (T0-T3). As in Neotko they have
    // no invalidation branch of their own and fall through to invalidate_all_steps(). Neotko mode: comAdvanced, except
    // colorstitch_monotonic_*, path_gradient_*, pathblend_* and neotko_surface_passes_*, which are comDevelop.

    def = this->add("multipass_prime_volume", coFloat);
    def->label = L("Sandwich purge");
    def->tooltip = L("Volume purged on the wipe tower before each tool change between Sandwich sublayers (MultiPass, "
                     "ColorStitch and PathBlend). 0 disables it. Only used by the Sandwich.");
    def->sidetext = L("mm³");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(10.));

    def = this->add("interlayer_colormix_enabled", coBool);
    def->label = L("Enable Surface ColorStitch");
    def->category = L("Quality");
    def->tooltip = L("Distributes the lines of the top and/or penultimate surfaces across several tools to create a "
                     "mixed-color effect.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("interlayer_colormix_surface", coInt);
    def->label = L("Surface effects");
    def->category = L("Quality");
    def->tooltip = L("0 = top and penultimate surfaces\n"
                     "1 = top surface only\n"
                     "2 = penultimate surfaces only");
    def->min = 0;
    def->max = 2;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_tool_a", coInt);
    def->label = L("First tool (A)");
    def->category = L("Quality");
    def->tooltip = L("Extruder index, starting at 0 (T0 to T3).");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_tool_b", coInt);
    def->label = L("Second tool (B)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("interlayer_colormix_tool_c", coInt);
    def->label = L("Third tool (C, -1 = off)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("interlayer_colormix_tool_d", coInt);
    def->label = L("Fourth tool (D, -1 = off)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("interlayer_colormix_min_length", coFloat);
    def->label = L("Minimum line length (ColorStitch)");
    def->category = L("Quality");
    def->tooltip = L("Lines shorter than this are printed with the region's own extruder instead of being split by "
                     "tool. 0 colors every line.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 50;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("interlayer_colormix_pattern_top", coString);
    def->label = L("Top surface tool sequence");
    def->category = L("Quality");
    def->tooltip = L("Tool sequence of the top surface lines, digits 1 to 4 for T0 to T3. \"12\" alternates T0 and T1; "
                     "\"1221\" prints T0, T1, T1, T0 and repeats.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString("12"));

    def = this->add("interlayer_colormix_pattern_penultimate", coString);
    def->label = L("Penultimate surface tool sequence");
    def->category = L("Quality");
    def->tooltip = L("Tool sequence of the penultimate surface lines, in the same format as the top surface one.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString("12"));

    def = this->add("interlayer_colormix_mode", coInt);
    def->label = L("Pattern mode");
    def->category = L("Quality");
    def->tooltip = L("How the tool of each line is chosen.\n"
                     "0 = tool sequence\n"
                     "1 = two-color dithered gradient\n"
                     "2 = "
                     "three-color dithered gradient\n"
                     "3 = bands counted in lines\n"
                     "4 = bands measured in millimeters");
    def->min = 0;
    def->max = 4;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_pct_a", coInt);
    def->label = L("Tool A share");
    def->category = L("Quality");
    def->tooltip = L("Share of the lines given to tool A in the gradient modes. Tool B, or tools B and C, get the "
                     "rest.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(50));

    def = this->add("interlayer_colormix_pct_b", coInt);
    def->label = L("Tool B share (three colors)");
    def->category = L("Quality");
    def->tooltip = L("Share of the lines given to tool B in the three-color gradient. Tool C gets what tools A and B "
                     "leave.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(33));

    def = this->add("interlayer_colormix_easing", coInt);
    def->label = L("Gradient easing");
    def->category = L("Quality");
    def->tooltip = L("Shape of the gradient across the surface.\n"
                     "0 = linear\n"
                     "1 = ease in\n"
                     "2 = ease out\n"
                     "3 = ease in "
                     "and out\n"
                     "4 = gamma\n"
                     "5 = hard bands");
    def->min = 0;
    def->max = 5;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_gamma", coFloat);
    def->label = L("Gradient gamma");
    def->category = L("Quality");
    def->tooltip = L("Gamma exponent of the gradient when the easing is Gamma. 1 is linear.");
    def->min = 0.1;
    def->max = 10;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("interlayer_colormix_min_surface_lines", coInt);
    def->label = L("Minimum surface lines");
    def->category = L("Quality");
    def->tooltip = L("Surfaces with fewer lines than this are printed with tool A only instead of a gradient. 0 never "
                     "falls back.");
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(3));

    def = this->add("interlayer_colormix_overlap", coFloat);
    def->label = L("Color overlap");
    def->category = L("Quality");
    def->tooltip = L("How much each color spreads into its neighbour's zone in the gradient modes. 0 gives hard "
                     "zones, 1 a strong overlap.");
    def->min = 0;
    def->max = 1;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.6));

    def = this->add("interlayer_colormix_invert", coBool);
    def->label = L("Invert gradient direction");
    def->category = L("Quality");
    def->tooltip = L("Reverses the generated gradient or band sequence. No effect with a tool sequence.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("interlayer_colormix_repetitions", coInt);
    def->label = L("Gradient repetitions");
    def->category = L("Quality");
    def->tooltip = L("How many times the gradient repeats across the surface.");
    def->min = 1;
    def->max = 50;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("interlayer_colormix_penu_mode", coInt);
    def->label = L("Pattern mode (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_pct_a", coInt);
    def->label = L("Tool A share (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(50));

    def = this->add("interlayer_colormix_penu_pct_b", coInt);
    def->label = L("Tool B share (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(33));

    def = this->add("interlayer_colormix_penu_easing", coInt);
    def->label = L("Gradient easing (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 5;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_gamma", coFloat);
    def->label = L("Gradient gamma (penultimate)");
    def->category = L("Quality");
    def->min = 0.1;
    def->max = 10;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("interlayer_colormix_penu_min_surface_lines", coInt);
    def->label = L("Minimum surface lines (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 100;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(3));

    def = this->add("interlayer_colormix_penu_overlap", coFloat);
    def->label = L("Color overlap (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 1;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.6));

    def = this->add("interlayer_colormix_penu_invert", coBool);
    def->label = L("Invert gradient direction (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("interlayer_colormix_penu_repetitions", coInt);
    def->label = L("Gradient repetitions (penultimate)");
    def->category = L("Quality");
    def->min = 1;
    def->max = 50;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("interlayer_colormix_penu_band_count_a", coInt);
    def->label = L("Band A lines (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("interlayer_colormix_penu_band_count_b", coInt);
    def->label = L("Band B lines (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("interlayer_colormix_penu_band_count_c", coInt);
    def->label = L("Band C lines (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_band_count_d", coInt);
    def->label = L("Band D lines (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_tool_a", coInt);
    def->label = L("First tool (A, penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_tool_b", coInt);
    def->label = L("Second tool (B, penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("interlayer_colormix_penu_tool_c", coInt);
    def->label = L("Third tool (C, penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(2));

    def = this->add("interlayer_colormix_penu_tool_d", coInt);
    def->label = L("Fourth tool (D, penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(3));

    def = this->add("interlayer_colormix_band_count_a", coInt);
    def->label = L("Band A lines");
    def->category = L("Quality");
    def->tooltip = L("Consecutive lines of each band in the bands mode. 0 skips that tool.");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("interlayer_colormix_band_count_b", coInt);
    def->label = L("Band B lines");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("interlayer_colormix_band_count_c", coInt);
    def->label = L("Band C lines");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_band_count_d", coInt);
    def->label = L("Band D lines");
    def->category = L("Quality");
    def->min = 0;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_band_mm_a", coFloat);
    def->label = L("Band A width");
    def->category = L("Quality");
    def->tooltip = L("Width of each band across the fill lines in the millimeter bands mode, rounded to whole lines. "
                     "0 skips that tool.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("interlayer_colormix_band_mm_b", coFloat);
    def->label = L("Band B width");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("interlayer_colormix_band_mm_c", coFloat);
    def->label = L("Band C width");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_band_mm_d", coFloat);
    def->label = L("Band D width");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_penu_band_mm_a", coFloat);
    def->label = L("Band A width (penultimate)");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("interlayer_colormix_penu_band_mm_b", coFloat);
    def->label = L("Band B width (penultimate)");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("interlayer_colormix_penu_band_mm_c", coFloat);
    def->label = L("Band C width (penultimate)");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_penu_band_mm_d", coFloat);
    def->label = L("Band D width (penultimate)");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_gradient_span_mm", coFloat);
    def->label = L("Gradient scale");
    def->category = L("Quality");
    def->tooltip = L("How the gradient is laid out.\n"
                     "-1 = by line index\n"
                     "0 = across the whole surface, by the real "
                     "position of each line\n"
                     "More than 0 = repeated every this many millimeters");
    def->sidetext = L("mm");
    def->min = -1;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_penu_gradient_span_mm", coFloat);
    def->label = L("Gradient scale (penultimate)");
    def->category = L("Quality");
    def->sidetext = L("mm");
    def->min = -1;
    def->max = 1000;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("interlayer_colormix_top_zone", coInt);
    def->label = L("Top surface zone");
    def->category = L("Quality");
    def->tooltip = L("Which top surfaces get the effect.\n"
                     "0 = all top surfaces\n"
                     "1 = only the topmost one");
    def->min = 0;
    def->max = 1;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_penu_zone", coInt);
    def->label = L("Penultimate surface zone");
    def->category = L("Quality");
    def->tooltip = L("Which penultimate surfaces get the effect.\n"
                     "0 = all penultimate surfaces\n"
                     "1 = only the one "
                     "right below the topmost top surface");
    def->min = 0;
    def->max = 1;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_filament_filter", coInt);
    def->label = L("Filament filter");
    def->category = L("Quality");
    def->tooltip = L("Only apply the effect to regions whose solid infill uses this filament, starting at 1. 0 "
                     "applies it to every filament.");
    def->min = 0;
    def->max = 16;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("interlayer_colormix_use_virtual", coBool);
    def->label = L("Use mixed filaments");
    def->category = L("Quality");
    def->tooltip = L("Lets digits 5 to 9 of the tool sequence refer to mixed (virtual) filaments.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("interlayer_colormix_angle", coInt);
    def->label = L("Infill angle override");
    def->category = L("Quality");
    def->tooltip = L("Fill line direction on the layers where ColorStitch is active. -1 follows the usual infill "
                     "rotation, which turns 90 degrees every other layer.");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(COLORSTITCH_DEFAULT_ANGLE_DEG));

    def = this->add("interlayer_colormix_penu_angle", coInt);
    def->label = L("Infill angle override (penultimate)");
    def->category = L("Quality");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("surface_color_mix_lane_mode", coInt);
    def->label = L("Line distribution mode");
    def->category = L("Quality");
    def->tooltip = L("How the slots of the pattern are mapped to the lines.\n"
                     "0 = by line index\n"
                     "1 = sorted by "
                     "position\n"
                     "2 = by lane\n"
                     "3 = by direction, then by lane");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("colorstitch_monotonic_replan", coInt);
    def->label = L("Monotonic line replan");
    def->category = L("Quality");
    def->tooltip = L("Test option: anchoring of the contour connector at acute corners of the Monotonic line pattern. "
                     "0 keeps the usual behavior.");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("colorstitch_monotonic_split", coBool);
    def->label = L("ColorStitch on continuous Monotonic");
    def->category = L("Quality");
    def->tooltip = L("Splits each continuous Monotonic path into runs of one color, so ColorStitch also works on that "
                     "pattern. Only affects surfaces with ColorStitch.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("multipass_enabled", coBool);
    def->label = L("Enable MultiPass blend");
    def->category = L("Quality");
    def->tooltip = L("Prints the top and/or penultimate surface again in several thinner passes with different tools, "
                     "blending their colors.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("multipass_surface", coInt);
    def->label = L("Apply MultiPass to");
    def->category = L("Quality");
    def->tooltip = L("0 = top and penultimate surfaces\n"
                     "1 = top surface only\n"
                     "2 = penultimate surfaces only");
    def->min = 0;
    def->max = 2;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("multipass_num_passes", coInt);
    def->label = L("Number of MultiPass passes");
    def->category = L("Quality");
    def->tooltip = L("1 to 3 passes. A single pass with a reduced ratio prints one light surface layer.");
    def->min = 1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(2));

    def = this->add("multipass_tool_1", coInt);
    def->label = L("Pass 1 tool");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("multipass_tool_2", coInt);
    def->label = L("Pass 2 tool");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("multipass_tool_3", coInt);
    def->label = L("Pass 3 tool");
    def->category = L("Quality");
    def->tooltip = L("Extruder index, starting at 0. -1 turns the pass off.");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_width_ratio_1", coFloat);
    def->label = L("Pass 1 layer ratio");
    def->category = L("Quality");
    def->tooltip = L("Share of the layer height printed by this pass. The ratios of the active passes should add up "
                     "to about 1.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("multipass_width_ratio_2", coFloat);
    def->label = L("Pass 2 layer ratio");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("multipass_width_ratio_3", coFloat);
    def->label = L("Pass 3 layer ratio");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.34));

    def = this->add("multipass_vary_pattern", coBool);
    def->label = L("Alternate pass direction");
    def->category = L("Quality");
    def->tooltip = L("Each pass runs in the opposite direction to the previous one.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("multipass_angle_1", coInt);
    def->label = L("Pass 1 fill angle");
    def->category = L("Quality");
    def->tooltip = L("Fill angle of the pass. -1 follows the top surface fill angle.");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_angle_2", coInt);
    def->label = L("Pass 2 fill angle");
    def->category = L("Quality");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_angle_3", coInt);
    def->label = L("Pass 3 fill angle");
    def->category = L("Quality");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_pa_mode", coInt);
    def->label = L("Pressure advance during MultiPass");
    def->category = L("Quality");
    def->tooltip = L("0 = unchanged\n"
                     "1 = use the value below\n"
                     "2 = set it to 0");
    def->min = 0;
    def->max = 2;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("multipass_pa_value", coFloat);
    def->label = L("Pressure advance value during MultiPass");
    def->category = L("Quality");
    def->min = 0;
    def->max = 2;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.1));

    def = this->add("multipass_fan_1", coInt);
    def->label = L("Pass 1 fan speed");
    def->category = L("Quality");
    def->tooltip = L("Fan PWM value (0 to 255) during the pass. -1 leaves it unchanged.");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_fan_2", coInt);
    def->label = L("Pass 2 fan speed");
    def->category = L("Quality");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_fan_3", coInt);
    def->label = L("Pass 3 fan speed");
    def->category = L("Quality");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("multipass_speed_pct_1", coInt);
    def->label = L("Pass 1 speed");
    def->category = L("Quality");
    def->tooltip = L("Speed factor applied with M220 during the pass. 100 leaves it unchanged.");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("multipass_speed_pct_2", coInt);
    def->label = L("Pass 2 speed");
    def->category = L("Quality");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("multipass_speed_pct_3", coInt);
    def->label = L("Pass 3 speed");
    def->category = L("Quality");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("multipass_gcode_start_1", coString);
    def->label = L("Pass 1 start G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_gcode_start_2", coString);
    def->label = L("Pass 2 start G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_gcode_start_3", coString);
    def->label = L("Pass 3 start G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_gcode_end_1", coString);
    def->label = L("Pass 1 end G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_gcode_end_2", coString);
    def->label = L("Pass 2 end G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_gcode_end_3", coString);
    def->label = L("Pass 3 end G-code");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_perimeter_override", coBool);
    def->label = L("Perimeter override");
    def->category = L("Quality");
    def->tooltip = L("Prints the perimeters once per MultiPass sublayer with that sublayer's tool instead of once at "
                     "the layer height, so the walls blend like the surface.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("penultimate_multipass_enabled", coBool);
    def->label = L("Enable penultimate MultiPass");
    def->category = L("Quality");
    def->tooltip = L("MultiPass for the penultimate surfaces with its own settings, independent of the top surface "
                     "ones.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("penultimate_multipass_num_passes", coInt);
    def->label = L("Number of penultimate passes");
    def->category = L("Quality");
    def->min = 1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(2));

    def = this->add("penultimate_multipass_tool_1", coInt);
    def->label = L("Pass 1 tool (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("penultimate_multipass_tool_2", coInt);
    def->label = L("Pass 2 tool (penultimate)");
    def->category = L("Quality");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("penultimate_multipass_tool_3", coInt);
    def->label = L("Pass 3 tool (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Extruder index, starting at 0. -1 turns the pass off.");
    def->min = -1;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_width_ratio_1", coFloat);
    def->label = L("Pass 1 layer ratio (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Share of the layer height printed by this pass. The ratios of the active passes should add up "
                     "to about 1.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("penultimate_multipass_width_ratio_2", coFloat);
    def->label = L("Pass 2 layer ratio (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("penultimate_multipass_width_ratio_3", coFloat);
    def->label = L("Pass 3 layer ratio (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.34));

    def = this->add("penultimate_multipass_vary_pattern", coBool);
    def->label = L("Alternate pass direction (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Each pass runs in the opposite direction to the previous one.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("penultimate_multipass_angle_1", coInt);
    def->label = L("Pass 1 fill angle (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Fill angle of the pass. -1 follows the top surface fill angle.");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_angle_2", coInt);
    def->label = L("Pass 2 fill angle (penultimate)");
    def->category = L("Quality");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_angle_3", coInt);
    def->label = L("Pass 3 fill angle (penultimate)");
    def->category = L("Quality");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_fan_1", coInt);
    def->label = L("Pass 1 fan speed (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Fan PWM value (0 to 255) during the pass. -1 leaves it unchanged.");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_fan_2", coInt);
    def->label = L("Pass 2 fan speed (penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_fan_3", coInt);
    def->label = L("Pass 3 fan speed (penultimate)");
    def->category = L("Quality");
    def->min = -1;
    def->max = 255;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(-1));

    def = this->add("penultimate_multipass_speed_pct_1", coInt);
    def->label = L("Pass 1 speed (penultimate)");
    def->category = L("Quality");
    def->tooltip = L("Speed factor applied with M220 during the pass. 100 leaves it unchanged.");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("penultimate_multipass_speed_pct_2", coInt);
    def->label = L("Pass 2 speed (penultimate)");
    def->category = L("Quality");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("penultimate_multipass_speed_pct_3", coInt);
    def->label = L("Pass 3 speed (penultimate)");
    def->category = L("Quality");
    def->sidetext = "%";
    def->min = 1;
    def->max = 200;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("penultimate_multipass_gcode_start_1", coString);
    def->label = L("Pass 1 start G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("penultimate_multipass_gcode_start_2", coString);
    def->label = L("Pass 2 start G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("penultimate_multipass_gcode_start_3", coString);
    def->label = L("Pass 3 start G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("penultimate_multipass_gcode_end_1", coString);
    def->label = L("Pass 1 end G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("penultimate_multipass_gcode_end_2", coString);
    def->label = L("Pass 2 end G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("penultimate_multipass_gcode_end_3", coString);
    def->label = L("Pass 3 end G-code (penultimate)");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("multipass_path_gradient", coBool);
    def->label = L("Path gradient blend");
    def->category = L("Quality");
    def->tooltip = L("With MultiPass, varies height and flow along each top surface path so the two passes blend in a "
                     "gradient.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("path_gradient_segments", coInt);
    def->label = L("Path gradient segments");
    def->category = L("Quality");
    def->tooltip = L("Reserved, not used.");
    def->min = 4;
    def->max = 256;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(32));

    def = this->add("path_gradient_min_flow_pct", coInt);
    def->label = L("Path gradient minimum flow");
    def->category = L("Quality");
    def->tooltip = L("Minimum flow at the low end of a PathBlend pass, so that it never extrudes nothing.");
    def->sidetext = "%";
    def->min = 0;
    def->max = 30;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(5));

    def = this->add("pathblend_num_passes", coInt);
    def->label = L("PathBlend passes");
    def->category = L("Quality");
    def->tooltip = L("Number of gradient passes, one per tool.");
    def->min = 1;
    def->max = 4;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(2));

    def = this->add("pathblend_tool_1", coInt);
    def->label = L("PathBlend tool 1");
    def->category = L("Quality");
    def->min = 0;
    def->max = 15;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("pathblend_tool_2", coInt);
    def->label = L("PathBlend tool 2");
    def->category = L("Quality");
    def->min = 0;
    def->max = 15;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("pathblend_tool_3", coInt);
    def->label = L("PathBlend tool 3");
    def->category = L("Quality");
    def->min = 0;
    def->max = 15;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(2));

    def = this->add("pathblend_tool_4", coInt);
    def->label = L("PathBlend tool 4");
    def->category = L("Quality");
    def->min = 0;
    def->max = 15;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(3));

    def = this->add("pathblend_layer_ratio_1", coFloat);
    def->label = L("PathBlend layer ratio 1");
    def->category = L("Quality");
    def->tooltip = L("Layer height ratio of the pass at its active end.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pathblend_layer_ratio_2", coFloat);
    def->label = L("PathBlend layer ratio 2");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pathblend_layer_ratio_3", coFloat);
    def->label = L("PathBlend layer ratio 3");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pathblend_layer_ratio_4", coFloat);
    def->label = L("PathBlend layer ratio 4");
    def->category = L("Quality");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pathblend_min_ratio", coFloat);
    def->label = L("PathBlend minimum ratio");
    def->category = L("Quality");
    def->tooltip = L("Minimum extrusion ratio of the leading pass, so that it never extrudes nothing.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(0.05));

    def = this->add("pathblend_max_ratio", coFloat);
    def->label = L("PathBlend maximum ratio");
    def->category = L("Quality");
    def->tooltip = L("Maximum extrusion ratio of the dominant pass at its peak; the other pass fills the rest.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pathblend_ease_mode", coInt);
    def->label = L("PathBlend gradient curve");
    def->category = L("Quality");
    def->tooltip = L("0 = linear\n"
                     "1 = ease in\n"
                     "2 = ease out\n"
                     "3 = ease in and out");
    def->min = 0;
    def->max = 3;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("pathblend_surface", coInt);
    def->label = L("PathBlend surface");
    def->category = L("Quality");
    def->tooltip = L("0 = top and penultimate surfaces\n"
                     "1 = top surface only\n"
                     "2 = penultimate surfaces only");
    def->min = 0;
    def->max = 2;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("pathblend_invert_gradient", coBool);
    def->label = L("Reverse PathBlend colors");
    def->category = L("Quality");
    def->tooltip = L("Swaps which tool dominates each side of the gradient. The height always rises in the same "
                     "direction.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("pathblend_fill_angle", coInt);
    def->label = L("PathBlend fill angle");
    def->category = L("Quality");
    def->tooltip = L("Fill line direction on PathBlend surfaces. It shapes the gradient itself, not only the finish. "
                     "-1 follows the usual infill rotation.");
    def->sidetext = u8"°";
    def->min = -1;
    def->max = 359;
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInt(COLORSTITCH_DEFAULT_ANGLE_DEG));

    def = this->add("pathblend_top", coString);
    def->label = L("PathBlend settings (top)");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("pathblend_penu", coString);
    def->label = L("PathBlend settings (penultimate)");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_surface_passes_top", coString);
    def->label = L("Sandwich pass stack (top)");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("neotko_surface_passes_penu", coString);
    def->label = L("Sandwich pass stack (penultimate)");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionString(""));

    m_neotko_option_keys.clear();
    for (const auto &[key, opt] : this->options)
        if (keys_before.count(key) == 0)
            m_neotko_option_keys.push_back(key);
}

} // namespace Slic3r
