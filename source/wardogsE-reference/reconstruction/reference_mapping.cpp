#include "reference_mapping.h"

namespace reconstruction::reference {
namespace {

// 9124 stack table: RVA 14af0/14b20/14b30/14b10, literals 11..14,
// RVA 14b60/14b70, literals 20/-1. Values name slots in the 21-point mapping.
constexpr int chains[6][5] = {
    {0, 1, 2, 3, 4}, {4, 5, 6, -1, -1}, {5, 7, 8, 9, 10},
    {5, 11, 12, 13, 14}, {1, 15, 16, 17, -1}, {1, 18, 19, 20, -1}
};

bool tree(const std::vector<NameRecord>& records) {
    if (records.size() < 21 || records.size() > 1200 || records[0].parent != -1)
        return false;
    for (std::size_t i = 1; i < records.size(); ++i)
        if (records[i].parent < 0 || records[i].parent >= static_cast<int>(i)) return false;
    return true;
}

bool asset_pointer(std::uint64_t value) { // 4ec0, not 73a8's array-data predicate.
    return value > 0x10000 && value < 0x7fffffffffffULL && (value & 7) == 0;
}

bool read_records(const Reader& reader, std::uint64_t data, int count,
                  std::vector<NameRecord>& records) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(count) * 12);
    if (!reader(data, bytes.data(), bytes.size())) return false;
    records.resize(count);
    for (int i = 0; i < count; ++i) {
        std::memcpy(&records[i].token, bytes.data() + std::size_t(i) * 12, 8);
        std::memcpy(&records[i].parent, bytes.data() + std::size_t(i) * 12 + 8, 4);
    }
    return true;
}

struct NameSource {
    bool found = false;
    std::uint64_t asset = 0, data = 0;
    std::vector<NameRecord> records;
};

NameSource find_source(const Reader& reader, std::uint64_t mesh, int count, bool for_learning) {
    for (std::uint64_t mesh_offset : {0x5a8ULL, 0x5b0ULL}) {
        const auto asset = paths::field<std::uint64_t>(reader, mesh + mesh_offset);
        if (!asset_pointer(asset.value)) continue;
        for (std::uint64_t offset : {0x300ULL, 0x2e0ULL}) {
            const auto desc = paths::split_descriptor(reader, asset.value + offset, true);
            if (!bone_description(desc) || desc.count.value != count) continue;
            NameSource source{true, asset.value, desc.data.value, {}};
            // 8a00 retries the next descriptor after a failed read/tree check.
            // 5c18 instead accepts this descriptor before its cache lookup.
            if (for_learning && (!read_records(reader, source.data, count, source.records) ||
                                 !tree(source.records))) continue;
            return source;
        }
    }
    return {};
}

} // namespace

MappingParse parse_mapping(const std::vector<NameRecord>& records, const MappingSignature& signature) {
    if (!tree(records)) return {};
    BoneMapping mapping;
    for (std::size_t slot = 0; slot < signature.size(); ++slot) {
        int found = -1;
        for (std::size_t index = 0; index < records.size(); ++index) {
            if (records[index].token != signature[slot]) continue;
            if (found >= 0) return {};
            found = static_cast<int>(index);
        }
        if (found < 0) return {};
        mapping.indices[slot] = found;
    }
    for (const auto& chain : chains) {
        for (int i = 1; i < 5 && chain[i] >= 0; ++i) {
            const int ancestor = mapping.indices[chain[i - 1]];
            int node = mapping.indices[chain[i]];
            while (node >= 0 && node > ancestor) node = records[node].parent;
            if (node != ancestor) return {};
        }
    }
    mapping.kind = 5;
    return {true, mapping};
}

bool MappingCache::learn_signature(const std::vector<NameRecord>& records) {
    if (ready_) return true;
    if (records.size() != 356 && records.size() != 448) return false;
    const auto fixed = fixed_bone_mapping(records.size());
    MappingSignature candidate{};
    for (std::size_t i = 0; i < candidate.size(); ++i)
        candidate[i] = records[fixed.indices[i]].token;
    if (!parse_mapping(records, candidate).accepted) return false;
    signature_ = candidate;
    ready_ = true;
    return true;
}

bool MappingCache::learn_from_pawn(const Reader& reader, const RangeCheck& range, std::uint64_t pawn) {
    if (ready_) return true;
    const auto mesh = paths::field<std::uint64_t>(reader, pawn + 0x3a0);
    const auto base = bone_pair_base(reader, range, mesh.value);
    if (base == 0) return false;
    auto desc = paths::split_descriptor(reader, mesh.value + base, true);
    if (!bone_description(desc)) desc = paths::split_descriptor(reader, mesh.value + base + 0x10, true);
    if (!bone_description(desc) || (desc.count.value != 356 && desc.count.value != 448)) return false;
    const auto source = find_source(reader, mesh.value, desc.count.value, true);
    return source.found && learn_signature(source.records);
}

MappingResult MappingCache::discover(const Reader& reader, std::uint64_t mesh, int count) {
    if (!ready_ || count < 21 || count > 1200) return {};
    const auto source = find_source(reader, mesh, count, false);
    if (!source.found) return {};
    ++tick_;
    std::size_t victim = 0;
    std::uint32_t greatest_age = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        auto& entry = entries_[i];
        if (entry.asset == source.asset && entry.data == source.data && entry.count == count) {
            ++hits_;
            entry.last_use = tick_;
            return {true, true, source.asset, source.data, count, static_cast<int>(i), entry.mapping};
        }
        const std::uint32_t age = tick_ - entry.last_use;
        if (age >= greatest_age) { greatest_age = age; victim = i; }
    }
    ++misses_;
    std::vector<NameRecord> records;
    if (!read_records(reader, source.data, count, records)) return {};
    const auto parsed = parse_mapping(records, signature_);
    if (!parsed.accepted) return {};
    entries_[victim] = {source.asset, source.data, count, parsed.mapping, tick_};
    return {true, false, source.asset, source.data, count, static_cast<int>(victim), parsed.mapping};
}

MappingStats MappingCache::stats() const {
    std::size_t count = 0;
    for (const auto& entry : entries_) if (entry.asset != 0) ++count;
    return {tick_, hits_, misses_, count};
}

void MappingCache::clear_context() { entries_ = {}; tick_ = 0; }
void MappingCache::reset() { clear_context(); signature_ = {}; ready_ = false; hits_ = misses_ = 0; }

} // namespace reconstruction::reference
