#pragma once

#include "reference_pose.h"

namespace reconstruction::reference {

// 19a40: 192 x 0x28 bytes. These are opaque identities, never dereferenced.
struct PoseCacheEntry {
    std::uint64_t pawn = 0, mesh = 0;
    std::int32_t descriptor = 0, bone_count = 0, mapping_kind = 0, mode = 0, score = 0;
    std::uint32_t reserved = 0;
};

class PoseCache {
public:
    static constexpr std::size_t capacity = 192;
    PoseCacheHint lookup(std::uint64_t pawn, std::uint64_t mesh) const;
    void write(std::uint64_t pawn, std::uint64_t mesh, int descriptor, int bone_count,
               int mapping_kind, int mode, int score);
    void clear_entity(std::uint64_t pawn);
    // 3536..359c: pass the already-selected context, not every frame's world pointer.
    bool observe_context(std::uint64_t context);
    // Local ownership policy only. Reference stop/restart clearing is unproven.
    void reset_local_session();

    const std::array<PoseCacheEntry, capacity>& entries() const { return entries_; }
    std::uint32_t replacement_cursor() const { return replacement_cursor_; }
    std::uint64_t context() const { return context_; }
private:
    std::array<PoseCacheEntry, capacity> entries_{};
    std::uint32_t replacement_cursor_ = 0;
    std::uint64_t context_ = 0;
};

} // namespace reconstruction::reference
