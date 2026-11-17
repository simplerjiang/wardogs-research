#include "reference_consistency.h"

#include <cstdio>
#include <map>
#include <stdexcept>

namespace {
using namespace reconstruction::reference;
constexpr std::uint64_t pawn = 0x10000, mesh = 0x20000, primary = 0x40000, secondary = 0x50000;
constexpr std::uint64_t base = mesh + 0x620, marker = base + 0x68;
using Read = std::pair<std::uint64_t, std::size_t>;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    std::map<std::uint64_t, std::vector<unsigned char>> bytes;
    std::map<std::uint64_t, int> occurrences;
    std::vector<Read> reads;
    std::function<int(std::uint64_t, void*, std::size_t, int)> intercept;
    unsigned calls = 0;
    template<class T> void put(std::uint64_t address, const T& value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        bytes[address] = std::vector<unsigned char>(data, data + sizeof(value));
    }
    void descriptor(std::uint64_t address, std::uint64_t data) {
        put(address, data); put(address + 8, std::int32_t{96}); put(address + 12, std::int32_t{96});
    }
    void transforms(std::uint64_t address) {
        std::array<unsigned char, 96> value{};
        const double one = 1, distance = 100;
        for (int offset : {24, 64, 72, 80}) std::memcpy(value.data() + offset, &one, 8);
        put(address, value); put(address + 96, value);
        std::memcpy(value.data() + 48, &distance, 8); put(address + 9 * 96, value);
    }
    Fixture() {
        put(pawn + 0x3a0, mesh); descriptor(base, primary); transforms(primary); put(marker, std::int32_t{7});
    }
    void add_secondary() { descriptor(base + 0x10, secondary); transforms(secondary); }
    Reader reader() {
        return [this](std::uint64_t address, void* output, std::size_t size) {
            reads.push_back({address, size});
            const int occurrence = ++occurrences[address];
            if (intercept) {
                const int result = intercept(address, output, size, occurrence);
                if (result >= 0) return result != 0;
            }
            auto found = bytes.upper_bound(address);
            if (found == bytes.begin()) return false;
            --found;
            const auto offset = address - found->first;
            if (offset > found->second.size() || size > found->second.size() - offset) return false;
            std::memcpy(output, found->second.data() + offset, size);
            return true;
        };
    }
    PoseSelection collect(PoseCache& cache) {
        ++calls;
        PoseSelection result;
        result.selected = result.postprocess_complete = true;
        result.pose.mask = 0x1fffff;
        result.pose.points[0] = {double(calls), 2, 3};
        result.score = 42; result.descriptor = 0; result.mapping_kind = 5;
        result.mode = 2; result.pass = 1; result.cache_action = CacheAction::write;
        cache.write(pawn, mesh, 0, 96, 5, 2, 42);
        return result;
    }
    ConsistencyResult run(PoseCache& cache, const std::function<void()>& change = {}) {
        return read_consistent_pose(reader(), [](std::uint64_t, std::size_t) { return true; }, pawn,
            cache, [&] { const auto result = collect(cache); if (change) change(); return result; });
    }
};

void stable_and_order() {
    Fixture f; PoseCache cache;
    const auto result = f.run(cache);
    require(result.status == ConsistencyStatus::stable && result.attempts == 1 && result.clears == 0 &&
            result.selection.pose.mask == 0x1fffff, "stable result");
    const std::vector<Read> expected = {
        {pawn+0x3a0,8}, {base,8}, {base+8,4}, {base+12,4},
        {primary,96}, {primary+96,96}, {primary+9*96,96},
        {marker,4}, {base,8}, {base+8,4}, {base+0x10,8},
        {marker,4}, {base,8}, {base+8,4}, {base+0x10,8}, {pawn+0x3a0,8}
    };
    require(f.reads == expected, "stable read order or widths");
    require(cache.lookup(pawn, mesh).key_matches, "stable collection cache lost");
    // Stable describes sampled identities, not semantic pose validity.
    Fixture empty; PoseCache untouched;
    const auto no_pose = read_consistent_pose(empty.reader(), [](std::uint64_t, std::size_t) {return true;},
        pawn, untouched, [] { return PoseSelection{}; });
    require(no_pose.status == ConsistencyStatus::stable && !no_pose.selection.selected &&
            no_pose.selection.pose.mask == 0, "stable must not imply selected pose");
}

void early_and_short_reads() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Fixture f; PoseCache cache; cache.write(pawn, mesh, 0, 96, 5, 2, 42);
        if (scenario == 0) f.intercept = [](std::uint64_t address, void* output, std::size_t, int) {
            if (address != pawn + 0x3a0) return -1;
            std::memcpy(output, &mesh, 4); return 0; // 7318 suppresses a partial pointer on false.
        };
        if (scenario == 1) f.bytes.erase(base);
        if (scenario >= 2) f.intercept = [scenario](std::uint64_t address, void* output, std::size_t, int n) {
            if (address != base + (scenario == 3 ? 8 : 0) || n != 2) return -1;
            static_cast<unsigned char*>(output)[0] = 0x55; return 0;
        };
        const auto result = f.run(cache);
        const auto expected = scenario == 0 ? ConsistencyStatus::no_mesh :
            scenario == 1 ? ConsistencyStatus::no_pair : ConsistencyStatus::initial_read_failed;
        require(result.status == expected && result.attempts == 0 && result.clears == 0 &&
                cache.lookup(pawn, mesh).key_matches, "early failure must not clear existing pose cache");
        if (scenario == 2) require(f.occurrences[base+8] == 1, "pre pointer failure should skip count");
    }
    for (std::uint64_t failed : {base, base + 8}) {
        Fixture f; PoseCache cache;
        f.intercept = [failed](std::uint64_t address, void* output, std::size_t, int n) {
            if (address != failed || n != 3) return -1;
            static_cast<unsigned char*>(output)[0] = 0x55; return 0;
        };
        const auto result = f.run(cache);
        require(result.status == ConsistencyStatus::stable && result.attempts == 2 && result.clears == 1 &&
                result.selection.pose.points[0][0] == 2, "post short read must clear and retry once");
    }
    Fixture second_pre; PoseCache cache;
    second_pre.intercept = [](std::uint64_t address, void* output, std::size_t, int n) {
        if (address != base || n != 4) return -1;
        static_cast<unsigned char*>(output)[0] = 0x55; return 0;
    };
    const auto result = second_pre.run(cache, [&] { second_pre.put(marker, std::int32_t{8}); });
    require(result.status == ConsistencyStatus::initial_read_failed && result.attempts == 1 &&
            result.clears == 1 && result.selection.pose.mask == 0 && result.selection.selected &&
            result.selection.pose.points[0][0] == 1 && !cache.lookup(pawn,mesh).key_matches,
            "second pre failure must preserve first attempt metadata and prior clear");
}

