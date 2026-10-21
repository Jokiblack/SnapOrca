// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0)
#pragma once

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
struct GCodeProcessorResult;

// NEOTKO_GCODE_REPROCESSOR
// Rewrites a finished G-code file according to the layer-ranged rule list stored in the project
// key "expert_gcode_reprocessor_rules" (JSON):
//
//   { "enabled": true,
//     "rules": [ { "type": "speed_multiplier", "enabled": true, "mode": "global" | "by_tool",
//                  "tool": 0, "layer_from": 0, "layer_to": -1, "value": 80,
//                  "avoid_wipetower": false }, ... ] }
//
// Rule types:
// - "speed_multiplier": M220 S<1-300> at the start of every active window, M220 S100 at its end.
// - "flow_multiplier":  M221 S<20-200>, same mechanics, restored with M221 S100.
// - "fan_override":     every plain M106 S<n> / M107 inside the window is rewritten to M106 S<0-255>.
// - "z_offset":         SET_GCODE_OFFSET Z=<-0.3..0.3> at the start, SET_GCODE_OFFSET Z=0 at the end.
//
// Layers are 0-based, in Z order, and come from the Extrude moves of the GCodeProcessorResult that
// was computed for this exact file, so the reprocessor must run before anything else changes the
// line count. layer_to = -1 means "to the end of the print". A "by_tool" rule only acts while the
// given filament (0-based: 0 = filament 1, the n of the T<n> command) is the active one; its windows
// open right after the T<n> that selects it and close right before the T<n> that leaves it.
// "avoid_wipetower" cuts every wipe tower block out of the rule's windows.
//
// Unknown rule types and keys are skipped. With an empty string, "enabled": false or no valid rule
// nothing is done and the file is left untouched.

// True when the rules JSON is enabled and holds at least one valid, enabled rule.
bool expert_gcode_reprocessor_has_rules(const std::string &rules_json);

// Applies the rules to `lines` (the G-code file split into lines, without line terminators).
// `filament_map` is the 1-based "filament_map" option (filament -> physical extruder); it is only used
// to name the filament of a tool change whose G-code line is not a plain T<n>. Returns true when
// `lines` was changed.
bool expert_gcode_reprocess_lines(std::vector<std::string> &lines, const std::string &rules_json,
                                  const GCodeProcessorResult &gcode_result, const std::vector<int> &filament_map);

// Reads `path`, applies the rules of `config` and writes the file back in place. Returns true when the
// file was rewritten; on false the file is untouched.
bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config,
                                  const GCodeProcessorResult &gcode_result);

} // namespace Slic3r
