#ifndef SLICESTOTRIANGLEMESH_HPP
#define SLICESTOTRIANGLEMESH_HPP

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ExPolygon.hpp"
#include <vector>

namespace Slic3r {

// Slices with arbitrary tops: grid[i] is the top of slices[i], the first one starts at zmin.
// Already defined in the .cpp; declared here for NeotkoSurfaceExtrude (Ported from OrcaFS-NeotkoCM by Neotko, AGPL-3.0).
indexed_triangle_set slices_to_mesh(const std::vector<ExPolygons> &slices,
                                    double                         zmin,
                                    const std::vector<float> &     grid);

void slices_to_mesh(indexed_triangle_set &         mesh,
                    const std::vector<ExPolygons> &slices,
                    double                         zmin,
                    double                         lh,
                    double                         ilh);

inline indexed_triangle_set slices_to_mesh(
    const std::vector<ExPolygons> &slices, double zmin, double lh, double ilh)
{
    indexed_triangle_set out;
    slices_to_mesh(out, slices, zmin, lh, ilh);

    return out;
}

} // namespace Slic3r

#endif // SLICESTOTRIANGLEMESH_HPP
