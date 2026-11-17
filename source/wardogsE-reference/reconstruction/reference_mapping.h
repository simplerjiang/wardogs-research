#pragma once

#include "reference_pose.h"

namespace reconstruction::reference {

// Materialized name ID pair and parent index. The reference wire stride is 12,
// independent of this C++ structure's padding; no text-name resolver is involved.
struct NameRecord { std::uint64_t token = 0; std::int32_t parent = -1; };
using MappingSignature = std::array<std::uint64_t, 21>;
struct MappingParse { bool accepted = false; BoneMapping mapping; };
// Failed results are empty; 9124 itself can leave a partial output array.
MappingParse parse_mapping(const std::vector<NameRecord>&, const MappingSignature&); // 9124

struct MappingResult {
    bool accepted = false;
    bool cache_hit = false;
    std::uint64_t asset = 0, data = 0;
    int count = 0, slot = -1;
    BoneMapping mapping;
};
struct MappingStats {
    std::uint32_t tick = 0, hits = 0, misses = 0;
    std::size_t entries = 0;
};

// All addresses refer to an owned Reader fixture. As for reader_paths, callers
// supply representable address additions. No process/driver or OS allocation API;
// the reference's lazy HeapAlloc failure and returned-pointer lifetime are not modeled.
class MappingCache {
public:
    // 9d80's materialized learning stage: only count 356/448, once per lifetime.
    // Failure does not publish a partial signature. Once ready, these are no-ops.
    bool learn_signature(const std::vector<NameRecord>&);
    bool learn_from_pawn(const Reader&, const RangeCheck&, std::uint64_t pawn);
    bool signature_ready() const { return ready_; }
    const MappingSignature& signature() const { return signature_; }
    MappingResult discover(const Reader&, std::uint64_t mesh, int count); // 5c18
    MappingStats stats() const;
    void clear_context(); // 3536..35c0: entries/tick only; signature and totals survive.
    void reset(); // Owned-session stop policy: also clear learned signature/totals.

private:
    struct Entry {
        std::uint64_t asset = 0, data = 0;
        int count = 0;
        BoneMapping mapping;
        std::uint32_t last_use = 0;
    };
    std::array<Entry, 384> entries_{};
    MappingSignature signature_{};
    bool ready_ = false;
    std::uint32_t tick_ = 0, hits_ = 0, misses_ = 0;
};

} // namespace reconstruction::reference
