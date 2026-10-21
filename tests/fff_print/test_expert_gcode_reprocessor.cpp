// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0)
// Expert G-code Reprocessor: layer-ranged and per-filament rewriting of a finished G-code file.
#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ExpertGCodeReprocessor.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Three layers (Z 0.2, 0.4, 0.6) printed with filaments 0 and 1. Line numbers are 1-based.
const std::vector<std::string> SAMPLE_GCODE = {
    "; start",      // 1
    "T0",           // 2
    "G1 Z0.2",      // 3
    "G1 X1 E1",     // 4  layer 0
    "G1 X2 E1",     // 5  layer 0
    "M106 S100",    // 6  layer 0
    "G1 Z0.4",      // 7
    "G1 X1 E1",     // 8  layer 1
    "T1",           // 9
    "G1 X2 E1",     // 10 layer 1, filament 1
    "M107",         // 11 layer 1
    "G1 Z0.6",      // 12
    "G1 X1 E1",     // 13 layer 2, filament 1
    "T0",           // 14
    "G1 X2 E1",     // 15 layer 2
    "; end",        // 16
    "M107",         // 17
};

GCodeProcessorResult::MoveVertex move(unsigned int line, EMoveType type, float z, unsigned char extruder, ExtrusionRole role = erPerimeter)
{
    GCodeProcessorResult::MoveVertex v;
    v.gcode_id       = line;
    v.type           = type;
    v.position       = Vec3f(0.f, 0.f, z);
    v.extruder_id    = extruder;
    v.extrusion_role = role;
    return v;
}

// GCodeProcessorResult cannot be copied, so the tests share one immutable instance.
const GCodeProcessorResult &sample_result()
{
    static GCodeProcessorResult result;
    if (result.moves.empty())
        result.moves = {
        move(2, EMoveType::Tool_change, 0.f, 0),  move(4, EMoveType::Extrude, 0.2f, 0),     move(5, EMoveType::Extrude, 0.2f, 0),
        move(8, EMoveType::Extrude, 0.4f, 0),     move(9, EMoveType::Tool_change, 0.4f, 1), move(10, EMoveType::Extrude, 0.4f, 1),
        move(13, EMoveType::Extrude, 0.6f, 1),    move(14, EMoveType::Tool_change, 0.6f, 0), move(15, EMoveType::Extrude, 0.6f, 0),
        };
    return result;
}

std::string rules(const std::string &rule_list) { return "{\"enabled\": true, \"rules\": [" + rule_list + "]}"; }

// Index of the first line starting with `prefix`, or -1.
long find_line(const std::vector<std::string> &lines, const std::string &prefix)
{
    auto it = std::find_if(lines.begin(), lines.end(), [&prefix](const std::string &l) { return l.rfind(prefix, 0) == 0; });
    return it == lines.end() ? -1 : long(std::distance(lines.begin(), it));
}

long find_exact(const std::vector<std::string> &lines, const std::string &line)
{
    auto it = std::find(lines.begin(), lines.end(), line);
    return it == lines.end() ? -1 : long(std::distance(lines.begin(), it));
}

} // namespace

TEST_CASE("Empty, disabled or unknown reprocessor rules leave the G-code unchanged", "[ExpertGCodeReprocessor]")
{
    const std::string json = GENERATE(std::string(), std::string("not json"),
                                      std::string("{\"enabled\": false, \"rules\": [{\"type\": \"speed_multiplier\", \"enabled\": true, "
                                                  "\"layer_from\": 0, \"value\": 50}]}"),
                                      rules("{\"type\": \"future_rule\", \"enabled\": true, \"layer_from\": 0, \"value\": 50}"),
                                      rules("{\"type\": \"speed_multiplier\", \"enabled\": false, \"layer_from\": 0, \"value\": 50}"));
    CAPTURE(json);
    std::vector<std::string> lines = SAMPLE_GCODE;
    CHECK_FALSE(expert_gcode_reprocessor_has_rules(json));
    CHECK_FALSE(expert_gcode_reprocess_lines(lines, json, sample_result(), {}));
    CHECK(lines == SAMPLE_GCODE);
}

TEST_CASE("A project without reprocessor rules keeps the G-code file byte for byte", "[ExpertGCodeReprocessor]")
{
    // No trailing newline and a CR on one line, to show the file is not even rewritten.
    const std::string   content = "; start\nT0\r\nG1 Z0.2\nG1 X1 E1\nM107";
    ScopedTemporaryFile file(".gcode");
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary);
        out << content;
    }
    DynamicPrintConfig config;
    config.set_key_value("expert_gcode_reprocessor_rules", new ConfigOptionString(""));
    CHECK_FALSE(run_expert_gcode_reprocessor(file.string(), config, sample_result()));

    boost::nowide::ifstream in(file.string(), std::ios::binary);
    std::stringstream       ss;
    ss << in.rdbuf();
    CHECK(ss.str() == content);
}

TEST_CASE("A global speed rule covers its layer range up to the next layer's first extrusion", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"speed_multiplier\", \"enabled\": true, \"layer_from\": 1, "
                                                      "\"layer_to\": 1, \"value\": 80}"),
                                         sample_result(), {}));
    REQUIRE(lines.size() == SAMPLE_GCODE.size() + 2);
    const long start   = find_line(lines, "M220 S80");
    const long restore = find_line(lines, "M220 S100");
    REQUIRE(start >= 0);
    REQUIRE(restore >= 0);
    // Start right before the first extrusion of layer 1, restore right before the first one of layer 2.
    CHECK(lines[start - 1] == "G1 Z0.4");
    CHECK(lines[start + 1] == "G1 X1 E1");
    CHECK(lines[restore - 1] == "G1 Z0.6");
    CHECK(lines[restore + 1] == "G1 X1 E1");
}

