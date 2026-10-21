// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// NEOTKO_HAE_TAG — see GLGizmoHeightAdaptiveEffects.hpp.
#include "GLGizmoHeightAdaptiveEffects.hpp"

#include "GizmoNeotkoStyle.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/format.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <imgui/imgui.h>
#include <glad/gl.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

// A "(?)" marker with a tooltip. Only after a short single-line label: SameLine() lands it where that item ended.
void help_marker(const std::string &text)
{
    ImGui::SameLine();
    ImGui::PushTextWrapPos(-1.f); // no wrapping for the marker itself
    ImGui::TextDisabled("(?)");
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320.f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

bool GLGizmoHeightAdaptiveEffects::expansion_effect_enabled()
{
    return NeoDebug::enabled(NeoDebug::PROFILE);
}

const std::vector<GLGizmoHeightAdaptiveEffects::EffectDef> &GLGizmoHeightAdaptiveEffects::effects()
{
    // Ranges are the editor axis, not a limit on the slicer; the soft pair is what gets warned about.
    static const std::vector<EffectDef> defs = {
        // XY expansion. Past ±2 mm the sparse infill regions fail to generate (measured by Neotko), so the axis
        // stops there. The real risk of a large offset, fused details or lost islands, depends on the geometry
        // and is reported after slicing.
        { "neotko_hae_xy_contour", L("XY contour"), "mm", HeightAdaptive::Interp::Smooth,
          true, -2.0, 2.0, -2.0, 2.0, /*env_gated*/ true, /*is_expansion*/ true, /*is_fuzzy*/ false },
        { "neotko_hae_xy_hole", L("XY hole"), "mm", HeightAdaptive::Interp::Smooth,
          true, -2.0, 2.0, -2.0, 2.0, true, true, false },
        // Steps by default: sparse infill keeps its spacing constant across layers so the lines stack, and a
        // width that drifts between consecutive layers walks them off. A slow ramp is allowed, and the panel
        // shows the shift it causes.
        { "neotko_hae_infill_width", L("Sparse infill width"), "mm", HeightAdaptive::Interp::Stepped,
          true, 0.2, 0.8, 0.3, 0.7, false, false, false },
        // Surface texture: nothing stacks on it, so a ramp by default. [0, 1] mm is the range of
        // fuzzy_skin_thickness itself; the soft limit follows its advice to stay below the wall width.
        { "neotko_hae_fuzzy_thickness", L("Fuzzy skin thickness"), "mm", HeightAdaptive::Interp::Smooth,
          true, 0.0, 1.0, 0.0, 0.6, false, false, true },
        // Only the widths that stack are offered; internal solid infill and top surface were left out on
        // purpose, since a ramp there varies the width of surfaces that should read as flat.
        { "neotko_hae_outer_wall_width", L("Outer wall width"), "mm", HeightAdaptive::Interp::Stepped,
          true, 0.2, 0.8, 0.3, 0.7, false, false, false },
        { "neotko_hae_inner_wall_width", L("Inner wall width"), "mm", HeightAdaptive::Interp::Stepped,
          true, 0.2, 0.8, 0.3, 0.7, false, false, false },
        { "neotko_hae_fuzzy_point_distance", L("Fuzzy skin point distance"), "mm", HeightAdaptive::Interp::Smooth,
          true, 0.05, 2.0, 0.1, 1.5, false, false, true },
        { "neotko_hae_fuzzy_scale", L("Fuzzy skin noise scale"), "mm", HeightAdaptive::Interp::Smooth,
          true, 0.1, 10.0, 0.1, 10.0, false, false, true },
        // Integer key: a curve over it is a staircase whatever the mode, so it does not offer a ramp.
        { "neotko_hae_fuzzy_octaves", L("Fuzzy skin noise octaves"), "", HeightAdaptive::Interp::Stepped,
          false, 1.0, 8.0, 1.0, 8.0, false, false, true },
        { "neotko_hae_fuzzy_persistence", L("Fuzzy skin noise persistence"), "", HeightAdaptive::Interp::Smooth,
          true, 0.0, 1.0, 0.0, 1.0, false, false, true },
    };
    return defs;
}

GLGizmoHeightAdaptiveEffects::GLGizmoHeightAdaptiveEffects(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoHeightAdaptiveEffects::on_init()
{
    return true; // no shortcut
}

std::string GLGizmoHeightAdaptiveEffects::on_get_name() const
{
    return _u8L("Height Adaptive Effects");
}

bool GLGizmoHeightAdaptiveEffects::on_is_selectable() const
{
    return wxGetApp().neotko_features_enabled();
}

bool GLGizmoHeightAdaptiveEffects::on_is_activable() const
{
    // The curves are per object, so an object has to be selected.
    return wxGetApp().neotko_features_enabled() && m_parent.get_selection().get_object_idx() >= 0;
}

void GLGizmoHeightAdaptiveEffects::on_set_state()
{
    if (get_state() == Off) {
        m_have_session = false;
        m_drag_effect  = -1;
        m_drag_node    = -1;
        m_show_add     = false;
    }
}

// ------------------------------------------------------------------------------- session

const ModelObject *GLGizmoHeightAdaptiveEffects::model_object() const
{
    const Model *model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= int(model->objects.size()))
        return nullptr;
    return model->objects[m_object_idx];
}

void GLGizmoHeightAdaptiveEffects::ensure_session()
{
    const Selection &sel     = m_parent.get_selection();
    const int        obj_idx = sel.get_object_idx();
    const Model     *model   = sel.get_model();
    if (obj_idx < 0 || model == nullptr || obj_idx >= int(model->objects.size())) {
        m_have_session = false;
        return;
    }
    if (m_have_session && obj_idx == m_object_idx)
        return; // same object: keep the edits in progress

    m_object_idx          = obj_idx;
    const ModelObject *mo = model->objects[obj_idx];

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_slicing_params = PrintObject::slicing_parameters(full_config, *mo, float(mo->max_z()), Vec3d::Ones());
    // Object-relative and raft excluded, the frame of Layer::slice_z.
    m_object_height      = m_slicing_params.object_print_z_uncompensated_height();
    m_first_layer_height = m_slicing_params.first_object_layer_height;
    if (m_first_layer_height <= 0.)
        m_first_layer_height = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;

    rebuild_layer_bands();
    load_curves_from_object();

    const std::vector<size_t> listed = listed_effects();
    m_active_effect = listed.empty() ? -1 : int(listed.front());
    m_body_max_h    = 0.f;
    m_have_session  = true;
}

void GLGizmoHeightAdaptiveEffects::rebuild_layer_bands()
{
    // The real layers, from the same function the slicer uses, fed the object's own layer height profile
    // when it has a usable one.
    m_layer_tops.clear();
    const ModelObject *mo = this->model_object();
    if (mo == nullptr || ! m_slicing_params.valid || m_object_height <= 0.)
        return;

    std::vector<coordf_t> profile = mo->layer_height_profile.get();
    // Usable only if it spans 0 .. object height, the condition the slicer checks before honouring it.
    const bool profile_usable = profile.size() >= 4 && profile.size() % 2 == 0 && profile.front() <= 1e-6 &&
                                std::abs(profile[profile.size() - 2] - m_object_height) < 1e-3;
    if (! profile_usable) {
        double h = m_slicing_params.layer_height;
        if (h <= 0.)
            h = m_slicing_params.max_layer_height > 0. ? m_slicing_params.max_layer_height : 0.2;
        profile = { 0., m_first_layer_height, m_object_height, h };
    }

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    const bool precise_z = full_config.has("precise_z_height") && full_config.opt_bool("precise_z_height");
    // (bottom, top) pairs per layer.
    const std::vector<coordf_t> bounds = generate_object_layers(m_slicing_params, profile, precise_z);
    m_layer_tops.reserve(bounds.size() / 2);
    for (size_t i = 1; i < bounds.size(); i += 2)
        m_layer_tops.emplace_back(double(bounds[i]));
}

HeightAdaptive::Interp GLGizmoHeightAdaptiveEffects::interp_of(size_t effect_idx) const
{
    return effect_idx < m_states.size() ? m_states[effect_idx].interp : effects()[effect_idx].default_interp;
}

bool GLGizmoHeightAdaptiveEffects::effect_in_use(size_t effect_idx) const
{
    // Two points is what commit() needs to write a curve at all.
    return effect_idx < m_states.size() && m_states[effect_idx].nodes.size() >= 2;
}

bool GLGizmoHeightAdaptiveEffects::effect_available(size_t effect_idx) const
{
    return effect_idx < effects().size() && (! effects()[effect_idx].env_gated || expansion_effect_enabled());
}

std::vector<size_t> GLGizmoHeightAdaptiveEffects::listed_effects() const
{
    std::vector<size_t> out;
    for (size_t e = 0; e < effects().size() && e < m_states.size(); ++e)
        if (m_states[e].added && effect_available(e))
            out.push_back(e);
    return out;
}

void GLGizmoHeightAdaptiveEffects::remove_effect(size_t effect_idx)
{
    if (effect_idx >= m_states.size())
        return;
    m_states[effect_idx].nodes.clear();
    m_states[effect_idx].added  = false;
    m_states[effect_idx].interp = effects()[effect_idx].default_interp;
    if (m_drag_effect == int(effect_idx)) {
        m_drag_effect = -1;
        m_drag_node   = -1;
    }
    if (m_hover_effect == int(effect_idx)) {
        m_hover_effect = -1;
        m_hover_node   = -1;
    }
    if (m_active_effect == int(effect_idx)) {
        const std::vector<size_t> rest = listed_effects();
        m_active_effect = rest.empty() ? -1 : int(rest.front());
    }
    commit(_u8L("Height adaptive effects - Remove effect"));
}

void GLGizmoHeightAdaptiveEffects::load_curves_from_object()
{
    m_states.assign(effects().size(), EffectState{});
    const ModelObject *mo = this->model_object();
    if (mo == nullptr)
        return;
    for (size_t e = 0; e < effects().size(); ++e) {
        const EffectDef &def = effects()[e];
        m_states[e].interp   = def.default_interp;
        const auto *opt      = dynamic_cast<const ConfigOptionString *>(mo->config.option(def.config_key));
        if (opt == nullptr || opt->value.empty())
            continue;
        const auto curve   = HeightAdaptive::HeightCurve::parse(opt->value, def.default_interp);
        m_states[e].interp = curve.interp();
        for (const HeightAdaptive::Node &n : curve.nodes())
            m_states[e].nodes.push_back(Node{ n.z, n.value, n.tension });
        // A committed object never holds a key with fewer than two points, so this lists exactly what it drives.
        m_states[e].added = m_states[e].nodes.size() >= 2;
    }
}

double GLGizmoHeightAdaptiveEffects::snap_z(double z) const
{
    if (m_layer_tops.empty())
        return z;
    auto it = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z);
    if (it == m_layer_tops.begin())
        return *it;
    if (it == m_layer_tops.end())
        return m_layer_tops.back();
    const double hi = *it, lo = *(it - 1);
    return (z - lo <= hi - z) ? lo : hi;
}

