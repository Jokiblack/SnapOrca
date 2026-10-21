// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include "InstanceContact.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "NeoDebug.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"

#include <cstddef>
#include <utility>

namespace Slic3r {
namespace InstanceContact {

// Pairs of instances whose XY boxes are farther apart than this are ignored. Only a performance
// cut: a branch would have to lean more than 50 mm outside its own object to reach the other one.
static constexpr double XOBJ_PRUNE_MARGIN_MM = 50.0;

bool cross_object_active(const PrintObject &object)
{
    const Print *print = object.print();
    return object.config().support_cross_object_avoidance.value
        && object.config().enable_support.value
        && print != nullptr
        && print->config().print_sequence == PrintSequence::ByLayer;
}

std::vector<Polygons> neighbor_occupancy(const PrintObject &object)
{
    const Print *print = object.print();
    if (print == nullptr || object.layer_count() == 0 || !cross_object_active(object))
        return {};

    auto object_bbox = [](const PrintObject &po) {
        BoundingBox bb;
        for (const Layer *layer : po.layers())
            for (const BoundingBox &b : layer->lslices_bboxes)
                bb.merge(b);
        return bb;
    };

    const BoundingBox a_bbox = object_bbox(object);
    if (!a_bbox.defined)
        return {};
    const coord_t margin = scale_(XOBJ_PRUNE_MARGIN_MM);

    const size_t          num_layers = object.layer_count();
    std::vector<Polygons> occupancy(num_layers);
    bool                  any = false;

    for (const PrintObject *other : print->objects()) {
        if (other == &object || other->layer_count() == 0)
            continue;

        // The support the neighbour already has is an obstacle too. Print::process() generates the
        // support of the objects with PerObject Support one after the other, in plate order, after
        // the rest, so what is read here is deterministic. SupportLayer::lslices only holds the
        // brim outline, so gather the printed areas instead.
        std::vector<std::pair<const Layer*, Polygons>> b_support;
        if (other->is_step_done(posSupportMaterial))
            for (const SupportLayer *sl : other->support_layers()) {
                Polygons geo = to_polygons(sl->support_islands);
                append(geo, to_polygons(sl->base_areas));
                append(geo, to_polygons(sl->tree_roof_areas()));
                append(geo, to_polygons(sl->tree_roof_1st_layer()));
                append(geo, to_polygons(sl->tree_floor_areas()));
                if (!geo.empty())
                    b_support.emplace_back(sl, std::move(geo));
            }

        // XY offsets from the frame of B to the frame of A of every pair of instances in range.
        BoundingBox b_bbox = object_bbox(*other);
        for (const auto &sl : b_support)
            b_bbox.merge(get_extents(sl.second));
        if (!b_bbox.defined)
            continue;
        std::vector<Point> deltas;
        for (const PrintInstance &ai : object.instances())
            for (const PrintInstance &bi : other->instances()) {
                const Point delta = bi.shift - ai.shift;
                BoundingBox b_in_a = b_bbox;
                b_in_a.translate(delta.x(), delta.y());
                b_in_a.offset(margin);
                if (b_in_a.overlap(a_bbox))
                    deltas.push_back(delta);
            }
        NEOTKO_LOG(XOBJ, "neighbor_occupancy: A=" << object.model_object()->name << " B=" << other->model_object()->name
                         << " pairs=" << object.instances().size() * other->instances().size()
                         << " kept=" << deltas.size() << " b_support_layers=" << b_support.size());
        if (deltas.empty())
            continue;

        // Both stacks ascend, so one forward cursor over B is enough. Runs over the layers of B and
        // then over its support layers.
        auto accumulate_stack = [&](size_t stack_size, auto &&layer_at, auto &&geo_at) {
            size_t jb = 0;
            for (size_t ia = 0; ia < num_layers; ++ia) {
                const Layer *la       = object.get_layer(int(ia));
                const double a_bottom = la->bottom_z();
                const double a_top    = la->print_z;
                while (jb < stack_size && layer_at(jb)->print_z <= a_bottom + EPSILON)
                    ++jb;
                Polygons b_local;
                for (size_t j = jb; j < stack_size; ++j) {
                    if (layer_at(j)->bottom_z() >= a_top - EPSILON)
                        break;
                    append(b_local, geo_at(j));
                }
                if (b_local.empty())
                    continue;
                b_local = union_(b_local);
                for (const Point &delta : deltas) {
                    Polygons shifted = b_local;
                    for (Polygon &poly : shifted)
                        poly.translate(delta);
                    append(occupancy[ia], std::move(shifted));
                }
                any = true;
            }
        };
        accumulate_stack(other->layer_count(),
            [&](size_t j) { return other->get_layer(int(j)); },
            [&](size_t j) { return to_polygons(other->get_layer(int(j))->lslices); });
        if (!b_support.empty())
            accumulate_stack(b_support.size(),
                [&](size_t j) { return b_support[j].first; },
                [&](size_t j) -> const Polygons & { return b_support[j].second; });
    }

    if (!any)
        return {};
    size_t occupied_layers = 0;
    for (Polygons &polys : occupancy)
        if (!polys.empty()) {
            polys = union_(polys);
            ++occupied_layers;
        }
    NEOTKO_LOG(XOBJ, "neighbor_occupancy: A=" << object.model_object()->name << " occupied_layers=" << occupied_layers << "/" << num_layers);
    return occupancy;
}

} // namespace InstanceContact
} // namespace Slic3r
