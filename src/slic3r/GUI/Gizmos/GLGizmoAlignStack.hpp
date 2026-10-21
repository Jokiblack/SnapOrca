// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
// NEOTKO_ALIGNSTACK_TAG_START
#pragma once

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/NeotkoSurfaceExtrude.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class wxMouseEvent;

namespace Slic3r {
class ModelObject;
class ModelVolume;
namespace GUI {

class GLCanvas3D;

// Align & Construct: relates exactly two objects, #1 (the anchor, it stays) and #2 (the one that
// moves). A third click swaps out #2. Each one is a whole object or one part of an object made of
// several parts. Only shown with the "Experimental features (Neotko)" preference.
//  - Place #2: next to #1 on a side, edges aligned, on top, centred, or back on the bed. Hovering a
//    button draws #2 translucent where the click would leave it, with the same maths as the click.
//  - Face of #1: pick a face in the viewport, drop #2 at its height, or extrude a solid from it
//    (Construct), as a part of #1's object or as a new object.
//  - Boolean of #1 with #2 (union, difference, intersection) without assembling them first.
// It only changes the model (moves, new parts or objects); it never touches the slicing.
class GLGizmoAlignStack : public GLGizmoBase
{
public:
    GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent& mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    // --- Order (#1 anchor, #2 the one that moves) ---------------------------
    // The first click on an object takes it whole, a second click on it takes just the part touched.
    struct Pick {
        int      obj = -1;
        int      vol = -1; // index into ModelObject::volumes, -1 = the whole object
        ObjectID vol_id;   // the part's id, so a part that changes index is followed
        bool is_part() const { return vol >= 0; }
        bool same(const Pick& o) const { return obj == o.obj && vol == o.vol; }
    };
    std::vector<Pick> m_ordered;

    // --- Params ---------------------------------------------------------------
    float m_epsilon_mm = 0.01f; // Z gap when stacking
    bool  m_flush_mode = false; // false = next to (contact), true = align edges

    // --- Face pick on #1 ------------------------------------------------------
    bool   m_face_pick_mode      = false;
    bool   m_has_picked_face     = false;
    double m_picked_face_world_z = 0.0;
    std::unique_ptr<MeshRaycaster> m_face_raycaster;
    Pick                           m_face_raycaster_pick; // what the raycaster was built for
    Transform3d                    m_face_raycaster_world_trafo{ Transform3d::Identity() };
    // Object-space mesh of #1 with cached normals and adjacency for the coplanar flood-fill.
    TriangleMesh         m_face_mesh;
    std::vector<Vec3f>   m_face_normals;
    std::vector<Vec3i32> m_face_neighbors;

    Vec2d   m_hover_mouse_pos{ Vec2d::Zero() };
    bool    m_have_hover_pos     = false;
    int     m_hover_facet_idx    = -1;
    int     m_picked_facet_idx   = -1;
    GLModel m_hover_face_model;  // teal overlay
    int     m_hover_model_facet  = -1;
    GLModel m_picked_face_model; // green overlay
    int     m_picked_model_facet = -1;

    // --- Scene tint: the original colours of the GLVolumes we tinted ---------
    // Keyed by (object_idx, volume_idx, instance_idx), so it survives volume rebuilds; the bool is
    // the volume's force_native_color, which lets a painted volume turn see-through.
    std::map<std::tuple<int, int, int>, std::pair<ColorRGBA, bool>> m_saved_colors;

    // A layer of the order colour drawn over each pick: the tint does not show on painted volumes.
    GLModel m_pick_overlay[2];
    Pick    m_pick_overlay_pick[2];
    size_t  m_pick_overlay_volumes[2] = { 0, 0 };

    // --- Placement --------------------------------------------------------------
    enum class Op { None, XNeg, XPos, YNeg, YPos, CenterXY, OnTop, CenterZ, Bed, Face };
    Op      m_hover_op = Op::None; // the placing button under the mouse (set by the panel)
    GLModel m_ghost_mesh;          // #2's own mesh, drawn translucent where the click would leave it
    Pick    m_ghost_mesh_pick;
    size_t  m_ghost_mesh_volumes = 0;
    bool    m_ghost_visible      = false;
    Vec3d   m_ghost_from{ Vec3d::Zero() };
    Vec3d   m_ghost_to{ Vec3d::Zero() };

