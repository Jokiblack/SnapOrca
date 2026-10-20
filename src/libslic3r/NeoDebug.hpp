// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

// Debug trace channels of the features ported from NeotkoCM. Each channel writes to its own file,
// <temp>/neotko_logs/<channel>.log, and starts on when its environment variable is set:
//   ORCA_DEBUG_COLORSTITCH  - Surface ColorStitch tool assignment and grouping
//   ORCA_DEBUG_MULTIPASS    - MultiPass fill generation
//   ORCA_DEBUG_PENULTIMATE  - penultimate surface classification
//   ORCA_DEBUG_TOOLORDER    - tool ordering of the ColorStitch / MultiPass sublayers
//   ORCA_DEBUG_ZBLEND       - sublayer heights
//   ORCA_DEBUG_WIPETOWER    - wipe tower primes of the sublayers
//   ORCA_DEBUG_PROFILE      - Surface Effect Profiles and the ColorStitch painter
//   ORCA_DEBUG_BOTTOM       - bottom surface sandwich and bridge expansion
//   ORCA_DEBUG_CONTACT      - contact between instances
//   ORCA_DEBUG_XOBJ         - PerObject Support (cross-object avoidance)
//   ORCA_DEBUG_INFILL       - surface fills before and during the fill loop
//   ORCA_DEBUG_SUPPORTZONES - Support Zones pillar geometry
//   ORCA_DEBUG_ALL          - every channel at once
// Nothing is written, and no file or folder is created, unless a channel is on.

#include <sstream>
#include <string>

namespace Slic3r {

namespace NeoDebug {
    enum Channel : int {
        COLORSTITCH,
        MULTIPASS,
        PENULTIMATE,
        TOOLORDER,
        ZBLEND,
        WIPETOWER,
        PROFILE,
        BOTTOM,
        CONTACT,
        XOBJ,
        INFILL,
        SUPPORTZONES,
        CH_COUNT
    };

    // On-screen render tuning panels: ORCA_DEBUG_RENDER only, so that ORCA_DEBUG_ALL opens logs and no windows.
    bool render_panels_enabled();
    // The channel switchboard window: ORCA_DEBUG_ALL or ORCA_DEBUG_RENDER.
    bool console_enabled();

    // True if the channel writes right now: its switch is on and logging is not paused. Thread-safe and cheap.
    bool enabled(Channel c);
    // Channel switch, independent of the pause state. The environment only sets its starting position.
    bool is_enabled(Channel c);
    void set_enabled(Channel c, bool on);
    // While paused every channel is silent and the switches keep their positions.
    bool paused();
    void set_paused(bool on);

    const char* channel_name(Channel c);  // "COLORSTITCH"
    std::string log_dir();                // <system temp folder>/neotko_logs
    std::string log_path(Channel c);      // <log_dir()>/colorstitch.log
    long long   log_size(Channel c);      // bytes, or -1 if the file does not exist
    void        clear(Channel c);         // removes the channel's log
    void        clear_all();

    // Appends msg and a newline to the channel's log. Thread-safe. Callers check enabled() first.
    void write(Channel c, const std::string& msg);
    // Writes a separator with a slice counter and the time to every enabled channel, to tell slices apart.
    void write_session_banner(const std::string& tag);
} // namespace NeoDebug

} // namespace Slic3r

// NEOTKO_LOG(CHANNEL, stream_expr) writes one line to a channel when it is on, e.g.
//   NEOTKO_LOG(COLORSTITCH, "layer=" << layer_idx << " fills=" << n);
#define NEOTKO_LOG(channel, body)                                         \
    do {                                                                  \
        if (::Slic3r::NeoDebug::enabled(::Slic3r::NeoDebug::channel)) {   \
            std::ostringstream neotko_log_stream_;                        \
            neotko_log_stream_ << body;                                   \
            ::Slic3r::NeoDebug::write(::Slic3r::NeoDebug::channel, neotko_log_stream_.str()); \
        }                                                                 \
    } while (0)
