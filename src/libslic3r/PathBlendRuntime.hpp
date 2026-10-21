// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#pragma once

#include <cstdlib>
#include <string>

namespace Slic3r {

// Toggles of the PathBlend sublayer scheduling and emission (read by the G-code side). They are
// tuning knobs of the emitter, not project data, so they stay process wide.

// Scheduler: when chain_atomic is true, every consecutive same-tool sublayer of one PathBlend chain is
// printed before moving to the next chain, so the scanlines of different objects do not interleave.
struct PathBlendSchedulerRuntime {
    bool chain_atomic = true;

    static PathBlendSchedulerRuntime&       mut();
    static const PathBlendSchedulerRuntime& get();
};

// XY distance (mm) below which two same-tool PathBlend sublayers are printed as one continuous chain,
// without retract, wipe and lift in between. ORCA_PB_CHAIN_XY overrides it for testing.
inline double pb_chain_max_xy_default()
{
    static const double v = [] {
        if (const char *e = std::getenv("ORCA_PB_CHAIN_XY")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 100.0)
                    return d;
            } catch (...) {}
        }
        return 2.0;
    }();
    return v;
}

// Dispatcher: chain_continuous enables the continuous chain described above.
struct PathBlendDispatcherRuntime {
    bool   chain_continuous = true;
    double chain_max_xy_mm  = pb_chain_max_xy_default();

    static PathBlendDispatcherRuntime&       mut();
    static const PathBlendDispatcherRuntime& get();
};

} // namespace Slic3r
