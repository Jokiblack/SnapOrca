// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

// Surface Effect Profiles: the saved "Sandwich" recipes that the ColorStitch Painter paints onto the
// faces of a model. A painted triangle stores a slot number (ModelVolume::colorstitch_paint_facets),
// each volume maps its slots to profile ids (ModelVolume::colorstitch_slot_to_profile_id) and the
// profiles themselves live in the project, in Model::surface_effect_profiles.
//
// Neotko keeps the library in a process-wide singleton. Here it belongs to the Model, so that the
// Print slices with its own copy (Print::apply() copies it with the objects), two projects never
// share a palette and the TBB fill threads only ever read it.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
struct SurfacePassStack;

enum class SurfaceEffectKind : uint8_t {
    ColorStitch = 1,
    PathBlend   = 2,
    MultiPass   = 3,
};

// Serialized config key -> value pairs (DynamicPrintConfig::opt_serialize / set_deserialize_strict).
// `present` tells a saved snapshot apart from an empty default.
struct SurfaceEffectPayload {
    bool                               present = false;
    std::map<std::string, std::string> kv;

    bool operator==(const SurfaceEffectPayload &rhs) const { return present == rhs.present && kv == rhs.kv; }
    bool operator!=(const SurfaceEffectPayload &rhs) const { return !(*this == rhs); }
};

struct SurfaceEffectProfile {
    int         id = 0;            // assigned by the library, 1-based
    std::string name;
    uint32_t    preview_argb = 0;  // swatch hint for the painter
    // Working colour created on the fly by the painter, as opposed to a saved palette entry.
    bool        auto_generated = false;

    // Flattened payloads, read by the slicer when a pass carries no key/value set of its own.
    SurfaceEffectPayload colorstitch;
    SurfaceEffectPayload pathblend;
    SurfaceEffectPayload multipass;

    // The recipe itself: one SurfacePassStack::to_json() per zone. Empty = no effect on that zone.
    std::string stack_top_json;
    std::string stack_penu_json;
    std::string stack_bottom_json;

    bool operator==(const SurfaceEffectProfile &rhs) const;
    bool operator!=(const SurfaceEffectProfile &rhs) const { return !(*this == rhs); }
};

class SurfaceEffectProfileManager
{
public:
    // Assigns a new id and returns it.
    int  add(SurfaceEffectProfile profile);
    // Inserts keeping the profile's own id (paint already refers to it). False if the id is taken or invalid.
    bool adopt(SurfaceEffectProfile profile);
    bool remove(int id);
    bool rename(int id, const std::string &new_name);

    const SurfaceEffectProfile              *find(int id) const;
    SurfaceEffectProfile                    *find_mut(int id);
    const std::vector<SurfaceEffectProfile> &list() const { return m_profiles; }
    size_t                                   size() const { return m_profiles.size(); }
    bool                                     empty() const { return m_profiles.empty(); }
    int                                      next_id() const { return m_next_id; }
    void                                     clear();

    bool operator==(const SurfaceEffectProfileManager &rhs) const { return m_next_id == rhs.m_next_id && m_profiles == rhs.m_profiles; }
    bool operator!=(const SurfaceEffectProfileManager &rhs) const { return !(*this == rhs); }

    // Snapshot of the given keys of a config, present if any key was found.
    static SurfaceEffectPayload snapshot_keys(const DynamicPrintConfig &cfg, const std::vector<std::string> &keys);
    static void                 restore_keys(DynamicPrintConfig &cfg, const SurfaceEffectPayload &payload);
    // Lifts the key/value sets of the ColorStitch and PathBlend passes of the stacks into the profile
    // payloads, so that a profile that only carries stacks is still picked up by the slicer.
    static void payload_from_stacks(const SurfacePassStack &top, const SurfacePassStack &penu, SurfaceEffectProfile &p);

    // Canonical key lists of the three payloads.
    static const std::vector<std::string> &colorstitch_keys();
    static const std::vector<std::string> &multipass_keys();
    static const std::vector<std::string> &pathblend_keys();

    // JSON round trip, as stored in the 3MF. Unknown fields are ignored on read.
    std::string to_json() const;
    bool        from_json(const std::string &text);

private:
    std::vector<SurfaceEffectProfile> m_profiles;
    int                               m_next_id = 1;
};

} // namespace Slic3r
