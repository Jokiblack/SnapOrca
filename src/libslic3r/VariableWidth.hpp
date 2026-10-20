#ifndef slic3r_VariableWidth_hpp_
#define slic3r_VariableWidth_hpp_

#include "ExtrusionEntity.hpp"
#include "Polyline.hpp"
#include <vector>

namespace Slic3r { class Flow; }

namespace Slic3r {
    ExtrusionMultiPath thick_polyline_to_multi_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float merge_tolerance);
    // Drop closed gap fill loops that nearly duplicate an earlier one, as variable_width() does for erGapFill.
    void remove_duplicate_gap_fill_loops(ThickPolylines& polylines);
    void variable_width(const ThickPolylines& polylines, ExtrusionRole role, const Flow& flow, std::vector<ExtrusionEntity*>& out);
}

#endif
