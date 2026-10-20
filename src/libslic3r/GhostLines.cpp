#include "GhostLines.hpp"

#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace Slic3r {

namespace {

// Outer surface of the layer: the contours of its islands, without the holes.
Polygons outer_contours(const ExPolygons &slices)
{
    Polygons out;
    out.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        out.emplace_back(ex.contour);
    return out;
}

// The part of the slices within band_depth of their outer surface.
ExPolygons outer_band(const ExPolygons &slices, const Polygons &contours, coord_t band_depth)
{
    return intersection_ex(slices, diff(contours, offset(contours, -float(band_depth))));
}

struct BackedBand
{
    // Part of the outer band with an internal feature behind it.
    ExPolygons area;
    // Share of the outer band with an internal feature behind it.
    double     fraction = 0.;
};

BackedBand backed_band(const ExPolygons &slices, coord_t band_depth, coord_t feature_depth)
{
    BackedBand     out;
    const Polygons contours = outer_contours(slices);
    const ExPolygons band   = outer_band(slices, contours, band_depth);
    if (band.empty())
        return out;
    // Material between feature_depth and feature_depth + band_depth from the outer surface: a plain wall thinner
    // than feature_depth has none.
    const ExPolygons deep = intersection_ex(slices, diff(offset(contours, -float(feature_depth)),
                                                         offset(contours, -float(feature_depth + band_depth))));
    if (deep.empty())
        return out;
    // Back to the outer surface it is closest to. Mitered, so that a floor reaches into the convex corners.
    out.area = intersection_ex(band, offset(deep, float(feature_depth + SCALED_EPSILON)));
    const double band_area = area(band);
    out.fraction           = band_area > 0. ? area(out.area) / band_area : 0.;
    return out;
}

// The layer about dz above (dz > 0) or below (dz < 0) layer, or nullptr past the top or the bottom of the object.
const Layer *layer_at_distance(const Layer &layer, double dz)
{
    const Layer *l = &layer;
    while (l != nullptr && std::abs(l->print_z - layer.print_z) < std::abs(dz) - EPSILON)
        l = dz > 0. ? l->upper_layer : l->lower_layer;
    return l;
}

// Backing of the layer's outer band, local features (less than this share) are isolated on any layer.
constexpr double LocalFeatureFraction = 0.5;
// A layer whose outer band is backed by this share more than one of the layers around it is a floor or ceiling.
constexpr double FloorFractionStep = 0.25;

} // namespace

ExPolygons ghost_lines_area(const Layer &layer, const PrintRegionConfig &config, coord_t band_depth)
{
    if (! config.ghost_lines_enable.value || layer.lower_layer == nullptr || layer.upper_layer == nullptr)
        return {};

    if (config.ghost_lines_scope.value == GhostLinesScope::Everywhere)
        return outer_band(layer.lslices, outer_contours(layer.lslices), band_depth);

    const double     feature_depth_mm = config.ghost_lines_feature_depth.value;
    const coord_t    feature_depth    = scaled<coord_t>(feature_depth_mm);
    BackedBand       here             = backed_band(layer.lslices, band_depth, feature_depth);
    if (here.area.empty() || here.fraction < LocalFeatureFraction)
        return std::move(here.area);
    for (const double dz : {-1., -0.5, 0.5, 1.})
        if (const Layer *other = layer_at_distance(layer, dz * feature_depth_mm);
            other != nullptr && backed_band(other->lslices, band_depth, feature_depth).fraction < here.fraction - FloorFractionStep)
            return std::move(here.area);
    return {};
}

bool ghost_lines_air_gap_on_layer(const Layer &layer, const PrintRegionConfig &config)
{
    const int bond_layers = config.ghost_lines_bond_layers.value;
    return config.ghost_lines_air_gap.value > 0. && (bond_layers <= 0 || layer.id() % size_t(bond_layers) != 0);
}

} // namespace Slic3r
