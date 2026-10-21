// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include "ColorStitch.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Model.hpp"
#include "NeoDebug.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "SurfaceEffectProfile.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace Slic3r {

// ---------------------------------------------------------------------------------------------------------
// Line helpers
// ---------------------------------------------------------------------------------------------------------

// Nearest-neighbour order of the lines of one tool, flipping a line when its far end is closer.
static void optimize_tool_block_travel(std::vector<ExtrusionPath *> &paths)
{
    const size_t n = paths.size();
    if (n <= 1)
        return;
    std::vector<bool>            used(n, false);
    std::vector<ExtrusionPath *> sorted;
    sorted.reserve(n);
    sorted.push_back(paths[0]);
    used[0]   = true;
    double cx = double(paths[0]->polyline.last_point().x()), cy = double(paths[0]->polyline.last_point().y());
    while (sorted.size() < n) {
        int    best_i    = -1;
        bool   best_flip = false;
        double best_d2   = std::numeric_limits<double>::max();
        for (size_t i = 0; i < n; ++i) {
            if (used[i])
                continue;
            const Polyline3 &pl = paths[i]->polyline;
            for (int end = 0; end < 2; ++end) {
                const Point3 &p  = end == 0 ? pl.first_point() : pl.last_point();
                const double  dx = cx - double(p.x()), dy = cy - double(p.y());
                if (dx * dx + dy * dy < best_d2) {
                    best_d2   = dx * dx + dy * dy;
                    best_i    = int(i);
                    best_flip = end == 1;
                }
            }
        }
        if (best_i < 0)
            break;
        used[best_i] = true;
        if (best_flip)
            paths[best_i]->polyline.reverse();
        cx = double(paths[best_i]->polyline.last_point().x());
        cy = double(paths[best_i]->polyline.last_point().y());
        sorted.push_back(paths[best_i]);
    }
    paths = std::move(sorted);
}

// Splits a rectilinear zig-zag into its lines at every turn sharper than about 20 degrees.
static std::vector<Polyline> split_path_into_lines(const ExtrusionPath &path)
{
    const Polyline        pl  = path.polyline.to_polyline();
    const Points         &pts = pl.points;
    std::vector<Polyline> lines;
    if (pts.size() < 2)
        return lines;
    Polyline current;
    current.points.push_back(pts[0]);
    double prev_dx = 0, prev_dy = 0;
    bool   has_prev = false;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx  = double(pts[i].x() - pts[i - 1].x());
        const double dy  = double(pts[i].y() - pts[i - 1].y());
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0) {
            current.points.push_back(pts[i]);
            continue;
        }
        const double nx = dx / len, ny = dy / len;
        if (has_prev && prev_dx * nx + prev_dy * ny < 0.94) {
            current.points.push_back(pts[i]);
            if (current.points.size() >= 2)
                lines.push_back(std::move(current));
            current = Polyline();
            current.points.push_back(pts[i]);
            has_prev = false;
            continue;
        }
        current.points.push_back(pts[i]);
        prev_dx  = nx;
        prev_dy  = ny;
        has_prev = true;
    }
    if (current.points.size() >= 2)
        lines.push_back(std::move(current));
    return lines;
}

// The continuous Monotonic pattern fuses scanlines and the contour arcs joining them into one path. Each
// fused path is cut into runs: `scan` is the colourable scanline, `tail` the connector arc that follows it,
// printed with the colour of its scanline. A segment is a scanline when it stays within half a line spacing
// across the surface axis (ax, ay), a connector when it crosses towards the next line.
struct MonotonicRun { Polyline scan; Polyline tail; };

static std::vector<MonotonicRun> split_monotonic_path_into_runs(const ExtrusionPath &path, double ax, double ay)
{
    const Polyline            pl  = path.polyline.to_polyline();
    const Points             &pts = pl.points;
    std::vector<MonotonicRun> runs;
    const size_t              np = pts.size();
    if (np < 2)
        return runs;
    const size_t nseg        = np - 1;
    const double px          = -ay, py = ax;
    const double lane_thresh = 0.5 * std::max(1.0, double(path.width) * 1e6);

    std::vector<bool> is_scan(nseg, false);
    for (size_t i = 0; i < nseg; ++i) {
        const double dx = double(pts[i + 1].x() - pts[i].x());
        const double dy = double(pts[i + 1].y() - pts[i].y());
        if (dx * dx + dy * dy < 1e-12) {
            is_scan[i] = i > 0 ? is_scan[i - 1] : true;
            continue;
        }
        is_scan[i] = std::abs(dx * px + dy * py) < lane_thresh;
    }
    // A new run starts at every connector -> scanline transition once the current run owns a scanline.
    std::vector<size_t> run_starts{ 0 };
    bool                run_has_scan = false;
    for (size_t i = 0; i < nseg; ++i) {
        if (i > 0 && is_scan[i] && !is_scan[i - 1] && run_has_scan) {
            run_starts.push_back(i);
            run_has_scan = false;
        }
        if (is_scan[i])
            run_has_scan = true;
    }
    for (size_t k = 0; k < run_starts.size(); ++k) {
        const size_t s = run_starts[k];
        const size_t e = k + 1 < run_starts.size() ? run_starts[k + 1] : np - 1;
        if (e <= s)
            continue;
        size_t scan_split = s;
        while (scan_split < e && is_scan[scan_split])
            ++scan_split;
        MonotonicRun r;
        if (scan_split == s) {
            // The run opens on a connector: keep it whole so no geometry is lost.
            for (size_t i = s; i <= e; ++i)
                r.scan.points.push_back(pts[i]);
        } else {
            for (size_t i = s; i <= scan_split; ++i)
                r.scan.points.push_back(pts[i]);
            if (scan_split < e)
                for (size_t i = scan_split; i <= e; ++i)
                    r.tail.points.push_back(pts[i]);
        }
        if (r.scan.points.size() >= 2)
            runs.push_back(std::move(r));
    }
    return runs;
}

// ---------------------------------------------------------------------------------------------------------
// Tool lists
// ---------------------------------------------------------------------------------------------------------

static std::vector<int> build_tool_list(const PrintRegionConfig &config)
{
    std::vector<int> tools{ config.interlayer_colormix_tool_a.value, config.interlayer_colormix_tool_b.value };
    if (config.interlayer_colormix_tool_c.value >= 0)
        tools.push_back(config.interlayer_colormix_tool_c.value);
    if (config.interlayer_colormix_tool_d.value >= 0)
        tools.push_back(config.interlayer_colormix_tool_d.value);
    return tools;
}

static int recipe_gcd(int a, int b) { return b == 0 ? a : recipe_gcd(b, a % b); }

// Bresenham dither of weighted tools over n slots: each step every tool accumulates its weight and the one
// most ahead is emitted, so [(T0,3),(T3,1)] gives T0 T0 T0 T3 ... instead of a T0 band and a T3 band.
static std::vector<int> dither_sequence(const std::vector<std::pair<int, int>> &tool_weights, int n)
{
    std::vector<int> seq;
    if (tool_weights.empty() || n <= 0)
        return seq;
    int total = 0;
    for (const auto &tw : tool_weights)
        total += std::max(0, tw.second);
    if (total <= 0)
        return seq;
    seq.reserve(n);
    std::vector<int> err(tool_weights.size(), 0);
    for (int step = 0; step < n; ++step) {
        for (size_t i = 0; i < tool_weights.size(); ++i)
            err[i] += tool_weights[i].second;
        size_t best = 0;
        for (size_t i = 1; i < tool_weights.size(); ++i)
            if (err[i] > err[best])
                best = i;
        seq.push_back(tool_weights[best].first);
        err[best] -= total;
    }
    return seq;
}

// Physical tool sequence (0-based) of a mixed filament recipe, in the order MixedFilamentManager resolves
// it: manual pattern verbatim, else gradient components dithered by weight, else component A / B dithered
// by ratio. At most 48 entries.
static std::vector<int> extract_recipe_tools(const MixedFilament &mf, size_t num_physical)
{
    static constexpr int kMaxSeqLen = 48;
    auto to_idx = [num_physical](unsigned int phys_1based) -> int {
        return phys_1based >= 1 && phys_1based <= unsigned(num_physical) ? int(phys_1based) - 1 : -1;
    };
    if (!mf.manual_pattern.empty()) {
        // Tokenized by the mixed filament manager, so that the modern "1/10/2" form and the [N] ids of
        // filaments past 9 read as they do in the slicer. The perimeter groups are concatenated.
        std::vector<int> seq;
        for (const std::string &group : MixedFilamentManager::split_pattern_groups(mf.manual_pattern))
            for (const std::string &token : MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical)) {
                const int idx = to_idx(MixedFilamentManager::physical_filament_from_token(token, mf, num_physical));
                if (idx >= 0 && int(seq.size()) < kMaxSeqLen)
                    seq.push_back(idx);
            }
        return seq;
    }
    if (!mf.gradient_component_ids.empty()) {
        std::vector<unsigned int> ids;
        for (char c : mf.gradient_component_ids)
            if (c >= '1' && c <= '9')
                ids.push_back(unsigned(c - '0'));
        std::vector<int> wts;
        if (!mf.gradient_component_weights.empty()) {
            std::istringstream ss(mf.gradient_component_weights);
            std::string        tok;
            while (std::getline(ss, tok, '/'))
                try {
                    wts.push_back(std::stoi(tok));
                } catch (...) {}
        }
        while (wts.size() < ids.size())
            wts.push_back(ids.empty() ? 1 : 100 / int(ids.size()));
        if (ids.empty())
            return {};
        int g = wts[0];
        for (int w : wts)
            if (w > 0)
                g = recipe_gcd(g, w);
        if (g <= 0)
            g = 1;
        int                              reduced_total = 0;
        std::vector<std::pair<int, int>> tw;
        for (size_t i = 0; i < ids.size() && i < wts.size(); ++i) {
            const int idx = to_idx(ids[i]);
            const int w   = wts[i] > 0 ? wts[i] / g : 1;
            if (idx >= 0) {
                tw.push_back({ idx, w });
                reduced_total += w;
            }
        }
        return dither_sequence(tw, std::min(reduced_total, kMaxSeqLen));
    }
    int       ra = std::max(1, mf.ratio_a), rb = std::max(1, mf.ratio_b);
    const int g  = recipe_gcd(ra, rb);
    ra /= g;
    rb /= g;
    const int idx_a = to_idx(mf.component_a), idx_b = to_idx(mf.component_b);
    if (idx_a < 0 || idx_b < 0)
        return {};
    return dither_sequence({ { idx_a, ra }, { idx_b, rb } }, std::min(ra + rb, kMaxSeqLen));
}