int GLGizmoHeightAdaptiveEffects::layers_between(double z_lo, double z_hi) const
{
    if (m_layer_tops.empty() || z_hi <= z_lo)
        return 0;
    const auto lo = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_lo);
    const auto hi = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_hi);
    return int(std::distance(lo, hi));
}

HeightAdaptive::HeightCurve GLGizmoHeightAdaptiveEffects::curve_of(size_t effect_idx) const
{
    HeightAdaptive::HeightCurve curve(interp_of(effect_idx));
    if (effect_idx >= m_states.size())
        return curve;
    std::vector<HeightAdaptive::Node> nodes;
    nodes.reserve(m_states[effect_idx].nodes.size());
    for (const Node &n : m_states[effect_idx].nodes)
        nodes.push_back(HeightAdaptive::Node{ n.z_mm, n.value, n.tension });
    curve.set_nodes(std::move(nodes));
    return curve;
}

void GLGizmoHeightAdaptiveEffects::commit(const std::string &snapshot_name)
{
    if (! m_have_session)
        return;
    ModelObject *mo = const_cast<ModelObject *>(this->model_object());
    if (mo == nullptr)
        return;

    wxGetApp().plater()->take_snapshot(snapshot_name);
    for (size_t e = 0; e < effects().size(); ++e) {
        const EffectDef  &def  = effects()[e];
        // One point is a constant, which is what a plain per-object override is for.
        const std::string blob = m_states[e].nodes.size() >= 2 ? curve_of(e).serialize() : std::string();
        if (blob.empty())
            mo->config.erase(def.config_key);
        else
            mo->config.set_key_value(def.config_key, new ConfigOptionString(blob));
    }
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(size_t(m_object_idx));
}

// ------------------------------------------------------------------------------- 3D band

void GLGizmoHeightAdaptiveEffects::build_band_model(double z_mm)
{
    m_band_model.reset();
    const ModelObject *mo = this->model_object();
    if (mo == nullptr)
        return;

    BoundingBoxf3 bb;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        bb.merge(mo->instance_bounding_box(i));
    if (! bb.defined)
        return;

    const double zc  = bb.min.z() + z_mm;
    const double pad = 1.5;
    const double ht  = std::max(0.15, 0.5 * m_slicing_params.layer_height);
    const float  x0 = float(bb.min.x() - pad), x1 = float(bb.max.x() + pad);
    const float  y0 = float(bb.min.y() - pad), y1 = float(bb.max.y() + pad);
    const float  z0 = float(zc - ht), z1 = float(zc + ht);

    indexed_triangle_set its;
    its.vertices = {
        Vec3f(x0, y0, z0), Vec3f(x1, y0, z0), Vec3f(x1, y1, z0), Vec3f(x0, y1, z0),
        Vec3f(x0, y0, z1), Vec3f(x1, y0, z1), Vec3f(x1, y1, z1), Vec3f(x0, y1, z1),
    };
    static const int box_tris[12][3] = {
        { 0, 1, 2 }, { 0, 2, 3 }, { 4, 6, 5 }, { 4, 7, 6 }, { 0, 4, 5 }, { 0, 5, 1 },
        { 1, 5, 6 }, { 1, 6, 2 }, { 2, 6, 7 }, { 2, 7, 3 }, { 3, 7, 4 }, { 3, 4, 0 },
    };
    its.indices.reserve(12);
    for (const auto &t : box_tris)
        its.indices.emplace_back(t[0], t[1], t[2]);
    m_band_model.init_from(its);
}

