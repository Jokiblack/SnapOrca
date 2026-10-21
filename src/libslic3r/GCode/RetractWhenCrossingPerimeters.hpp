#ifndef slic3r_RetractWhenCrossingPerimeters_hpp_
#define slic3r_RetractWhenCrossingPerimeters_hpp_

#include <vector>

#include "../AABBTreeIndirect.hpp"
#include "../AABBTreeLines.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

// Forward declarations.
class ExPolygon;
class Layer;
class Polyline;

class RetractWhenCrossingPerimeters
{
public:
    bool    travel_inside_internal_regions(const Layer &layer, const Polyline &travel);
    // Whether the travel runs continuously close to the walls of the layer (closer than the perimeter line width)
    // for at least max_near_wall_travel. Used to keep the retraction of such travels (retract_on_long_wall_travel).
    bool    travel_near_perimeters_too_long(const Layer &layer, const Polyline &travel, coord_t max_near_wall_travel);

private:
    // Last object layer visited, for which a cache of internal islands was created.
    const Layer                        *m_layer = nullptr;
    // Last object layer visited, for which a cache of wall lines was created.
    const Layer                        *m_walls_layer = nullptr;
    // Search structure over the perimeters and the internal islands contours of m_walls_layer.
    AABBTreeLines::LinesDistancer<Line> m_walls_distancer;
    // Distance from a wall centerline below which the nozzle is considered to be travelling along the wall.
    coord_t                             m_wall_proximity_distance = 0;
    // Internal islands only, referencing data owned by m_layer->regions()->surfaces().
    std::vector<const ExPolygon*>       m_internal_islands;
    // Search structure over internal islands.
    using AABBTree = AABBTreeIndirect::Tree<2, coord_t>;
    AABBTree                            m_aabbtree_internal_islands;
};

} // namespace Slic3r

#endif // slic3r_RetractWhenCrossingPerimeters_hpp_