std::vector<int> ColorStitch::build_tool_list_from_pattern(const std::string &pattern, const PrintRegionConfig &config,
                                                           const MixedFilamentManager *mgr, size_t num_physical)
{
    std::vector<int> tools;
    const bool use_virtual = config.interlayer_colormix_use_virtual.value && mgr != nullptr && num_physical > 0;
    for (char c : pattern) {
        if (c >= '1' && c <= '4') {
            tools.push_back(int(c - '1'));
        } else if (c >= '5' && c <= '9' && use_virtual) {
            // Digit 5 is the first live mixed filament, whatever the number of physical filaments: our
            // virtual filament ids start right after the physical ones (MixedFilamentManager).
            const unsigned int   virtual_id = unsigned(num_physical) + unsigned(c - '4');
            const MixedFilament *mf         = mgr->mixed_filament_from_id(virtual_id, num_physical);
            if (mf != nullptr) {
                // The recipe is the pattern: its repetitions carry the weights.
                const std::vector<int> recipe = extract_recipe_tools(*mf, num_physical);
                tools.insert(tools.end(), recipe.begin(), recipe.end());
            }
        }
    }
    const std::set<int> unique_tools(tools.begin(), tools.end());
    // A pattern of a single filament ("33") is that filament on every line. Returned twice to pass the
    // two-tool guard of the caller, which then keeps the original zig-zag.
    if (unique_tools.size() == 1)
        return { *unique_tools.begin(), *unique_tools.begin() };
    if (unique_tools.size() < 2) {
        NEOTKO_LOG(COLORSTITCH, "PATTERN_FALLBACK pattern=\"" << pattern << "\" -> tool_a/b/c/d");
        return build_tool_list(config);
    }
    return tools;
}

bool ColorStitch::should_process_role(ExtrusionRole role, int surface)
{
    switch (surface) {
    case kColormixSurface_Top: return role == erTopSolidInfill;
    case kColormixSurface_Penultimate: return role == erPenultimateInfill;
    case kColormixSurface_Both:
    default: return role == erTopSolidInfill || role == erPenultimateInfill;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Painter lookups
// ---------------------------------------------------------------------------------------------------------

const SurfaceEffectProfileManager *ColorStitch::profiles_of(const ModelObject *mo)
{
    return mo != nullptr && mo->get_model() != nullptr ? &mo->get_model()->surface_effect_profiles : nullptr;
}

const SurfaceEffectProfileManager *ColorStitch::profiles_of(const PrintObject *po)
{
    return po != nullptr ? profiles_of(po->model_object()) : nullptr;
}

const SurfaceEffectProfile *ColorStitch::find_profile(const PrintObject *po, int profile_id)
{
    const SurfaceEffectProfileManager *mgr = profiles_of(po);
    return mgr != nullptr && profile_id > 0 ? mgr->find(profile_id) : nullptr;
}

std::shared_ptr<const ColorStitchPaintCache> ColorStitchPaintCache::build(const PrintObject &po)
{
    auto                               cache = std::make_shared<ColorStitchPaintCache>();
    const ModelObject                 *mo    = po.model_object();
    const SurfaceEffectProfileManager *mgr   = ColorStitch::profiles_of(mo);
    if (mo == nullptr || mgr == nullptr)
        return cache;
    // Slice frame: the fill polygons are centered by -center_offset, hence trafo_centered().
    const Transform3d trafo = po.trafo_centered();
    for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
        const ModelVolume *mv = mo->volumes[vi];
        if (mv == nullptr || !mv->is_model_part() || mv->colorstitch_paint_facets.empty())
            continue;
        // Paint of a slot that maps to no profile, or to a deleted one, is ignored.
        bool any_live = false;
        for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT && !any_live; ++s)
            any_live = mgr->find(mv->colorstitch_slot_to_profile_id[s]) != nullptr;
        if (!any_live)
            continue;
        std::vector<indexed_triangle_set> per_state;
        mv->colorstitch_paint_facets.get_facets(*mv, per_state);
        const Transform3d vt = trafo * mv->get_matrix();
        for (size_t s = 1; s < per_state.size() && s < size_t(ModelVolume::COLORSTITCH_SLOT_COUNT); ++s) {
            const indexed_triangle_set &its = per_state[s];
            if (its.indices.empty() || mgr->find(mv->colorstitch_slot_to_profile_id[s]) == nullptr)
                continue;
            std::vector<Tri> &dst = cache->slots[int(s)];
            dst.reserve(dst.size() + its.indices.size());
            for (const stl_triangle_vertex_indices &tri : its.indices) {
                Tri t;
                t.v0         = vt * its.vertices[tri[0]].cast<double>();
                t.v1         = vt * its.vertices[tri[1]].cast<double>();
                t.v2         = vt * its.vertices[tri[2]].cast<double>();
                t.volume_idx = int(vi);
                dst.push_back(t);
            }
        }
    }
    return cache;
}

std::shared_ptr<const ColorStitchPaintCache> ColorStitch::paint_cache(const PrintObject *po)
{
    if (po == nullptr)
        return std::make_shared<ColorStitchPaintCache>();
    if (std::shared_ptr<const ColorStitchPaintCache> cache = po->colorstitch_paint_cache())
        return cache;
    return ColorStitchPaintCache::build(*po);
}

// A painted triangle belongs to the band when its top (bottom with `downward`) lies inside [z_min, z_max]
// and it faces up (down). The band of a layer spans its whole height, so that a step whose mesh top sits
// below the print_z of its layer is still found.
static bool tri_in_band(const ColorStitchPaintCache::Tri &t, double z_min, double z_max, bool downward)
{
    static constexpr double z_tol = 0.02;
    const double            nz    = (t.v1 - t.v0).cross(t.v2 - t.v0).z();
    if (downward ? nz >= 0.0 : nz <= 0.0)
        return false;
    const double z = downward ? std::min({ t.v0.z(), t.v1.z(), t.v2.z() }) : std::max({ t.v0.z(), t.v1.z(), t.v2.z() });
    return z >= z_min - z_tol && z <= z_max + z_tol;
}

int ColorStitch::dominant_painted_slot_in_z_range(const PrintObject *po, double z_min, double z_max, bool downward)
{
    const std::shared_ptr<const ColorStitchPaintCache> cache = paint_cache(po);
    int best_slot = 0, best_count = 0;
    for (const auto &[slot, tris] : cache->slots) {
        int count = 0;
        for (const ColorStitchPaintCache::Tri &t : tris)
            if (tri_in_band(t, z_min, z_max, downward))
                ++count;
        if (count > best_count) {
            best_count = count;
            best_slot  = slot;
        }
    }
    return best_slot;
}

std::vector<int> ColorStitch::enumerate_painted_slots_in_z_range(const PrintObject *po, double z_min, double z_max, bool downward)
{
    std::vector<int>                                   out;
    const std::shared_ptr<const ColorStitchPaintCache> cache = paint_cache(po);
    for (const auto &[slot, tris] : cache->slots)
        if (std::any_of(tris.begin(), tris.end(), [&](const ColorStitchPaintCache::Tri &t) { return tri_in_band(t, z_min, z_max, downward); }))
            out.push_back(slot);
    return out;
}

// Union of a large triangle soup in batches: Clipper's cost grows with the intersections of one pass, and
// adjacent triangles have many. The union is associative, so the result is the same.
static ExPolygons union_triangle_soup(Polygons &&tris)
{
    constexpr size_t kChunk = 512;
    if (tris.size() <= kChunk)
        return union_ex(tris);
    Polygons level = std::move(tris);
    while (level.size() > kChunk) {
        Polygons next;
        next.reserve(level.size() / 4 + 8);
        for (size_t i = 0; i < level.size(); i += kChunk) {
            const size_t j = std::min(level.size(), i + kChunk);
            Polygons     part(std::make_move_iterator(level.begin() + i), std::make_move_iterator(level.begin() + j));
            append(next, to_polygons(union_ex(part)));
        }
        if (next.size() >= level.size())
            return union_ex(next);
        level = std::move(next);
    }
    return union_ex(level);
}

ExPolygons ColorStitch::painted_footprint_in_z_range(const PrintObject *po, int slot, double z_min, double z_max, bool downward)
{
    const std::shared_ptr<const ColorStitchPaintCache> cache = paint_cache(po);
    const auto                                         it    = cache->slots.find(slot);
    if (it == cache->slots.end())
        return {};
    Polygons tris;
    for (const ColorStitchPaintCache::Tri &t : it->second) {
        if (!tri_in_band(t, z_min, z_max, downward))
            continue;
        Polygon p;
        p.points = { Point(scale_(t.v0.x()), scale_(t.v0.y())), Point(scale_(t.v1.x()), scale_(t.v1.y())),
                     Point(scale_(t.v2.x()), scale_(t.v2.y())) };
        if (std::abs(p.area()) < SCALED_EPSILON)
            continue;
        p.make_counter_clockwise();
        tris.push_back(std::move(p));
    }
    return tris.empty() ? ExPolygons() : union_triangle_soup(std::move(tris));
}

int ColorStitch::painted_slot_owner_volume_index_in_z_range(const PrintObject *po, int slot, double z_min, double z_max, bool downward)
{
    const std::shared_ptr<const ColorStitchPaintCache> cache = paint_cache(po);
    const auto                                         it    = cache->slots.find(slot);
    if (it == cache->slots.end())
        return -1;
    int owner = -1;
    for (const ColorStitchPaintCache::Tri &t : it->second)
        if (tri_in_band(t, z_min, z_max, downward) && (owner < 0 || t.volume_idx < owner))
            owner = t.volume_idx;
    return owner;
}

int ColorStitch::profile_id_for_slot(const PrintObject *po, int slot)
{
    if (po == nullptr || slot <= 0 || slot >= ModelVolume::COLORSTITCH_SLOT_COUNT || po->model_object() == nullptr)
        return 0;
    for (const ModelVolume *mv : po->model_object()->volumes)
        if (mv != nullptr && mv->is_model_part() && mv->colorstitch_slot_to_profile_id[slot] != 0)
            return mv->colorstitch_slot_to_profile_id[slot];
    return 0;
}

int ColorStitch::profile_id_for_slot_in_volume(const PrintObject *po, int slot, int vol_idx)
{
    if (po == nullptr || slot <= 0 || slot >= ModelVolume::COLORSTITCH_SLOT_COUNT || po->model_object() == nullptr)
        return 0;
    const ModelObject *mo = po->model_object();
    if (vol_idx >= 0 && vol_idx < int(mo->volumes.size())) {
        const ModelVolume *mv = mo->volumes[vol_idx];
        if (mv != nullptr && mv->is_model_part() && mv->colorstitch_slot_to_profile_id[slot] != 0)
            return mv->colorstitch_slot_to_profile_id[slot];
    }
    return profile_id_for_slot(po, slot);
}

bool ColorStitch::object_has_any_colorstitch_paint(const ModelObject *mo)
{
    if (mo == nullptr)
        return false;
    for (const ModelVolume *mv : mo->volumes) {
        // Painted triangles are required: the eraser clears the facets and leaves the slot table.
        if (mv == nullptr || !mv->is_model_part() || mv->colorstitch_paint_facets.empty())
            continue;
        for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s)
            if (mv->colorstitch_slot_to_profile_id[s] != 0)
                return true;
    }
    return false;
}

// Calls `fn` once for every distinct live profile the object's slot tables refer to, until it returns true.
template<typename Fn> static bool any_painted_profile(const ModelObject *mo, Fn &&fn)
{
    const SurfaceEffectProfileManager *mgr = ColorStitch::profiles_of(mo);
    if (mgr == nullptr)
        return false;
    std::set<int> seen;
    for (const ModelVolume *mv : mo->volumes) {
        if (mv == nullptr || !mv->is_model_part())
            continue;
        for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid <= 0 || !seen.insert(pid).second)
                continue;
            if (const SurfaceEffectProfile *p = mgr->find(pid); p != nullptr && fn(*p))
                return true;
        }
    }
    return false;
}

