#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// Periodic modifier (Bambu Studio 87536f3f0): a modifier volume is applied for
// `periodic_modifier_apply_layers` layers, then ignored for `periodic_modifier_skip_layers`
// layers, starting at layer 0. On a skipped layer the parent region is sliced as if the
// modifier were not there.

namespace {

size_t count_perimeter_items(const ExtrusionEntityCollection &coll)
{
    size_t n = 0;
    for (const ExtrusionEntity *entity : coll.flatten().entities)
        if (is_perimeter(entity->role()))
            ++n;
    return n;
}

size_t layer_perimeter_count(const Layer &layer)
{
    size_t n = 0;
    for (const LayerRegion *region : layer.regions())
        n += count_perimeter_items(region->perimeters);
    return n;
}

bool layer_has_internal_solid(const Layer &layer)
{
    for (const LayerRegion *region : layer.regions())
        for (const Surface &surface : region->fill_surfaces.surfaces)
            if (surface.surface_type == stInternalSolid)
                return true;
    return false;
}

// Fill surfaces on the layer that belong to a region with the given sparse infill density, in
// percent. With `only_sparse` only the sparse (stInternal) ones are counted.
size_t layer_fill_surfaces_with_density(const Layer &layer, double density, bool only_sparse)
{
    size_t n = 0;
    for (const LayerRegion *region : layer.regions()) {
        if (std::abs(region->region().config().sparse_infill_density.value - density) > 1e-6)
            continue;
        for (const Surface &surface : region->fill_surfaces.surfaces)
            if (! only_sparse || surface.surface_type == stInternal)
                ++n;
    }
    return n;
}

void add_periodic_wall_modifier(ModelObject *object, int skip_layers, int apply_layers)
{
    ModelVolume *mod = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    mod->config.set("wall_loops", 2);
    mod->config.set("periodic_modifier", true);
    mod->config.set("periodic_modifier_skip_layers", skip_layers);
    mod->config.set("periodic_modifier_apply_layers", apply_layers);
}

// The full default print config with `items` on top.
DynamicPrintConfig print_config(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(items);
    return config;
}

// A 20 mm cube with the modifiers `add_modifiers` adds, sliced with `config`. test_helpers'
// init_print() applies the model before any modifier can be added, so the object is assembled here.
void process_cube_with_modifiers(Print &print, const std::function<void(ModelObject *)> &add_modifiers,
                                 const DynamicPrintConfig &config)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 20.));
    add_modifiers(object);
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

// Print settings for the wall tests: the object itself has no walls and no infill, so every
// perimeter on a layer comes from a modifier. 20 mm at 0.2 mm layers gives 100 layers.
// Returned as a config, not as an initializer_list: the array behind a returned
// initializer_list dies with the function, and reading it afterwards is undefined behavior.
DynamicPrintConfig wall_test_config(const char *wall_generator, const char *ensure_vertical)
{
    return print_config({
        { "wall_loops", 0 },
        { "sparse_infill_density", 0 },
        { "top_shell_layers", 0 },
        { "bottom_shell_layers", 0 },
        { "detect_overhang_wall", 0 },
        { "only_one_wall_first_layer", 0 },
        { "only_one_wall_top", 0 },
        { "ensure_vertical_shell_thickness", ensure_vertical },
        { "layer_height", 0.2f },
        { "initial_layer_print_height", 0.2f },
        { "wall_generator", wall_generator }
    });
}

} // namespace

