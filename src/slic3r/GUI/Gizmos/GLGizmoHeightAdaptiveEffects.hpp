// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// NEOTKO_HAE_TAG — Height Adaptive Effects gizmo.
#pragma once

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Feature/HeightAdaptive/HeightCurve.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;

// Curve editor for the Height Adaptive Effects: slicer parameters each driven by its own Z → value curve,
// edited over the object's real layer bands (adaptive layer height included), so the layer being affected is
// visible while it is affected. Height range modifiers already vary settings by height, but there the Z is
// typed blind and the transition is only seen after slicing.
//
// The curves live in the ModelObject config (one coString key per effect), so undo/redo and the 3MF carry them
// with no extra code. An empty key is the effect off. Shown only with the "Experimental features (Neotko)"
// preference; the slicer honours a curve it finds in the config either way.
class GLGizmoHeightAdaptiveEffects : public GLGizmoBase
{
public:
    GLGizmoHeightAdaptiveEffects(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    // One row per curve.
    struct EffectDef
    {
        const char            *config_key;
        const char            *label; // marked with L(), translated at use
        const char            *units;
        // What a new curve starts as, and what a curve stored without a mode token is read as. Each curve
        // stores its own mode: whether a ramp is safe depends on how steep that curve is.
        HeightAdaptive::Interp default_interp;
        bool                   mode_selectable;
        double                 range_lo; // editor axis, effect units
        double                 range_hi;
        double                 soft_lo; // outside [soft_lo, soft_hi] a value is warned about, never blocked
        double                 soft_hi;
        bool                   env_gated;    // XY expansion rows: hidden unless ORCA_DEBUG_PROFILE is set
        bool                   is_expansion; // drives the overhang angle readout
        bool                   is_fuzzy;     // drives the "fuzzy skin is off" notice
    };

    static const std::vector<EffectDef> &effects();
    // UI gate only: the engine always honours an expansion curve found in the config, so a 3MF prints the
    // same on every machine.
    static bool expansion_effect_enabled();

    struct Node
    {
        double z_mm    = 0.;
        double value   = 0.;
        double tension = 0.5; // Smooth only
    };

    struct EffectState
    {
        std::vector<Node>      nodes; // empty = effect off
        HeightAdaptive::Interp interp = HeightAdaptive::Interp::Stepped;
        // In this object's list. An effect just added has no points and writes no key yet, but keeps its row.
        bool                   added = false;
    };

    HeightAdaptive::Interp interp_of(size_t effect_idx) const;
    bool                   effect_in_use(size_t effect_idx) const;
    bool                   effect_available(size_t effect_idx) const;
    // The effects added to this object, in registry order.
    std::vector<size_t>    listed_effects() const;
    // Drops the row and the curve; commit() erases the key.
    void                   remove_effect(size_t effect_idx);

    void   ensure_session();
    void   rebuild_layer_bands();
    void   load_curves_from_object();
    // Nearest real layer top to z; z itself when there are no bands.
    double snap_z(double z) const;
    // Number of real layer tops in [z_lo, z_hi).
    int    layers_between(double z_lo, double z_hi) const;

    HeightAdaptive::HeightCurve curve_of(size_t effect_idx) const;
    const ModelObject          *model_object() const;

    // Writes every curve into the ModelObject config inside one undo snapshot and schedules a re-slice. A curve
    // with fewer than two points erases its key rather than storing "", so the object matches one that never had it.
    void commit(const std::string &snapshot_name);

    void   build_band_model(double z_mm);
    // Wall angle from vertical, in degrees, implied by an expansion curve's slope at z. Information only.
    double overhang_angle_deg(size_t effect_idx, double z_mm) const;
    void   apply_preset(size_t effect_idx, const char *preset_id);
    // Rewrites a ramp as `bands` steps following its shape, each edge on a real layer boundary.
    void   quantize_to_steps(size_t effect_idx, int bands);
    // XY shift between two consecutive layers caused by a sparse infill width ramp, at the far edge of the
    // object; out_pct is the same as a percentage of the line width. 0 for steps and other effects.
    double misalignment_per_layer_mm(size_t effect_idx, double *out_pct) const;

    void render_editor(float width, float height);
    // The panel body; split out so its early returns cannot skip the style pops around it.
    void render_panel_body(float win_w);

    bool                     m_have_session       = false;
    int                      m_object_idx         = -1;
    double                   m_object_height      = 0.;
    double                   m_first_layer_height = 0.2;
    SlicingParameters        m_slicing_params;
    // Top Z of every real layer, object-relative and raft excluded: the frame of Layer::slice_z, where the
    // slicer evaluates the curves.
    std::vector<double>      m_layer_tops;
    std::vector<EffectState> m_states; // parallel to effects()

    int  m_active_effect = -1;
    bool m_snap_to_layer = true;

    int m_drag_effect  = -1;
    int m_drag_node    = -1;
    int m_hover_effect = -1;
    int m_hover_node   = -1;

    // Z band highlighted on the object for the node being hovered or dragged.
    GLModel m_band_model;
    double  m_band_key = -1.;

    int   m_quantize_bands = 6;
    bool  m_show_points    = false;
    bool  m_show_add       = false;
    // The panel only grows within a session, so it does not jump size when switching effects.
    float m_body_max_h     = 0.f;
};

} // namespace GUI
} // namespace Slic3r
