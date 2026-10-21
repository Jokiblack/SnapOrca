// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

// Geometry shared between objects of a plate, for "PerObject Support" (support_cross_object_avoidance):
// the support of an object treats the other objects of the plate, and the support they already have,
// as obstacles.
//
// Frames: every object works in its own local frame, and an instance adds its PrintInstance::shift.
// A point of object B seen from object A is local_B + shift_B - shift_A; trafo_centered() alone is
// local to one object and never comparable across two.

#include <vector>

#include "Polygon.hpp"

namespace Slic3r {

class PrintObject;

namespace InstanceContact {

// True when the support of this object must avoid the other objects: support and
// support_cross_object_avoidance are on and the plate prints by layer (in by-object printing the
// neighbours are not on the bed yet). The single gate of every PerObject Support branch.
bool cross_object_active(const PrintObject &object);

// Footprint of the OTHER objects of the plate, per layer of `object` and in its local frame: their
// slices, plus their support when it is already generated. Empty when cross_object_active() is false
// or no other object is within range, so callers do nothing.
//  - Layers are matched by real Z range overlap, never by index: the two stacks may use different
//    (adaptive) layer heights.
//  - With several instances of `object` the result is the union of what every instance sees, since
//    the support is generated once per object and repeated per instance.
// Needs the slices of every object (posSlice done) and is read-only.
std::vector<Polygons> neighbor_occupancy(const PrintObject &object);

} // namespace InstanceContact
} // namespace Slic3r
