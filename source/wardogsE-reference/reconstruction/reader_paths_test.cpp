#include "reader_paths.h"

#include <cstdio>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace reconstruction::paths;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::map<std::uint64_t, unsigned char> bytes;
    std::vector<std::pair<std::uint64_t, std::size_t>> trace;
    std::uint64_t denied = 0;

    template<class T> void put(std::uint64_t address, const T& value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[address + i] = data[i];
    }
    void descriptor(std::uint64_t address, std::uint64_t data,
                    std::int32_t count, std::int32_t capacity) {
        put(address, data);
        put(address + 8, count);
        put(address + 12, capacity);
    }
    Reader reader() {
        return [this](std::uint64_t address, void* output, std::size_t size) {
            trace.emplace_back(address, size);
            if (address == denied) return false;
            auto* dest = static_cast<unsigned char*>(output);
            for (std::size_t i = 0; i < size; ++i) {
                const auto found = bytes.find(address + i);
                if (found == bytes.end()) return false; // Deliberate partial write.
                dest[i] = found->second;
            }
            return true;
        };
    }
};

constexpr std::uint64_t pawn = 0x10000, root = 0x20000, mesh = 0x30000;
constexpr std::uint64_t component = 0x40000, seat = 0x50000;

void position_paths() {
    Fixture f;
    f.put(pawn + 0x1c0, root);
    f.put(pawn + 0x3a0, mesh);
    f.put(root + 0x230, Position{1, 2, 3});
    f.put(root + 0x210, Position{4, 5, 6});
    f.put(root + 0x168, Position{7, 8, 9});
    f.put(mesh + 0x230, Position{10, 11, 12});
    const auto ref = positions(f.reader(), Source::reference, pawn);
    const auto local = positions(f.reader(), Source::local, pawn);
    require(ref.source == Source::reference && local.source == Source::local,
            "position source lost");
    require(ref.root_primary.read && ref.root_primary.value == Position{1, 2, 3} &&
            ref.root_secondary.value == Position{4, 5, 6}, "reference position paths");
    require(local.root_primary.read && local.root_primary.value == Position{7, 8, 9} &&
            !local.root_secondary.read, "local position path");
    require(ref.mesh_position.value == local.mesh_position.value &&
            ref.mesh_position.value == Position{10, 11, 12}, "mesh candidate");
    f.denied = root + 0x230;
    const auto failed = positions(f.reader(), Source::reference, pawn);
    require(!failed.root_primary.read && failed.root_secondary.read && failed.mesh_position.read,
            "candidate failure leaked across fields");
    f.put(root + 0x168, Position{});
    require(positions(f.reader(), Source::local, pawn).root_primary.read,
            "readable zero incorrectly rejected");
    std::puts("PASS position candidates: separate sources and independent observations");
}

void association_paths() {
    Fixture f;
    f.put(pawn + 0x768, component);
    f.put(component + 0x188, seat);
    f.put(seat + 0x20, std::uint64_t{0x60000});
    f.put(seat + 0xc8, std::uint64_t{0x70000});
    require(association(f.reader(), Source::reference, pawn).candidate.value == 0x60000 &&
            association(f.reader(), Source::local, pawn).candidate.value == 0x70000,
            "association endpoints mixed");
    f.denied = component + 0x188;
    f.trace.clear();
    const auto failed = association(f.reader(), Source::reference, pawn);
    require(!failed.seat.read && !failed.candidate.read && f.trace.size() == 2,
            "failed dependency still traversed");
    f.denied = 0;
    f.put(component + 0x188, std::uint64_t{0});
    const auto zero = association(f.reader(), Source::reference, pawn);
    require(zero.seat.read && zero.seat.value == 0 && !zero.candidate.read,
            "zero pointer confused with read failure");
    f.put(component + 0x188, seat);
    f.put(seat + 0x20, pawn);
    require(association(f.reader(), Source::reference, pawn).candidate.value == pawn,
            "raw cycle candidate incorrectly accepted or rejected semantically");
    std::puts("PASS association candidates: distinct ends, failed/zero dependencies, cycle observation");
}