TEST_CASE("A by-tool flow rule follows the tool changes of its filament", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"flow_multiplier\", \"enabled\": true, \"mode\": \"by_tool\", "
                                                      "\"tool\": 1, \"layer_from\": 0, \"value\": 90}"),
                                         sample_result(), {}));
    const long start   = find_line(lines, "M221 S90");
    const long restore = find_line(lines, "M221 S100");
    REQUIRE(start >= 0);
    REQUIRE(restore >= 0);
    // Applied right after T1, restored right before the T0 that leaves filament 1.
    CHECK(lines[start - 1] == "T1");
    CHECK(lines[restore + 1] == "T0");
    CHECK(restore > start);
}

TEST_CASE("A by-tool rule names the filament from the filament map when the line is not a T command", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    lines[8]                       = "; custom tool change"; // line 9 selected filament 1
    GCodeProcessorResult result;
    result.moves                   = sample_result().moves;
    result.moves[4].extruder_id    = 2;                      // filament 1 runs on extruder 3
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"flow_multiplier\", \"enabled\": true, \"mode\": \"by_tool\", "
                                                      "\"tool\": 1, \"layer_from\": 0, \"value\": 90}"),
                                         result, { 1, 3, 2 }));
    const long start = find_line(lines, "M221 S90");
    REQUIRE(start >= 0);
    CHECK(lines[start - 1] == "; custom tool change");
}

TEST_CASE("A fan override rewrites only the fan commands inside its layers", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"fan_override\", \"enabled\": true, \"layer_from\": 1, "
                                                      "\"layer_to\": -1, \"value\": 255}"),
                                         sample_result(), {}));
    REQUIRE(lines.size() == SAMPLE_GCODE.size());
    CHECK(lines[5] == "M106 S100");          // layer 0
    CHECK(lines[10].rfind("M106 S255", 0) == 0); // the M107 of layer 1
    CHECK(lines[16] == "M107");              // end G-code
}

TEST_CASE("A Z offset rule is clamped to 0.3 mm and reset to zero", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"z_offset\", \"enabled\": true, \"layer_from\": 0, "
                                                      "\"layer_to\": 0, \"value\": 1.0}"),
                                         sample_result(), {}));
    CHECK(find_line(lines, "SET_GCODE_OFFSET Z=0.3") >= 0);
    CHECK(find_line(lines, "SET_GCODE_OFFSET Z=0 ") > find_line(lines, "SET_GCODE_OFFSET Z=0.3"));
}

TEST_CASE("Adjacent speed rules restore before the next one starts", "[ExpertGCodeReprocessor]")
{
    std::vector<std::string> lines = SAMPLE_GCODE;
    REQUIRE(expert_gcode_reprocess_lines(lines,
                                         rules("{\"type\": \"speed_multiplier\", \"enabled\": true, \"layer_from\": 0, \"layer_to\": 0, "
                                               "\"value\": 50}, {\"type\": \"speed_multiplier\", \"enabled\": true, \"layer_from\": 1, "
                                               "\"layer_to\": 1, \"value\": 70}"),
                                         sample_result(), {}));
    const long restore = find_line(lines, "M220 S100");
    const long second  = find_line(lines, "M220 S70");
    REQUIRE(restore >= 0);
    REQUIRE(second >= 0);
    CHECK(restore < second);
}

TEST_CASE("A speed rule survives the wipe tower speed reset and restore", "[ExpertGCodeReprocessor]")
{
    // Layer 1 starts with a wipe tower block wrapped in M220 B / M220 R, as on the Snapmaker U1.
    std::vector<std::string> lines = {
        "G1 Z0.2",   // 1
        "G1 X1 E1",  // 2 layer 0
        "G1 Z0.4",   // 3
        "M220 B",    // 4
        "M220 S100", // 5
        "G1 X5 E1",  // 6 layer 1, wipe tower
        "M220 R",    // 7
        "G1 X1 E1",  // 8 layer 1
        "G1 Z0.6",   // 9
        "M220 B",    // 10
        "M220 S100", // 11
        "G1 X5 E1",  // 12 layer 2, wipe tower
        "M220 R",    // 13
        "G1 X1 E1",  // 14 layer 2
    };
    GCodeProcessorResult result;
    result.moves = { move(2, EMoveType::Extrude, 0.2f, 0), move(6, EMoveType::Extrude, 0.4f, 0, erWipeTower),
                     move(8, EMoveType::Extrude, 0.4f, 0), move(12, EMoveType::Extrude, 0.6f, 0, erWipeTower),
                     move(14, EMoveType::Extrude, 0.6f, 0) };
    REQUIRE(expert_gcode_reprocess_lines(lines, rules("{\"type\": \"speed_multiplier\", \"enabled\": true, \"layer_from\": 1, "
                                                      "\"layer_to\": 1, \"value\": 80}"),
                                         result, {}));
    // The window starts inside the first block: its restore would bring back 100, so it applies 80.
    CHECK(find_line(lines, "M220 S80 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier (was M220 R)") >= 0);
    CHECK(find_exact(lines, "M220 R") == -1);
    // The window ends inside the second block: its restore would bring back 80, so it applies 100.
    CHECK(find_line(lines, "M220 S100 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier (was M220 R)") >= 0);
    // The M220 S100 reset inside the window is rewritten, the one outside is not.
    CHECK(find_line(lines, "M220 S80 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier re-apply") >= 0);
    CHECK(find_exact(lines, "M220 S100") >= 0);
}