void ignored_failures_keep_bytes() {
    for (int failed_read : {1, 2}) {
        Fixture marker_partial; PoseCache cache;
        marker_partial.put(marker, std::int32_t{-239}); // Little-endian bytes: 11 ff ff ff.
        marker_partial.intercept = [failed_read](std::uint64_t address, void* output, std::size_t, int n) {
            if (address != marker || n != failed_read) return -1;
            static_cast<unsigned char*>(output)[0] = 0x11; return 0;
        };
        const auto result = marker_partial.run(cache);
        require(result.status == ConsistencyStatus::stable && result.attempts == 1,
                "ignored preferred failure must retain partial bytes over -1 sentinel");
    }
    for (std::uint64_t ignored : {base + 0x10, base + 0x18}) {
        Fixture f; PoseCache local; f.add_secondary();
        f.intercept = [ignored](std::uint64_t address, void* output, std::size_t, int n) {
            if (address != ignored || n != 2) return -1; // After secondary ab80, before collect.
            if (address == base + 0x10) std::memcpy(output, &secondary, 3);
            else { const std::int32_t count = 96; std::memcpy(output, &count, 1); }
            return 0;
        };
        const auto result = f.run(local);
        require(result.status == ConsistencyStatus::stable && result.attempts == 1,
                "ignored pre secondary false must retain copied prefix, not normalize to zero");
    }
    for (std::uint64_t checked : {base + 0x10, base + 0x18}) {
        Fixture f; PoseCache local; f.add_secondary();
        f.intercept = [checked](std::uint64_t address, void* output, std::size_t, int n) {
            if (address != checked || n != 3) return -1; // After collect, no second ab80 when pre ptr exists.
            static_cast<unsigned char*>(output)[0] = 0; return 0;
        };
        const auto result = f.run(local);
        require(result.status == ConsistencyStatus::stable && result.attempts == 2 && result.clears == 1,
                "post secondary failure must not be ignored");
    }
}

void mutation_and_limits() {
    Fixture appear; PoseCache a;
    auto result = appear.run(a, [&] { if (appear.calls == 1) appear.add_secondary(); });
    require(result.status == ConsistencyStatus::stable && result.attempts == 2 && result.clears == 1,
            "secondary appearance must force retry");
    Fixture disappear; PoseCache b; disappear.add_secondary();
    result = disappear.run(b, [&] { if (disappear.calls == 1) disappear.bytes.erase(base+0x10); });
    require(result.status == ConsistencyStatus::stable && result.attempts == 2 && result.clears == 1,
            "secondary disappearance must force retry");
    Fixture persistent; PoseCache c;
    result = persistent.run(c, [&] { persistent.put(marker, std::int32_t(7 + persistent.calls)); });
    require(result.status == ConsistencyStatus::unstable && result.attempts == 2 && result.clears == 2 &&
            result.selection.selected && result.selection.postprocess_complete && result.selection.pose.mask == 0 &&
            result.selection.mode == 2 && result.selection.score == 42 &&
            result.selection.cache_action == CacheAction::write && result.selection.pose.points[0][0] == 2 &&
            !c.lookup(pawn,mesh).key_matches, "exhaustion only clears mask and matching pose cache");
    Fixture moved; PoseCache d;
    result = moved.run(d, [&] { moved.put(pawn+0x3a0, std::uint64_t{mesh + 0x1000}); });
    require(result.status == ConsistencyStatus::unstable && result.attempts == 2 && result.clears == 2 &&
            moved.occurrences[base+12] == 1 && moved.occurrences[base] == 5 &&
            moved.occurrences[mesh+0x1000+0x620] == 0,
            "wrapper retry must not refresh pinned mesh/base or rerun initial 2054");
    // Wrapper detects sampled fields, not transient ABA or array-content changes.
    Fixture undetected; PoseCache e;
    result = undetected.run(e, [&] {
        undetected.put(pawn+0x3a0, std::uint64_t{mesh+0x1000}); undetected.put(pawn+0x3a0, mesh);
        undetected.put(base+12, std::int32_t{97});
        std::array<unsigned char,96> changed{}; undetected.put(primary, changed);
    });
    require(result.status == ConsistencyStatus::stable && result.attempts == 1,
            "capacity/content/ABA are outside compared snapshot");
}
} // namespace

int main() {
    try {
        stable_and_order(); early_and_short_reads(); ignored_failures_keep_bytes(); mutation_and_limits();
        std::puts("PASS reference consistency: snapshot order, partial failures, two attempts, cache clear, known blind spots");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL reference consistency: %s\n", error.what()); return 1;
    }
}