void descriptor_paths() {
    Fixture f;
    f.descriptor(mesh + 0x620, 0x80000, 2001, 2401);
    f.descriptor(mesh + 0x630, 0x90000, 21, 30);
    f.descriptor(mesh + 0xa18, 0xa0000, 1201, 9);
    const auto ref = bones(f.reader(), Source::reference, mesh);
    require(ref.first.count.value == 2001 && ref.first.capacity.value == 2401 &&
            ref.second.data.value == 0x90000, "reference descriptor candidates");
    const std::vector<std::pair<std::uint64_t, std::size_t>> expected{
        {mesh + 0x620, 8}, {mesh + 0x628, 4}, {mesh + 0x62c, 4},
        {mesh + 0x630, 8}, {mesh + 0x638, 4}, {mesh + 0x63c, 4}};
    require(f.trace == expected, "reference split widths/order");
    f.trace.clear();
    const auto local = bones(f.reader(), Source::local, mesh);
    require(local.first.data.value == 0xa0000 && local.first.count.value == 1201 &&
            local.second.count.value == 2001 && !local.first.capacity.read &&
            !local.second.capacity.read && f.trace.size() == 4,
            "local descriptor invented capacity or selection");
    f.denied = mesh + 0x628;
    f.trace.clear();
    const auto failed = bones(f.reader(), Source::reference, mesh);
    require(failed.first.data.read && !failed.first.count.read && !failed.first.capacity.read &&
            failed.second.capacity.read && f.trace.size() == 5,
            "descriptor failure propagation");
    // Deliberately invalid counts remain raw observations, not accepted poses.
    for (std::int32_t count : {20, 21, 1200, 1201, 2000, 2001}) {
        f.put(mesh + 0xa20, count);
        require(bones(f.reader(), Source::local, mesh).first.count.value == count,
                "raw descriptor unexpectedly normalized");
    }
    std::puts("PASS bone descriptor candidates: 8/4/4 vs 8/4; no pose validity claim");
}

void container_paths() {
    constexpr std::uint64_t world = 0xb0000, list = 0xc0000;
    constexpr std::uint64_t level1 = 0xd0000, level2 = 0xe0000;
    Fixture f;
    f.descriptor(world + 0x1c8, list, 2, 2);
    f.put(world + 0x30, level2);
    f.put(list, level1);
    f.put(list + 8, level2);
    f.descriptor(level1 + 0xa0, 0xf0000, 1, 1);
    f.descriptor(level2 + 0xa0, 0x100000, 2, 2);
    f.put(0xf0000, pawn);
    f.put(0x100000, mesh);
    f.put(0x100008, pawn);
    const auto ref = levels(f.reader(), Source::reference, world);
    require(f.trace.size() == 1 && f.trace.front().second == 16,
            "level descriptor is not one 16-byte read");
    const auto first = actors(f.reader(), ref.source, item(f.reader(), ref.list, 0));
    const auto second = actors(f.reader(), ref.source, item(f.reader(), ref.list, 1));
    const auto local = levels(f.reader(), Source::local, world);
    const auto single = actors(f.reader(), local.source, local.single);
    require(first.capacity.read && second.capacity.read && !single.capacity.read,
            "actor descriptor source widths");
    require(item(f.reader(), first, 0).value == pawn &&
            item(f.reader(), second, 0).value == mesh &&
            item(f.reader(), single, 0).value == mesh &&
            item(f.reader(), second, 1).value == pawn, "container addressing or duplicates lost");
    f.bytes.erase(world + 0x1c8 + 15);
    const auto truncated = levels(f.reader(), Source::reference, world);
    require(!truncated.list.data.read && !truncated.list.count.read &&
            truncated.list.data.value == 0, "partial packed read leaked as success");
    std::puts("PASS containers: multi/single level paths, packed/split reads, duplicate raw entries");
}

void lifetime_observations() {
    Fixture f;
    f.put(pawn + 0x1c0, root);
    f.put(root + 0x168, Position{1, 2, 3});
    const auto old = positions(f.reader(), Source::local, pawn);
    f.bytes.erase(root + 0x168 + 23);
    const auto removed = positions(f.reader(), Source::local, pawn);
    require(!removed.root_primary.read && removed.root_primary.value == Position{},
            "partial position reused old output");
    f.put(root + 0x168, Position{4, 5, 6});
    const auto reused = positions(f.reader(), Source::local, pawn);
    require(reused.root_primary.value == Position{4, 5, 6} &&
            old.root_primary.value == Position{1, 2, 3}, "observations alias or cache stale bytes");
    f.put(pawn + 0x1c0, std::uint64_t{0x110000});
    require(!positions(f.reader(), Source::local, pawn).root_primary.read,
            "changed pointer retained previous result");
    std::puts("PASS fixture lifetime: erase, partial read, same-address reuse, changed pointer");
}

int main() {
    try {
        position_paths();
        association_paths();
        descriptor_paths();
        container_paths();
        lifetime_observations();
        std::puts("PASS raw path fixture suite; semantic selectors and live game remain NOT_TESTED");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
