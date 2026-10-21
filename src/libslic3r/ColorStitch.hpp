// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

// Surface "Sandwich" engine: the top, penultimate and bottom surfaces of an object are printed as a stack
// of thin passes inside their layer (MultiPassSubLayer, see Print.hpp). A pass is Solid (one tool),
// ColorStitch (the tool is chosen per fill line) or PathBlend (a Z and flow ramp across the surface).
// A recipe comes either from the region preset (the interlayer_colormix_*, multipass_*, pathblend_* and
// neotko_surface_passes_* keys) or, in "painter mode", from the Surface Effect Profiles painted on the
// faces of the object (ModelVolume::colorstitch_paint_facets). See docs/HLSD/sandwich.md.
//
// Tools are 0-based filament indices (0 = filament 1). In a ColorStitch pattern string the digits 1-4 are
// filaments 1-4 and, with interlayer_colormix_use_virtual, the digits 5-9 are the first to fifth live mixed
// filament, expanded to their physical components.

#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "MixedFilament.hpp"
#include "Point.hpp"
#include "Polyline.hpp"
#include "PrintConfig.hpp"
#include "SurfaceEffectProfile.hpp"
#include "SurfacePassKind.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;
class PrintObject;
struct MultiPassConfig;
struct PathBlendPassConfig;

// One selectable entry of the ColorStitch pattern picker.
struct ColorStitchOption {
    std::string label;          // "Mixed (F3+F4)" or "F1"
    std::string pattern;        // "12", "1221", "123", ...
    std::string display_color;  // "#RRGGBB"
    bool        is_physical = false;
    int         filament_id = 0; // 1-based: 1..N physical, N+1.. virtual mixed
    // 0-based physical tool -> normalized weight, for virtual options only.
    std::map<int, float> tool_weights;
};

// Painted triangles of an object, per paint slot, in the slice frame (PrintObject::trafo_centered()).
// Building it means deserializing a TriangleSelector per volume, so PrintObject::infill() builds it once
// before the fill threads start and they only read it.
struct ColorStitchPaintCache {
    struct Tri {
        Vec3d v0, v1, v2;
        int   volume_idx = -1; // index into ModelObject::volumes
    };
    // slot -> triangles. Only slots mapped to an existing profile are kept.
    std::map<int, std::vector<Tri>> slots;

    bool empty() const { return slots.empty(); }
    static std::shared_ptr<const ColorStitchPaintCache> build(const PrintObject &po);
};

// Return flags of ColorStitch::assign_and_group_tools().
static constexpr int COLORSTITCH_FLAG_MODIFIED     = 1; // at least one surface was split per tool
static constexpr int COLORSTITCH_FLAG_UNSPLITTABLE = 2; // a surface could not be split into lines

class ColorStitch {
public:
    // Splits the top / penultimate (and painted bottom) fill paths of `fills` into lines and assigns a
    // tool to each line, encoding it in mm3_per_mm (see encode_tool_in_path()). Lines of one tool are
    // grouped and travel-optimized. `allow_top` / `allow_penu` filter the roles. With `mgr` and
    // `num_physical` the digits 5-9 of a pattern expand to mixed filament recipes. With `print_object`
    // a painted profile overrides the preset at this layer unless `config_has_pass_override` says that
    // `config` already carries the key/value set of the pass being printed.
    static int assign_and_group_tools(ExtrusionEntityCollection  &fills,
                                      const PrintRegionConfig    &config,
                                      ExtrusionRole               role,
                                      int                         layer_idx,
                                      bool                        allow_top                = true,
                                      bool                        allow_penu               = true,
                                      const MixedFilamentManager *mgr                      = nullptr,
                                      size_t                      num_physical             = 0,
                                      const PrintObject          *print_object             = nullptr,
                                      double                      layer_print_z            = 0.0,
                                      double                      layer_height             = 0.0,
                                      bool                        config_has_pass_override = false);

    // surface: kColormixSurface_Both / _Top / _Penultimate.
    static bool should_process_role(ExtrusionRole role, int surface);

    // The profile library of the model the object belongs to (Model::surface_effect_profiles).
    static const SurfaceEffectProfileManager *profiles_of(const ModelObject *mo);
    static const SurfaceEffectProfileManager *profiles_of(const PrintObject *po);
    static const SurfaceEffectProfile        *find_profile(const PrintObject *po, int profile_id);