    // The move `op` would make, with the exact maths the click uses. False if it can't be done.
    bool  op_delta(Op op, Vec3d& delta) const;
    void  apply_op(Op op);
    void  swap_order();
    Vec3d compute_place_delta(int axis, int dir, bool flush, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;
    Vec3d compute_center_delta(int axis, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;
    void  translate_pick(const Pick& p, const Vec3d& delta);

    // --- Order helpers ------------------------------------------------------------
    void seed_order_from_selection();
    void prune_dead_objects();
    void click_object(int object_idx, int vol_idx); // vol_idx -1 = not on a part
    void remove_pick(size_t i);
    void clear_order();
    void sync_selection_of(int object_idx); // the object stays selected while a pick uses it
    int  ordered_obj(size_t i) const { return i < m_ordered.size() ? m_ordered[i].obj : -1; }

    const ModelObject* pick_object(const Pick& p) const; // nullptr when the pick is stale
    const ModelVolume* pick_volume(const Pick& p) const; // nullptr for a whole object
    Transform3d        pick_trafo(const Pick& p) const;  // pick_mesh() -> world (first instance)
    TriangleMesh       pick_mesh(const Pick& p) const;   // object raw mesh, or the part's mesh
    std::string        pick_name(const Pick& p) const;
    int                model_part_count(int object_idx) const;
    BoundingBoxf3      world_bbox_of(const Pick& p) const;
    // #2's object when #2 is a whole object, else nullptr. Its Auto Drop decides whether it can stay
    // on top of #1: with it on, Orca drops the object back to the bed after every move.
    ModelObject*       second_whole_object() const;

    void apply_highlight();
    void restore_highlight();
    void refresh_highlight();

    // --- Rendering ------------------------------------------------------------------
    void render_panel_body(); // apart, so that no early exit can skip the style pops
    void render_pick_overlays();
    void render_badges();
    void render_place_ghost();
    void clear_face_pick();
    void ensure_face_raycaster_for_A();
    void update_hover_face(const Vec2d& mouse_pos);
    // The connected coplanar region grown from `facet_idx`, lifted a hair along its normals.
    void build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col, float cos_thresh);
    void render_face_highlights();

    // NEOTKO_CONSTRUCT_TAG_START — Construct: a solid extruded from the picked face.
    float m_construct_height    = 5.0f;  // mm
    float m_construct_offset    = 0.0f;  // mm, negative shrinks the outline
    bool  m_construct_both      = false; // half the height outwards, half inwards
    bool  m_construct_holes     = false; // extrude the holes of the face (filled) instead of the face
    bool  m_construct_as_object = false; // false = part of #1's object, true = separate object
    // Translucent preview, in world coordinates, rebuilt only when one of its inputs changes.
    GLModel     m_construct_ghost;
    int         m_construct_ghost_facet  = -2;
    float       m_construct_ghost_height = 0.f;
    float       m_construct_ghost_offset = 0.f;
    bool        m_construct_ghost_both   = false;
    bool        m_construct_ghost_holes  = false;
    Transform3d m_construct_ghost_trafo{ Transform3d::Identity() };
    std::string m_construct_block_reason; // why "Create" is off (empty = it can create)

    // Builds the solid (world coordinates) from the picked face. False + `why` when it can't.
    // preview = coarser steps for the curved solid, so dragging the curve stays fluid.
    bool build_construct_mesh(indexed_triangle_set& out, std::string& why, bool preview);
    void update_construct_ghost();
    void render_construct_ghost();
    void apply_construct();

    // Boolean of #1 with #2. op is the mcut name ("UNION", "A_NOT_B", "INTERSECTION"). Two parts of
    // the same object give a part of that object, in place; anything else gives a new object.
    bool        m_bool_keep_inputs = false; // false = #1 and #2 are removed after the operation
    std::string m_bool_error;               // last failure, shown under the buttons
    void apply_boolean(const std::string& op, const std::string& suffix);

    // Extrusion along an editable curve. Hidden: only offered in the NeoDebug console mode.
    bool                                   m_curve_on = false;
    std::vector<SurfaceExtrude::CurveNode> m_curve;
    int      m_curve_sel   = -1;  // 2*i = node i, 2*i+1 = the handle of the span i -> i+1
    int      m_curve_drag  = -1;  // same coding, while the mouse holds one
    float    m_curve_range = 1.f; // half-width of the editor in mm (frozen while dragging)
    unsigned m_curve_rev   = 0;   // bumped on every edit, so the ghost knows to rebuild
    unsigned m_construct_ghost_curve_rev = 0;
    bool     m_construct_ghost_curve_on  = false;
    bool curve_active() const;
    void reset_curve();
    void set_curve_point(int code, double t, double e); // keeps heights in order
    void render_curve_editor();
    // NEOTKO_CONSTRUCT_TAG_END
};

} // namespace GUI
} // namespace Slic3r
// NEOTKO_ALIGNSTACK_TAG_END
