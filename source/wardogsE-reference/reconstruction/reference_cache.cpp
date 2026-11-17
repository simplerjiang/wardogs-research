#include "reference_cache.h"

namespace reconstruction::reference {

PoseCacheHint PoseCache::lookup(std::uint64_t pawn, std::uint64_t mesh) const {
    for (const auto& entry : entries_) {
        if (entry.pawn == pawn && entry.mesh == mesh && entry.score >= 8)
            return {true, entry.descriptor, entry.bone_count, entry.mapping_kind, entry.mode, entry.score};
    }
    return {};
}

void PoseCache::write(std::uint64_t pawn, std::uint64_t mesh, int descriptor, int bone_count,
                      int mapping_kind, int mode, int score) {
    if (score < 8) return;
    std::size_t slot = capacity;
    for (std::size_t i = 0; i < capacity; ++i) {
        if (entries_[i].pawn == pawn) { slot = i; break; }
        if (slot == capacity && entries_[i].pawn == 0) slot = i;
    }
    if (slot == capacity) slot = replacement_cursor_++ % capacity;
    auto& entry = entries_[slot];
    entry.pawn = pawn;
    entry.mesh = mesh;
    entry.descriptor = descriptor;
    entry.bone_count = bone_count;
    entry.mapping_kind = mapping_kind;
    entry.mode = mode;
    entry.score = score;
}

void PoseCache::clear_entity(std::uint64_t pawn) {
    for (auto& entry : entries_) {
        if (entry.pawn == pawn) {
            entry.pawn = entry.mesh = 0;
            entry.score = 0;
            return;
        }
    }
}

bool PoseCache::observe_context(std::uint64_t context) {
    if (context_ == context) return false;
    context_ = context;
    entries_ = {};
    replacement_cursor_ = 0;
    return true;
}

void PoseCache::reset_local_session() {
    entries_ = {};
    replacement_cursor_ = 0;
    context_ = 0;
}

} // namespace reconstruction::reference