bool ColorStitch::object_painter_wants_penu(const ModelObject *mo)
{
    return any_painted_profile(mo, [](const SurfaceEffectProfile &p) {
        // The penultimate stack of the profile is the source of truth: a [None] stack does not count.
        if (SurfacePassStack::from_json(p.stack_penu_json).any_effect())
            return true;
        auto kv_is = [](const SurfaceEffectPayload &payload, const char *key, std::initializer_list<const char *> values) {
            if (!payload.present)
                return false;
            const auto it = payload.kv.find(key);
            if (it == payload.kv.end())
                return false;
            for (const char *v : values)
                if (it->second == v)
                    return true;
            return false;
        };
        // Older profiles: the flattened payloads. Surface 0 = both zones, 2 = penultimate only.
        return kv_is(p.multipass, "penultimate_multipass_enabled", { "1", "true" }) ||
               kv_is(p.colorstitch, "interlayer_colormix_surface", { "0", "2" }) ||
               kv_is(p.pathblend, "pathblend_surface", { "0", "2" });
    });
}

bool ColorStitch::object_painter_wants_bottom(const ModelObject *mo)
{
    return any_painted_profile(mo, [](const SurfaceEffectProfile &p) { return SurfacePassStack::from_json(p.stack_bottom_json).any_effect(); });
}

std::vector<unsigned int> ColorStitch::painted_profile_tools_1based(const SurfaceEffectProfile &p, bool top_role)
{
    std::vector<unsigned int> out;
    if (!p.colorstitch.present)
        return out;
    const auto       &kv     = p.colorstitch.kv;
    const std::string prefix = top_role ? "interlayer_colormix_" : "interlayer_colormix_penu_";
    auto kv_get = [&kv](const std::string &key) -> std::string {
        const auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };
    auto get_int = [&](const char *base, int dflt) {
        const std::string s = kv_get(prefix + base);
        try {
            return s.empty() ? dflt : std::stoi(s);
        } catch (...) {
            return dflt;
        }
    };
    auto get_dbl = [&](const char *base, double dflt) {
        const std::string s = kv_get(prefix + base);
        try {
            return s.empty() ? dflt : std::stod(s);
        } catch (...) {
            return dflt;
        }
    };
    const int mode   = get_int("mode", 0);
    const int tool_a = get_int("tool_a", 0), tool_b = get_int("tool_b", 1), tool_c = get_int("tool_c", -1), tool_d = get_int("tool_d", -1);
    auto add_phys = [&out](int t) {
        if (t >= 0 && std::find(out.begin(), out.end(), unsigned(t + 1)) == out.end())
            out.push_back(unsigned(t + 1));
    };
    // The same filters as the preflight of assign_and_group_tools(): the tool ordering registers what the
    // slicer consumes.
    if (mode == 1) {
        if (tool_a >= 0 && tool_b >= 0 && tool_a != tool_b) {
            add_phys(tool_a);
            add_phys(tool_b);
            return out;
        }
    } else if (mode == 2) {
        if (tool_a >= 0 && tool_b >= 0 && tool_c >= 0 && (tool_a != tool_b || tool_b != tool_c)) {
            add_phys(tool_a);
            add_phys(tool_b);
            add_phys(tool_c);
            return out;
        }
    } else if (mode == 3 || mode == 4) {
        const char *keys[4][2] = { { "band_count_a", "band_mm_a" }, { "band_count_b", "band_mm_b" },
                                   { "band_count_c", "band_mm_c" }, { "band_count_d", "band_mm_d" } };
        const int   tools[4]   = { tool_a, tool_b, tool_c, tool_d };
        for (int i = 0; i < 4; ++i)
            if (tools[i] >= 0 && (mode == 3 ? get_int(keys[i][0], 0) > 0 : get_dbl(keys[i][1], 0.0) > 1e-4))
                add_phys(tools[i]);
        if (out.size() >= 2)
            return out;
        out.clear();
    }
    // Mode 0 or fallback: the pattern string, then tool_a..d.
    for (char c : kv_get(top_role ? "interlayer_colormix_pattern_top" : "interlayer_colormix_pattern_penultimate"))
        if (c >= '1' && c <= '4')
            add_phys(int(c - '1'));
    if (out.size() >= 2)
        return out;
    out.clear();
    add_phys(tool_a);
    add_phys(tool_b);
    add_phys(tool_c);
    add_phys(tool_d);
    return out;
}

bool ColorStitch::painted_perim_override_from_profile(const SurfaceEffectPayload &payload)
{
    const auto it = payload.kv.find("multipass_perimeter_override");
    return it != payload.kv.end() && (it->second == "1" || it->second == "true");
}

bool ColorStitch::any_painted_profile_has_perim_override(const PrintObject *po, double print_z, double height)
{
    if (po == nullptr)
        return false;
    const std::pair<int, bool> zones[2] = { { dominant_painted_slot_in_z_range(po, print_z - height, print_z), false },
                                            { dominant_painted_slot_in_z_range(po, print_z, print_z + height), true } };
    for (const auto &[slot, is_penu] : zones) {
        if (slot <= 0)
            continue;
        const SurfaceEffectProfile *p = find_profile(po, profile_id_for_slot(po, slot));
        if (p == nullptr)
            continue;
        const SurfacePassStack st = SurfacePassStack::from_json(is_penu ? p->stack_penu_json : p->stack_top_json);
        if (st.any_effect() && st.perimeter_override)
            return true;
        if (p->multipass.present && painted_perim_override_from_profile(p->multipass))
            return true;
    }
    return false;
}