void GLGizmoHeightAdaptiveEffects::on_render()
{
    // Highlights on the object the slice of it that the hovered or dragged point affects.
    const int eff  = (m_drag_effect >= 0) ? m_drag_effect : m_hover_effect;
    const int node = (m_drag_effect >= 0) ? m_drag_node : m_hover_node;
    if (! m_have_session || eff < 0 || eff >= int(m_states.size()))
        return;
    if (node < 0 || node >= int(m_states[eff].nodes.size()))
        return;

    const double z_mm = m_states[eff].nodes[node].z_mm;
    const double key  = eff * 1e6 + z_mm;
    if (key != m_band_key) {
        build_band_model(z_mm);
        m_band_key = key;
    }
    if (! m_band_model.is_initialized())
        return;

    const bool dragging = m_drag_effect >= 0;
    m_band_model.set_color(dragging ? ColorRGBA(1.00f, 0.75f, 0.16f, 0.42f) : ColorRGBA(0.00f, 0.59f, 0.53f, 0.30f));

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_band_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// ------------------------------------------------------------------------------- info

double GLGizmoHeightAdaptiveEffects::overhang_angle_deg(size_t effect_idx, double z_mm) const
{
    // d(offset)/dz is the tangent of the wall angle from vertical.
    const auto curve = curve_of(effect_idx);
    if (curve.empty())
        return 0.;
    const double dz = std::max(1e-3, 0.02 * std::max(m_object_height, 1.));
    const double v0 = curve.at(std::max(0., z_mm - dz), 0.);
    const double v1 = curve.at(z_mm + dz, 0.);
    return std::atan(std::abs(v1 - v0) / (2. * dz)) * 180. / M_PI;
}

void GLGizmoHeightAdaptiveEffects::apply_preset(size_t effect_idx, const char *preset_id)
{
    if (effect_idx >= m_states.size() || m_object_height <= 0.)
        return;
    const double       h = m_object_height;
    std::vector<Node> &n = m_states[effect_idx].nodes;
    n.clear();

    const std::string id = preset_id;
    if (id == "bullnose") {
        // Widest at the equator, closing towards both ends.
        n = { { 0., 0.0, 0.5 }, { 0.45 * h, 0.6, 0.5 }, { h, 0.0, 0.5 } };
    } else if (id == "chamfer") {
        // Tension 0: straight segments.
        n = { { 0., 0.5, 0.0 }, { 0.30 * h, 0.0, 0.0 }, { h, 0.0, 0.0 } };
    } else if (id == "barrel") {
        n = { { 0., -0.2, 1.0 }, { 0.50 * h, 0.4, 1.0 }, { h, -0.2, 1.0 } };
    } else if (id == "fine_bottom") {
        // Thin lines low down where the infill supports the solid above it, coarse ones deep inside.
        n = { { 0., 0.30, 0.0 }, { 0.35 * h, 0.55, 0.0 } };
    } else if (id == "fine_top") {
        n = { { 0., 0.55, 0.0 }, { 0.65 * h, 0.30, 0.0 } };
    } else if (id == "fade_in") {
        n = { { 0., 0.0, 0.7 }, { h, 0.35, 0.7 } };
    } else if (id == "fade_out") {
        n = { { 0., 0.35, 0.7 }, { h, 0.0, 0.7 } };
    } else if (id == "fade_band") {
        n = { { 0., 0.0, 0.8 }, { 0.5 * h, 0.35, 0.8 }, { h, 0.0, 0.8 } };
    }

    // Expansion and fuzzy presets are continuous profiles; the infill ones are bands.
    const EffectDef &def        = effects()[effect_idx];
    m_states[effect_idx].interp = def.is_expansion || def.is_fuzzy ? HeightAdaptive::Interp::Smooth : HeightAdaptive::Interp::Stepped;
    if (interp_of(effect_idx) == HeightAdaptive::Interp::Stepped || m_snap_to_layer)
        for (Node &node : n)
            node.z_mm = snap_z(node.z_mm);
}

void GLGizmoHeightAdaptiveEffects::quantize_to_steps(size_t effect_idx, int bands)
{
    // Samples the ramp at the middle of `bands` equal bands and emits steps through those samples, every edge
    // on a real layer boundary: constant spacing inside each band, a gradient across the object.
    if (effect_idx >= m_states.size() || m_object_height <= 0. || bands < 2)
        return;
    const auto curve = curve_of(effect_idx);
    if (curve.empty())
        return;

    std::vector<Node> out;
    out.reserve(size_t(bands));
    for (int i = 0; i < bands; ++i) {
        const double z_lo = m_object_height * double(i) / double(bands);
        const double z_hi = m_object_height * double(i + 1) / double(bands);
        Node         n;
        n.z_mm    = (i == 0) ? 0. : snap_z(z_lo);
        n.value   = curve.at(0.5 * (z_lo + z_hi), 0.);
        n.tension = 0.;
        // On a short object two bands can snap onto the same layer; keep the first.
        if (! out.empty() && n.z_mm <= out.back().z_mm + 1e-9)
            continue;
        out.push_back(n);
    }
    if (out.size() >= 2) {
        m_states[effect_idx].nodes  = std::move(out);
        m_states[effect_idx].interp = HeightAdaptive::Interp::Stepped;
    }
}

double GLGizmoHeightAdaptiveEffects::misalignment_per_layer_mm(size_t effect_idx, double *out_pct) const
{
    if (out_pct != nullptr)
        *out_pct = 0.;
    // Only the sparse infill width drives a spacing that has to stack.
    if (effect_idx >= m_states.size() || std::string(effects()[effect_idx].config_key) != "neotko_hae_infill_width")
        return 0.;
    if (interp_of(effect_idx) != HeightAdaptive::Interp::Smooth)
        return 0.;
    const auto curve = curve_of(effect_idx);
    if (curve.empty() || m_object_height <= 0.)
        return 0.;

    // Half the XY diagonal stands in for the far side of the object from the pattern's origin.
    double far_dist = 0.;
    if (const ModelObject *mo = this->model_object(); mo != nullptr) {
        BoundingBoxf3 bb;
        for (size_t i = 0; i < mo->instances.size(); ++i)
            bb.merge(mo->instance_bounding_box(i));
        if (bb.defined)
            far_dist = 0.5 * std::hypot(bb.size().x(), bb.size().y());
    }
    if (far_dist <= 0.)
        return 0.;

    const double  layer_h = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;
    double        worst = 0., worst_pct = 0.;
    constexpr int kSamples = 64;
    for (int i = 0; i < kSamples; ++i) {
        const double z  = m_object_height * double(i) / double(kSamples - 1);
        const double dz = std::max(1e-3, 0.5 * layer_h);
        const double w0 = curve.at(std::max(0., z - dz), 0.);
        const double w1 = curve.at(std::min(m_object_height, z + dz), 0.);
        const double w  = curve.at(z, 0.);
        if (w <= 1e-6)
            continue;
        // Relative width change over one layer is how far the Nth line moves per unit distance from the origin.
        const double rel_per_layer = std::abs(w1 - w0) / (2. * dz) * layer_h / w;
        const double shift         = far_dist * rel_per_layer;
        if (shift > worst) {
            worst     = shift;
            worst_pct = 100. * shift / w;
        }
    }
    if (out_pct != nullptr)
        *out_pct = worst_pct;
    return worst;
}

// ------------------------------------------------------------------------------- editor

void GLGizmoHeightAdaptiveEffects::render_editor(float width, float height)
{
    ImGuiIO     &io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hae_band", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    // IsItemDeactivated(), not a raw mouse-up flag, so a release frame without a repaint is not lost.
    const bool  deactivated = ImGui::IsItemDeactivated();
    ImDrawList *dl          = ImGui::GetWindowDrawList();

    if (m_active_effect < 0 || size_t(m_active_effect) >= m_states.size() || ! m_states[m_active_effect].added ||
        ! effect_available(size_t(m_active_effect)))
        return;
    const size_t     e         = size_t(m_active_effect);
    const EffectDef &def       = effects()[e];
    const bool       stepped   = interp_of(e) == HeightAdaptive::Interp::Stepped;
    const double     z_max     = std::max(m_object_height, 1e-6);
    const float      x0        = p0.x;
    const float      x1        = p0.x + width;

    auto z_to_y = [&](double z) { return p0.y + float(1.0 - z / z_max) * height; };
    auto y_to_z = [&](float y) { return z_max * (1.0 - std::clamp(double(y - p0.y) / double(height), 0.0, 1.0)); };
    auto v_to_x = [&](double v) { return x0 + float((v - def.range_lo) / (def.range_hi - def.range_lo)) * width; };
    auto x_to_v = [&](float x) {
        return def.range_lo + std::clamp(double(x - x0) / double(width), 0.0, 1.0) * (def.range_hi - def.range_lo);
    };

    // The real layers, skipped when there are so many that the lines would merge into a block.
    neo_draw_canvas(dl, p0, width, height);
    if (! m_layer_tops.empty() && m_layer_tops.size() <= size_t(height * 0.75f))
        for (size_t i = 0; i < m_layer_tops.size(); ++i) {
            const float y = z_to_y(m_layer_tops[i]);
            dl->AddLine(ImVec2(x0 + 1.f, y), ImVec2(x1 - 1.f, y),
                        (i % 5 == 4) ? neo_fade(NeoCol::GridMajor, 0.55f) : neo_col_u32(NeoCol::Grid), 1.0f);
        }
    // The first layer is fixed, so it gets its own marker.
    if (! m_layer_tops.empty()) {
        const float y = z_to_y(m_layer_tops.front());
        dl->AddLine(ImVec2(x0 + 1.f, y), ImVec2(x1 - 1.f, y), neo_col_u32(NeoCol::GridMajor), 1.5f);
    }
    // Z scale down the left edge.
    for (int t = 0; t <= 4; ++t) {
        const double z = z_max * double(t) / 4.;
        const float  y = std::clamp(z_to_y(z), p0.y + 1.f, p0.y + height - 1.f);
        char         lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.1f", z);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        const float  ly  = std::clamp(y - tsz.y * 0.5f, p0.y + 2.f, p0.y + height - tsz.y - 2.f);
        dl->AddRectFilled(ImVec2(x0 + 2.f, ly - 1.f), ImVec2(x0 + 6.f + tsz.x, ly + tsz.y + 1.f), neo_fade(NeoCol::Canvas, 0.72f), 3.f);
        dl->AddText(ImVec2(x0 + 4.f, ly), neo_fade(NeoCol::TextDim, 0.75f), lbl);
    }

    dl->AddRect(ImVec2(x0, p0.y), ImVec2(x1, p0.y + height), neo_fade(NeoCol::Accent, 0.75f), 6.0f, 0, 1.5f);

    // Value axis: gridlines, with the ends and quarters labelled.
    {
        constexpr int kTicks = 8;
        const float   lbl_h  = ImGui::GetTextLineHeight();
        dl->AddRectFilled(ImVec2(x0 + 1.f, p0.y + 1.f), ImVec2(x1 - 1.f, p0.y + lbl_h + 3.f), neo_fade(NeoCol::Canvas, 0.82f), 5.f,
                          ImDrawFlags_RoundCornersTop);
        for (int t = 0; t <= kTicks; ++t) {
            const double v    = def.range_lo + (def.range_hi - def.range_lo) * double(t) / double(kTicks);
            const float  xv   = v_to_x(v);
            const bool   edge = t == 0 || t == kTicks;
            dl->AddLine(ImVec2(xv, p0.y + lbl_h + 3.f), ImVec2(xv, p0.y + height - 1.f),
                        edge ? neo_fade(NeoCol::GridMajor, 0.6f) : neo_col_u32(NeoCol::Grid), 1.0f);
            if (! edge && (t * 4) % kTicks != 0)
                continue;
            char lbl[24];
            std::snprintf(lbl, sizeof(lbl), "%.2f", v);
            const ImVec2 tsz = ImGui::CalcTextSize(lbl);
            // The ends are aligned to their edge rather than centred on it, so they do not collide.
            float lx = (t == 0) ? x0 + 3.f : (t == kTicks) ? x1 - tsz.x - 3.f : xv - tsz.x * 0.5f;
            lx       = std::clamp(lx, x0 + 3.f, x1 - tsz.x - 3.f);
            dl->AddText(ImVec2(lx, p0.y + 2.f), neo_fade(NeoCol::TextDim, 0.9f), lbl);
        }
    }
    if (def.range_lo < 0. && def.range_hi > 0.) {
        const float xz = v_to_x(0.);
        dl->AddLine(ImVec2(xz, p0.y), ImVec2(xz, p0.y + height), neo_fade(NeoCol::GridMajor, 0.7f), 1.0f);
    }
    // Soft limits: a gradient warning towards the edge, not a wall.
    const ImU32 warn_edge = neo_fade(NeoCol::Warn, 0.16f);
    const ImU32 warn_none = neo_fade(NeoCol::Warn, 0.f);
    if (def.soft_lo > def.range_lo)
        dl->AddRectFilledMultiColor(ImVec2(x0 + 1.f, p0.y + 1.f), ImVec2(v_to_x(def.soft_lo), p0.y + height - 1.f), warn_edge, warn_none,
                                    warn_none, warn_edge);
    if (def.soft_hi < def.range_hi)
        dl->AddRectFilledMultiColor(ImVec2(v_to_x(def.soft_hi), p0.y + 1.f), ImVec2(x1 - 1.f, p0.y + height - 1.f), warn_none, warn_edge,
                                    warn_edge, warn_none);

    // The curve, value on X and Z on Y, filled towards the left edge.
    const auto  curve     = curve_of(e);
    const ImU32 curve_col = neo_col_u32(NeoCol::AccentBright);
    const ImU32 glow_col  = neo_fade(NeoCol::AccentBright, 0.20f);
    const ImU32 fill_col  = neo_col_u32(NeoCol::AccentGhost);
    const float fill_x0   = x0 + 1.f;
    if (! curve.empty() && m_states[e].nodes.size() >= 2) {
        if (stepped) {
            // Steps drawn as steps, with the one transition layer of each marked.
            const auto &ns = curve.nodes();
            for (size_t i = 0; i < ns.size(); ++i) {
                const float xv   = v_to_x(ns[i].value);
                const float ytop = z_to_y(i + 1 < ns.size() ? ns[i + 1].z : z_max);
                const float ybot = z_to_y(i == 0 ? 0. : ns[i].z);
                dl->AddRectFilled(ImVec2(fill_x0, ytop), ImVec2(xv, ybot), fill_col);
                dl->AddLine(ImVec2(xv, ybot), ImVec2(xv, ytop), glow_col, 5.0f);
                dl->AddLine(ImVec2(xv, ybot), ImVec2(xv, ytop), curve_col, 2.0f);
                if (i + 1 < ns.size()) {
                    dl->AddLine(ImVec2(xv, ytop), ImVec2(v_to_x(ns[i + 1].value), ytop), curve_col, 2.0f);
                    dl->AddLine(ImVec2(x0 + 1.f, ytop), ImVec2(x1 - 1.f, ytop), neo_fade(NeoCol::Warn, 0.35f), 1.0f);
                }
            }
        } else {
            constexpr int kSamples = 120;
            ImVec2        pts[kSamples + 1];
            for (int s = 0; s <= kSamples; ++s) {
                const double z = z_max * double(s) / double(kSamples);
                pts[s]         = ImVec2(v_to_x(curve.at(z, 0.)), z_to_y(z));
                if (s > 0)
                    dl->AddQuadFilled(ImVec2(fill_x0, pts[s - 1].y), pts[s - 1], pts[s], ImVec2(fill_x0, pts[s].y), fill_col);
            }
            dl->AddPolyline(pts, kSamples + 1, glow_col, ImDrawFlags_None, 6.0f);
            dl->AddPolyline(pts, kSamples + 1, curve_col, ImDrawFlags_None, 2.2f);
        }
    }

    int new_hover_node = -1;
    for (size_t i = 0; i < m_states[e].nodes.size(); ++i) {
        const Node  &n = m_states[e].nodes[i];
        const ImVec2 c(v_to_x(n.value), z_to_y(n.z_mm));
        const bool   soft_bad = n.value < def.soft_lo - 1e-9 || n.value > def.soft_hi + 1e-9;
        const bool   dragged  = m_drag_effect == int(e) && m_drag_node == int(i);
        if (hovered && std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 8.0f)
            new_hover_node = int(i);
        const bool lit = dragged || new_hover_node == int(i);
        neo_draw_node(dl, c, dragged ? 6.5f : (lit ? 6.0f : 4.5f), soft_bad ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright), lit);
    }

    if (m_drag_effect == int(e)) {
        // A drag in progress is followed through ImGui's active item, not the hover: the cursor often leaves the
        // widget while the button is held, and gating on hover would lose the release and its commit.
        if (ImGui::IsItemActive() && m_drag_node >= 0 && m_drag_node < int(m_states[e].nodes.size())) {
            Node &n = m_states[e].nodes[size_t(m_drag_node)];
            n.value = x_to_v(io.MousePos.x);
            // Free Z while dragging, snapped on release, so the point does not stutter between bands.
            n.z_mm         = std::clamp(y_to_z(io.MousePos.y), 0., z_max);
            new_hover_node = m_drag_node;
            m_parent.set_as_dirty();
            m_parent.request_extra_frame();
        }
        if (deactivated) {
            if (m_drag_node >= 0 && m_drag_node < int(m_states[e].nodes.size())) {
                Node &n = m_states[e].nodes[size_t(m_drag_node)];
                if (stepped || m_snap_to_layer)
                    n.z_mm = snap_z(n.z_mm);
                std::stable_sort(m_states[e].nodes.begin(), m_states[e].nodes.end(),
                                 [](const Node &a, const Node &b) { return a.z_mm < b.z_mm; });
                commit(_u8L("Height adaptive effects - Move point"));
            }
            m_drag_effect = -1;
            m_drag_node   = -1;
        }
    } else if (hovered) {
        if (new_hover_node >= 0 && io.MouseClicked[1]) {
            m_states[e].nodes.erase(m_states[e].nodes.begin() + new_hover_node);
            new_hover_node = -1;
            commit(_u8L("Height adaptive effects - Delete point"));
        } else if (new_hover_node >= 0 && io.MouseClicked[0]) {
            m_drag_effect = int(e);
            m_drag_node   = new_hover_node;
        } else if (new_hover_node < 0 && io.MouseClicked[0]) {
            Node n;
            n.z_mm  = y_to_z(io.MousePos.y);
            n.value = x_to_v(io.MousePos.x);
            if (stepped || m_snap_to_layer)
                n.z_mm = snap_z(n.z_mm);
            auto  &ns = m_states[e].nodes;
            size_t at = ns.size();
            for (size_t i = 0; i < ns.size(); ++i)
                if (n.z_mm < ns[i].z_mm) {
                    at = i;
                    break;
                }
            ns.insert(ns.begin() + at, n);
            commit(_u8L("Height adaptive effects - Add point"));
        }
    }
    m_hover_effect = new_hover_node >= 0 ? int(e) : -1;
    m_hover_node   = new_hover_node;

    // Crosshair readout anywhere in the graph: the Z under the cursor and what the curve is worth there.
    if (hovered && m_drag_effect < 0 && new_hover_node < 0) {
        const double z = y_to_z(io.MousePos.y);
        dl->AddLine(ImVec2(x0 + 1.f, io.MousePos.y), ImVec2(x1 - 1.f, io.MousePos.y), neo_fade(NeoCol::Ink, 0.22f), 1.0f);
        char buf[96];
        if (curve.empty())
            std::snprintf(buf, sizeof(buf), "Z %.2f mm", z);
        else
            std::snprintf(buf, sizeof(buf), "Z %.2f mm  \xe2\x86\x92  %.3f %s", z, curve.at(z, 0.), def.units);
        const ImVec2 tsz = ImGui::CalcTextSize(buf);
        const ImVec2 lp(std::clamp(io.MousePos.x + 10.f, x0, x1 - tsz.x), std::clamp(io.MousePos.y + 8.f, p0.y, p0.y + height - tsz.y));
        neo_draw_pill(dl, lp, buf, neo_fade(NeoCol::Accent, 0.5f));
    }

    // Readout of the point being hovered or dragged.
    if (m_hover_node >= 0 && m_hover_node < int(m_states[e].nodes.size())) {
        const Node &n     = m_states[e].nodes[size_t(m_hover_node)];
        std::string label = "Z " + format(float(n.z_mm), 2) + " mm   " + format(float(n.value), 3) + " " + def.units;
        if (def.is_expansion) {
            const double a = overhang_angle_deg(e, n.z_mm);
            if (a > 0.5)
                label += "   " + Slic3r::GUI::format(_u8L("wall %1%°"), format(float(a), 0));
        }
        const ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(std::clamp(x0 + width * 0.5f - tsz.x * 0.5f, x0, x1 - tsz.x), z_to_y(n.z_mm) - tsz.y - 10.0f);
        neo_draw_pill(dl, lp, label.c_str(), neo_col_u32(NeoCol::Accent));
    }

    // Caption under the graph, with a short accent rule.
    const std::string caption = _u8L(def.label);
    const ImVec2      tsz     = ImGui::CalcTextSize(caption.c_str());
    const float       lx      = x0 + std::max(0.f, (width - tsz.x) * 0.5f);
    dl->AddText(ImVec2(lx, p0.y + height + 5.f), neo_col_u32(NeoCol::AccentBright), caption.c_str());
    dl->AddLine(ImVec2(lx, p0.y + height + tsz.y + 7.f), ImVec2(lx + tsz.x, p0.y + height + tsz.y + 7.f), neo_fade(NeoCol::Accent, 0.6f), 1.5f);
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight() + 10.f));
}