    // Painter mode: some model part has painted facets and a slot mapped to a profile.
    static bool object_has_any_colorstitch_paint(const ModelObject *mo);
    // Painter queries over a Z band of the slice frame. `downward` looks at the faces looking down
    // (bottom surfaces, min Z) instead of the ones looking up (top surfaces, max Z).
    // The slot with most painted triangles in the band, 0 if none.
    static int              dominant_painted_slot_in_z_range(const PrintObject *po, double z_min, double z_max, bool downward = false);
    // Every painted slot in the band, ascending.
    static std::vector<int> enumerate_painted_slots_in_z_range(const PrintObject *po, double z_min, double z_max, bool downward = false);
    // XY footprint of a slot in the band (scaled, slice frame), the area the recipe of that slot covers.
    static ExPolygons       painted_footprint_in_z_range(const PrintObject *po, int slot, double z_min, double z_max, bool downward = false);
    // Index of the first model part whose paint of `slot` falls in the band, -1 if none. Slot tables are
    // per volume, so on an assembled object two parts may map the same slot to different recipes.
    static int              painted_slot_owner_volume_index_in_z_range(const PrintObject *po, int slot, double z_min, double z_max, bool downward = false);
    // Profile id of a slot: from the volume `vol_idx` when given, else from the first model part mapping it.
    static int              profile_id_for_slot(const PrintObject *po, int slot);
    static int              profile_id_for_slot_in_volume(const PrintObject *po, int slot, int vol_idx);

    // 1-based tools the ColorStitch payload of a profile uses for a role (for the tool ordering).
    static std::vector<unsigned int> painted_profile_tools_1based(const SurfaceEffectProfile &p, bool top_role);
    static bool                      painted_perim_override_from_profile(const SurfaceEffectPayload &payload);
    // Whether a profile painted at this layer asks for the walls to be reprinted by its passes.
    static bool                      any_painted_profile_has_perim_override(const PrintObject *po, double print_z, double height);
    // Fixed ColorStitch angle of a profile (degrees) or -1 for auto.
    static int                       colorstitch_angle_for_profile(const SurfaceEffectProfile *p, bool penu);
    static PathBlendPassConfig       pathblend_from_profile_payload(const SurfaceEffectPayload &payload);

    // A painted profile declares a penultimate (resp. bottom) zone: those surfaces have to exist even
    // when the preset does not create them.
    static bool object_painter_wants_penu(const ModelObject *mo);
    static bool object_painter_wants_bottom(const ModelObject *mo);

    // Tool encoding of a ColorStitch line: mm3_per_mm += (tool + 1) * 10. It never leaves the fill
    // step: eec_to_tool_buckets() decodes it into one collection per tool, in first-appearance order,
    // unencoded paths going to `default_tool`. The caller owns the cloned entities.
    static void encode_tool_in_path(ExtrusionPath *path, int tool_idx);
    static std::vector<std::pair<int, ExtrusionEntityCollection>> eec_to_tool_buckets(const ExtrusionEntityCollection &encoded,
                                                                                      int default_tool);

    // Easing of the gradient position t in [0, 1] before the dither.
    enum ColormixEasing : int {
        kColormixEasing_Linear    = 0,
        kColormixEasing_EaseIn    = 1,
        kColormixEasing_EaseOut   = 2,
        kColormixEasing_EaseInOut = 3,
        kColormixEasing_Gamma     = 4,
        kColormixEasing_HardBand  = 5,
    };
    static double colorstitch_easing_apply(double t, int easing, double gamma = 1.0);