int ColorStitch::colorstitch_angle_for_profile(const SurfaceEffectProfile *p, bool penu)
{
    if (p == nullptr)
        return -1;
    auto read = [penu](const std::map<std::string, std::string> &kv) -> int {
        // A penultimate pass without its own key inherits the top one.
        auto it = kv.find(penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle");
        if (it == kv.end() && penu)
            it = kv.find("interlayer_colormix_angle");
        if (it != kv.end())
            try {
                return std::stoi(it->second);
            } catch (...) {}
        return -2; // absent
    };
    const std::string &js = penu ? p->stack_penu_json : p->stack_top_json;
    if (!js.empty())
        for (const SurfacePass &sp : SurfacePassStack::from_json(js).passes)
            if (sp.kind == SurfacePassKind::ColorStitch && !sp.colorstitch.kv.empty())
                if (const int a = read(sp.colorstitch.kv); a != -2)
                    return a;
    if (p->colorstitch.present && !p->colorstitch.kv.empty())
        if (const int a = read(p->colorstitch.kv); a != -2)
            return a;
    // The painter only writes the angle once the user touches it, so a profile without the key uses the
    // default angle, which is also what the preview draws.
    return COLORSTITCH_DEFAULT_ANGLE_DEG;
}

void ColorStitch::encode_tool_in_path(ExtrusionPath *path, int tool_idx)
{
    path->mm3_per_mm += double(std::clamp(tool_idx, 0, 14) + 1) * 10.0;
}

// ---------------------------------------------------------------------------------------------------------
// assign_and_group_tools
// ---------------------------------------------------------------------------------------------------------

int ColorStitch::assign_and_group_tools(ExtrusionEntityCollection  &fills,
                                        const PrintRegionConfig    &config,
                                        ExtrusionRole               /* role: read per path */,
                                        int                         layer_idx,
                                        bool                        allow_top,
                                        bool                        allow_penu,
                                        const MixedFilamentManager *mgr,
                                        size_t                      num_physical,
                                        const PrintObject          *print_object,
                                        double                      layer_print_z,
                                        double                      layer_height,
                                        bool                        config_has_pass_override)
{
    // Painter mode: the object carries ColorStitch paint, so the preset gates do not apply and a role gets
    // ColorStitch only where a painted profile (or the pass override in `config`) asks for it.
    const ModelObject *model_object     = print_object != nullptr ? print_object->model_object() : nullptr;
    const bool         painter_mode_obj = object_has_any_colorstitch_paint(model_object);

    const SurfaceEffectProfile *painted_top_profile  = nullptr;
    const SurfaceEffectProfile *painted_penu_profile = nullptr;
    if (print_object != nullptr && layer_height > 0.0) {
        // The top role reads the paint of this layer's slab, the penultimate role the slab above. One more
        // layer is added on top, where the mesh top of the object may sit above the last print_z.
        const int top_slot  = dominant_painted_slot_in_z_range(print_object, layer_print_z - layer_height, layer_print_z + layer_height);
        const int penu_slot = dominant_painted_slot_in_z_range(print_object, layer_print_z, layer_print_z + 2.0 * layer_height);
        painted_top_profile  = find_profile(print_object, profile_id_for_slot(print_object, top_slot));
        painted_penu_profile = find_profile(print_object, profile_id_for_slot(print_object, penu_slot));
    }
    if (!painter_mode_obj && !config.interlayer_colormix_enabled.value)
        return 0;

    const double min_length_mm   = config.interlayer_colormix_min_length.value;
    const int    surface         = config.interlayer_colormix_surface.value;
    bool         any_modified    = false;
    bool         any_unsplittable = false;

    for (ExtrusionEntity *top_entity : fills.entities) {
        auto *sub = dynamic_cast<ExtrusionEntityCollection *>(top_entity);
        if (sub == nullptr || sub->entities.empty())
            continue;
        // Already encoded.
        if (std::any_of(sub->entities.begin(), sub->entities.end(), [](const ExtrusionEntity *e) {
                const auto *p = dynamic_cast<const ExtrusionPath *>(e);
                return p != nullptr && p->mm3_per_mm >= 10.0;
            }))
            continue;

        const ExtrusionPath *first_path = nullptr;
        int                  n_paths    = 0;
        for (const ExtrusionEntity *e : sub->entities)
            if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
                if (first_path == nullptr)
                    first_path = p;
                ++n_paths;
            }
        if (first_path == nullptr)
            continue;

        // Bottom zones are authored with the top keys.
        const ExtrusionRole         r              = first_path->role();
        const bool                  gv_is_top_role = r == erTopSolidInfill || r == erBottomSurface || r == erBridgeInfill;
        const SurfaceEffectProfile *eff_profile    = gv_is_top_role ? painted_top_profile : painted_penu_profile;
        const bool                  painted_override = (config_has_pass_override && config.interlayer_colormix_enabled.value) ||
                                                       (eff_profile != nullptr && eff_profile->colorstitch.present);
        if (painter_mode_obj) {
            if (r != erTopSolidInfill && r != erPenultimateInfill && r != erBottomSurface && r != erBridgeInfill)
                continue;
            if (!painted_override)
                continue;
        } else {
            if (!should_process_role(r, surface))
                continue;
            if ((!allow_top && r == erTopSolidInfill) || (!allow_penu && r == erPenultimateInfill))
                continue;
        }

        // Per role view of the gradient settings.
        struct GV {
            int    cm_mode, pct_a, pct_b, easing, min_lines;
            double gamma, overlap;
            bool   invert;
            int    band_a, band_b, band_c, band_d;
            double band_mm_a, band_mm_b, band_mm_c, band_mm_d;
            double grad_span_mm; // 0 = fit to the surface, > 0 = physical period
            int    tool_a, tool_b, tool_c, tool_d;
            int    repetitions;
            int    angle; // authored angle, -1 = auto
        } gv;
#define CS_GV_LOAD(PFX)                                                                                   \
    gv.cm_mode      = config.PFX##mode.value;                                                             \
    gv.pct_a        = config.PFX##pct_a.value;                                                            \
    gv.pct_b        = config.PFX##pct_b.value;                                                            \
    gv.easing       = config.PFX##easing.value;                                                           \
    gv.gamma        = config.PFX##gamma.value;                                                            \
    gv.min_lines    = config.PFX##min_surface_lines.value;                                                \
    gv.overlap      = config.PFX##overlap.value;                                                          \
    gv.invert       = config.PFX##invert.value;                                                           \
    gv.band_a       = config.PFX##band_count_a.value;                                                     \
    gv.band_b       = config.PFX##band_count_b.value;                                                     \
    gv.band_c       = config.PFX##band_count_c.value;                                                     \
    gv.band_d       = config.PFX##band_count_d.value;                                                     \
    gv.band_mm_a    = config.PFX##band_mm_a.value;                                                        \
    gv.band_mm_b    = config.PFX##band_mm_b.value;                                                        \
    gv.band_mm_c    = config.PFX##band_mm_c.value;                                                        \
    gv.band_mm_d    = config.PFX##band_mm_d.value;                                                        \
    gv.grad_span_mm = std::max(0.0, double(config.PFX##gradient_span_mm.value));                          \
    gv.tool_a       = config.PFX##tool_a.value;                                                           \
    gv.tool_b       = config.PFX##tool_b.value;                                                           \
    gv.tool_c       = config.PFX##tool_c.value;                                                           \
    gv.tool_d       = config.PFX##tool_d.value;                                                           \
    gv.repetitions  = config.PFX##repetitions.value;
        if (gv_is_top_role) {
            CS_GV_LOAD(interlayer_colormix_)
            gv.angle = config.interlayer_colormix_angle.value;
        } else {
            CS_GV_LOAD(interlayer_colormix_penu_)
            // The penultimate surface does not inherit the top angle: -1 measures the axis of its own lines.
            gv.angle = config.interlayer_colormix_penu_angle.value;
        }
#undef CS_GV_LOAD

        // A painted profile without a pass override of its own: its flattened payload overrides the preset.
        if (painted_override && !config_has_pass_override) {
            const auto       &kv     = eff_profile->colorstitch.kv;
            const std::string prefix = gv_is_top_role ? "interlayer_colormix_" : "interlayer_colormix_penu_";
            auto find = [&](const char *base) { return kv.find(prefix + base); };
            auto get_int = [&](const char *base, int dflt) {
                const auto it = find(base);
                try {
                    return it == kv.end() ? dflt : std::stoi(it->second);
                } catch (...) {
                    return dflt;
                }
            };
            auto get_dbl = [&](const char *base, double dflt) {
                const auto it = find(base);
                try {
                    return it == kv.end() ? dflt : std::stod(it->second);
                } catch (...) {
                    return dflt;
                }
            };
            auto get_bool = [&](const char *base, bool dflt) {
                const auto it = find(base);
                return it == kv.end() ? dflt : (it->second == "1" || it->second == "true");
            };
            gv.cm_mode      = get_int("mode", gv.cm_mode);
            gv.pct_a        = get_int("pct_a", gv.pct_a);
            gv.pct_b        = get_int("pct_b", gv.pct_b);
            gv.easing       = get_int("easing", gv.easing);
            gv.gamma        = get_dbl("gamma", gv.gamma);
            gv.min_lines    = get_int("min_surface_lines", gv.min_lines);
            gv.overlap      = get_dbl("overlap", gv.overlap);
            gv.invert       = get_bool("invert", gv.invert);
            gv.band_a       = get_int("band_count_a", gv.band_a);
            gv.band_b       = get_int("band_count_b", gv.band_b);
            gv.band_c       = get_int("band_count_c", gv.band_c);
            gv.band_d       = get_int("band_count_d", gv.band_d);
            gv.band_mm_a    = get_dbl("band_mm_a", gv.band_mm_a);
            gv.band_mm_b    = get_dbl("band_mm_b", gv.band_mm_b);
            gv.band_mm_c    = get_dbl("band_mm_c", gv.band_mm_c);
            gv.band_mm_d    = get_dbl("band_mm_d", gv.band_mm_d);
            gv.grad_span_mm = std::max(0.0, get_dbl("gradient_span_mm", gv.grad_span_mm));
            gv.tool_a       = get_int("tool_a", gv.tool_a);
            gv.tool_b       = get_int("tool_b", gv.tool_b);
            gv.tool_c       = get_int("tool_c", gv.tool_c);
            gv.tool_d       = get_int("tool_d", gv.tool_d);
            gv.repetitions  = get_int("repetitions", gv.repetitions);
            gv.angle        = get_int("angle", gv.angle);
        }

        // Preflight tool list per mode:
        //   0 pattern string, 1 linear 2 colours, 2 linear 3 colours, 3 bands in lines, 4 bands in mm.
        // For modes 1-4 `tools` is the set the per-line decision draws from; the decision itself is taken once
        // the lines are known.
        const int        cm_mode = gv.cm_mode;
        std::vector<int> tools;
        auto             fallback_to_pattern_string = [&]() {
            tools = build_tool_list_from_pattern(gv_is_top_role ? config.interlayer_colormix_pattern_top.value :
                                                                  config.interlayer_colormix_pattern_penultimate.value,
                                                 config, mgr, num_physical);
        };
        constexpr double kMinBandMM = 1e-4;
        if (painted_override && !config_has_pass_override) {
            // The same tools the tool ordering registers for this profile.
            for (unsigned int t : painted_profile_tools_1based(*eff_profile, gv_is_top_role))
                if (t > 0)
                    tools.push_back(int(t) - 1);
            // Mode 0 needs the pattern sequence with its repetitions; use it when it covers the same set.
            if (cm_mode == 0 && tools.size() >= 2) {
                const auto &pkv = eff_profile->colorstitch.kv;
                const auto  pit = pkv.find(gv_is_top_role ? "interlayer_colormix_pattern_top" : "interlayer_colormix_pattern_penultimate");
                if (pit != pkv.end()) {
                    std::vector<int> seq;
                    for (char c : pit->second)
                        if (c >= '1' && c <= '4')
                            seq.push_back(int(c - '1'));
                    if (std::set<int>(seq.begin(), seq.end()) == std::set<int>(tools.begin(), tools.end()))
                        tools = std::move(seq);
                }
            }
        } else if (cm_mode == 1) {
            if (gv.tool_a >= 0 && gv.tool_b >= 0 && gv.tool_a != gv.tool_b)
                tools = { gv.tool_a, gv.tool_b };
            else
                fallback_to_pattern_string();
        } else if (cm_mode == 2) {
            if (gv.tool_a >= 0 && gv.tool_b >= 0 && gv.tool_c >= 0 && (gv.tool_a != gv.tool_b || gv.tool_b != gv.tool_c))
                tools = { gv.tool_a, gv.tool_b, gv.tool_c };
            else
                fallback_to_pattern_string();
        } else if (cm_mode == 3 || cm_mode == 4) {
            // A negative tool is an unused slot even when it has a band width.
            const bool on[4] = { cm_mode == 3 ? gv.band_a > 0 : gv.band_mm_a > kMinBandMM, cm_mode == 3 ? gv.band_b > 0 : gv.band_mm_b > kMinBandMM,
                                 cm_mode == 3 ? gv.band_c > 0 : gv.band_mm_c > kMinBandMM, cm_mode == 3 ? gv.band_d > 0 : gv.band_mm_d > kMinBandMM };
            const int  t[4]  = { gv.tool_a, gv.tool_b, gv.tool_c, gv.tool_d };
            for (int i = 0; i < 4; ++i)
                if (on[i] && t[i] >= 0)
                    tools.push_back(t[i]);
            if (tools.size() < 2)
                fallback_to_pattern_string();
        } else {
            fallback_to_pattern_string();
        }

        // Band widths aligned with `tools` (modes 3 and 4). Mode 3 counts lines: the counts become mm once the
        // real spacing of this surface is known. In both modes inverting flips the band cycle.
        std::vector<double> band_mm_active;
        const bool          band_mm_in_lines = cm_mode == 3;
        if (cm_mode == 3 || cm_mode == 4) {
            const double w[4] = { cm_mode == 3 ? double(gv.band_a) : gv.band_mm_a, cm_mode == 3 ? double(gv.band_b) : gv.band_mm_b,
                                  cm_mode == 3 ? double(gv.band_c) : gv.band_mm_c, cm_mode == 3 ? double(gv.band_d) : gv.band_mm_d };
            const int    t[4] = { gv.tool_a, gv.tool_b, gv.tool_c, gv.tool_d };
            for (int i = 0; i < 4; ++i)
                if (w[i] > (cm_mode == 3 ? 0.0 : kMinBandMM) && t[i] >= 0)
                    band_mm_active.push_back(w[i]);
            if (gv.invert && band_mm_active.size() == tools.size() && band_mm_active.size() > 1) {
                std::reverse(band_mm_active.begin(), band_mm_active.end());
                std::reverse(tools.begin(), tools.end());
            }
        }
        if (tools.size() < 2)
            continue;

        // Collect the lines. Monotonic Line gives one path per line; Monotonic fuses them and is cut into
        // runs (the connectors ride with their scanline); a lone rectilinear zig-zag is split at its turns.
        struct RawLine { Polyline pl; ExtrusionRole role; double mm3; float width; float height; Polyline tail{}; };
        std::vector<RawLine> raw_lines;
        if (n_paths > 1) {
            // Dominant fill axis of the whole surface (length weighted doubled angle), refined with the
            // segments within 15 degrees of it, which drops the contour connectors.
            auto dominant_angle = [&sub](const double *ref) {
                const double c2 = ref ? std::cos(2.0 * *ref) : 0.0, s2 = ref ? std::sin(2.0 * *ref) : 0.0;
                const double cos30 = std::cos(M_PI / 6.0);
                double       sx = 0.0, sy = 0.0;
                for (const ExtrusionEntity *e : sub->entities) {
                    const auto *p = dynamic_cast<const ExtrusionPath *>(e);
                    if (p == nullptr)
                        continue;
                    const auto &pp = p->polyline.points;
                    for (size_t i = 1; i < pp.size(); ++i) {
                        const double dx = double(pp[i].x() - pp[i - 1].x()), dy = double(pp[i].y() - pp[i - 1].y());
                        const double l  = std::sqrt(dx * dx + dy * dy);
                        if (l < 1e-6)
                            continue;
                        const double ux = dx / l, uy = dy / l;
                        const double dx2 = ux * ux - uy * uy, dy2 = 2.0 * ux * uy;
                        if (ref != nullptr && dx2 * c2 + dy2 * s2 < cos30)
                            continue;
                        sx += dx2 * l;
                        sy += dy2 * l;
                    }
                }
                return std::pair<bool, double>(sx * sx + sy * sy > 1e-12, 0.5 * std::atan2(sy, sx));
            };
            double sang = dominant_angle(nullptr).second;
            if (const auto refined = dominant_angle(&sang); refined.first)
                sang = refined.second;
            const double surf_ax = std::cos(sang), surf_ay = std::sin(sang);
            for (const ExtrusionEntity *e : sub->entities) {
                const auto *p = dynamic_cast<const ExtrusionPath *>(e);
                if (p == nullptr)
                    continue;
                std::vector<MonotonicRun> mono_runs = split_monotonic_path_into_runs(*p, surf_ax, surf_ay);
                if (mono_runs.size() > 1)
                    fused_monotonic_seen().store(true, std::memory_order_relaxed);
                for (MonotonicRun &mr : mono_runs) {
                    if (mr.scan.length() / 1e6 < min_length_mm)
                        continue;
                    raw_lines.push_back({ std::move(mr.scan), p->role(), p->mm3_per_mm, p->width, p->height, std::move(mr.tail) });
                }
            }
        } else {
            std::vector<Polyline> split = split_path_into_lines(*first_path);
            if (split.size() < 2) {
                any_unsplittable = true;
                continue;
            }
            for (Polyline &pl : split)
                if (pl.length() / 1e6 >= min_length_mm)
                    raw_lines.push_back({ std::move(pl), first_path->role(), first_path->mm3_per_mm, first_path->width, first_path->height });
        }
        if (raw_lines.size() < 2) {
            any_unsplittable = true;
            continue;
        }

        // Anchor of every positional distribution: the object origin in the slice frame, so the design moves
        // with the part and the top and the layer below share it.
        double anchor_x_mm = 0.0, anchor_y_mm = 0.0;
        if (print_object != nullptr) {
            const Vec3d o = print_object->trafo_centered() * Vec3d(0.0, 0.0, 0.0);
            anchor_x_mm   = o.x();
            anchor_y_mm   = o.y();
        }
        // Real spacing of the lines (not their width), the size of a lane.
        const double sp_mm = std::max(1e-3, double(raw_lines[0].width) - (layer_height > 1e-4 ? layer_height : 0.2) * (1.0 - 0.25 * M_PI));

        // Gradients (modes 1, 2): one dither entry per lane, evaluated at the real position of the lane.
        std::vector<int> grad_field_order;
        if (cm_mode == 1 || cm_mode == 2) {
            const int n = int(raw_lines.size());
            if (gv.min_lines > 0 && n < gv.min_lines) {
                // Tiny surface: tool A only.
                tools.assign(size_t(n), gv.tool_a);
            } else if (tools.size() >= 2) {
                // `repetitions` on a fitted gradient is a period of span / repetitions.
                const int reps   = std::max(1, gv.repetitions);
                double    period = gv.grad_span_mm;
                if (period <= 1e-4 && reps > 1) {
                    const FieldGroups probe = compute_field_groups(raw_lines, gv.angle, 0.0, sp_mm, anchor_x_mm, anchor_y_mm, false);
                    period = probe.t_of_group.size() > 1 ? double(probe.t_of_group.size() - 1) * sp_mm / double(reps) : 0.0;
                }
                std::string       field_summary;
                const FieldGroups fg = compute_field_groups(raw_lines, gv.angle, period, sp_mm, anchor_x_mm, anchor_y_mm, gv.invert,
                                                            NeoDebug::enabled(NeoDebug::COLORSTITCH) ? &field_summary : nullptr);
                if (fg.t_of_group.size() >= 2) {
                    tools = cm_mode == 1 ? build_dithered_tools_2color_at(fg.t_of_group, tools[0], tools[1], gv.pct_a, gv.easing, gv.gamma) :
                                           build_dithered_tools_3color_at(fg.t_of_group, tools[0], tools[1], tools.size() > 2 ? tools[2] : tools[1],
                                                                          gv.pct_a, gv.pct_b, gv.easing, gv.gamma, gv.overlap);
                    grad_field_order = fg.group_of_line;
                }
                NEOTKO_LOG(COLORSTITCH, "GRAD_FIELD layer=" << layer_idx << " mode=" << cm_mode << " (" << field_summary << ") groups="
                                                            << fg.t_of_group.size() << " period=" << period);
            }
        }

        // Recipe order of the passes, for the caller.
        for (int t : tools)
            if (std::find(last_tool_order_tls().begin(), last_tool_order_tls().end(), t) == last_tool_order_tls().end())
                last_tool_order_tls().push_back(t);

        // Slot (index into `tools`) of every line.
        const int        n_slots          = int(tools.size());
        std::vector<int> slot_per_line;
        const bool       use_grad_field   = !grad_field_order.empty() && grad_field_order.size() == raw_lines.size();
        const bool       use_band_mm      = (cm_mode == 3 || cm_mode == 4) && band_mm_active.size() == tools.size() && band_mm_active.size() >= 2;
        const bool       use_pattern_field = cm_mode == 0 && tools.size() >= 2;
        if (use_grad_field) {
            slot_per_line = grad_field_order;
            for (int &sl : slot_per_line)
                sl = std::clamp(sl, 0, std::max(0, n_slots - 1));
        } else if (use_band_mm) {
            if (band_mm_in_lines)
                for (double &b : band_mm_active)
                    b *= sp_mm;
            if (gv.min_lines > 0 && int(raw_lines.size()) < gv.min_lines)
                // Tiny surface: one colour, the single tool short circuit below keeps the zig-zag.
                slot_per_line.assign(raw_lines.size(), 0);
            else
                slot_per_line = compute_slot_per_line_band_mm(raw_lines, band_mm_active, gv.angle, anchor_x_mm, anchor_y_mm);
        } else if (use_pattern_field) {
            // Every line takes the pattern entry of its lane, counted from the object origin: a hole cannot
            // shift the pattern and two layers with the same angle match line by line.
            slot_per_line.assign(raw_lines.size(), 0);
            if (gv.min_lines <= 0 || int(raw_lines.size()) >= gv.min_lines) {
                const LaneVec2 perp = lane_perp_axis(raw_lines, gv.angle, nullptr, nullptr, false);
                FieldSampler   smp;
                smp.sp     = sp_mm;
                smp.anchor = anchor_x_mm * perp.x + anchor_y_mm * perp.y;
                for (size_t i = 0; i < raw_lines.size(); ++i) {
                    const LaneVec2  c = lane_centroid(raw_lines[i].pl);
                    const long long k = smp.lane_of((c.x * perp.x + c.y * perp.y) / 1e6);
                    slot_per_line[i]  = int(((k % n_slots) + n_slots) % n_slots);
                }
            }
        } else {
            // One tool or a uniform sequence: the emission order is exact for that.
            slot_per_line.resize(raw_lines.size());
            for (size_t i = 0; i < raw_lines.size(); ++i)
                slot_per_line[i] = int(i % size_t(std::max(1, n_slots)));
        }

        // Group the lines by tool, in first appearance order.
        std::vector<int>                             unique_tool_order;
        std::map<int, std::vector<ExtrusionPath *>> tool_blocks;
        for (size_t i = 0; i < raw_lines.size(); ++i) {
            const int tool_idx = tools[size_t(slot_per_line[i])];
            if (tool_blocks.find(tool_idx) == tool_blocks.end())
                unique_tool_order.push_back(tool_idx);
            RawLine &rl = raw_lines[i];
            // The connector arc prints with the colour of its scanline.
            if (!rl.tail.points.empty()) {
                const Points &tp = rl.tail.points;
                const size_t  k0 = !rl.pl.points.empty() && rl.pl.points.back() == tp.front() ? 1 : 0;
                rl.pl.points.insert(rl.pl.points.end(), tp.begin() + k0, tp.end());
            }
            auto *new_path     = new ExtrusionPath(rl.role, rl.mm3, rl.width, rl.height);
            new_path->polyline = Polyline3(rl.pl);
            encode_tool_in_path(new_path, tool_idx);
            tool_blocks[tool_idx].push_back(new_path);
        }
        if (tool_blocks.empty())
            continue;

        if (unique_tool_order.size() == 1) {
            // A single tool: splitting would only break the zig-zag. Keep the original paths, with that tool.
            for (auto &kv : tool_blocks)
                for (ExtrusionPath *p : kv.second)
                    delete p;
            for (ExtrusionEntity *e : sub->entities)
                if (auto *p = dynamic_cast<ExtrusionPath *>(e); p != nullptr && p->mm3_per_mm < 10.0)
                    encode_tool_in_path(p, unique_tool_order.front());
            continue;
        }

        for (int t : unique_tool_order)
            optimize_tool_block_travel(tool_blocks[t]);
        // The separate paths are intentional: each line keeps its own colour. no_sort keeps the blocks together.
        for (ExtrusionEntity *e : sub->entities)
            delete e;
        sub->entities.clear();
        sub->no_sort = true;
        for (int t : unique_tool_order)
            for (ExtrusionPath *p : tool_blocks[t])
                sub->entities.push_back(p);
        any_modified = true;

        if (NeoDebug::enabled(NeoDebug::COLORSTITCH)) {
            std::ostringstream s;
            s << "SPLIT layer=" << layer_idx << " role=" << ExtrusionEntity::role_to_string(r) << " lines=" << raw_lines.size() << " tools=[";
            for (int t : unique_tool_order)
                s << " T" << t << ":" << tool_blocks[t].size();
            s << " ]";
            NeoDebug::write(NeoDebug::COLORSTITCH, s.str());
        }
    }

    int flags = 0;
    if (any_modified)
        flags |= COLORSTITCH_FLAG_MODIFIED;
    if (any_unsplittable)
        flags |= COLORSTITCH_FLAG_UNSPLITTABLE;
    return flags;
}

// ---------------------------------------------------------------------------------------------------------
// Dither builders
// ---------------------------------------------------------------------------------------------------------

double ColorStitch::colorstitch_easing_apply(double t, int easing, double gamma)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (easing) {
    case kColormixEasing_EaseIn: return t * t;
    case kColormixEasing_EaseOut: return 1.0 - (1.0 - t) * (1.0 - t);
    case kColormixEasing_EaseInOut: return t * t * (3.0 - 2.0 * t);
    case kColormixEasing_Gamma: return std::pow(t, std::clamp(gamma, 0.1, 10.0));
    case kColormixEasing_HardBand: return t < 0.5 ? 0.0 : 1.0;
    case kColormixEasing_Linear:
    default: return t;
    }
}

static std::vector<double> linear_t(int n_lines)
{
    std::vector<double> t(size_t(std::max(0, n_lines)));
    const double        denom = std::max(1.0, double(n_lines - 1));
    for (int i = 0; i < n_lines; ++i)
        t[i] = n_lines == 1 ? 0.5 : double(i) / denom;
    return t;
}

std::vector<int> ColorStitch::build_dithered_tools_2color(int n_lines, int tool_a, int tool_b, int pct_a, int easing, double gamma)
{
    return n_lines <= 0 ? std::vector<int>() : build_dithered_tools_2color_at(linear_t(n_lines), tool_a, tool_b, pct_a, easing, gamma);
}

// Bresenham: exactly round(n * pct_b / 100) lines of B, emitted whenever the count lags behind the eased
// target curve at the position of the line.
std::vector<int> ColorStitch::build_dithered_tools_2color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int pct_a, int easing,
                                                             double gamma)
{
    std::vector<int> out;
    const int        n_lines = int(t_asc.size());
    if (n_lines <= 0)
        return out;
    const int p_a = std::clamp(pct_a, 0, 100);
    const int p_b = 100 - p_a;
    if (p_b == 0) {
        out.assign(size_t(n_lines), tool_a);
        return out;
    }
    if (p_a == 0) {
        out.assign(size_t(n_lines), tool_b);
        return out;
    }
    out.reserve(size_t(n_lines));
    const int total_b   = int(std::lround(double(n_lines) * double(p_b) / 100.0));
    int       emitted_b = 0;
    for (int i = 0; i < n_lines; ++i) {
        const double t_eff  = colorstitch_easing_apply(std::clamp(t_asc[size_t(i)], 0.0, 1.0), easing, gamma);
        const int    need_b = int(std::lround(t_eff * double(total_b)));
        if (emitted_b < need_b && emitted_b < total_b) {
            out.push_back(tool_b);
            ++emitted_b;
        } else
            out.push_back(tool_a);
    }
    return out;
}

