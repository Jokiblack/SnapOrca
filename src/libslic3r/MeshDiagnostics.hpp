#pragma once

#include <admesh/stl.h>

#include <cstddef>

namespace Slic3r {

// Topology of a triangle set, counted over undirected edges.
struct MeshDiagnosticStats {
    size_t non_manifold_edges   = 0; // shared by more than two faces
    size_t open_edges           = 0; // referenced by exactly one face
    size_t same_direction_edges = 0; // with at least two half-edges of the same winding
    bool   has_reversed_faces   = false;
};

// Counts open, non-manifold and same-direction edges. Does not run the ray test and leaves
// has_reversed_faces false.
MeshDiagnosticStats its_edge_diagnostics(const indexed_triangle_set &its);

// Sets has_reversed_faces from already computed edge stats:
//   non-manifold or same-direction edges -> true, without rays;
//   open edges                           -> left unchanged (not watertight);
//   otherwise (watertight)               -> rays from outside the AABB. A closed mesh yields an
//                                           even hit count, odd counts are discarded; rays whose
//                                           outermost hit is coincident with the next one, is a
//                                           sliver, lands on an edge or vertex or is grazing are
//                                           discarded too. A back-facing outermost hit sets the flag.
void its_detect_reversed_faces(const indexed_triangle_set &its, MeshDiagnosticStats &stats);

// its_edge_diagnostics followed by its_detect_reversed_faces.
MeshDiagnosticStats its_quick_diagnostics(const indexed_triangle_set &its);

// Whether TriangleMesh fills TriangleMeshStats::has_reversed_faces when its statistics are computed
// (model import, flips). Off by default: the ray test adds to the import time of every mesh. The GUI
// sets it from the "detect_reversed_faces" preference.
void set_reversed_faces_detection(bool enabled);
bool reversed_faces_detection_enabled();

} // namespace Slic3r
