// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

#include "GLGizmoBase.hpp"

#include <string>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;

// Holds the GLGizmosManager slot of a NeotkoCM gizmo that has not been ported yet, so that the
// batches porting them do not all edit the gizmo list. It is never shown in the toolbar nor opened.
class GLGizmoNeotkoStub : public GLGizmoBase
{
public:
    GLGizmoNeotkoStub(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
        : GLGizmoBase(parent, icon_filename, sprite_id)
    {}

protected:
    bool        on_init() override { return true; }
    std::string on_get_name() const override { return {}; }
    bool        on_is_activable() const override { return false; }
    bool        on_is_selectable() const override { return false; }
    void        on_render() override {}
};

} // namespace GUI
} // namespace Slic3r