    // Bresenham dithered tool sequences, one entry per line (or per lane for the _at variants, which
    // evaluate the target curve at the real position t of each lane, ascending). With t = i / (n - 1) the
    // _at variants return exactly what the counting ones return.
    static std::vector<int> build_dithered_tools_2color(int n_lines, int tool_a, int tool_b, int pct_a,
                                                        int easing = kColormixEasing_Linear, double gamma = 1.0);
    static std::vector<int> build_dithered_tools_3color(int n_lines, int tool_a, int tool_b, int tool_c, int pct_a, int pct_b,
                                                        int easing = kColormixEasing_Linear, double gamma = 1.0, double overlap = 0.6);
    static std::vector<int> build_dithered_tools_2color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int pct_a,
                                                           int easing = kColormixEasing_Linear, double gamma = 1.0);
    static std::vector<int> build_dithered_tools_3color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int tool_c,
                                                           int pct_a, int pct_b, int easing = kColormixEasing_Linear,
                                                           double gamma = 1.0, double overlap = 0.6);
    // Hard bands: cnt_a lines of tool_a, then cnt_b of tool_b, ... repeated until n_lines.
    static std::vector<int> build_custom_bands(int n_lines, int tool_a, int cnt_a, int tool_b, int cnt_b,
                                               int tool_c, int cnt_c, int tool_d, int cnt_d);
    // Rough fill line count of a surface, for UI feedback.
    static int estimate_surface_line_count(double area_mm2, double line_width_mm, double overlap_fraction = 0.0, double pattern_factor = 1.0);

    // Mixed filament helpers of the pattern picker.
    static std::vector<ColorStitchOption> get_mix_options(const std::string &mixed_defs, const std::vector<std::string> &filament_colours);
    static std::string                    mixed_filament_to_pattern(const MixedFilament &mf);
    static std::map<int, float>           extract_recipe_weights(const MixedFilament &mf, size_t num_physical);

    // The pattern string as a tool sequence (0-based). Repetitions are kept ("1122" is two bands of two).
    // A pattern naming a single filament returns it twice; fewer than one falls back to tool_a..d.
    static std::vector<int> build_tool_list_from_pattern(const std::string &pattern, const PrintRegionConfig &config,
                                                         const MixedFilamentManager *mgr = nullptr, size_t num_physical = 0);

    // Painter paint lookups go through the cache of the object when it has one.
    static std::shared_ptr<const ColorStitchPaintCache> paint_cache(const PrintObject *po);
};

// Legacy MultiPass settings of a role (multipass_* for the top, penultimate_multipass_* for the
// penultimate surface). Up to three passes.
struct MultiPassConfig {
    bool        enabled        = false;
    int         surface        = 0; // 0 both, 1 top only, 2 penultimate only
    int         num_passes     = 2;
    int         tool[3]        = { 0, 1, -1 }; // -1 = pass disabled
    double      width_ratio[3] = { 0.50, 0.50, 0.34 };
    bool        vary_pattern   = false;
    int         angle[3]       = { -1, -1, -1 }; // -1 = auto
    int         fan[3]         = { -1, -1, -1 }; // 0-255, -1 = no change
    int         speed_pct[3]   = { 100, 100, 100 };
    std::string gcode_start[3] = { "", "", "" };
    std::string gcode_end[3]   = { "", "", "" };

    static MultiPassConfig from_region_config(const PrintRegionConfig &cfg, ExtrusionRole role = erTopSolidInfill);
};

// One PathBlend pass. The ramp of tool_bottom climbs from floor_mm (t = 0) to mid_end_mm (t = 1) above the
// layer below; in Full mode a cap of tool_top fills the rest of the layer, so that ramp + cap = layer height
// at every point. t runs across the fill lines, one height per line.
struct PathBlendPassConfig {
    enum class Mode : int { Half = 0, Full = 1 };

    bool  enabled     = false;
    int   surface     = 0; // 0 both, 1 top, 2 penultimate
    Mode  mode        = Mode::Full;
    float floor_mm    = 0.01f;
    float mid_end_mm  = -1.0f; // < 0 = auto: the tallest legal ramp
    int   tool_bottom = 0;
    int   tool_top    = 1;     // Full only, -1 in Half
    int   ease_mode   = 0;     // 0 linear, 1 ease in, 2 ease out, 3 ease in-out
    int   fill_angle  = COLORSTITCH_DEFAULT_ANGLE_DEG; // -1 = auto (alternates per layer)
    // Scale of the ramp: 0 = fit to the surface, > 0 = physical period in mm.
    float span_mm     = 0.0f;
    // Start / end zone: the ramp stays flat at the floor until in_t and at mid_end from out_t on.
    float in_t        = 0.0f;
    float out_t       = 1.0f;

    // Remapped ramp position, shared by the ramp and the cap so that their heights add up.
    double profile_u(double t) const;

    // Legacy view: num_passes 1 (Half) or 2 (Full), tool[] = { tool_bottom, tool_top }.
    int  num_passes = 2;
    int  tool[4]    = { 0, 1, -1, -1 };
    void sync_legacy_view();

