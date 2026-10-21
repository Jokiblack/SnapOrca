// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0)
#include "ExpertGCodeReprocessor.hpp"

#include "GCodeProcessor.hpp"
#include "../ExtrusionEntity.hpp"
#include "../LocalesUtils.hpp"
#include "../PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

using json = nlohmann::json;

enum class RuleType { Speed, Flow, Fan, ZOffset };
enum class RuleMode { Global, ByTool };

struct Rule
{
    RuleType type{ RuleType::Speed };
    RuleMode mode{ RuleMode::Global };
    int      tool{ -1 };       // 0-based filament index, by_tool rules only
    int      layer_from{ -1 };
    int      layer_to{ -1 };   // -1 == to the end of the print
    double   value{ 0. };
    bool     avoid_wipetower{ false };
};

// Inclusive, 1-based G-code line range.
struct LineWindow
{
    unsigned int start;
    unsigned int end;
    bool valid() const { return start <= end; }
};

struct LayerLineRange
{
    unsigned int first_gcode_id;
    unsigned int last_gcode_id;
};

struct ToolChangeEvent
{
    unsigned int gcode_id; // the line that selects the tool
    int          filament; // 0-based filament selected by that line
};

std::optional<RuleType> parse_rule_type(const std::string &type)
{
    if (type == "speed_multiplier") return RuleType::Speed;
    if (type == "flow_multiplier") return RuleType::Flow;
    if (type == "fan_override") return RuleType::Fan;
    if (type == "z_offset") return RuleType::ZOffset;
    return std::nullopt;
}

// Parses the rules JSON. A malformed blob, "enabled": false or a rule missing a required key yields
// fewer rules, never an error: older builds must tolerate rule files written by newer ones.
std::vector<Rule> parse_rules(const std::string &rules_json)
{
    std::vector<Rule> rules;
    if (rules_json.empty())
        return rules;

    json root;
    try {
        root = json::parse(rules_json);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to parse rules JSON: " << ex.what();
        return rules;
    }
    if (!root.is_object() || !root.value("enabled", true) || !root.contains("rules") || !root["rules"].is_array())
        return rules;

    for (const json &r : root["rules"]) {
        if (!r.is_object() || !r.contains("type") || !r["type"].is_string())
            continue;
        const std::optional<RuleType> type = parse_rule_type(r["type"].get<std::string>());
        if (!type)
            continue;
        if (!r.contains("enabled") || !r["enabled"].is_boolean() || !r["enabled"].get<bool>())
            continue;
        if (!r.contains("layer_from") || !r["layer_from"].is_number_integer())
            continue;
        // Z offsets are fractional; speed, flow and fan stay integers.
        if (!r.contains("value") || !(*type == RuleType::ZOffset ? r["value"].is_number() : r["value"].is_number_integer()))
            continue;

        Rule rule;
        rule.type       = *type;
        rule.mode       = (r.contains("mode") && r["mode"].is_string() && r["mode"].get<std::string>() == "by_tool") ? RuleMode::ByTool :
                                                                                                                       RuleMode::Global;
        rule.tool       = (rule.mode == RuleMode::ByTool && r.contains("tool") && r["tool"].is_number_integer()) ? r["tool"].get<int>() : -1;
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to   = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.avoid_wipetower = r.contains("avoid_wipetower") && r["avoid_wipetower"].is_boolean() && r["avoid_wipetower"].get<bool>();
        switch (rule.type) {
        case RuleType::Speed: rule.value = std::clamp(r["value"].get<int>(), 1, 300); break;
        case RuleType::Flow: rule.value = std::clamp(r["value"].get<int>(), 20, 200); break;
        case RuleType::Fan: rule.value = std::clamp(r["value"].get<int>(), 0, 255); break;
        case RuleType::ZOffset: rule.value = std::clamp(r["value"].get<double>(), -0.3, 0.3); break;
        }
        rules.push_back(rule);
    }
    return rules;
}

// Groups the Extrude moves by contiguous Z, like the preview does, keyed on the G-code line number.
std::vector<LayerLineRange> build_layer_line_ranges(const GCodeProcessorResult &gcode_result)
{
    std::vector<LayerLineRange> layers;
    constexpr float EPSILON = 1e-4f;
    float           last_z  = std::numeric_limits<float>::quiet_NaN();
    for (const GCodeProcessorResult::MoveVertex &move : gcode_result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        const float z = move.position.z();
        if (layers.empty() || std::isnan(last_z) || std::abs(z - last_z) > EPSILON) {
            layers.push_back({ move.gcode_id, move.gcode_id });
            last_z = z;
        } else
            layers.back().last_gcode_id = std::max(layers.back().last_gcode_id, move.gcode_id);
    }
    return layers;
}

