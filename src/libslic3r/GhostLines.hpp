#pragma once

#include "ExPolygon.hpp"
#include "libslic3r.h"

namespace Slic3r {

class Layer;
class PrintRegionConfig;

// SnapOrca: ghost lines (ghost_lines_* options).
//
// A floor, ceiling, rib or partition joined to the back of the outer wall shrinks as it cools and pulls the outer
// wall inwards, leaving a line on the outer surface along the joint. The treatments (an air gap behind the outer
// wall, as is done in CAD to hide these lines, and the others that hang from ghost_lines_enable) act on the layers
// where such an internal feature joins the outer wall. This module finds those layers; the perimeter generator and
// the G-code export apply the treatments.
//
// Returns the part of the layer within band_depth of the outer surface (the outer contours of Layer::lslices, holes
// are left alone) where an internal feature joins the outer wall, or nothing when the layer is not treated: when
// ghost_lines_enable is off, on the first and the topmost layer, and, for the InternalFeatures scope, where no
// internal feature joins the outer wall. Everywhere returns the whole band on every layer.
//
// InternalFeatures is detected from the slices alone. Material reaching deeper than ghost_lines_feature_depth from
// the outer surface is a feature behind the wall at the part of the outer surface it is closest to. When such
// features back less than half of the outer surface of the layer, they are local ones (ribs, partitions, bosses)
// and the layer is treated. When they back most of it, the layer is either a floor or ceiling, which is treated, or
// a solid part, which is not: they are told apart by comparing with the layers up to a feature depth above and
// below, whose outer surface is much less backed next to a floor or ceiling.
ExPolygons ghost_lines_area(const Layer &layer, const PrintRegionConfig &config, coord_t band_depth);

// Whether the air gap is left on this layer: ghost_lines_air_gap > 0 and not a bond layer (ghost_lines_bond_layers).
bool ghost_lines_air_gap_on_layer(const Layer &layer, const PrintRegionConfig &config);

} // namespace Slic3r
