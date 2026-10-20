#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Polyline.hpp"
#include "libslic3r/Surface.hpp"
#include <vector>
#include <algorithm>
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Line.hpp"
#include <cstddef>
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/libslic3r.h"
#include <cassert>
#include "libslic3r/Polygon.hpp"

#include "RetractWhenCrossingPerimeters.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/SurfaceCollection.hpp"

namespace Slic3r {

bool RetractWhenCrossingPerimeters::travel_inside_internal_regions(const Layer &layer, const Polyline &travel)
{
    if (m_layer != &layer) {
        // Update cache.
        m_layer = &layer;
        m_internal_islands.clear();
        m_aabbtree_internal_islands.clear();
        // Collect expolygons of internal slices.
        for (const LayerRegion *layerm : layer.regions())
            for (const Surface &surface : layerm->get_slices().surfaces)
                if (surface.is_internal())
                    m_internal_islands.emplace_back(&surface.expolygon);
        // Calculate bounding boxes of internal slices.
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
        bboxes.reserve(m_internal_islands.size());
        for (size_t i = 0; i < m_internal_islands.size(); ++ i)
            bboxes.emplace_back(i, get_extents(*m_internal_islands[i]));
        // Build AABB tree over bounding boxes of internal slices.
        m_aabbtree_internal_islands.build_modify_input(bboxes);
    }

    BoundingBox           bbox_travel = get_extents(travel);
    AABBTree::BoundingBox bbox_travel_eigen{ bbox_travel.min, bbox_travel.max };
    int result = -1;
    bbox_travel.offset(SCALED_EPSILON);
    AABBTreeIndirect::traverse(m_aabbtree_internal_islands, 
        [&bbox_travel_eigen](const AABBTree::Node &node) {
            return bbox_travel_eigen.intersects(node.bbox);
        },
        [&travel, &bbox_travel, &result, &islands = m_internal_islands](const AABBTree::Node &node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            Polygons clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*islands[node.idx], bbox_travel);
            if (diff_pl(travel, clipped).empty()) {
                // Travel path is completely inside an "internal" island. Don't retract.
                result = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return result != -1;
}

// Ported from BambuStudio 1d601fac9.
bool RetractWhenCrossingPerimeters::travel_near_perimeters_too_long(const Layer &layer, const Polyline &travel, coord_t max_near_wall_travel)
{
    if (max_near_wall_travel <= 0 || travel.points.size() < 2 || travel.length() <= max_near_wall_travel)
        return false;

    if (m_walls_layer != &layer) {
        // Update cache: perimeters and contours of the internal islands of the regions containing internal surfaces.
        m_walls_layer             = &layer;
        m_wall_proximity_distance = 0;
        Lines     lines;
        Polylines perimeters_polylines;
        for (const LayerRegion *layerm : layer.regions()) {
            bool has_internal = false;
            for (const Surface &surface : layerm->get_slices().surfaces)
                if (surface.is_internal()) {
                    has_internal = true;
                    append(lines, surface.expolygon.lines());
                }
            if (has_internal) {
                layerm->perimeters.collect_polylines(perimeters_polylines);
                m_wall_proximity_distance = std::max(m_wall_proximity_distance, layerm->flow(frPerimeter).scaled_width());
            }
        }
        for (const Polyline &polyline : perimeters_polylines)
            append(lines, polyline.lines());
        m_walls_distancer = AABBTreeLines::LinesDistancer<Line>{std::move(lines)};
    }

    if (m_wall_proximity_distance <= 0 || m_walls_distancer.get_lines().empty())
        return false;

    // Sample the travel path: steps of half the threshold while close to a wall, and while far from it steps of
    // (wall distance - proximity distance), as the distance to the walls changes at most by the distance moved.
    const double near_wall_sample_distance = 0.5 * double(max_near_wall_travel);
    size_t       segment_idx               = 0;
    double       segment_offset            = 0.;
    Vec2d        sample_point              = travel.points.front().cast<double>();

    // Move the sample point along the travel by the given distance, crossing to the next segments as needed.
    auto advance_sample = [&travel, &segment_idx, &segment_offset, &sample_point](double distance) {
        while (segment_idx + 1 < travel.points.size()) {
            const Vec2d  segment_start  = travel.points[segment_idx].cast<double>();
            const Vec2d  segment        = travel.points[segment_idx + 1].cast<double>() - segment_start;
            const double segment_length = segment.norm();
            if (segment_length <= 0.) {
                ++segment_idx;
                segment_offset = 0.;
                continue;
            }
            const double available = segment_length - segment_offset;
            if (distance <= available) {
                segment_offset += distance;
                sample_point = segment_start + segment * (segment_offset / segment_length);
                return true;
            }
            distance -= available;
            ++segment_idx;
            segment_offset = 0.;
            sample_point   = travel.points[segment_idx].cast<double>();
        }
        // End of the travel reached.
        return false;
    };

    // Only the path between two consecutive near-wall samples counts as near-wall length.
    bool   previous_sample_near_wall     = false;
    double distance_from_previous_sample = 0.;
    double consecutive_near_wall_length  = 0.;
    while (true) {
        const double wall_distance    = m_walls_distancer.distance_from_lines<false>(sample_point.cast<coord_t>());
        const bool   sample_near_wall = wall_distance <= double(m_wall_proximity_distance);
        if (sample_near_wall) {
            consecutive_near_wall_length = previous_sample_near_wall ? consecutive_near_wall_length + distance_from_previous_sample : 0.;
            if (consecutive_near_wall_length >= double(max_near_wall_travel))
                return true;
        } else
            consecutive_near_wall_length = 0.;

        const double next_sample_distance = sample_near_wall ?
            near_wall_sample_distance :
            std::max(near_wall_sample_distance, wall_distance - double(m_wall_proximity_distance));
        previous_sample_near_wall     = sample_near_wall;
        distance_from_previous_sample = next_sample_distance;
        if (!advance_sample(next_sample_distance))
            break;
    }
    return false;
}

} // namespace Slic3r