// ------------------------------------------------------------------------------- panel

void GLGizmoHeightAdaptiveEffects::on_render_input_window(float x, float y, float bottom_limit)
{
    ensure_session();

    const float win_w = 540.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    neo_push_window_style(); // before Begin: the window background and title are resolved when it opens
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    // An absolute wrap column: with an auto-resizing window, "wrap at the window width" is circular.
    ImGui::PushTextWrapPos(win_w - 16.f);
    neo_push_panel_style();

    const float body_y0 = ImGui::GetCursorPosY();
    render_panel_body(win_w);
    const float body_h = ImGui::GetCursorPosY() - body_y0;
    m_body_max_h       = std::max(m_body_max_h, body_h);
    if (m_have_session && body_h < m_body_max_h - 1.f)
        ImGui::Dummy(ImVec2(1.f, m_body_max_h - body_h));

    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
    neo_pop_window_style();
}

void GLGizmoHeightAdaptiveEffects::render_panel_body(float win_w)
{
    if (! m_have_session) {
        ImGui::TextWrapped("%s", _u8L("Select a single object to edit its height adaptive effects.").c_str());
        return;
    }

    // The panel lists the effects added to this object, not every effect there is.
    const std::vector<size_t> listed = listed_effects();
    std::vector<size_t>       candidates;
    for (size_t e = 0; e < effects().size(); ++e)
        if (effect_available(e) && ! (e < m_states.size() && m_states[e].added))
            candidates.push_back(e);

    ImGui::TextUnformatted(_u8L("Effects on this object").c_str());
    help_marker(_u8L("Click in the graph to add a point, drag to move it, right-click to delete it. "
                     "The horizontal lines are this object's real layers."));
    if (! candidates.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(((m_show_add ? _u8L("Cancel") : _u8L("Add")) + "##hae_add_btn").c_str()))
            m_show_add = ! m_show_add;
    } else {
        m_show_add = false;
    }
    if (m_show_add) {
        // An inline list rather than a popup inside the gizmo window.
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const int   shown = std::min<int>(int(candidates.size()), 6);
        ImGui::BeginChild("##hae_add_list", ImVec2(win_w - 16.f, row_h * float(shown) + 2.f * ImGui::GetStyle().WindowPadding.y + 2.f), true);
        for (size_t e : candidates)
            if (ImGui::Selectable((_u8L(effects()[e].label) + "##hae_add_" + std::to_string(e)).c_str())) {
                m_states[e].added  = true;
                m_states[e].nodes.clear();
                m_states[e].interp = effects()[e].default_interp;
                m_active_effect    = int(e);
                m_show_add         = false;
                // No commit: no points, no key. The first point commits it.
            }
        ImGui::EndChild();
    }

    if (listed.empty()) {
        ImGui::Dummy(ImVec2(0.f, 4.f));
        ImGui::TextWrapped("%s", _u8L("No effect on this object yet. Press Add to drive a setting with a curve along the object's height.").c_str());
        return;
    }
    if (m_active_effect < 0 || ! (size_t(m_active_effect) < m_states.size() && m_states[m_active_effect].added))
        m_active_effect = int(listed.front());

    // One row per effect, with its point count, a sparkline of its curve and a delete button.
    int remove_at = -1;
    {
        const ImGuiStyle &style   = ImGui::GetStyle();
        const float       row_h   = ImGui::GetTextLineHeightWithSpacing();
        const int         shown   = std::min<int>(int(listed.size()), 6);
        const float       child_h = row_h * float(shown) + 2.f * style.WindowPadding.y + 2.f;
        ImGui::BeginChild("##hae_effect_list", ImVec2(win_w - 16.f, child_h), true);
        const float btn_w = ImGui::CalcTextSize("x").x + 2.f * style.FramePadding.x;
        const float row_w = ImGui::GetContentRegionAvail().x;
        for (size_t e : listed) {
            const EffectDef &def = effects()[e];
            const std::string row = _u8L(def.label) + "   " +
                                    (effect_in_use(e) ? Slic3r::GUI::format(into_u8(_L_PLURAL("%1% point", "%1% points", m_states[e].nodes.size())),
                                                                            m_states[e].nodes.size()) :
                                                        _u8L("(no points yet)"));
            // AllowItemOverlap plus SetItemAllowOverlap, so the delete button inside the row gets the hover.
            if (ImGui::Selectable((row + "##hae_sel" + std::to_string(e)).c_str(), m_active_effect == int(e),
                                  ImGuiSelectableFlags_AllowItemOverlap))
                m_active_effect = int(e);
            ImGui::SetItemAllowOverlap();
            {
                ImDrawList  *rdl  = ImGui::GetWindowDrawList();
                const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                if (m_active_effect == int(e))
                    rdl->AddRectFilled(ImVec2(rmin.x, rmin.y + 1.f), ImVec2(rmin.x + 2.5f, rmax.y - 1.f), neo_col_u32(NeoCol::AccentBright), 1.f);
                // Sparkline, Z left to right and value bottom to top: it answers "which shape is this".
                const auto  cur = curve_of(e);
                const float sw = 46.f, pad = 4.f;
                const float sx1 = rmax.x - btn_w - 6.f, sx0 = sx1 - sw;
                const float sy0 = rmin.y + pad, sy1 = rmax.y - pad;
                if (! cur.empty() && m_states[e].nodes.size() >= 2 && sx0 > rmin.x + 40.f && sy1 > sy0) {
                    const double  span = std::max(1e-9, def.range_hi - def.range_lo);
                    constexpr int kS   = 24;
                    ImVec2        sp[kS + 1];
                    for (int s = 0; s <= kS; ++s) {
                        const double z = m_object_height * double(s) / double(kS);
                        const double v = std::clamp((cur.at(z, 0.) - def.range_lo) / span, 0.0, 1.0);
                        sp[s]          = ImVec2(sx0 + sw * float(s) / float(kS), sy1 - float(v) * (sy1 - sy0));
                    }
                    rdl->AddPolyline(sp, kS + 1,
                                     m_active_effect == int(e) ? neo_col_u32(NeoCol::AccentBright) : neo_fade(NeoCol::AccentBright, 0.5f),
                                     ImDrawFlags_None, 1.6f);
                }
            }
            ImGui::SameLine(row_w - btn_w);
            ImGui::PushID(int(e));
            if (ImGui::SmallButton("x"))
                remove_at = int(e); // applied after the loop, which iterates `listed`
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Remove this effect from the object").c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (remove_at >= 0) {
        remove_effect(size_t(remove_at));
        return; // the rest of the panel described a row that is gone
    }

    ImGui::Dummy(ImVec2(0.f, 4.f));
    render_editor(win_w - 16.0f, 260.0f);
    ImGui::Dummy(ImVec2(0.f, 6.f));

    const size_t       e   = size_t(m_active_effect);
    const EffectDef   &def = effects()[e];
    const std::string  key = def.config_key;
    bool               stepped = interp_of(e) == HeightAdaptive::Interp::Stepped;
    const ModelObject *mo      = this->model_object();

    ImGui::Separator();
    ImGui::TextUnformatted(Slic3r::GUI::format(_u8L("Range: %1% to %2% %3%"), format(float(def.range_lo), 2), format(float(def.range_hi), 2), def.units).c_str());
    if (def.is_expansion)
        help_marker(_u8L("Past 2 mm the sparse infill regions fail to generate. The limit is measured, not a round number."));

    if (def.mode_selectable) {
        ImGui::TextUnformatted(_u8L("Transition").c_str());
        help_marker(_u8L("Steps: the value is constant inside each band and changes in one jump at a layer boundary, and every point "
                         "is snapped to a real layer. Safest for sparse infill width, because the lines of every layer keep stacking "
                         "on the ones below.\n\nRamp: the value changes continuously with height."));
        ImGui::SameLine();
        bool changed = false;
        // TRN Transition mode of a height curve: the value changes in jumps at layer boundaries.
        if (ImGui::RadioButton((_u8L_CONTEXT("Steps", "Height curve") + "##hae_mode_step").c_str(), stepped)) {
            m_states[e].interp = HeightAdaptive::Interp::Stepped;
            changed            = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton((_u8L("Ramp") + "##hae_mode_smooth").c_str(), ! stepped)) {
            m_states[e].interp = HeightAdaptive::Interp::Smooth;
            changed            = true;
        }
        if (changed) {
            // Steps re-snap every point: a step edge in the middle of a layer cannot be seen or predicted.
            if (m_states[e].interp == HeightAdaptive::Interp::Stepped)
                for (Node &n : m_states[e].nodes)
                    n.z_mm = snap_z(n.z_mm);
            commit(_u8L("Height adaptive effects - Transition mode"));
        }
        stepped = interp_of(e) == HeightAdaptive::Interp::Stepped;
    }
    if (! stepped)
        ImGui::Checkbox(_u8L("Snap points to layer boundaries").c_str(), &m_snap_to_layer);

    // The measured shift a ramp causes, instead of forbidding ramps.
    if (! stepped && key == "neotko_hae_infill_width") {
        double       pct   = 0.;
        const double shift = misalignment_per_layer_mm(e, &pct);
        if (shift > 0.) {
            const bool risky = pct > 25.;
            if (risky)
                ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextUnformatted(Slic3r::GUI::format(_u8L("Line shift per layer: %1% mm (%2% of the line width)"), format(float(shift), 3),
                                                       format(float(pct), 0) + "%")
                                       .c_str());
            if (risky)
                ImGui::PopStyleColor();
            help_marker(_u8L("How far the infill lines of one layer land from the ones below, at the far edge of the object. Above "
                             "roughly a quarter of the line width the sparse infill stops stacking. Flatten the curve, or press "
                             "Staircase to keep the shape with aligned bands."));
        }
        ImGui::SetNextItemWidth(110.f);
        ImGui::SliderInt((_u8L("Bands") + "##hae_bands").c_str(), &m_quantize_bands, 2, 20);
        ImGui::SameLine();
        if (ImGui::Button((_u8L("Staircase") + "##hae_quantize").c_str())) {
            quantize_to_steps(e, m_quantize_bands);
            commit(_u8L("Height adaptive effects - Staircase"));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Turns this ramp into that many aligned steps.").c_str());
    }

    // A band shorter than about four layers is not worth the jump.
    if (stepped && m_states[e].nodes.size() >= 2) {
        const auto &ns       = m_states[e].nodes;
        int         shortest = INT_MAX;
        for (size_t i = 0; i + 1 < ns.size(); ++i)
            shortest = std::min(shortest, layers_between(ns[i].z_mm, ns[i + 1].z_mm));
        if (shortest != INT_MAX && shortest < 4) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextWrapped("%s", Slic3r::GUI::format(into_u8(_L_PLURAL("The shortest band is %1% layer. Under about 4 layers the jump costs more than it gives.",
                                                                   "The shortest band is %1% layers. Under about 4 layers the jump costs more than it gives.",
                                                                   size_t(shortest))),
                                                         shortest)
                                         .c_str());
            ImGui::PopStyleColor();
        }
    }

    // The fuzzy curves adjust fuzzy skin; they do not turn it on.
    if (def.is_fuzzy) {
        std::string fz;
        if (mo != nullptr && mo->config.has("fuzzy_skin")) {
            if (const ConfigOption *o = mo->config.option("fuzzy_skin"))
                fz = o->serialize();
        } else if (const ConfigOption *o = wxGetApp().preset_bundle->full_config().option("fuzzy_skin")) {
            fz = o->serialize();
        }
        if (fz.empty() || fz == "none") {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextWrapped("%s", _u8L("Fuzzy skin is off for this object, so this curve does nothing. It adjusts the fuzzy skin; it does not turn it on.").c_str());
            ImGui::PopStyleColor();
        } else if (key == "neotko_hae_fuzzy_thickness") {
            ImGui::TextDisabled("%s", _u8L("Reaching 0 switches fuzzy skin off.").c_str());
            help_marker(_u8L("Where the curve reaches 0 the fuzzy skin is switched off for that layer, not just flattened. "
                             "Otherwise a zero-amplitude fuzzy skin would still resample every wall and bloat the G-code."));
        }
    }

    // The slicer drops XY compensation on painted objects, and the curve with it.
    if (def.is_expansion && mo != nullptr && (mo->is_mm_painted() || mo->is_fuzzy_skin_painted())) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Forbid));
        ImGui::TextWrapped("%s", _u8L("This object is color or fuzzy skin painted. The slicer drops XY size compensation on painted objects, "
                                      "so this curve will have no effect.").c_str());
        ImGui::PopStyleColor();
    }

    // Tension per segment, ramps only.
    if (! stepped && m_states[e].nodes.size() > 1) {
        ImGui::Separator();
        ImGui::TextUnformatted(_u8L("Tension per segment").c_str());
        help_marker(_u8L("0 = straight segment, 1 = smooth curve."));
        auto                    &ns = m_states[e].nodes;
        std::vector<std::string> range_lbl(ns.size() - 1);
        float                    range_w = 0.f;
        for (size_t i = 0; i + 1 < ns.size(); ++i) {
            range_lbl[i] = format(float(ns[i].z_mm), 1) + " - " + format(float(ns[i + 1].z_mm), 1) + " mm";
            range_w      = std::max(range_w, ImGui::CalcTextSize(range_lbl[i].c_str()).x);
        }
        range_w += ImGui::GetStyle().ItemSpacing.x;
        const float slider_w = win_w - 16.f - range_w - 2.f * ImGui::GetStyle().ItemSpacing.x;
        for (size_t i = 0; i + 1 < ns.size(); ++i) {
            float t = float(ns[i].tension);
            ImGui::TextDisabled("%s", range_lbl[i].c_str());
            ImGui::SameLine(range_w);
            ImGui::SetNextItemWidth(slider_w);
            if (ImGui::SliderFloat(("##hae_tension" + std::to_string(i)).c_str(), &t, 0.f, 1.f)) {
                ns[i].tension = std::clamp(double(t), 0., 1.);
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                commit(_u8L("Height adaptive effects - Tension"));
        }
    }

    // The points as numbers, for typing an exact value.
    if (! m_states[e].nodes.empty()) {
        ImGui::Separator();
        if (ImGui::Button(((m_show_points ? _u8L("Close Z points") : _u8L("Open Z points")) + "##hae_points_toggle").c_str()))
            m_show_points = ! m_show_points;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Slic3r::GUI::format(into_u8(_L_PLURAL("%1% point", "%1% points", m_states[e].nodes.size())), m_states[e].nodes.size()).c_str());
    }
    if (m_show_points && ! m_states[e].nodes.empty()) {
        ImGui::TextUnformatted((def.units[0] != '\0' ? Slic3r::GUI::format(_u8L("Z (mm) / value (%1%)"), def.units) : _u8L("Z (mm) / value")).c_str());
        auto &ns = m_states[e].nodes;
        // Erasing and sorting wait until after the loop.
        int  erase_at   = -1;
        bool edit_ended = false;
        for (size_t i = 0; i < ns.size(); ++i) {
            ImGui::PushID(int(i));
            float z = float(ns[i].z_mm);
            float v = float(ns[i].value);
            ImGui::SetNextItemWidth(78.f);
            const bool z_edited = ImGui::DragFloat("##hae_z", &z, 0.05f, 0.f, float(m_object_height), "%.2f");
            // Asked right after its own widget: it always refers to the last item.
            edit_ended = ImGui::IsItemDeactivatedAfterEdit() || edit_ended;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(78.f);
            const bool v_edited = ImGui::DragFloat("##hae_v", &v, 0.005f, float(def.range_lo), float(def.range_hi), "%.3f");
            edit_ended          = ImGui::IsItemDeactivatedAfterEdit() || edit_ended;
            if (z_edited || v_edited) {
                ns[i].z_mm  = std::clamp(double(z), 0., m_object_height);
                ns[i].value = std::clamp(double(v), def.range_lo, def.range_hi);
                m_parent.set_as_dirty();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Slic3r::GUI::format(_u8L("layer %1%"), std::max(1, layers_between(0., ns[i].z_mm + 1e-9))).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                erase_at = int(i);
            ImGui::PopID();
        }
        if (erase_at >= 0) {
            ns.erase(ns.begin() + erase_at);
            commit(_u8L("Height adaptive effects - Delete point"));
        } else if (edit_ended) {
            if (stepped || m_snap_to_layer)
                for (Node &n : ns)
                    n.z_mm = snap_z(n.z_mm);
            std::stable_sort(ns.begin(), ns.end(), [](const Node &a, const Node &b) { return a.z_mm < b.z_mm; });
            commit(_u8L("Height adaptive effects - Edit point"));
        }
    }

    ImGui::Separator();
    auto preset_button = [&](const char *id, const std::string &label) {
        if (ImGui::Button((label + "##hae_preset_" + id).c_str())) {
            apply_preset(e, id);
            commit(_u8L("Height adaptive effects - Preset"));
        }
    };
    // Presets hold absolute values in the effect's units, so only the effects with sensible shapes have them.
    if (def.is_expansion || key == "neotko_hae_fuzzy_thickness" || key == "neotko_hae_infill_width") {
        // TRN Predefined curve shapes of a height adaptive effect.
        ImGui::TextUnformatted(_u8L_CONTEXT("Presets", "Height curve").c_str());
        if (def.is_expansion) {
            preset_button("bullnose", _u8L("Bullnose"));
            ImGui::SameLine();
            preset_button("chamfer", _u8L("Chamfer"));
            ImGui::SameLine();
            preset_button("barrel", _u8L("Barrel"));
        } else if (key == "neotko_hae_fuzzy_thickness") {
            preset_button("fade_in", _u8L("Fade in"));
            ImGui::SameLine();
            preset_button("fade_out", _u8L("Fade out"));
            ImGui::SameLine();
            preset_button("fade_band", _u8L("Rough middle"));
        } else {
            preset_button("fine_bottom", _u8L("Fine low, coarse high"));
            ImGui::SameLine();
            preset_button("fine_top", _u8L("Coarse low, fine high"));
        }
        ImGui::SameLine();
    }
    if (ImGui::Button((_u8L("Clear") + "##hae_clear").c_str())) {
        m_states[e].nodes.clear();
        commit(_u8L("Height adaptive effects - Clear"));
    }

    ImGui::Separator();
    ImGui::TextUnformatted(Slic3r::GUI::format(into_u8(_L_PLURAL("Object height: %1% mm, %2% layer", "Object height: %1% mm, %2% layers", m_layer_tops.size())),
                                               format(float(m_object_height), 2), m_layer_tops.size())
                               .c_str());
    if (m_slicing_params.has_raft())
        ImGui::TextWrapped("%s", _u8L("Z is measured from the bottom of the object, not from the bed. The raft is not part of this axis.").c_str());
    if (ImGui::Button(_u8L("Refresh layers").c_str()))
        rebuild_layer_bands();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Re-reads the object's real layers. Needed after changing the adaptive layer height profile.").c_str());
}

} // namespace GUI
} // namespace Slic3r