std::string trim_right(const std::string &line)
{
    const size_t end = line.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? std::string() : line.substr(0, end + 1);
}

// The filament a tool-change line selects: T<n> or M1020 S<n>, as GCodeWriter::toolchange() emits it.
std::optional<int> filament_from_line(const std::string &line)
{
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos)
        return std::nullopt;
    if (line.compare(pos, 7, "M1020 S") == 0)
        pos += 7;
    else if (line[pos] == 'T')
        pos += 1;
    else
        return std::nullopt;
    if (pos >= line.size() || !std::isdigit(static_cast<unsigned char>(line[pos])))
        return std::nullopt;
    int n = 0;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos])) && n < 100000)
        n = n * 10 + (line[pos++] - '0');
    return n;
}

// Rules count filaments, while GCodeProcessor records the physical extruder of each move. When the
// G-code line itself does not name the filament, fall back to the filament -> extruder map, which is
// only conclusive when a single filament is mapped to that extruder.
int filament_from_extruder(unsigned char extruder_id, const std::vector<int> &filament_map)
{
    if (filament_map.empty())
        return extruder_id;
    int found = -1;
    for (size_t i = 0; i < filament_map.size(); ++i)
        if (filament_map[i] - 1 == int(extruder_id)) {
            if (found != -1)
                return -1;
            found = int(i);
        }
    return found;
}

std::vector<ToolChangeEvent> build_tool_change_events(const GCodeProcessorResult &gcode_result, const std::vector<std::string> &lines,
                                                      const std::vector<int> &filament_map)
{
    std::vector<ToolChangeEvent> events;
    for (const GCodeProcessorResult::MoveVertex &move : gcode_result.moves) {
        if (move.type != EMoveType::Tool_change || move.gcode_id == 0)
            continue;
        std::optional<int> filament;
        if (move.gcode_id <= lines.size())
            filament = filament_from_line(lines[move.gcode_id - 1]);
        events.push_back({ move.gcode_id, filament ? *filament : filament_from_extruder(move.extruder_id, filament_map) });
    }
    return events;
}

// Every stretch of the file where `target` is the active filament: from the line after the T<n>
// that selects it to the line before the T<n> that leaves it, so restoring at window.end + 1 lands
// on the next tool change and never inside it.
std::vector<LineWindow> build_tool_windows(const std::vector<ToolChangeEvent> &tool_changes, int initial_filament, int target,
                                           unsigned int eof_line)
{
    std::vector<LineWindow> windows;
    unsigned int            window_start = 1;
    bool                    open         = initial_filament == target;
    for (const ToolChangeEvent &ev : tool_changes) {
        if (open && ev.filament == target)
            continue; // re-selecting the active filament
        if (open) {
            const LineWindow w{ window_start, ev.gcode_id - 1 };
            if (w.valid())
                windows.push_back(w);
            open = false;
        }
        if (ev.filament == target) {
            window_start = ev.gcode_id + 1;
            open         = true;
        }
    }
    if (open) {
        const LineWindow w{ window_start, eof_line };
        if (w.valid())
            windows.push_back(w);
    }
    return windows;
}

// Every run of lines whose moves are tagged as wipe tower. The role is sticky, so the travels around
// the tower carry it too.
std::vector<LineWindow> build_wipetower_windows(const GCodeProcessorResult &gcode_result)
{
    std::vector<LineWindow> windows;
    bool                    in_wipetower = false;
    for (const GCodeProcessorResult::MoveVertex &move : gcode_result.moves) {
        const bool is_wt = move.extrusion_role == erWipeTower;
        if (is_wt && !in_wipetower)
            windows.push_back({ move.gcode_id, move.gcode_id });
        else if (is_wt)
            windows.back().end = std::max(windows.back().end, move.gcode_id);
        in_wipetower = is_wt;
    }
    return windows;
}

// Removes every range of `subtract` from `windows`, splitting a window around a range inside it.
std::vector<LineWindow> subtract_windows(std::vector<LineWindow> windows, const std::vector<LineWindow> &subtract)
{
    for (const LineWindow &sub : subtract) {
        std::vector<LineWindow> next;
        for (const LineWindow &w : windows) {
            if (sub.end < w.start || sub.start > w.end) {
                next.push_back(w);
                continue;
            }
            if (sub.start > w.start)
                next.push_back({ w.start, sub.start - 1 });
            if (sub.end < w.end)
                next.push_back({ sub.end + 1, w.end });
        }
        windows = std::move(next);
    }
    return windows;
}

struct Context
{
    std::vector<LayerLineRange>  layers;
    std::vector<ToolChangeEvent> tool_changes;
    std::vector<LineWindow>      wipetower;
    int                          initial_filament{ 0 };
};

