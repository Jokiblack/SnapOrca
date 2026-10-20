# Ghost lines — High Level Design

## Purpose and scope

A floor, ceiling, rib or partition joined to the back of the outer wall shrinks
as it cools and pulls the outer wall inwards, leaving a line on the outer
surface along the joint: a ghost line. The usual CAD remedy is an air gap of a
few tenths of a millimetre between the outer wall and the internal feature, so
the outer wall prints decoupled from it. The `ghost_lines_*` options
(process settings, Quality page, group "Ghost lines") do this and a few other
things in the slicer, on the layers where such a feature joins the outer wall,
without changing the model. Everything hangs from `ghost_lines_enable`, off by
default; with it off no G-code changes.

## Finding the treated layers

`ghost_lines_area()` (`GhostLines.cpp`) works from the layer slices alone,
during perimeter generation, and returns the part of the band behind the outer
surface (the outer contours of `Layer::lslices`; holes are never treated) where
an internal feature joins the outer wall. Nothing is returned for the first and
the topmost layer.

With `ghost_lines_scope` = *Internal features*, material deeper than
`ghost_lines_feature_depth` from the outer surface is a feature behind the wall
at the part of the outer surface it is closest to (the deep material is grown by
the feature depth and intersected with the band). A plain wall thinner than the
feature depth has no such material, so the depth is set a little above the
model's wall thickness. Two cases follow from how much of the band is backed:

- less than half: local features (ribs, partitions, bosses). Treated, along the
  part of the band they back;
- most of it: either a floor or ceiling, or a solid part of the model. The layers
  up to a feature depth above and below are compared: next to a floor or
  ceiling at least one of them is much less backed, and the layer is treated
  along the backed band. A solid part is backed the same all along and is left
  alone.

With *Everywhere* the whole band of every layer is treated; the air gap then
needs the bond interval to keep the outer wall attached.

The result is recorded on the layer (`Layer::ghost_lines`,
`Layer::ghost_lines_area`, the union over the regions) for the treatments that
act at G-code export, and handed to `PerimeterGenerator` for those that act on
the walls.

## Treatments

All act only on the treated layers, and each is off by default:

- **Air gap** (`ghost_lines_air_gap`): the classic perimeter generator takes the
  band (less the gap) out of the region the inner walls and infill are built
  from after the outer wall is placed, so their edge ends `gap` short of the
  outer wall's inner edge. The cut is clipped to the slices of the layers above
  and below, so it never opens a hole in a top or bottom surface, and is skipped
  in spiral vase mode. Arachne builds the outer and inner walls together from
  one region and is not supported. `ghost_lines_bond_layers` prints every Nth
  layer without the gap, and `ghost_lines_gap_taper_layers` opens the gap
  gradually over N layers before the first and after the last treated layer
  (those layers take the nearest treated layer's area, at a fraction of the
  gap).
- **Extra wall** (`ghost_lines_extra_wall`): one more wall loop, both
  generators.
- **Outer wall first** (`ghost_lines_outer_wall_first`): the layer's walls are
  ordered outer-to-inner as if `wall_sequence` were outer/inner, both
  generators; the other layers keep the profile's order.
- **Outer wall flow and speed** (`ghost_lines_outer_wall_flow`,
  `ghost_lines_outer_wall_speed`): `GCode::_extrude()` scales the outer wall's
  flow and speed by these percentages (0 leaves them unchanged).
- **Minimum fan** (`ghost_lines_fan`): `GCode::process_layer()` tells the
  `CoolingBuffer` the floor for the layer, which it applies after its own
  layer-time logic and the first-layer ramp, never on the layers without
  cooling.
- **Concentric solid infill** (`ghost_lines_concentric_solid_infill`):
  `group_fills()` splits the non-bridge solid surfaces at the band behind the
  joined outer wall, as deep as the feature depth, and fills that band with
  concentric lines (`ipConcentric` for top/bottom surfaces, `ipConcentricInternal`
  otherwise), so it shrinks along the wall rather than towards it.

## Constraints

The detection is purely geometric and per layer, so a feature whose depth is
below `ghost_lines_feature_depth` is not seen, and a model whose walls are
thicker than it is treated as all feature. The treated layers of a floor are the
floor's own layers; the layers of plain wall right above and below, where the
wall is being pulled, are reached only through the taper.
