#pragma once

#include "reference_cache.h"

namespace reconstruction::reference {

enum class ConsistencyStatus { stable, no_mesh, no_pair, initial_read_failed, unstable };
struct ConsistencyResult {
    PoseSelection selection;
    ConsistencyStatus status = ConsistencyStatus::no_mesh;
    unsigned attempts = 0, clears = 0; // collect calls and clear_entity calls, respectively.
};

// 7468, using owned Reader bytes. A false Reader call may partially overwrite
// its destination. No OS read/exception or protection-cache behavior is modeled.
// collect owns the 76e4 stage (including its mesh/base reads and cache_action).
// Wrapper snapshots keep the initially selected mesh/base across both attempts.
ConsistencyResult read_consistent_pose(const Reader&, const RangeCheck&, std::uint64_t pawn,
                                      PoseCache&, const std::function<PoseSelection()>& collect);

} // namespace reconstruction::reference