std::vector<int> ColorStitch::build_dithered_tools_3color(int n_lines, int tool_a, int tool_b, int tool_c, int pct_a, int pct_b, int easing,
                                                          double gamma, double overlap)
{
    return n_lines <= 0 ? std::vector<int>() :
                          build_dithered_tools_3color_at(linear_t(n_lines), tool_a, tool_b, tool_c, pct_a, pct_b, easing, gamma, overlap);
}

// Three-way Bresenham morphing A -> B -> C. Each tool has a hat weight anchored at t = 0, 0.5 and 1 whose half
// width grows with `overlap` (0 = hard bands, 1 = every colour everywhere); the tool most behind its expected
// cumulative share is emitted, so the global ratios stay exact and only the local distribution changes.
std::vector<int> ColorStitch::build_dithered_tools_3color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int tool_c, int pct_a,
                                                             int pct_b, int easing, double gamma, double overlap)
{
    std::vector<int> out;
    const int        n_lines = int(t_asc.size());
    if (n_lines <= 0)
        return out;
    out.reserve(size_t(n_lines));
    const int    p_a            = std::clamp(pct_a, 0, 100);
    const int    p_b            = std::clamp(pct_b, 0, 100 - p_a);
    const double n              = double(n_lines);
    const int    target_a_total = int(std::lround(n * p_a / 100.0));
    const int    target_b_total = int(std::lround(n * p_b / 100.0));
    const int    target_c_total = n_lines - target_a_total - target_b_total;
    const double halfwidth      = 0.5 + std::clamp(overlap, 0.0, 1.0) * 0.5;

    std::vector<double> cum_a(n_lines), cum_b(n_lines), cum_c(n_lines);
    double              sum_a = 0.0, sum_b = 0.0, sum_c = 0.0;
    for (int i = 0; i < n_lines; ++i) {
        const double t_eff = colorstitch_easing_apply(std::clamp(t_asc[size_t(i)], 0.0, 1.0), easing, gamma);
        auto         hat   = [&](double anchor) { return std::max(0.0, halfwidth - std::abs(t_eff - anchor)) / halfwidth; };
        cum_a[i] = (sum_a += hat(0.0));
        cum_b[i] = (sum_b += hat(0.5));
        cum_c[i] = (sum_c += hat(1.0));
    }
    const double area_a = std::max(1e-9, sum_a), area_b = std::max(1e-9, sum_b), area_c = std::max(1e-9, sum_c);
    int          emitted_a = 0, emitted_b = 0, emitted_c = 0;
    for (int i = 0; i < n_lines; ++i) {
        const double def_a = emitted_a < target_a_total ? cum_a[i] / area_a * target_a_total - emitted_a : -1e9;
        const double def_b = emitted_b < target_b_total ? cum_b[i] / area_b * target_b_total - emitted_b : -1e9;
        const double def_c = emitted_c < target_c_total ? cum_c[i] / area_c * target_c_total - emitted_c : -1e9;
        if (def_a >= def_b && def_a >= def_c) {
            out.push_back(tool_a);
            ++emitted_a;
        } else if (def_b >= def_c) {
            out.push_back(tool_b);
            ++emitted_b;
        } else {
            out.push_back(tool_c);
            ++emitted_c;
        }
    }
    return out;
}

