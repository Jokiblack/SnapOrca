# Sandwich — High Level Design

Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0).

## Purpose and scope

A Sandwich prints one surface of a layer (the top surface, the penultimate
solid layer below it, or the bottom surface) as a stack of thinner passes
instead of a single one. Each pass takes a share of the layer height and is one
of three kinds:

- **Solid**: the whole surface with one filament, optionally with its own
  angle, fan, speed (M220) and start / end G-code (the legacy MultiPass);
- **ColorStitch**: the fill lines of the surface shared out between several
  filaments (a tool sequence, a dithered gradient of two or three filaments, or
  bands counted in lines or measured in millimetres);
- **PathBlend**: a ramp of one filament that climbs across the surface, capped
  by a second filament so that ramp and cap add up to the layer height.

Everything is off by default. With every Sandwich key at its default and no
ColorStitch paint, no pass is built, no G-code changes and a 3MF without
ColorStitch data reads and writes as before.

## Where a stack comes from

`SurfacePassStack` (`ColorStitch.hpp`) is the recipe of one zone: up to three
`SurfacePass`es, bottom to top, plus the perimeter override and, for the bottom
zone, whether the passes above the first print as solid infill instead of
bridge. It has two sources.

- **Preset mode.** `SurfacePassStack::resolve()` reads the
  `neotko_surface_passes_top` / `_penu` blob of the region, and when that is
  empty builds a stack from the legacy keys (`multipass_*`,
  `interlayer_colormix_*`, `pathblend_*`). A `[None]` stack is an authored
  "no effect" and is kept apart from an unauthored one in the JSON.
- **Painter mode.** The ColorStitch Painter paints slot numbers onto the
  triangles of a volume (`ModelVolume::colorstitch_paint_facets`, one state per
  slot, up to 254). Each volume maps its slots to profile ids
  (`colorstitch_slot_to_profile_id`), and the profiles, each with a stack per
  zone, live in the project library `Model::surface_effect_profiles`
  (`SurfaceEffectProfileManager`). Paint of a slot that maps to no live profile
  is ignored.

The library belongs to the `Model`, not to the process: two projects never share
a palette, and `Print::apply()` copies it into the print's model with the
objects, so that the TBB fill threads only read a copy that cannot change under
them. A paint, table or library change invalidates the infill step of the
objects it touches (`model_colorstitch_paint_data_changed`).

## Building the passes

`PrintObject::infill()` presizes `m_multipass_sublayers` (one vector per layer)
and, for a painted object, builds a `ColorStitchPaintCache`: the painted
triangles per slot, in the slice frame, built once before the parallel loop.

In `make_fills()` each surface that a stack applies to is split into the
painted pieces and the rest (`sandwich_split_painted`). A piece with an effect
is filled once per pass at the pass's share of the layer height and stored as
`MultiPassSubLayer`s (Z, height, tool, kind, fills, G-code hooks), instead of
the ordinary fill of that surface; the rest of the layer is filled as usual.
Twin passes of the same island are merged at the end of the layer.

- The first layer is never treated, except by the bottom zone.
- A pass thinner than the printable minimum is folded into its neighbour
  (`m_minlayer_collapsed_slots`), and the object warns once with the slots.
- Layers split by Subdivide Mix Layer (Local-Z) are left alone, with a warning.
- ColorStitch tags each fill line with its tool in `mm3_per_mm`, and the tags
  are split into one collection per tool (`eec_to_tool_buckets`) before the
  passes are stored: a tagged path never reaches the G-code.

The distributions are positional: every fill line takes the colour of the place
it lands on, in millimetres from the object origin along the axis across the
lines (`FieldSampler`, `compute_field_groups`, `compute_slot_per_line_band_mm`).
Line width, spacing and holes that cut a line in two cannot shift the pattern,
and the two pieces of a cut line share one colour.

A ColorStitch pattern names filaments by digit, `1` being filament 1 (tool 0).
With `interlayer_colormix_use_virtual`, digits 5 to 9 name the live mixed
filaments in order, after the physical ones (virtual filament id
`num_physical + digit - 4`), and expand into their recipe.

## Emitting the passes

`GCode::extrude_sandwich_passes()` prints the passes of an object's layer, once
per instance, before the object's other extrusions of that layer, ordered by
Z. Each pass travels to its own Z (`real_extrude_z` for PathBlend, `print_z`
otherwise), injects its start / end G-code and speed override, and the nozzle
goes back to the layer's Z afterwards.

The passes print with the tool active when the object is visited: the pass
tools are not registered with the tool ordering, so a multi-filament recipe
does not change filament between passes yet, and the walls are not reprinted
by a perimeter override.

## Project files

The 3MF stores the paint as a `paint_colormix` triangle attribute, the slot
table of each volume as the `colormix_slot_to_profile_id` volume metadata (only
up to the last mapped slot) and the library as the base64
`colormix_profiles_b64` model metadata, written only when the library is not
empty. A project without ColorStitch data writes none of them, and older
readers ignore them.