// The lines a rule acts on. Rules that insert a restore command (speed, flow, Z offset) run until the
// first extrusion of the next layer, so the override rides through the layer change; the fan rewrite
// stops at the last extrusion of layer_to. Both stop at the last extrusion of the print, so the end
// G-code is never touched.
std::vector<LineWindow> active_windows(const Rule &rule, const Context &ctx)
{
    const std::vector<LayerLineRange> &layers = ctx.layers;
    if (rule.layer_from < 0 || size_t(rule.layer_from) >= layers.size())
        return {};
    const bool         for_insertion = rule.type != RuleType::Fan;
    const unsigned int range_start   = layers[rule.layer_from].first_gcode_id;
    unsigned int       range_end     = layers.back().last_gcode_id;
    if (rule.layer_to >= 0 && size_t(rule.layer_to) < layers.size()) {
        const size_t next = size_t(rule.layer_to) + 1;
        if (!for_insertion)
            range_end = layers[rule.layer_to].last_gcode_id;
        else if (next < layers.size())
            range_end = layers[next].first_gcode_id - 1;
    }
    if (range_start > range_end)
        return {};

    std::vector<LineWindow> windows;
    if (rule.mode == RuleMode::Global)
        windows.push_back({ range_start, range_end });
    else if (rule.tool >= 0) {
        for (const LineWindow &tw : build_tool_windows(ctx.tool_changes, ctx.initial_filament, rule.tool, layers.back().last_gcode_id)) {
            const LineWindow clipped{ std::max(tw.start, range_start), std::min(tw.end, range_end) };
            if (clipped.valid())
                windows.push_back(clipped);
        }
    }
    return rule.avoid_wipetower ? subtract_windows(std::move(windows), ctx.wipetower) : windows;
}

// Rewrites a plain fan line to M106 S<value>. Indented or foreign lines are left alone.
bool try_override_fan_line(const std::string &line, int value, std::string &out)
{
    const std::string trimmed = trim_right(line);
    if (trimmed == "M107") {
        out = "M106 S" + std::to_string(value) + " ; NEOTKO_GCODE_REPROCESSOR fan_override (was M107)";
        return true;
    }
    static const std::string prefix = "M106 S";
    if (trimmed.compare(0, prefix.size(), prefix) != 0)
        return false;
    size_t digits_end = prefix.size();
    while (digits_end < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits_end])))
        ++digits_end;
    if (digits_end == prefix.size())
        return false;
    out = "M106 S" + std::to_string(value) + " ; NEOTKO_GCODE_REPROCESSOR fan_override" + trimmed.substr(digits_end);
    return true;
}

struct Insertion
{
    unsigned int before_line; // 1-based line the text goes in front of
    int          order;       // restores (0) go before starts (1) at the same line
    std::string  text;
};

const char *command_of(RuleType type)
{
    switch (type) {
    case RuleType::Speed: return "M220 S";
    case RuleType::Flow: return "M221 S";
    case RuleType::ZOffset: return "SET_GCODE_OFFSET Z=";
    default: return "";
    }
}

const char *name_of(RuleType type)
{
    switch (type) {
    case RuleType::Speed: return "speed_multiplier";
    case RuleType::Flow: return "flow_multiplier";
    case RuleType::ZOffset: return "z_offset";
    default: return "fan_override";
    }
}

std::string value_text(const Rule &rule)
{
    return rule.type == RuleType::ZOffset ? float_to_string_decimal_point(rule.value, 4) : std::to_string(int(std::lround(rule.value)));
}

// The wipe tower resets the speed factor at every tool change: "M220 S100", wrapped on Marlin and on
// the Snapmaker U1 in "M220 B" / "M220 R" (back up / restore). Inside a speed window the reset is
// rewritten to the rule's value. A restore whose block overlaps a window is rewritten to the value
// the window wants at that line, because the backed-up value would otherwise undo the window start
// or the window end that fell inside the block.
void handle_wipe_tower_speed_resets(std::vector<std::string> &lines, const std::vector<std::pair<LineWindow, int>> &speed_windows)
{
    if (speed_windows.empty())
        return;
    auto speed_at = [&speed_windows](unsigned int line) {
        int speed = 100;
        for (const auto &[w, value] : speed_windows)
            if (w.start <= line && line <= w.end)
                speed = value;
        return speed;
    };
    unsigned int backup_line = 0;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const unsigned int line    = (unsigned int) (idx + 1);
        const std::string  trimmed = trim_right(lines[idx]);
        if (trimmed == "M220 B")
            backup_line = line;
        else if (trimmed == "M220 S100") {
            const int speed = speed_at(line);
            if (speed != 100)
                lines[idx] = "M220 S" + std::to_string(speed) +
                             " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier re-apply after toolchange reset (was M220 S100)";
        } else if (trimmed == "M220 R") {
            const unsigned int block_start = backup_line != 0 ? backup_line : line;
            backup_line                    = 0;
            const bool overlaps = std::any_of(speed_windows.begin(), speed_windows.end(), [block_start, line](const auto &sw) {
                return sw.first.start <= line && block_start <= sw.first.end;
            });
            if (overlaps)
                lines[idx] = "M220 S" + std::to_string(speed_at(line)) + " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier (was M220 R)";
        }
    }
}

} // namespace