std::vector<int> ColorStitch::build_custom_bands(int n_lines, int tool_a, int cnt_a, int tool_b, int cnt_b, int tool_c, int cnt_c, int tool_d, int cnt_d)
{
    std::vector<int> out;
    if (n_lines <= 0)
        return out;
    struct Band { int tool; int count; };
    std::vector<Band> bands;
    for (const Band b : { Band{ tool_a, cnt_a }, Band{ tool_b, cnt_b }, Band{ tool_c, cnt_c }, Band{ tool_d, cnt_d } })
        if (b.count > 0 && b.tool >= 0)
            bands.push_back(b);
    if (bands.empty()) {
        out.assign(size_t(n_lines), tool_a >= 0 ? tool_a : 0);
        return out;
    }
    out.reserve(size_t(n_lines));
    while (int(out.size()) < n_lines)
        for (const Band &b : bands)
            for (int j = 0; j < b.count && int(out.size()) < n_lines; ++j)
                out.push_back(b.tool);
    return out;
}

int ColorStitch::estimate_surface_line_count(double area_mm2, double line_width_mm, double overlap_fraction, double pattern_factor)
{
    if (area_mm2 <= 0.0 || line_width_mm <= 0.0)
        return 0;
    const double spacing = line_width_mm * (1.0 - std::clamp(overlap_fraction, 0.0, 0.99));
    if (spacing <= 1e-6)
        return 0;
    // Diagonal of a square of that area, in lines.
    const double n = std::sqrt(area_mm2) * std::sqrt(2.0) / spacing * std::clamp(pattern_factor, 0.5, 1.5);
    return n > 1e6 ? 1000000 : int(std::lround(n));
}

// ---------------------------------------------------------------------------------------------------------
// Mixed filament helpers
// ---------------------------------------------------------------------------------------------------------

std::string ColorStitch::mixed_filament_to_pattern(const MixedFilament &mf)
{
    if (!mf.manual_pattern.empty())
        return mf.manual_pattern;
    if (!mf.gradient_component_ids.empty())
        return mf.gradient_component_ids;
    std::string pat;
    for (int i = 0; i < std::max(1, mf.ratio_a); ++i)
        pat += std::to_string(mf.component_a);
    for (int i = 0; i < std::max(1, mf.ratio_b); ++i)
        pat += std::to_string(mf.component_b);
    return pat;
}

std::vector<ColorStitchOption> ColorStitch::get_mix_options(const std::string &mixed_defs, const std::vector<std::string> &filament_colours)
{
    std::vector<ColorStitchOption> result;
    const int                      num_physical = int(filament_colours.size());
    if (!mixed_defs.empty()) {
        MixedFilamentManager mgr;
        // The auto rows have to exist before the custom entries are matched against them.
        const bool was_auto = MixedFilamentManager::auto_generate_enabled();
        MixedFilamentManager::set_auto_generate_enabled(true);
        mgr.auto_generate(filament_colours);
        MixedFilamentManager::set_auto_generate_enabled(was_auto);
        mgr.load_custom_entries(mixed_defs, filament_colours);
        mgr.apply_gradient_settings(0, 0.0f, 1.0f, false);
        auto get_color = [&](unsigned int id) { return id >= 1 && id <= filament_colours.size() ? filament_colours[id - 1] : std::string("#888888"); };
        int  virtual_counter = 0;
        for (const MixedFilament &mf : mgr.mixed_filaments()) {
            if (!mf.enabled || mf.deleted)
                continue;
            ColorStitchOption opt;
            opt.filament_id  = num_physical + virtual_counter + 1;
            opt.pattern      = mixed_filament_to_pattern(mf);
            const int pct_b  = std::clamp(mf.mix_b_percent, 0, 100);
            opt.label        = "Mixed " + std::to_string(virtual_counter + 1) + ": F" + std::to_string(mf.component_a) + "+F" +
                               std::to_string(mf.component_b) + " (" + std::to_string(100 - pct_b) + "%/" + std::to_string(pct_b) + "%)";
            opt.tool_weights = extract_recipe_weights(mf, size_t(num_physical));
            opt.display_color = !mf.display_color.empty() ?
                                    mf.display_color :
                                    MixedFilamentManager::blend_color(get_color(mf.component_a), get_color(mf.component_b), 100 - pct_b, pct_b);
            result.push_back(std::move(opt));
            ++virtual_counter;
        }
    }
    for (int i = 0; i < num_physical; ++i) {
        ColorStitchOption opt;
        opt.is_physical   = true;
        opt.filament_id   = i + 1;
        opt.label         = "F" + std::to_string(i + 1);
        opt.pattern       = std::to_string(i + 1);
        opt.display_color = filament_colours[i];
        result.push_back(std::move(opt));
    }
    return result;
}

std::map<int, float> ColorStitch::extract_recipe_weights(const MixedFilament &mf, size_t num_physical)
{
    const std::vector<int> seq = extract_recipe_tools(mf, num_physical);
    std::map<int, float>   weights;
    for (int t : seq)
        weights[t] += 1.f / float(seq.size());
    return weights;
}

// ---------------------------------------------------------------------------------------------------------
// MultiPass and PathBlend settings
// ---------------------------------------------------------------------------------------------------------

MultiPassConfig MultiPassConfig::from_region_config(const PrintRegionConfig &cfg, ExtrusionRole role)
{
    MultiPassConfig c;
#define MP_LOAD(PFX)                                                                                                       \
    c.enabled      = cfg.PFX##enabled.value;                                                                               \
    c.num_passes   = cfg.PFX##num_passes.value;                                                                            \
    c.vary_pattern = cfg.PFX##vary_pattern.value;                                                                          \
    c.tool[0] = cfg.PFX##tool_1.value, c.tool[1] = cfg.PFX##tool_2.value, c.tool[2] = cfg.PFX##tool_3.value;              \
    c.width_ratio[0] = cfg.PFX##width_ratio_1.value, c.width_ratio[1] = cfg.PFX##width_ratio_2.value,                      \
    c.width_ratio[2] = cfg.PFX##width_ratio_3.value;                                                                       \
    c.angle[0] = cfg.PFX##angle_1.value, c.angle[1] = cfg.PFX##angle_2.value, c.angle[2] = cfg.PFX##angle_3.value;        \
    c.fan[0] = cfg.PFX##fan_1.value, c.fan[1] = cfg.PFX##fan_2.value, c.fan[2] = cfg.PFX##fan_3.value;                    \
    c.speed_pct[0] = cfg.PFX##speed_pct_1.value, c.speed_pct[1] = cfg.PFX##speed_pct_2.value,                              \
    c.speed_pct[2] = cfg.PFX##speed_pct_3.value;                                                                           \
    c.gcode_start[0] = cfg.PFX##gcode_start_1.value, c.gcode_start[1] = cfg.PFX##gcode_start_2.value,                      \
    c.gcode_start[2] = cfg.PFX##gcode_start_3.value;                                                                       \
    c.gcode_end[0] = cfg.PFX##gcode_end_1.value, c.gcode_end[1] = cfg.PFX##gcode_end_2.value,                              \
    c.gcode_end[2] = cfg.PFX##gcode_end_3.value;
    if (role == erPenultimateInfill) {
        MP_LOAD(penultimate_multipass_)
        c.surface = 2;
    } else {
        MP_LOAD(multipass_)
        c.surface = cfg.multipass_surface.value;
    }
#undef MP_LOAD
    return c;
}

