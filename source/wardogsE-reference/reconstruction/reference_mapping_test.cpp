#include "reference_mapping.h"

#include <cstdio>
#include <map>
#include <stdexcept>

namespace {
using namespace reconstruction::reference;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr std::uint64_t pawn = 0x18000, mesh = 0x20000, asset = 0x30000, data = 0x40000;
constexpr int edges[][2] = {{0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{5,7},{7,8},{8,9},{9,10},
    {5,11},{11,12},{12,13},{13,14},{1,15},{15,16},{16,17},{1,18},{18,19},{19,20}};
BoneMapping layout(int step = 2) {
    BoneMapping result;
    for (int i = 0; i < 21; ++i) result.indices[i] = i * step;
    result.kind = 5;
    return result;
}
MappingSignature tokens() {
    MappingSignature result;
    for (int i = 0; i < 21; ++i) result[i] = 0x123400000000ULL + i;
    return result;
}
std::vector<NameRecord> records(int count, const BoneMapping& mapping) {
    std::vector<NameRecord> result(count);
    for (int i = 0; i < count; ++i) result[i] = {0x567800000000ULL + i, i == 0 ? -1 : 0};
    for (int i = 0; i < 21; ++i) result[mapping.indices[i]].token = tokens()[i];
    for (const auto& edge : edges) result[mapping.indices[edge[1]]].parent = mapping.indices[edge[0]];
    return result;
}
struct Fixture {
    std::map<std::uint64_t, std::vector<unsigned char>> blocks;
    std::vector<std::pair<std::uint64_t, std::size_t>> reads;
    template<class T> void put(std::uint64_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        blocks[address] = std::vector<unsigned char>(bytes, bytes + sizeof(value));
    }
    void names(std::uint64_t address, const std::vector<NameRecord>& value) {
        std::vector<unsigned char> bytes(value.size() * 12);
        for (std::size_t i = 0; i < value.size(); ++i) {
            std::memcpy(bytes.data() + i * 12, &value[i].token, 8);
            std::memcpy(bytes.data() + i * 12 + 8, &value[i].parent, 4);
        }
        blocks[address] = bytes;
    }
    void descriptor(std::uint64_t address, std::uint64_t pointer, int count, int capacity = -1) {
        put(address, pointer); put(address + 8, count); put(address + 12, capacity < 0 ? count : capacity);
    }
    Reader reader() {
        return [this](std::uint64_t address, void* target, std::size_t size) {
            reads.push_back({address, size});
            auto found = blocks.upper_bound(address);
            if (found == blocks.begin()) return false;
            --found;
            const auto offset = address - found->first;
            if (offset > found->second.size() || size > found->second.size() - offset) return false;
            std::memcpy(target, found->second.data() + offset, size);
            return true;
        };
    }
    void source(std::uint64_t address, const std::vector<NameRecord>& value) {
        put(mesh + 0x5a8, asset);
        descriptor(asset + 0x300, address, static_cast<int>(value.size())); names(address, value);
    }
    bool saw(std::uint64_t address, std::size_t size) const {
        for (const auto& read : reads) if (read.first == address && read.second == size) return true;
        return false;
    }
};

void parse_tests() {
    const auto expected = layout();
    const auto base = records(96, expected);
    const auto parsed = parse_mapping(base, tokens());
    require(parsed.accepted && parsed.mapping.kind == 5 && parsed.mapping.indices == expected.indices,
            "dynamic token mapping differs");
    for (int count : {21, 1200})
        require(parse_mapping(records(count, layout(1)), tokens()).accepted, "count endpoint rejected");
    for (int count : {20, 1201}) {
        auto value = base; value.resize(count);
        require(!parse_mapping(value, tokens()).accepted, "count endpoint outside accepted");
    }
    for (int parent : {-1, 20, 99}) {
        auto value = base; value[20].parent = parent;
        require(!parse_mapping(value, tokens()).accepted, "invalid parent accepted");
    }
    auto changed = base; changed[0].parent = 0;
    require(!parse_mapping(changed, tokens()).accepted, "root parent accepted");
    changed = base; changed[95].token = changed[expected.indices[6]].token;
    require(!parse_mapping(changed, tokens()).accepted, "duplicate wanted token accepted");
    changed = base; changed[expected.indices[6]].token ^= 1ULL << 32;
    require(!parse_mapping(changed, tokens()).accepted, "token high half ignored");
    changed = base; changed[expected.indices[6]].token ^= 1;
    const auto rejected = parse_mapping(changed, tokens());
    require(!rejected.accepted && rejected.mapping.kind == 0, "missing token or failed output differs");
    changed = base; changed[94].token = changed[95].token;
    require(parse_mapping(changed, tokens()).accepted, "unwanted duplicate wrongly rejected");
    for (const auto& edge : edges) {
        if (edge[0] == 0) continue;
        changed = base; changed[expected.indices[edge[1]]].parent = 0;
        require(!parse_mapping(changed, tokens()).accepted, "broken ancestor chain accepted");
    }
    changed = base; changed[5].parent = 4; changed[6].parent = 5;
    require(parse_mapping(changed, tokens()).accepted, "intermediate ancestor rejected");
    MappingSignature same{}; same.fill(base[0].token);
    require(parse_mapping(base, same).accepted, "reference allows repeated target token slots");
}

void learning_tests() {
    MappingCache cache;
    require(!cache.learn_signature(records(148, fixed_bone_mapping(148))), "148 cannot train");
    auto training = records(356, fixed_bone_mapping(356));
    auto broken = training; broken[355].token = training[0].token;
    require(!cache.learn_signature(broken) && !cache.signature_ready(), "partial training published");
    require(cache.learn_signature(training) && cache.signature() == tokens(), "356 learning failed");
    require(cache.learn_signature({}) && cache.signature() == tokens(), "ready state should short circuit");
    cache.reset();
    require(cache.learn_signature(records(448, fixed_bone_mapping(448))), "448 learning failed");
    cache.reset();
    Fixture f; f.put(pawn + 0x3a0, mesh);
    constexpr std::uint64_t transforms = 0x50000;
    f.descriptor(mesh + 0x620, transforms, 356);
    std::array<unsigned char, 96> transform{};
    const double one = 1, hundred = 100;
    for (std::size_t offset : {24U, 64U, 72U, 80U}) std::memcpy(transform.data() + offset, &one, 8);
    f.put(transforms, transform); f.put(transforms + 96, transform);
    std::memcpy(transform.data() + 48, &hundred, 8); f.put(transforms + 9 * 96, transform);
    f.source(data, training);
    // 8a00 retries another descriptor if the first records' parent tree fails.
    broken = training; broken[2].parent = 2; f.names(data, broken);
    f.descriptor(asset + 0x2e0, 0x60000, 356); f.names(0x60000, training);
    int probes = 0;
    const RangeCheck range = [&](std::uint64_t address, std::size_t size) {
        ++probes; return address == transforms && size == 356 * 96;
    };
    require(cache.learn_from_pawn(f.reader(), range, pawn), "pawn learning/fallback failed");
    require(probes == 1 && f.saw(data, 356 * 12) && f.saw(0x60000, 356 * 12), "learning read widths/order");
    f.reads.clear();
    require(cache.learn_from_pawn(f.reader(), range, pawn) && f.reads.empty(), "ready pawn learning read again");
    cache.reset();
    broken = training; broken[355].token = training[0].token; f.names(data, broken); f.reads.clear();
    require(!cache.learn_from_pawn(f.reader(), range, pawn) && !f.saw(0x60000, 356 * 12),
            "9124 failure must not restart 8a00 discovery");
}

void cache_tests() {
    MappingCache cache;
    Fixture f; const auto names = records(96, layout()); f.source(data, names);
    require(!cache.discover(f.reader(), mesh, 96).accepted && f.reads.empty(), "untrained cache read");
    require(cache.learn_signature(records(356, fixed_bone_mapping(356))), "cache training failed");
    for (int count : {20, 1201}) require(!cache.discover(f.reader(), mesh, count).accepted, "bad count accepted");
    require(f.reads.empty(), "bad count read");
    auto result = cache.discover(f.reader(), mesh, 96);
    require(result.accepted && !result.cache_hit && result.slot == 383, "fresh equal-age victim differs");
    require(f.reads == std::vector<std::pair<std::uint64_t, std::size_t>>{
        {mesh + 0x5a8, 8}, {asset + 0x300, 8}, {asset + 0x308, 4}, {asset + 0x30c, 4}, {data, 96 * 12}},
        "discover read order/width differs");
    auto altered = names; altered[0].parent = 0; f.names(data, altered); f.reads.clear();
    result = cache.discover(f.reader(), mesh, 96);
    require(result.accepted && result.cache_hit && !f.saw(data, 96 * 12), "same key should retain stale mapping");
    f.descriptor(asset + 0x300, data + 0x10000, 96); f.reads.clear();
    require(!cache.discover(f.reader(), mesh, 96).accepted && cache.stats().entries == 1,
            "failed miss overwrote cached entry");
    require(!f.saw(asset + 0x2e0, 8), "bulk failure must not try next descriptor");
    f.names(data + 0x10000, names);
    result = cache.discover(f.reader(), mesh, 96);
    require(result.accepted && !result.cache_hit && result.slot == 382, "changed data key not a miss");
    require(cache.stats().tick == 4 && cache.stats().hits == 1 && cache.stats().misses == 3,
            "failed miss must advance usage and miss totals");
    const auto before = cache.stats(); cache.clear_context();
    require(cache.signature_ready() && cache.stats().entries == 0 && cache.stats().tick == 0 &&
            cache.stats().hits == before.hits && cache.stats().misses == before.misses, "context clear scope");
    require(cache.discover(f.reader(), mesh, 96).accepted, "signature did not survive context clear");
    cache.reset(); f.reads.clear();
    require(!cache.signature_ready() && cache.stats().entries == 0 && cache.stats().hits == 0 &&
            cache.stats().misses == 0 && !cache.discover(f.reader(), mesh, 96).accepted && f.reads.empty(),
            "owned stop reset scope");
    require(cache.learn_signature(records(356, fixed_bone_mapping(356))), "retraining failed");
    // Asset pointer and candidate ordering, independently of data pointer rules.
    f.put(mesh + 0x5a8, std::uint64_t{0x10000}); f.put(mesh + 0x5b0, asset);
    f.reads.clear(); require(cache.discover(f.reader(), mesh, 96).accepted, "second asset fallback");
    require(!f.saw(0x10300, 8) && f.saw(mesh + 0x5b0, 8), "asset lower bound not enforced");
    cache.clear_context(); f.put(mesh + 0x5a8, asset + 1); f.reads.clear();
    require(cache.discover(f.reader(), mesh, 96).accepted && !f.saw(asset + 0x301, 8), "asset alignment");
    cache.clear_context(); f.put(mesh + 0x5a8, asset); f.descriptor(asset + 0x300, data, 95);
    f.descriptor(asset + 0x2e0, data + 0x10000, 96); f.reads.clear();
    require(cache.discover(f.reader(), mesh, 96).accepted && f.saw(asset + 0x2e0, 8), "count mismatch fallback");
    for (const auto& item : std::vector<std::pair<std::uint64_t, bool>>{
             {0x10000, false}, {0x10001, false}, {0x10008, true},
             {0x7ffffffffff8ULL, true}, {0x7fffffffffffULL, false}}) {
        cache.clear_context(); f.put(mesh + 0x5a8, item.first); f.put(mesh + 0x5b0, std::uint64_t{0});
        f.descriptor(item.first + 0x300, data + 0x10000, 96);
        require(cache.discover(f.reader(), mesh, 96).accepted == item.second, "asset address boundary");
    }
    cache.clear_context(); f.source(data, names);
    // Fill 384 entries, refresh the oldest, then evict the second-oldest.
    for (int i = 0; i < 384; ++i) {
        const auto pointer = 0x100000ULL + std::uint64_t(i) * 0x10000;
        f.source(pointer, names); result = cache.discover(f.reader(), mesh, 96);
        require(result.accepted && result.slot == 383 - i, "384-slot insertion order");
    }
    require(cache.stats().entries == 384, "cache capacity differs");
    f.source(0x100000, names); require(cache.discover(f.reader(), mesh, 96).cache_hit, "refresh oldest");
    f.source(0x2000000, names); result = cache.discover(f.reader(), mesh, 96);
    require(result.accepted && result.slot == 382 && cache.stats().entries == 384, "age victim differs");
    f.source(0x100000, names); require(cache.discover(f.reader(), mesh, 96).cache_hit, "recently refreshed entry lost");
}
} // namespace

int main() {
    try {
        parse_tests(); learning_tests(); cache_tests();
        std::puts("PASS reference mapping: token/tree parsing, signature learning, 384-slot owned cache lifecycle");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL reference mapping: %s\n", error.what());
        return 1;
    }
}