TEST_CASE("periodic_modifier_active follows the apply/skip cycle from layer 0", "[PeriodicModifier]")
{
    PrintRegionConfig cfg;
    cfg.periodic_modifier.value              = true;
    cfg.periodic_modifier_skip_layers.value  = 1;
    cfg.periodic_modifier_apply_layers.value = 1;

    REQUIRE(periodic_modifier_active(cfg, 0));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 1));
    REQUIRE(periodic_modifier_active(cfg, 2));

    cfg.periodic_modifier_skip_layers.value = 2;
    REQUIRE(periodic_modifier_active(cfg, 0));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 1));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 2));
    REQUIRE(periodic_modifier_active(cfg, 3));

    cfg.periodic_modifier_skip_layers.value  = 1;
    cfg.periodic_modifier_apply_layers.value = 2;
    REQUIRE(periodic_modifier_active(cfg, 0));
    REQUIRE(periodic_modifier_active(cfg, 1));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 2));
    REQUIRE(periodic_modifier_active(cfg, 3));

    // A 5 mm period with 0.4 mm applied, at 0.2 mm layers: 2 layers on, 23 off.
    cfg.periodic_modifier_apply_layers.value = 2;
    cfg.periodic_modifier_skip_layers.value  = 23;
    REQUIRE(periodic_modifier_active(cfg, 0));
    REQUIRE(periodic_modifier_active(cfg, 1));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 2));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 24));
    REQUIRE(periodic_modifier_active(cfg, 25));
    REQUIRE(periodic_modifier_active(cfg, 26));
    REQUIRE_FALSE(periodic_modifier_active(cfg, 27));

    // Skip 0 applies on every layer.
    cfg.periodic_modifier_skip_layers.value = 0;
    REQUIRE(periodic_modifier_active(cfg, 7));

    // Disabled: always active, whatever the counters say.
    cfg.periodic_modifier.value             = false;
    cfg.periodic_modifier_skip_layers.value = 5;
    REQUIRE(periodic_modifier_active(cfg, 1));
    REQUIRE(periodic_modifier_active(cfg, 3));
}

TEST_CASE("Periodic modifier options are off by default", "[PeriodicModifier]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE_FALSE(config.opt_bool("periodic_modifier"));
    REQUIRE(config.opt_int("periodic_modifier_skip_layers") == 1);
    REQUIRE(config.opt_int("periodic_modifier_apply_layers") == 1);
    REQUIRE_FALSE(config.opt_bool("modifier_ignore_infill"));
}

TEST_CASE("A periodic wall modifier adds walls only on its apply layers", "[PeriodicModifier]")
{
    // (skip, apply, wall generator)
    const auto [skip_layers, apply_layers, wall_generator] = GENERATE(table<int, int, const char *>({
        { 1, 1, "classic" },
        { 1, 1, "arachne" },
        { 2, 1, "classic" },
        { 3, 2, "classic" },
    }));
    INFO("skip " << skip_layers << " apply " << apply_layers << " " << wall_generator);

    Print print;
    process_cube_with_modifiers(
        print, [&](ModelObject *object) { add_periodic_wall_modifier(object, skip_layers, apply_layers); },
        wall_test_config(wall_generator, "ensure_all"));
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == 100);

    const int period      = apply_layers + skip_layers;
    size_t    apply_count = 0;
    size_t    skip_count  = 0;
    for (const Layer *layer : object.layers()) {
        const int    layer_id = static_cast<int>(layer->id());
        const size_t walls    = layer_perimeter_count(*layer);
        INFO("layer " << layer_id << " walls " << walls);
        if ((layer_id % period) < apply_layers) {
            ++apply_count;
            REQUIRE(walls > 0);
        } else {
            ++skip_count;
            REQUIRE(walls == 0);
            // The missing-wall band of a skipped layer is not turned into solid shell.
            REQUIRE_FALSE(layer_has_internal_solid(*layer));
        }
    }
    REQUIRE(apply_count == size_t((100 / period) * apply_layers + std::min(100 % period, apply_layers)));
    REQUIRE(skip_count == 100 - apply_count);
}

TEST_CASE("A modifier without the periodic flag applies on every layer", "[PeriodicModifier]")
{
    Print print;
    process_cube_with_modifiers(
        print,
        [](ModelObject *object) {
            ModelVolume *mod = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
            mod->config.set("wall_loops", 2);
            // The counters alone mean nothing while the flag is off.
            mod->config.set("periodic_modifier_skip_layers", 3);
        },
        wall_test_config("classic", "ensure_all"));
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == 100);
    for (const Layer *layer : object.layers()) {
        INFO("layer " << layer->id());
        REQUIRE(layer_perimeter_count(*layer) > 0);
    }
}