// Lossy conversion of the first PathBlend model (pass count, ratios of the layer height) to the geometry model.
static PathBlendPassConfig pathblend_convert_legacy_to_v2(int legacy_num_passes, int legacy_tool_first, int legacy_tool_last, float legacy_min_ratio,
                                                          float legacy_max_ratio, int legacy_ease_mode, int legacy_fill_angle)
{
    PathBlendPassConfig c;
    c.mode = legacy_num_passes <= 1 ? PathBlendPassConfig::Mode::Half : PathBlendPassConfig::Mode::Full;
    // The ratios were fractions of the layer height; 0.2 mm is assumed until apply_constraints() knows it.
    const double H_default = 0.20;
    c.floor_mm    = float(std::clamp(double(legacy_min_ratio) * H_default, 0.01, H_default - 0.04));
    c.mid_end_mm  = float(std::clamp(double(legacy_max_ratio) * H_default, double(c.floor_mm), H_default - 0.04));
    c.tool_bottom = legacy_tool_first;
    c.tool_top    = c.mode == PathBlendPassConfig::Mode::Full ? legacy_tool_last : -1;
    c.ease_mode   = std::clamp(legacy_ease_mode, 0, 3);
    c.fill_angle  = legacy_fill_angle;
    c.sync_legacy_view();
    return c;
}

PathBlendPassConfig ColorStitch::pathblend_from_profile_payload(const SurfaceEffectPayload &payload)
{
    auto gi = [&](const char *k, int def) {
        const auto it = payload.kv.find(k);
        return it == payload.kv.end() ? def : std::atoi(it->second.c_str());
    };
    auto gf = [&](const char *k, double def) {
        const auto it = payload.kv.find(k);
        return it == payload.kv.end() ? def : std::atof(it->second.c_str());
    };
    const int legacy_np    = std::clamp(gi("pathblend_num_passes", 2), 1, 4);
    const int legacy_t0    = gi("pathblend_tool_1", 0);
    const int legacy_tlast = legacy_np >= 4 ? gi("pathblend_tool_4", 3) :
                             legacy_np == 3 ? gi("pathblend_tool_3", 2) :
                             legacy_np == 2 ? gi("pathblend_tool_2", 1) :
                                              legacy_t0;
    PathBlendPassConfig c = pathblend_convert_legacy_to_v2(legacy_np, legacy_t0, legacy_tlast,
                                                           float(std::clamp(gf("pathblend_min_ratio", 0.05), 0.01, 0.49)),
                                                           float(std::clamp(gf("pathblend_max_ratio", 1.0), 0.51, 1.0)),
                                                           std::clamp(gi("pathblend_ease_mode", 0), 0, 3), gi("pathblend_fill_angle", -1));
    const auto en = payload.kv.find("multipass_path_gradient");
    c.enabled     = en != payload.kv.end() && (en->second == "1" || en->second == "true");
    c.surface     = gi("pathblend_surface", 0);
    c.sync_legacy_view();
    return c;
}

void PathBlendPassConfig::sync_legacy_view()
{
    num_passes = mode == Mode::Full ? 2 : 1;
    tool[0]    = tool_bottom;
    tool[1]    = mode == Mode::Full ? tool_top : -1;
    tool[2]    = -1;
    tool[3]    = -1;
}

void PathBlendPassConfig::apply_constraints(double layer_height_mm)
{
    constexpr float kRampGap = 0.001f; // the ramp must climb: mid_end > floor
    constexpr float kAutoCap = 0.04f;  // default cap of an auto Full ramp, not a limit
    constexpr float kMinSpan = 0.02f;
    const double    H         = std::max(0.04, layer_height_mm);
    const float     floor_max = std::max(0.01f, float(H) - kRampGap);
    floor_mm                  = std::clamp(floor_mm, 0.01f, floor_max);
    if (mode == Mode::Full) {
        const float mid_min = floor_mm + kRampGap;
        if (mid_end_mm < 0.f)
            mid_end_mm = std::max(mid_min, float(H) - kAutoCap);
        // mid_end may reach H: the ramp then fills the layer there and the cap vanishes.
        mid_end_mm = std::clamp(mid_end_mm, mid_min, float(H));
    } else {
        mid_end_mm = float(H);
    }
    in_t  = std::clamp(in_t, 0.0f, 1.0f - kMinSpan);
    out_t = std::clamp(out_t, in_t + kMinSpan, 1.0f);
}

double PathBlendPassConfig::profile_u(double t) const
{
    const double a    = std::clamp(double(in_t), 0.0, 1.0);
    const double span = std::clamp(double(out_t), 0.0, 1.0) - a;
    return span <= 1e-6 ? std::clamp(t, 0.0, 1.0) : std::clamp((t - a) / span, 0.0, 1.0);
}

std::string PathBlendPassConfig::to_blob_json() const
{
    nlohmann::json j;
    j["v"]           = 4;
    j["mode"]        = mode == Mode::Full ? "full" : "half";
    j["floor_mm"]    = floor_mm;
    j["mid_end_mm"]  = mid_end_mm;
    j["tool_bottom"] = tool_bottom;
    j["tool_top"]    = tool_top;
    j["ease_mode"]   = ease_mode;
    j["fill_angle"]  = fill_angle;
    j["in_t"]        = in_t;
    j["out_t"]       = out_t;
    j["span_mm"]     = span_mm;
    return j.dump();
}

PathBlendPassConfig PathBlendPassConfig::from_blob_json(const std::string &blob)
{
    PathBlendPassConfig c;
    c.sync_legacy_view();
    if (blob.empty())
        return c;
    try {
        const nlohmann::json j = nlohmann::json::parse(blob);
        if (!j.is_object())
            return c;
        auto gi = [&](const char *k, int def) { return j.contains(k) && j[k].is_number() ? j[k].get<int>() : def; };
        auto gf = [&](const char *k, double def) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : def; };
        if (gi("v", 1) >= 2 || j.contains("mode") || j.contains("floor_mm") || j.contains("mid_end_mm")) {
            const std::string mode_str = j.contains("mode") && j["mode"].is_string() ? j["mode"].get<std::string>() : "full";
            c.mode        = mode_str == "half" ? Mode::Half : Mode::Full;
            c.floor_mm    = std::max(0.01f, float(gf("floor_mm", 0.01)));
            c.mid_end_mm  = float(gf("mid_end_mm", -1.0)); // < 0 stays auto
            c.tool_bottom = gi("tool_bottom", 0);
            c.tool_top    = gi("tool_top", 1);
            c.ease_mode   = std::clamp(gi("ease_mode", 0), 0, 3);
            c.fill_angle  = gi("fill_angle", -1);
            c.in_t        = float(gf("in_t", 0.0));
            c.out_t       = float(gf("out_t", 1.0));
            // The fitted field is the only distribution left: a stored -1 reads as 0.
            c.span_mm = std::max(0.f, float(gf("span_mm", 0.0)));
            if (c.mid_end_mm >= 0.f && c.mid_end_mm < c.floor_mm)
                c.mid_end_mm = c.floor_mm;
        } else {
            // First blob schema: pass count, tool list and ratios.
            const int legacy_np = std::clamp(gi("num_passes", 2), 1, 4);
            int       legacy_t0 = 0, legacy_tlast = 1;
            if (j.contains("tool") && j["tool"].is_array()) {
                const nlohmann::json &t = j["tool"];
                if (!t.empty() && t[0].is_number())
                    legacy_t0 = t[0].get<int>();
                if (int(t.size()) > legacy_np - 1 && t[legacy_np - 1].is_number())
                    legacy_tlast = t[legacy_np - 1].get<int>();
            }
            return pathblend_convert_legacy_to_v2(legacy_np, legacy_t0, legacy_tlast, float(std::clamp(gf("min_ratio", 0.05), 0.01, 0.49)),
                                                  float(std::clamp(gf("max_ratio", 1.0), 0.51, 1.0)), std::clamp(gi("ease_mode", 0), 0, 3),
                                                  gi("fill_angle", -1));
        }
    } catch (const std::exception &e) {
        NEOTKO_LOG(MULTIPASS, "PB_BLOB parse error: " << e.what() << ", PathBlend defaults used");
    }
    c.sync_legacy_view();
    return c;
}

PathBlendPassConfig PathBlendPassConfig::from_region_config(const PrintRegionConfig &cfg, ExtrusionRole role)
{
    const std::string  &blob = role == erPenultimateInfill ? cfg.pathblend_penu.value : cfg.pathblend_top.value;
    PathBlendPassConfig c;
    if (!blob.empty()) {
        c = from_blob_json(blob);
    } else {
        const int np = cfg.pathblend_num_passes.value;
        c = pathblend_convert_legacy_to_v2(std::clamp(np, 1, 4), cfg.pathblend_tool_1.value,
                                           np >= 4 ? cfg.pathblend_tool_4.value :
                                           np == 3 ? cfg.pathblend_tool_3.value :
                                           np == 2 ? cfg.pathblend_tool_2.value :
                                                     cfg.pathblend_tool_1.value,
                                           float(std::clamp(cfg.pathblend_min_ratio.value, 0.01, 0.49)),
                                           float(std::clamp(cfg.pathblend_max_ratio.value, 0.51, 1.0)),
                                           std::clamp(cfg.pathblend_ease_mode.value, 0, 3), cfg.pathblend_fill_angle.value);
    }
    // Enable and surface are shared keys, never stored in the per-zone blob.
    c.enabled = cfg.multipass_path_gradient.value;
    c.surface = cfg.pathblend_surface.value;
    c.sync_legacy_view();
    return c;
}

// ---------------------------------------------------------------------------------------------------------
// SurfacePassStack
// ---------------------------------------------------------------------------------------------------------

static nlohmann::json sandwich_payload_to_json(const SurfaceEffectPayload &p)
{
    nlohmann::json j;
    j["present"] = p.present;
    j["kv"]      = p.kv;
    return j;
}

static SurfaceEffectPayload sandwich_payload_from_json(const nlohmann::json &j)
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

bool SurfacePassStack::all_solid() const
{
    return std::all_of(passes.begin(), passes.end(), [](const SurfacePass &p) { return p.kind == SurfacePassKind::Solid; });
}

bool SurfacePassStack::any_effect() const
{
    return std::any_of(passes.begin(), passes.end(), [](const SurfacePass &p) { return p.kind != SurfacePassKind::None; });
}

std::string SurfacePassStack::to_json() const
{
    // Three states: unauthored (no passes) -> "", authored without effect -> a single None pass, so that the
    // round trip keeps "no effect" apart from "not authored", and authored with effect -> every pass.
    if (passes.empty())
        return std::string();
    nlohmann::json root;
    root["v"]                        = 1;
    root["perimeter_override"]       = perimeter_override;
    root["bottom_supported_control"] = bottom_supported_control;
    nlohmann::json arr               = nlohmann::json::array();
    if (!enabled || !any_effect()) {
        root["enabled"] = false;
        nlohmann::json e;
        e["kind"]  = int(SurfacePassKind::None);
        e["ratio"] = 1.0;
        arr.push_back(std::move(e));
    } else {
        root["enabled"] = enabled;
        for (const SurfacePass &p : passes) {
            nlohmann::json e;
            e["kind"]        = int(p.kind);
            e["ratio"]       = p.ratio;
            e["solid_tool"]  = p.solid_tool;
            e["angle"]       = p.angle;
            e["fan"]         = p.fan;
            e["speed_pct"]   = p.speed_pct;
            e["gcode_start"] = p.gcode_start;
            e["gcode_end"]   = p.gcode_end;
            e["colormix"]    = sandwich_payload_to_json(p.colorstitch);
            e["pathblend"]   = sandwich_payload_to_json(p.pathblend);
            arr.push_back(std::move(e));
        }
    }
    root["passes"] = std::move(arr);
    return root.dump();
}

