#pragma once
#include <string>

namespace gs {

// Promote the calling thread to real-time scheduling (SCHED_FIFO).
// Tries the direct call first (needs an rtprio limit, see scripts/setup_realtime.sh), then rtkit (D-Bus, no root).
// Returns the real-time priority obtained (>0), or 0 on failure (the nice level is raised instead);
// msg receives a human-readable explanation.
int makeThreadRealtime(int wantPriority, std::string& msg);

}  // namespace gs
