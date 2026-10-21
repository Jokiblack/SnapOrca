// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include "PathBlendRuntime.hpp"

namespace Slic3r {

PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::mut()
{
    static PathBlendSchedulerRuntime g;
    return g;
}
const PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::get() { return mut(); }

PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::mut()
{
    static PathBlendDispatcherRuntime g;
    return g;
}
const PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::get() { return mut(); }

} // namespace Slic3r