TEST_CASE("A later non-periodic modifier still applies on the skipped layers of a periodic one", "[PeriodicModifier]")
{
    Print print;
    process_cube_with_modifiers(
        print,
        [](ModelObject *object) {
            add_periodic_wall_modifier(object, 1, 1);
            ModelVolume *inner = object->add_volume(make_cube(10., 10., 20.), ModelVolumeType::PARAMETER_MODIFIER);
            inner->config.set("wall_loops", 1);
        },
        wall_test_config("classic", "none"));
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == 100);
    size_t skip_layers_with_walls = 0;
    for (const Layer *layer : object.layers())
        if ((static_cast<int>(layer->id()) % 2) == 1 && layer_perimeter_count(*layer) > 0)
            ++skip_layers_with_walls;
    REQUIRE(skip_layers_with_walls > 0);
}

TEST_CASE("A periodic infill modifier changes the infill only on its apply layers", "[PeriodicModifier]")
{
    // 20 mm cube at 15 % grid; a 100 % infill modifier applied 2 layers out of every 10
    // (2 mm period, 0.4 mm dense, at 0.2 mm layers). A 100 % region fills with solid infill
    // (stInternalSolid, or an internal bridge right above sparse infill), never with stInternal,
    // so on the apply layers the dense region is checked for any fill surface.
    Print print;
    process_cube_with_modifiers(
        print,
        [](ModelObject *object) {
            ModelVolume *mod = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
            mod->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
            mod->config.set("periodic_modifier", true);
            mod->config.set("periodic_modifier_apply_layers", 2);
            mod->config.set("periodic_modifier_skip_layers", 8);
        },
        print_config({
            { "wall_loops", 2 },
            { "sparse_infill_density", 15 },
            { "sparse_infill_pattern", "grid" },
            { "top_shell_layers", 0 },
            { "bottom_shell_layers", 0 },
            { "ensure_vertical_shell_thickness", "none" },
            { "layer_height", 0.2f },
            { "initial_layer_print_height", 0.2f },
            { "wall_generator", "classic" }
        }));
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == 100);
    size_t dense_layers = 0;
    for (const Layer *layer : object.layers()) {
        const int    layer_id = static_cast<int>(layer->id());
        const size_t dense    = layer_fill_surfaces_with_density(*layer, 100., false);
        const size_t sparse   = layer_fill_surfaces_with_density(*layer, 15., false);
        INFO("layer " << layer_id << " dense " << dense << " sparse " << sparse);
        if (layer_id % 10 < 2) {
            ++dense_layers;
            REQUIRE(dense > 0);
            REQUIRE(sparse == 0);
        } else {
            REQUIRE(dense == 0);
            REQUIRE(layer_fill_surfaces_with_density(*layer, 15., true) > 0);
        }
    }
    REQUIRE(dense_layers == 20);
}

TEST_CASE("modifier_ignore_infill keeps the parent sparse infill under the modifier walls", "[PeriodicModifier]")
{
    Print print;
    process_cube_with_modifiers(
        print,
        [](ModelObject *object) {
            ModelVolume *mod = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
            mod->config.set("wall_loops", 2);
            mod->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
            mod->config.set("modifier_ignore_infill", true);
        },
        print_config({
            { "wall_loops", 0 },
            { "sparse_infill_density", 20 },
            { "sparse_infill_pattern", "zigzag" },
            { "top_shell_layers", 0 },
            { "bottom_shell_layers", 0 },
            { "detect_overhang_wall", 0 },
            { "only_one_wall_first_layer", 0 },
            { "only_one_wall_top", 0 },
            { "ensure_vertical_shell_thickness", "none" },
            { "layer_height", 0.2f },
            { "initial_layer_print_height", 0.2f },
            { "wall_generator", "classic" }
        }));
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == 100);
    size_t layers_with_walls  = 0;
    size_t layers_with_infill = 0;
    for (const Layer *layer : object.layers()) {
        if (layer_perimeter_count(*layer) > 0)
            ++layers_with_walls;
        for (const LayerRegion *region : layer->regions()) {
            if (!region->fills.entities.empty())
                ++layers_with_infill;
            REQUIRE(region->region().config().sparse_infill_density.value > 1.);
        }
    }
    REQUIRE(layers_with_walls > 0);
    REQUIRE(layers_with_infill > 0);
}