    // Physical clamp of floor_mm / mid_end_mm for a layer height.
    void apply_constraints(double layer_height_mm);

    // From the per-zone blob (pathblend_top / pathblend_penu) or the flat pathblend_* keys.
    static PathBlendPassConfig from_region_config(const PrintRegionConfig &cfg, ExtrusionRole role = erTopSolidInfill);
    std::string                to_blob_json() const;
    static PathBlendPassConfig from_blob_json(const std::string &blob);
};

// One pass of a Sandwich stack.
struct SurfacePass {
    SurfacePassKind kind  = SurfacePassKind::Solid;
    double          ratio = 0.0; // share of the layer height; the shares of a stack add up to about 1

    // Solid pass.
    int         solid_tool = 0;   // 0-based filament
    int         angle      = -1;  // -1 = auto
    int         fan        = -1;  // 0-255, -1 = no change
    int         speed_pct  = 100; // M220 override, 100 = none
    std::string gcode_start;
    std::string gcode_end;

    // ColorStitch pass: interlayer_colormix_* keys (empty = the region preset). PathBlend pass: "blob" =
    // PathBlendPassConfig::to_blob_json().
    SurfaceEffectPayload colorstitch;
    SurfaceEffectPayload pathblend;
};

// The stack of passes of one zone (top, penultimate or bottom), bottom to top, at most kMaxPasses.
struct SurfacePassStack {
    static constexpr int kMaxPasses = 3;

    bool enabled            = false;
    bool perimeter_override = false; // the walls are reprinted by the passes
    // Bottom zone only: passes above the first one print as solid infill instead of bridge.
    bool bottom_supported_control = false;
    std::vector<SurfacePass> passes;

    bool empty() const { return passes.empty(); }
    bool all_solid() const;
    // The zone does something: some pass is not None. A [None] stack is authored "no effect".
    bool any_effect() const;

    // "" for an empty (unauthored) stack.
    std::string             to_json() const;
    static SurfacePassStack from_json(const std::string &text);

    // A stack built from the legacy keys of a region (multipass_*, interlayer_colormix_*, pathblend_*).
    static SurfacePassStack synthesize_from_legacy(const PrintRegionConfig &cfg, ExtrusionRole role);
    // The stack a region preset asks for: its neotko_surface_passes_* blob, else the legacy keys.
    static SurfacePassStack resolve(const PrintRegionConfig &cfg, ExtrusionRole role);
    static SurfacePassStack resolve_for_zone(const DynamicPrintConfig &cfg, bool penu);

    MultiPassConfig         to_multipass_config(ExtrusionRole role) const;
    static SurfacePassStack from_multipass_config(const MultiPassConfig &mp);
};

// PathBlend discretized in bands along t. Ramp bands climb; in Full mode cap bands sit at the layer top
// with the height left above their ramp band.
struct PBBand {
    bool  is_cap;
    float t_lo;
    float t_hi;
    float t_mid;
    float z_top;  // absolute Z of the nozzle
    float h_step; // bead height
};

// Thinnest bead a PathBlend pass emits (mm): the ramp floor, the ramp top below the layer top and the cap
// are held at or above it. Below about 0.02 mm at top surface speed the bead starves and breaks into
// islands. ORCA_PB_MIN_H overrides it for testing (0 = no ceiling at all).
inline double pb_min_printable_h()
{
    static const double v = [] {
        if (const char *e = std::getenv("ORCA_PB_MIN_H")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 0.5)
                    return d;
            } catch (...) {}
        }
        return 0.035;
    }();
    return v;
}

// Bands of one PathBlend pass. `min_band_h` only sets how finely the ramp is diced; `min_printable_h` is the
// physical floor (0 = reuse min_band_h). `target_k` > 0 forces the band count. Empty = degenerate pass.
std::vector<PBBand> compute_pb_bands(const PathBlendPassConfig &pb, double bottom_z, double H, double min_band_h, bool want_cap,
                                     int target_k = 0, double min_printable_h = 0.0);

// --- Positional field shared by the ColorStitch distributions -------------------------------------------
// The design lives in millimetres on the surface, measured from the object origin, and every fill line asks
// for the colour of the place it lands on. Nothing is counted, so the line width, the line spacing and the
// lines a hole cuts in two cannot move the pattern. RawLineT is any type with `.pl` (Polyline) and `.width`.

