#include "reference_consistency.h"

namespace reconstruction::reference {
namespace {

struct Snapshot {
    // +00 primary data, +08 secondary data, +10 primary count,
    // +14 secondary count, +18 preferred, +1c zero padding. No capacities.
    std::array<unsigned char, 32> bytes{};
    Snapshot() {
        const std::int32_t preferred = -1;
        std::memcpy(bytes.data() + 24, &preferred, 4);
    }
    std::uint64_t secondary() const {
        std::uint64_t value;
        std::memcpy(&value, bytes.data() + 8, 8);
        return value;
    }
    bool primary(const Reader& reader, std::uint64_t base) {
        reader(base + 0x68, bytes.data() + 24, 4); // Return intentionally ignored.
        return reader(base, bytes.data(), 8) && reader(base + 8, bytes.data() + 16, 4);
    }
};

} // namespace

ConsistencyResult read_consistent_pose(const Reader& reader, const RangeCheck& range,
                                      std::uint64_t pawn, PoseCache& cache,
                                      const std::function<PoseSelection()>& collect) {
    ConsistencyResult result;
    const auto mesh = paths::field<std::uint64_t>(reader, pawn + 0x3a0).value; // 7318
    if (mesh == 0) return result;
    const auto offset = bone_pair_base(reader, range, mesh);
    if (offset == 0) { result.status = ConsistencyStatus::no_pair; return result; }
    const auto base = mesh + offset;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Snapshot before;
        if (!before.primary(reader, base)) {
            result.status = ConsistencyStatus::initial_read_failed;
            return result;
        }
        if (bone_candidate(reader, range, mesh, offset + 0x10)) {
            reader(base + 0x10, before.bytes.data() + 8, 8);
            reader(base + 0x18, before.bytes.data() + 20, 4); // Both returns ignored.
        }
        result.selection = collect();
        ++result.attempts;
        Snapshot after;
        bool consistent = after.primary(reader, base);
        if (consistent) {
            if (before.secondary() == 0) {
                consistent = !bone_candidate(reader, range, mesh, offset + 0x10);
            } else {
                consistent = reader(base + 0x10, after.bytes.data() + 8, 8) &&
                             reader(base + 0x18, after.bytes.data() + 20, 4);
            }
        }
        if (consistent && before.bytes == after.bytes &&
            paths::field<std::uint64_t>(reader, pawn + 0x3a0).value == mesh) {
            result.status = ConsistencyStatus::stable;
            return result;
        }
        result.selection.pose.mask = 0; // Retain collect's points and metadata.
        cache.clear_entity(pawn);
        ++result.clears;
    }
    result.status = ConsistencyStatus::unstable;
    return result;
}

} // namespace reconstruction::reference
