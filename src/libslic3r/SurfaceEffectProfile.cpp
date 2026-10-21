// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include "SurfaceEffectProfile.hpp"

#include "ColorStitch.hpp"
#include "Config.hpp"
#include "NeoDebug.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

bool SurfaceEffectProfile::operator==(const SurfaceEffectProfile &rhs) const
{
    return id == rhs.id && name == rhs.name && preview_argb == rhs.preview_argb && auto_generated == rhs.auto_generated &&
           colorstitch == rhs.colorstitch && pathblend == rhs.pathblend && multipass == rhs.multipass &&
           stack_top_json == rhs.stack_top_json && stack_penu_json == rhs.stack_penu_json && stack_bottom_json == rhs.stack_bottom_json;
}

int SurfaceEffectProfileManager::add(SurfaceEffectProfile profile)
{
    profile.id = m_next_id++;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_profiles.push_back(std::move(profile));
    const SurfaceEffectProfile &p = m_profiles.back();
    NEOTKO_LOG(PROFILE, "MGR add id=" << p.id << " name='" << p.name << "' total=" << m_profiles.size());
    return p.id;
}

bool SurfaceEffectProfileManager::adopt(SurfaceEffectProfile profile)
{
    if (profile.id <= 0 || find(profile.id) != nullptr)
        return false;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_next_id = std::max(m_next_id, profile.id + 1);
    m_profiles.push_back(std::move(profile));
    return true;
}

bool SurfaceEffectProfileManager::remove(int id)
{
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it)
        if (it->id == id) {
            m_profiles.erase(it);
            return true;
        }
    return false;
}

bool SurfaceEffectProfileManager::rename(int id, const std::string &new_name)
{
    if (SurfaceEffectProfile *p = find_mut(id)) {
        p->name = new_name;
        return true;
    }
    return false;
}

