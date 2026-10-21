// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// NEOTKO_CONSTRUCT_TAG_START — "Construct" (Align & Construct): a solid extruded from the flat face
// picked on an object. Geometry only, no interface.
#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r { namespace SurfaceExtrude {

// Facets connected to `seed` whose normal stays within acos(cos_thresh) of the seed's normal.
// `normals` / `neighbors` come from its_face_normals() / its_face_neighbors().
std::vector<int> coplanar_region(const std::vector<Vec3f>   &normals,
                                 const std::vector<Vec3i32> &neighbors,
                                 int                         seed,
                                 float                       cos_thresh,
                                 size_t                      max_facets = 40000);

// The region projected onto XY after `trafo` (scaled coordinates), holes kept.
// mean_z receives the average Z of the region's vertices after `trafo`.
ExPolygons region_outline(const indexed_triangle_set &its,
                          const std::vector<int>     &region,
                          const Transform3d          &trafo,
                          double                     *mean_z = nullptr);

// The holes of `outline`, each one as a filled shape (counter-clockwise, no holes of its own).
ExPolygons outline_holes(const ExPolygons &outline);

// Closed prism of `outline` grown/shrunk by offset_mm, from z0 to z0 + height.
// Empty when the offset eats the whole shape or height <= 0.
indexed_triangle_set extrude_outline(const ExPolygons &outline,
                                     double            offset_mm,
                                     double            z0,
                                     double            height);

// Extrusion along a curve (hidden: only offered in the NeoDebug console mode).
// A node is a height (t = fraction 0..1 of the total height) the curve must pass through, with its
// offset e in mm (positive grows, negative shrinks). (ht, he) is the control point of the span from
// this node to the next one (quadratic Bezier).
struct CurveNode {
    double t  = 0.;
    double e  = 0.;
    double ht = 0.;
    double he = 0.;
};

// The curve as a polyline of (t, e), t ascending.
std::vector<Vec2d> sample_curve(const std::vector<CurveNode> &curve, int per_segment = 24);
// e at height fraction t, read off a sample_curve() polyline.
double curve_value(const std::vector<Vec2d> &samples, double t);

// Like extrude_outline(), but the outline is offset by offset_mm + curve(t) at each height.
// t = 0 is the picked face: with grow_down the solid hangs below z0 + height instead of rising
// from z0. The solid is built in thin steps of at most max_step_mm.
indexed_triangle_set extrude_outline_curved(const ExPolygons             &outline,
                                            double                        offset_mm,
                                            double                        z0,
                                            double                        height,
                                            const std::vector<CurveNode> &curve,
                                            bool                          grow_down,
                                            double                        max_step_mm = 0.025);

}} // namespace Slic3r::SurfaceExtrude
// NEOTKO_CONSTRUCT_TAG_END