struct LaneVec2 { double x = 0.0, y = 0.0; };

inline LaneVec2 lane_centroid(const Polyline &pl)
{
    LaneVec2 c{ 0.0, 0.0 };
    if (pl.points.empty())
        return c;
    for (const Point &pt : pl.points) {
        c.x += double(pt.x());
        c.y += double(pt.y());
    }
    c.x /= double(pl.points.size());
    c.y /= double(pl.points.size());
    return c;
}

inline LaneVec2 lane_direction(const Polyline &pl)
{
    if (pl.points.size() < 2)
        return { 1.0, 0.0 };
    const double dx = double(pl.points.back().x() - pl.points.front().x());
    const double dy = double(pl.points.back().y() - pl.points.front().y());
    const double n  = std::sqrt(dx * dx + dy * dy);
    if (n < 1e-6)
        return { 1.0, 0.0 };
    return { dx / n, dy / n };
}

inline double lane_angle_mod_pi(const Polyline &pl)
{
    const LaneVec2 d = lane_direction(pl);
    double         a = std::atan2(d.y, d.x);
    while (a < 0.0)
        a += M_PI;
    while (a >= M_PI)
        a -= M_PI;
    return a;
}

template<class RawLineT> inline size_t lane_pick_reference(const std::vector<RawLineT> &raw_lines)
{
    size_t best     = 0;
    double best_len = -1.0;
    for (size_t i = 0; i < raw_lines.size(); ++i) {
        const double len = raw_lines[i].pl.length();
        if (len > best_len) {
            best_len = len;
            best     = i;
        }
    }
    return best;
}

// Axis across the lines. An authored angle (>= 0) wins over the measured one, so the engine and the preview
// agree by construction. The measured axis is folded to one half plane, so that two nearly parallel surfaces
// never get opposite axes; `half_plane_when_authored` = false keeps an authored axis as it is.
template<class RawLineT>
inline LaneVec2 lane_perp_axis(const std::vector<RawLineT> &raw_lines, int authored_angle_deg, double *out_ref_ang = nullptr,
                               bool *out_flipped = nullptr, bool half_plane_when_authored = true)
{
    const size_t ref      = lane_pick_reference(raw_lines);
    const bool   authored = authored_angle_deg >= 0;
    const double ref_ang  = authored ? std::fmod(double(authored_angle_deg) * M_PI / 180.0, M_PI) : lane_angle_mod_pi(raw_lines[ref].pl);
    LaneVec2     perp{ -std::sin(ref_ang), std::cos(ref_ang) };
    const bool   apply_half_plane = !authored || half_plane_when_authored;
    const bool   flipped = apply_half_plane && ((perp.y < -1e-12) || (std::abs(perp.y) <= 1e-12 && perp.x < 0.0));
    if (flipped) {
        perp.x = -perp.x;
        perp.y = -perp.y;
    }
    if (out_ref_ang)
        *out_ref_ang = ref_ang;
    if (out_flipped)
        *out_flipped = flipped;
    return perp;
}

