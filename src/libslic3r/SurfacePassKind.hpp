// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

namespace Slic3r {

// Effect kind of one pass of a Sandwich stack (SurfacePassStack) and of the sublayer it produces
// (MultiPassSubLayer). Kept apart from ColorStitch.hpp so that Print.hpp and the G-code side need
// only this enum. The values are stored in project files: never renumber them.
enum class SurfacePassKind : int {
    None        = 0, // passthrough: the natural surface, no effect
    Solid       = 1, // a flat pass with its own tool (the classic MultiPass pass)
    ColorStitch = 2, // filament chosen per fill line
    PathBlend   = 3, // Z and flow gradient across the surface
    ZBump       = 4, // reserved by Neotko for the ZBump reinforcement passes (not ported)
};

} // namespace Slic3r
