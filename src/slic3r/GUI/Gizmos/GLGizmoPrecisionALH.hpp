// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// NEOTKO_PRECISIONALH_TAG — core only: the Adapt to Color and Slope recolor submodules are not ported.
#pragma once

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Slicing.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;

// Precision Adaptive Layer Height: point-based curve editor for ModelObject::layer_height_profile.
// Instead of the free-hand brush of the stock "Variable layer height" tool (GLCanvas3D::LayersEditing,
// untouched by this gizmo) it edits exact (Z, height) control points with a per-segment tension: a
// blend between a straight line and a Fritsch-Carlson monotone cubic, which never overshoots past the
// heights of the two points of a segment.
//
// The result is written to the ordinary ModelObject::layer_height_profile, exactly like the stock
// brush, so the slicer and the 3MF format see nothing new. Shown only with the "Experimental features
// (Neotko)" preference.
class GLGizmoPrecisionALH : public GLGizmoBase
{
public:
    GLGizmoPrecisionALH(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    struct ALHPoint
    {
        double z_mm;
        double height_mm;
        double tension; // tension of the segment starting at this point, [0,1]; unused on the last point
    };

    // Session: the object being edited and its control points, sorted by z, with front().z_mm == 0
    // and back().z_mm == object height. Kept while the same object stays selected and its profile is
    // not changed from outside (undo/redo, the stock brush), so tensions survive a panel redraw.
    bool                  m_have_session       = false;
    int                   m_object_idx         = -1;
    ObjectID              m_object_id;
    ObjectBase::Timestamp m_profile_timestamp  = 0;   // of ModelObject::layer_height_profile when loaded or committed
    double                m_object_height      = 0.0; // == SlicingParameters::object_print_z_uncompensated_height()
    double                m_first_layer_height = 0.2; // the fixed first layer; the profile starts at this height
    SlicingParameters     m_slicing_params;
    std::vector<ALHPoint> m_points;

    // The stored profile was at the old height of an object scaled in Z and the editor stretched it to
    // the current height. Until it is committed, the slicer still discards the stored profile.
    bool m_rescaled_on_load = false;

    int m_dragging_point = -1; // index into m_points, -1 = none
    int m_hover_point    = -1; // point under the cursor in the curve editor, -1 = none

    // Z-band highlight on the object, in world coordinates; rebuilt when the point or its Z changes.
    GLModel m_band_model;
    double  m_band_key = -1.0;

    // The bottom point (z = 0) is the fixed first layer and cannot be edited.
    bool point_is_locked(int idx) const { return idx == 0; }

    void ensure_session();
    void seed_flat_profile();
    // Rebuilds m_points from a stored profile, one point per (z, h) pair with straight segments. Falls
    // back to a flat start for an empty profile or one too dense to edit by points (the stock brush).
    void load_points_from_profile(const std::vector<coordf_t> &profile);

    // Fritsch-Carlson monotone tangents for the current points, one per point.
    std::vector<double> compute_tangents() const;
    // Height at t in [0,1] along segment [seg_idx, seg_idx + 1].
    double blended_height(size_t seg_idx, double t, const std::vector<double> &tangents) const;
    // [z0, h0, z1, h1, ...] as consumed by generate_object_layers().
    std::vector<coordf_t> build_profile_vector() const;

    // Takes an undo snapshot, writes ModelObject::layer_height_profile and schedules a re-slice, the
    // same commit path as the stock brush.
    void commit(const std::string &snapshot_name);
    // Clears the profile like the stock "Reset" (the object slices with its regular layer height again)
    // and restarts the editor from a flat curve.
    void reset_profile();

    void build_band_model(int idx);
    void render_curve_editor(float width, float height);
};

} // namespace GUI
} // namespace Slic3r