// Bands in millimetres (ColorStitch mode 4): each line takes the band its centroid falls in, the band cycle
// repeating every sum(band_mm) from the anchor (object origin in the slice frame). Returns an index into
// band_mm per line. Each line is compared with the exact boundary, so the quantization error never adds up.
template<class RawLineT>
inline std::vector<int> compute_slot_per_line_band_mm(const std::vector<RawLineT> &raw_lines, const std::vector<double> &band_mm,
                                                      int authored_angle_deg, double anchor_x_mm, double anchor_y_mm,
                                                      std::string *debug_summary = nullptr)
{
    const int        n = int(raw_lines.size());
    std::vector<int> slot_per_line(n, 0);
    const int        n_bands = int(band_mm.size());
    if (n <= 0 || n_bands <= 0)
        return slot_per_line;

    double period = 0.0;
    for (double b : band_mm)
        period += std::max(0.0, b);
    if (period <= 1e-6) {
        if (debug_summary)
            *debug_summary = "BandMM period=0 -> all slot 0";
        return slot_per_line;
    }

    double         ref_ang = 0.0;
    bool           flipped = false;
    const LaneVec2 perp    = lane_perp_axis(raw_lines, authored_angle_deg, &ref_ang, &flipped, false);
    const double   anchor_proj_mm = anchor_x_mm * perp.x + anchor_y_mm * perp.y;

    int    hist[4] = { 0, 0, 0, 0 };
    double proj_lo = 1e30, proj_hi = -1e30;
    for (int i = 0; i < n; ++i) {
        const LaneVec2 c       = lane_centroid(raw_lines[i].pl);
        const double   proj_mm = (c.x * perp.x + c.y * perp.y) / 1e6 - anchor_proj_mm;
        proj_lo = std::min(proj_lo, proj_mm);
        proj_hi = std::max(proj_hi, proj_mm);
        double r = std::fmod(proj_mm, period);
        if (r < 0.0)
            r += period;
        int    slot = n_bands - 1;
        double acc  = 0.0;
        for (int k = 0; k < n_bands; ++k) {
            acc += std::max(0.0, band_mm[k]);
            if (r < acc) {
                slot = k;
                break;
            }
        }
        slot_per_line[i] = slot;
        if (slot >= 0 && slot < 4)
            ++hist[slot];
    }

    if (debug_summary) {
        std::ostringstream o;
        o << "axis=" << int(std::round(ref_ang * 180.0 / M_PI)) << "deg src=" << (authored_angle_deg >= 0 ? "cfg" : "geo")
          << " perp=(" << std::fixed << std::setprecision(2) << perp.x << "," << perp.y << ")" << std::defaultfloat
          << " cano=" << (flipped ? 1 : 0) << " BandMM period=" << period << "mm span=" << (proj_hi - proj_lo) << "mm lines_per_band=[";
        for (int k = 0; k < n_bands && k < 4; ++k)
            o << (k ? "," : "") << hist[k];
        o << "]";
        *debug_summary = o.str();
    }
    return slot_per_line;
}

// Lines grouped by lane (a lane is one line spacing wide across the axis): the two pieces of a line cut by a
// hole fall in the same group, so the dither gives them one colour.
struct FieldGroups {
    std::vector<int>    group_of_line; // line -> group in [0, m)
    std::vector<double> t_of_group;    // group -> t in [0, 1], ascending
};

// Position in mm along the axis -> lane -> t. The single owner of that arithmetic for ColorStitch and PathBlend.
struct FieldSampler {
    double    sp     = 1.0;  // real line spacing, mm
    double    period = 0.0;  // > 0: physical period; <= 0: fit to the observed lanes
    double    anchor = 0.0;  // anchor projected on the axis, mm
    bool      invert = false;
    long long k0     = 0;    // lowest observed lane (fit mode)
    long long k1     = 0;    // highest observed lane (fit mode)

    bool periodic() const { return period > 1e-4; }

    long long lane_of(double proj_mm) const
    {
        double pos_mm = proj_mm - anchor;
        if (periodic()) {
            pos_mm = std::fmod(pos_mm, period);
            if (pos_mm < 0.0)
                pos_mm += period;
        }
        return std::llround(pos_mm / std::max(1e-3, sp));
    }

    double t_of_lane(long long k) const
    {
        double t;
        if (periodic())
            t = std::clamp(double(k) / std::max(1.0, period / std::max(1e-3, sp)), 0.0, 1.0);
        else if (k1 <= k0)
            t = 0.5;
        else
            t = double(k - k0) / std::max(1.0, double(k1 - k0));
        return invert ? (1.0 - t) : t;
    }

    double t_of(double proj_mm) const { return t_of_lane(lane_of(proj_mm)); }
};

// Recipe order of the passes of the last ColorStitch lamina: the tool sequence assign_and_group_tools()
// resolved, without repeats. The fill step clears it, calls ColorStitch and sorts the per-tool sublayers by it,
// so the first colour of the recipe is always printed first. Thread local: layers are filled in parallel.
inline std::vector<int> &last_tool_order_tls()
{
    static thread_local std::vector<int> v;
    return v;
}

// Set when ColorStitch had to cut fused Monotonic paths into lines (some lines may lose their colour);
// PrintObject::infill() clears it before filling and warns once. Only one Print slices at a time.
inline std::atomic<bool> &fused_monotonic_seen()
{
    static std::atomic<bool> f{ false };
    return f;
}