SurfacePassStack SurfacePassStack::from_json(const std::string &text)
{
    SurfacePassStack st;
    if (text.empty())
        return st;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception &e) {
        NEOTKO_LOG(MULTIPASS, "SANDWICH from_json parse error: " << e.what());
        return st;
    }
    if (!root.is_object() || !root.contains("passes") || !root["passes"].is_array())
        return st;
    if (root.contains("enabled") && root["enabled"].is_boolean())
        st.enabled = root["enabled"].get<bool>();
    if (root.contains("perimeter_override") && root["perimeter_override"].is_boolean())
        st.perimeter_override = root["perimeter_override"].get<bool>();
    if (root.contains("bottom_supported_control") && root["bottom_supported_control"].is_boolean())
        st.bottom_supported_control = root["bottom_supported_control"].get<bool>();
    for (const nlohmann::json &e : root["passes"]) {
        if (!e.is_object())
            continue;
        if (int(st.passes.size()) >= kMaxPasses)
            break;
        SurfacePass p;
        if (e.contains("kind") && e["kind"].is_number_integer()) {
            const int k = e["kind"].get<int>();
            if (k >= 0 && k <= 3)
                p.kind = SurfacePassKind(k);
        }
        if (e.contains("ratio") && e["ratio"].is_number())                 p.ratio       = e["ratio"].get<double>();
        if (e.contains("solid_tool") && e["solid_tool"].is_number_integer()) p.solid_tool = e["solid_tool"].get<int>();
        if (e.contains("angle") && e["angle"].is_number_integer())         p.angle       = e["angle"].get<int>();
        if (e.contains("fan") && e["fan"].is_number_integer())             p.fan         = e["fan"].get<int>();
        if (e.contains("speed_pct") && e["speed_pct"].is_number_integer()) p.speed_pct   = e["speed_pct"].get<int>();
        if (e.contains("gcode_start") && e["gcode_start"].is_string())     p.gcode_start = e["gcode_start"].get<std::string>();
        if (e.contains("gcode_end") && e["gcode_end"].is_string())         p.gcode_end   = e["gcode_end"].get<std::string>();
        if (e.contains("colormix"))  p.colorstitch = sandwich_payload_from_json(e["colormix"]);
        if (e.contains("pathblend")) p.pathblend   = sandwich_payload_from_json(e["pathblend"]);
        st.passes.push_back(std::move(p));
    }
    // A disabled stack with effect passes meant "no effect": normalize it to the explicit [None] passthrough.
    if (!st.enabled && st.any_effect()) {
        st.passes.clear();
        SurfacePass none_pass;
        none_pass.kind  = SurfacePassKind::None;
        none_pass.ratio = 1.0;
        st.passes.push_back(std::move(none_pass));
    }
    return st;
}

SurfacePassStack SurfacePassStack::synthesize_from_legacy(const PrintRegionConfig &cfg, ExtrusionRole role)
{
    SurfacePassStack st;
    const bool       is_penu = role == erPenultimateInfill;
    // In practice the legacy effects exclude each other per surface: MultiPass, then ColorStitch, then PathBlend.
    if (is_penu ? cfg.penultimate_multipass_enabled.value : cfg.multipass_enabled.value) {
        const MultiPassConfig mp = MultiPassConfig::from_region_config(cfg, role);
        if (ColorStitch::should_process_role(role, mp.surface)) {
            // Disabled passes (tool < 0) are kept: their share still counts in the Z of the passes above.
            const int n = std::clamp(mp.num_passes, 1, kMaxPasses);
            for (int i = 0; i < n; ++i) {
                SurfacePass p;
                p.kind        = SurfacePassKind::Solid;
                p.ratio       = mp.width_ratio[i];
                p.solid_tool  = mp.tool[i];
                p.angle       = mp.angle[i];
                p.fan         = mp.fan[i];
                p.speed_pct   = mp.speed_pct[i];
                p.gcode_start = mp.gcode_start[i];
                p.gcode_end   = mp.gcode_end[i];
                st.passes.push_back(std::move(p));
            }
            st.enabled            = true;
            st.perimeter_override = cfg.multipass_perimeter_override.value;
            return st;
        }
    }
    auto wanted = [is_penu](int surf) { return surf == 0 || (is_penu ? surf == 2 : surf == 1); };
    if (cfg.interlayer_colormix_enabled.value && wanted(cfg.interlayer_colormix_surface.value)) {
        // An empty key/value set: the engine reads the region preset.
        SurfacePass p;
        p.kind                = SurfacePassKind::ColorStitch;
        p.ratio               = 1.0;
        p.colorstitch.present = true;
        st.passes.push_back(std::move(p));
        st.enabled = true;
        return st;
    }
    if (cfg.multipass_path_gradient.value && wanted(cfg.pathblend_surface.value)) {
        SurfacePass p;
        p.kind                = SurfacePassKind::PathBlend;
        p.ratio               = 1.0;
        p.pathblend.present   = true;
        p.pathblend.kv["blob"] = PathBlendPassConfig::from_region_config(cfg, role).to_blob_json();
        st.passes.push_back(std::move(p));
        st.enabled = true;
    }
    return st;
}

SurfacePassStack SurfacePassStack::resolve(const PrintRegionConfig &cfg, ExtrusionRole role)
{
    SurfacePassStack st = from_json(role == erPenultimateInfill ? cfg.neotko_surface_passes_penu.value : cfg.neotko_surface_passes_top.value);
    if (st.passes.empty())
        st = synthesize_from_legacy(cfg, role);
    return st;
}

SurfacePassStack SurfacePassStack::resolve_for_zone(const DynamicPrintConfig &cfg, bool penu)
{
    PrintRegionConfig rc;
    rc.apply(cfg, true);
    return resolve(rc, penu ? erPenultimateInfill : erTopSolidInfill);
}

MultiPassConfig SurfacePassStack::to_multipass_config(ExtrusionRole role) const
{
    MultiPassConfig c;
    c.enabled      = enabled && !passes.empty();
    c.surface      = role == erPenultimateInfill ? 2 : 0;
    c.num_passes   = std::min(kMaxPasses, int(passes.size()));
    c.vary_pattern = false;
    for (int i = 0; i < c.num_passes; ++i) {
        const SurfacePass &p = passes[i];
        c.tool[i]            = p.kind == SurfacePassKind::Solid ? p.solid_tool : -1;
        c.width_ratio[i]     = p.ratio;
        c.angle[i]           = p.angle;
        c.fan[i]             = p.fan;
        c.speed_pct[i]       = p.speed_pct;
        c.gcode_start[i]     = p.gcode_start;
        c.gcode_end[i]       = p.gcode_end;
    }
    return c;
}

SurfacePassStack SurfacePassStack::from_multipass_config(const MultiPassConfig &mp)
{
    SurfacePassStack st;
    st.enabled  = mp.enabled;
    const int n = std::clamp(mp.num_passes, 1, kMaxPasses);
    for (int i = 0; i < n; ++i) {
        SurfacePass p;
        p.kind        = SurfacePassKind::Solid;
        p.ratio       = mp.width_ratio[i];
        p.solid_tool  = mp.tool[i];
        p.angle       = mp.angle[i];
        p.fan         = mp.fan[i];
        p.speed_pct   = mp.speed_pct[i];
        p.gcode_start = mp.gcode_start[i];
        p.gcode_end   = mp.gcode_end[i];
        st.passes.push_back(std::move(p));
    }
    return st;
}

// Clones every leaf of an encoded collection, decodes its tool and routes it to that tool's bucket.
static void sandwich_bucket_visit(const ExtrusionEntity *e, int default_tool, std::vector<std::pair<int, ExtrusionEntityCollection>> &buckets)
{
    if (e == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        for (const ExtrusionEntity *c : coll->entities)
            sandwich_bucket_visit(c, default_tool, buckets);
        return;
    }
    ExtrusionEntity *cl   = e->clone();
    int              tool = default_tool;
    if (auto *p = dynamic_cast<ExtrusionPath *>(cl); p != nullptr && p->mm3_per_mm >= 10.0) {
        const int t = int(std::floor(p->mm3_per_mm / 10.0)) - 1;
        if (t >= 0) {
            tool = t;
            p->mm3_per_mm -= double(t + 1) * 10.0;
        }
    }
    auto it = std::find_if(buckets.begin(), buckets.end(), [tool](const auto &b) { return b.first == tool; });
    if (it == buckets.end()) {
        buckets.emplace_back(tool, ExtrusionEntityCollection());
        it = std::prev(buckets.end());
    }
    it->second.entities.push_back(cl);
}

std::vector<std::pair<int, ExtrusionEntityCollection>> ColorStitch::eec_to_tool_buckets(const ExtrusionEntityCollection &encoded, int default_tool)
{
    std::vector<std::pair<int, ExtrusionEntityCollection>> buckets;
    for (const ExtrusionEntity *e : encoded.entities)
        sandwich_bucket_visit(e, default_tool, buckets);
    return buckets;
}

// ---------------------------------------------------------------------------------------------------------
// PathBlend bands
// ---------------------------------------------------------------------------------------------------------

std::vector<PBBand> compute_pb_bands(const PathBlendPassConfig &pb, double bottom_z, double H, double min_band_h, bool want_cap, int target_k,
                                     double min_printable_h)
{
    std::vector<PBBand> out;
    if (H <= 0.0 || min_band_h <= 0.0)
        return out;
    const double min_h_phys = min_printable_h > 0.0 ? min_printable_h : min_band_h;
    const float  floor_mm   = std::max(float(min_h_phys), std::max(0.01f, pb.floor_mm));
    const float  mid_end_mm = pb.mode == PathBlendPassConfig::Mode::Full ? std::min(pb.mid_end_mm, float(H - min_h_phys)) :
                                                                           std::min(pb.mid_end_mm, float(H));
    if (mid_end_mm <= floor_mm)
        return out;
    const double range_mm  = double(mid_end_mm) - double(floor_mm);
    const int    K_natural = std::max(1, int(std::floor(range_mm / min_band_h)));
    const int    K_ramp    = target_k > 0 ? std::min(target_k, K_natural) : K_natural;
    const double h_step    = range_mm / double(K_ramp);
    const double t_step    = 1.0 / double(K_ramp);
    for (int k = 0; k < K_ramp; ++k) {
        PBBand b;
        b.is_cap = false;
        b.t_lo   = float(k * t_step);
        b.t_hi   = float((k + 1) * t_step);
        b.t_mid  = 0.5f * (b.t_lo + b.t_hi);
        b.z_top  = float(bottom_z + double(floor_mm) + double(k + 1) * h_step);
        b.h_step = float(h_step);
        out.push_back(b);
    }
    if (!want_cap)
        return out;
    for (int k = 0; k < K_ramp; ++k) {
        const PBBand r = out[k];
        PBBand       c = r;
        c.is_cap       = true;
        c.z_top        = float(bottom_z + H);
        c.h_step       = float(std::max(min_h_phys, H - (double(floor_mm) + double(r.t_mid) * range_mm)));
        out.push_back(c);
    }
    return out;
}

} // namespace Slic3r