bool expert_gcode_reprocessor_has_rules(const std::string &rules_json) { return !parse_rules(rules_json).empty(); }

bool expert_gcode_reprocess_lines(std::vector<std::string> &lines, const std::string &rules_json,
                                  const GCodeProcessorResult &gcode_result, const std::vector<int> &filament_map)
{
    const std::vector<Rule> rules = parse_rules(rules_json);
    if (rules.empty())
        return false;

    Context ctx;
    ctx.layers = build_layer_line_ranges(gcode_result);
    if (ctx.layers.empty())
        return false;
    ctx.tool_changes = build_tool_change_events(gcode_result, lines, filament_map);
    ctx.wipetower    = build_wipetower_windows(gcode_result);
    if (!gcode_result.moves.empty()) {
        const int f          = filament_from_extruder(gcode_result.moves.front().extruder_id, filament_map);
        ctx.initial_filament = f < 0 ? 0 : f;
    }

    bool                                    fan_modified = false;
    std::vector<Insertion>                  insertions;
    std::vector<std::pair<LineWindow, int>> speed_windows;
    for (const Rule &rule : rules) {
        const std::vector<LineWindow> windows = active_windows(rule, ctx);
        if (rule.type == RuleType::Fan) {
            // In-place rewrite only: past the window Orca's own fan commands take over again.
            for (const LineWindow &w : windows)
                for (unsigned int line = w.start; line <= w.end && line <= lines.size(); ++line) {
                    std::string new_line;
                    if (try_override_fan_line(lines[line - 1], int(rule.value), new_line)) {
                        lines[line - 1] = std::move(new_line);
                        fan_modified    = true;
                    }
                }
            continue;
        }
        const std::string start_text = std::string(command_of(rule.type)) + value_text(rule) + " ; NEOTKO_GCODE_REPROCESSOR " +
                                       name_of(rule.type) + " start";
        const std::string restore_text = std::string(command_of(rule.type)) + (rule.type == RuleType::ZOffset ? "0" : "100") +
                                         " ; NEOTKO_GCODE_REPROCESSOR " + name_of(rule.type) + " restore";
        for (const LineWindow &w : windows) {
            insertions.push_back({ w.start, 1, start_text });
            insertions.push_back({ w.end + 1, 0, restore_text });
            if (rule.type == RuleType::Speed)
                speed_windows.emplace_back(w, int(rule.value));
        }
    }
    handle_wipe_tower_speed_resets(lines, speed_windows);

    if (insertions.empty())
        return fan_modified;

    std::stable_sort(insertions.begin(), insertions.end(), [](const Insertion &a, const Insertion &b) {
        return a.before_line != b.before_line ? a.before_line < b.before_line : a.order < b.order;
    });
    std::vector<std::string> out;
    out.reserve(lines.size() + insertions.size());
    size_t next = 0;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        for (; next < insertions.size() && insertions[next].before_line <= idx + 1; ++next)
            out.push_back(insertions[next].text);
        out.push_back(std::move(lines[idx]));
    }
    for (; next < insertions.size(); ++next)
        out.push_back(insertions[next].text);
    lines = std::move(out);
    return true;
}

bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config, const GCodeProcessorResult &gcode_result)
{
    const ConfigOptionString *rules_opt = config.option<ConfigOptionString>("expert_gcode_reprocessor_rules");
    if (rules_opt == nullptr || !expert_gcode_reprocessor_has_rules(rules_opt->value))
        return false;

    std::vector<std::string> lines;
    bool                     ends_with_newline = false;
    {
        boost::nowide::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to open " << path << " for reading";
            return false;
        }
        std::string line;
        while (std::getline(in, line))
            lines.push_back(std::move(line));
        in.clear();
        in.seekg(-1, std::ios::end);
        char last = 0;
        ends_with_newline = in.get(last) && last == '\n';
    }

    std::vector<int> filament_map;
    if (const ConfigOptionInts *map_opt = config.option<ConfigOptionInts>("filament_map"))
        filament_map = map_opt->values;
    if (!expert_gcode_reprocess_lines(lines, rules_opt->value, gcode_result, filament_map))
        return false;

    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to open " << path << " for writing";
        return false;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size() || ends_with_newline)
            out << '\n';
    }
    return out.good();
}

} // namespace Slic3r