const SurfaceEffectProfile *SurfaceEffectProfileManager::find(int id) const
{
    for (const SurfaceEffectProfile &p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

SurfaceEffectProfile *SurfaceEffectProfileManager::find_mut(int id)
{
    for (SurfaceEffectProfile &p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

void SurfaceEffectProfileManager::clear()
{
    m_profiles.clear();
    m_next_id = 1;
}

SurfaceEffectPayload SurfaceEffectProfileManager::snapshot_keys(const DynamicPrintConfig &cfg, const std::vector<std::string> &keys)
{
    SurfaceEffectPayload out;
    for (const std::string &k : keys)
        if (const ConfigOption *opt = cfg.option(k))
            out.kv[k] = opt->serialize();
    out.present = !out.kv.empty();
    return out;
}

void SurfaceEffectProfileManager::restore_keys(DynamicPrintConfig &cfg, const SurfaceEffectPayload &payload)
{
    if (!payload.present)
        return;
    for (const auto &[k, v] : payload.kv)
        if (cfg.has(k))
            cfg.set_deserialize_strict(k, v);
}

void SurfaceEffectProfileManager::payload_from_stacks(const SurfacePassStack &top, const SurfacePassStack &penu, SurfaceEffectProfile &p)
{
    bool cm_top = false, cm_penu = false, pb_top = false, pb_penu = false;
    auto lift = [&p](const SurfacePassStack &st, bool &cm_hit, bool &pb_hit) {
        for (const SurfacePass &pass : st.passes) {
            if (pass.kind == SurfacePassKind::ColorStitch && pass.colorstitch.present) {
                // The keys are already role prefixed (interlayer_colormix_ / interlayer_colormix_penu_).
                for (const auto &[k, v] : pass.colorstitch.kv)
                    p.colorstitch.kv[k] = v;
                cm_hit = true;
            } else if (pass.kind == SurfacePassKind::PathBlend && pass.pathblend.present) {
                for (const auto &[k, v] : pass.pathblend.kv)
                    p.pathblend.kv[k] = v;
                pb_hit = true;
            }
        }
    };
    lift(top, cm_top, pb_top);
    lift(penu, cm_penu, pb_penu);

    // 0 = both zones, 1 = top only, 2 = penultimate only.
    auto surface_enum = [](bool t, bool pen) -> std::string { return (t && pen) ? "0" : (t ? "1" : "2"); };
    if (cm_top || cm_penu) {
        p.colorstitch.present                           = true;
        p.colorstitch.kv["interlayer_colormix_enabled"] = "1";
        p.colorstitch.kv["interlayer_colormix_surface"] = surface_enum(cm_top, cm_penu);
    }
    if (pb_top || pb_penu) {
        p.pathblend.present                       = true;
        p.pathblend.kv["multipass_path_gradient"] = "1";
        p.pathblend.kv["pathblend_surface"]       = surface_enum(pb_top, pb_penu);
    }
}

const std::vector<std::string> &SurfaceEffectProfileManager::colorstitch_keys()
{
    static const std::vector<std::string> k = {
        "interlayer_colormix_enabled", "interlayer_colormix_surface", "interlayer_colormix_filament_filter",
        "interlayer_colormix_min_length", "interlayer_colormix_use_virtual", "interlayer_colormix_top_zone",
        "interlayer_colormix_penu_zone", "interlayer_colormix_pattern_top", "interlayer_colormix_pattern_penultimate",
        "interlayer_colormix_mode", "interlayer_colormix_pct_a", "interlayer_colormix_pct_b", "interlayer_colormix_easing",
        "interlayer_colormix_gamma", "interlayer_colormix_min_surface_lines", "interlayer_colormix_overlap",
        "interlayer_colormix_invert", "interlayer_colormix_repetitions", "interlayer_colormix_band_count_a",
        "interlayer_colormix_band_count_b", "interlayer_colormix_band_count_c", "interlayer_colormix_band_count_d",
        "interlayer_colormix_band_mm_a", "interlayer_colormix_band_mm_b", "interlayer_colormix_band_mm_c",
        "interlayer_colormix_band_mm_d", "interlayer_colormix_gradient_span_mm", "interlayer_colormix_tool_a",
        "interlayer_colormix_tool_b", "interlayer_colormix_tool_c", "interlayer_colormix_tool_d",
        "interlayer_colormix_penu_mode", "interlayer_colormix_penu_pct_a", "interlayer_colormix_penu_pct_b",
        "interlayer_colormix_penu_easing", "interlayer_colormix_penu_gamma", "interlayer_colormix_penu_min_surface_lines",
        "interlayer_colormix_penu_overlap", "interlayer_colormix_penu_invert", "interlayer_colormix_penu_repetitions",
        "interlayer_colormix_penu_band_count_a", "interlayer_colormix_penu_band_count_b",
        "interlayer_colormix_penu_band_count_c", "interlayer_colormix_penu_band_count_d",
        "interlayer_colormix_penu_band_mm_a", "interlayer_colormix_penu_band_mm_b", "interlayer_colormix_penu_band_mm_c",
        "interlayer_colormix_penu_band_mm_d", "interlayer_colormix_penu_gradient_span_mm",
        "interlayer_colormix_penu_tool_a", "interlayer_colormix_penu_tool_b", "interlayer_colormix_penu_tool_c",
        "interlayer_colormix_penu_tool_d",
    };
    return k;
}

const std::vector<std::string> &SurfaceEffectProfileManager::multipass_keys()
{
    // multipass_prime_volume is print wide and stays out of the profile.
    static const std::vector<std::string> k = []() {
        std::vector<std::string> keys = { "multipass_enabled", "multipass_surface", "multipass_num_passes", "multipass_vary_pattern",
                                          "multipass_pa_mode", "multipass_pa_value", "multipass_perimeter_override",
                                          "multipass_path_gradient", "penultimate_multipass_enabled",
                                          "penultimate_multipass_num_passes", "penultimate_multipass_vary_pattern" };
        for (const char *prefix : { "multipass_", "penultimate_multipass_" })
            for (const char *base : { "tool_", "width_ratio_", "angle_", "fan_", "speed_pct_", "gcode_start_", "gcode_end_" })
                for (int i = 1; i <= 3; ++i)
                    keys.emplace_back(std::string(prefix) + base + std::to_string(i));
        return keys;
    }();
    return k;
}

const std::vector<std::string> &SurfaceEffectProfileManager::pathblend_keys()
{
    static const std::vector<std::string> k = {
        "multipass_path_gradient", "pathblend_num_passes", "pathblend_tool_1", "pathblend_tool_2", "pathblend_tool_3",
        "pathblend_tool_4", "pathblend_layer_ratio_1", "pathblend_layer_ratio_2", "pathblend_layer_ratio_3",
        "pathblend_layer_ratio_4", "pathblend_min_ratio", "pathblend_max_ratio", "pathblend_ease_mode",
        "pathblend_surface", "pathblend_invert_gradient", "pathblend_fill_angle",
    };
    return k;
}

// Schema (Neotko's, kept for 3MF compatibility):
//   { "v": 2, "next_id": 7, "profiles": [ { "id": 1, "name": "...", "preview_argb": 0,
//     "colormix": { "present": true, "kv": { ... } }, "pathblend": {...}, "multipass": {...},
//     "stack_top_json": "...", "stack_penu_json": "...", "stack_bottom_json": "...", "auto": false }, ... ] }
static nlohmann::json payload_to_json(const SurfaceEffectPayload &p)
{
    nlohmann::json j;
    j["present"] = p.present;
    j["kv"]      = p.kv;
    return j;
}

static SurfaceEffectPayload payload_from_json(const nlohmann::json &j)
{
    SurfaceEffectPayload out;
    if (j.is_object()) {
        if (j.contains("present") && j["present"].is_boolean())
            out.present = j["present"].get<bool>();
        if (j.contains("kv") && j["kv"].is_object())
            for (auto it = j["kv"].begin(); it != j["kv"].end(); ++it)
                if (it.value().is_string())
                    out.kv[it.key()] = it.value().get<std::string>();
    }
    return out;
}

std::string SurfaceEffectProfileManager::to_json() const
{
    nlohmann::json root;
    root["v"]       = 2;
    root["next_id"] = m_next_id;
    nlohmann::json arr = nlohmann::json::array();
    for (const SurfaceEffectProfile &p : m_profiles) {
        nlohmann::json e;
        e["id"]                = p.id;
        e["name"]              = p.name;
        e["preview_argb"]      = p.preview_argb;
        e["colormix"]          = payload_to_json(p.colorstitch);
        e["pathblend"]         = payload_to_json(p.pathblend);
        e["multipass"]         = payload_to_json(p.multipass);
        e["stack_top_json"]    = p.stack_top_json;
        e["stack_penu_json"]   = p.stack_penu_json;
        e["stack_bottom_json"] = p.stack_bottom_json;
        e["auto"]              = p.auto_generated;
        arr.push_back(std::move(e));
    }
    root["profiles"] = std::move(arr);
    return root.dump();
}

bool SurfaceEffectProfileManager::from_json(const std::string &text)
{
    if (text.empty())
        return false;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "SurfaceEffectProfileManager::from_json parse error: " << e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("profiles") || !root["profiles"].is_array())
        return false;

    this->clear();
    for (const nlohmann::json &e : root["profiles"]) {
        if (!e.is_object())
            continue;
        SurfaceEffectProfile p;
        if (e.contains("id") && e["id"].is_number_integer())    p.id           = e["id"].get<int>();
        if (e.contains("name") && e["name"].is_string())        p.name         = e["name"].get<std::string>();
        if (e.contains("preview_argb") && e["preview_argb"].is_number()) p.preview_argb = e["preview_argb"].get<uint32_t>();
        if (e.contains("colormix"))  p.colorstitch = payload_from_json(e["colormix"]);
        if (e.contains("pathblend")) p.pathblend   = payload_from_json(e["pathblend"]);
        if (e.contains("multipass")) p.multipass   = payload_from_json(e["multipass"]);
        if (e.contains("stack_top_json") && e["stack_top_json"].is_string())       p.stack_top_json    = e["stack_top_json"].get<std::string>();
        if (e.contains("stack_penu_json") && e["stack_penu_json"].is_string())     p.stack_penu_json   = e["stack_penu_json"].get<std::string>();
        if (e.contains("stack_bottom_json") && e["stack_bottom_json"].is_string()) p.stack_bottom_json = e["stack_bottom_json"].get<std::string>();
        if (e.contains("auto") && e["auto"].is_boolean())       p.auto_generated = e["auto"].get<bool>();
        // A profile that only carries stacks still needs its payloads for the slicer.
        if (!p.colorstitch.present && !p.pathblend.present && (!p.stack_top_json.empty() || !p.stack_penu_json.empty()))
            payload_from_stacks(SurfacePassStack::from_json(p.stack_top_json), SurfacePassStack::from_json(p.stack_penu_json), p);
        if (p.id <= 0)
            p.id = m_next_id;
        m_next_id = std::max(m_next_id, p.id + 1);
        m_profiles.push_back(std::move(p));
    }
    if (root.contains("next_id") && root["next_id"].is_number_integer())
        m_next_id = std::max(m_next_id, root["next_id"].get<int>());
    NEOTKO_LOG(PROFILE, "MGR from_json loaded " << m_profiles.size() << " profiles, next_id=" << m_next_id);
    return true;
}

} // namespace Slic3r