// Groups positions already projected on an axis (mm) by lane and gives each group its t.
inline FieldGroups field_groups_from_projections(const std::vector<double> &proj_mm, double period_mm, double spacing_mm,
                                                 double anchor_proj_mm, bool invert, int *out_lanes = nullptr)
{
    FieldGroups out;
    const int   n = int(proj_mm.size());
    out.group_of_line.assign(std::max(0, n), 0);
    if (n <= 0)
        return out;

    FieldSampler smp;
    smp.sp     = std::max(1e-3, spacing_mm);
    smp.period = period_mm;
    smp.anchor = anchor_proj_mm;

    std::vector<long long> key(n, 0);
    for (int i = 0; i < n; ++i)
        key[i] = smp.lane_of(proj_mm[i]);
    // Distinct keys in ascending order, the order the _at dither builders expect.
    std::map<long long, int> group_of_key;
    for (int i = 0; i < n; ++i)
        group_of_key.emplace(key[i], 0);
    int m = 0;
    for (auto &kv : group_of_key)
        kv.second = m++;
    for (int i = 0; i < n; ++i)
        out.group_of_line[i] = group_of_key[key[i]];
    if (out_lanes)
        *out_lanes = m;

    smp.k0 = group_of_key.begin()->first;
    smp.k1 = group_of_key.rbegin()->first;
    out.t_of_group.assign(size_t(m), 0.0);
    if (m == 1) {
        out.t_of_group[0] = 0.5;
    } else {
        int g = 0;
        for (const auto &kv : group_of_key)
            out.t_of_group[g++] = smp.t_of_lane(kv.first);
    }
    if (invert) {
        // Flip the field, not the list: t_of_group stays ascending.
        for (double &t : out.t_of_group)
            t = 1.0 - t;
        std::reverse(out.t_of_group.begin(), out.t_of_group.end());
        for (int i = 0; i < n; ++i)
            out.group_of_line[i] = m - 1 - out.group_of_line[i];
    }
    return out;
}

// Lane groups of the raw lines of a surface, on the axis of lane_perp_axis() (authored angle when there is
// one), anchored at (anchor_x_mm, anchor_y_mm). `spacing_mm` is the real line spacing, not the line width.
template<class RawLineT>
inline FieldGroups compute_field_groups(const std::vector<RawLineT> &raw_lines, int authored_angle_deg, double period_mm,
                                        double spacing_mm, double anchor_x_mm, double anchor_y_mm, bool invert,
                                        std::string *debug_summary = nullptr)
{
    FieldGroups out;
    const int   n = int(raw_lines.size());
    out.group_of_line.assign(std::max(0, n), 0);
    if (n <= 0)
        return out;

    double         ref_ang = 0.0;
    bool           flipped = false;
    const LaneVec2 perp    = lane_perp_axis(raw_lines, authored_angle_deg, &ref_ang, &flipped, false);
    const double   anchor_proj_mm = anchor_x_mm * perp.x + anchor_y_mm * perp.y;
    // Without a usable spacing the width of the first line still groups the split pieces.
    const double sp = (spacing_mm > 1e-4) ? spacing_mm : std::max(1e-3, double(raw_lines[0].width));

    std::vector<double> proj_mm(size_t(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const LaneVec2 c = lane_centroid(raw_lines[i].pl);
        proj_mm[i]       = (c.x * perp.x + c.y * perp.y) / 1e6;
    }
    int m = 0;
    out   = field_groups_from_projections(proj_mm, period_mm, sp, anchor_proj_mm, invert, &m);

    if (debug_summary) {
        std::ostringstream o;
        o << "axis=" << int(std::round(ref_ang * 180.0 / M_PI)) << "deg src=" << (authored_angle_deg >= 0 ? "cfg" : "geo")
          << " perp=(" << std::fixed << std::setprecision(2) << perp.x << "," << perp.y << ")" << std::defaultfloat
          << " cano=" << (flipped ? 1 : 0) << " Field " << (period_mm > 1e-4 ? "periodic" : "fit-surface") << " lines=" << n
          << " lanes=" << m << " spacing=" << sp << "mm";
        if (period_mm > 1e-4)
            o << " period=" << period_mm << "mm";
        o << " inv=" << (invert ? 1 : 0);
        *debug_summary = o.str();
    }
    return out;
}

} // namespace Slic3r
