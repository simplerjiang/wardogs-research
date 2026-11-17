#include "reference_rules.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using namespace reconstruction;
using namespace reconstruction::reference;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Access {
    char kind;
    std::uint64_t address;
    std::size_t size;
    bool operator==(const Access& other) const {
        return kind == other.kind && address == other.address && size == other.size;
    }
};

struct Fixture {
    std::map<std::uint64_t, unsigned char> bytes;
    std::vector<Access> accesses;
    std::uint64_t denied = 0;
    bool allow_range = true;

    template<class T> void put(std::uint64_t address, const T& value) {
        const auto* source = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[address + i] = source[i];
    }
    void descriptor(std::uint64_t address, std::uint64_t data,
                    std::int32_t count = 21, std::int32_t capacity = 21) {
        put(address, data);
        put(address + 8, count);
        put(address + 12, capacity);
    }
    void pose(std::uint64_t address, const Transform& value = {}) {
        std::array<unsigned char, 96> source;
        source.fill(0xcd); // Distinct padding that must not enter the output.
        std::memcpy(source.data(), value.rotation.data(), 32);
        std::memcpy(source.data() + 32, value.translation.data(), 24);
        std::memcpy(source.data() + 64, value.scale.data(), 24);
        put(address, source);
    }
    void bones(std::uint64_t descriptor_address, std::uint64_t data, double distance = 28) {
        descriptor(descriptor_address, data);
        for (std::uint64_t i = 0; i < 21; ++i) pose(data + i * 96);
        Transform ninth;
        ninth.translation[0] = distance;
        pose(data + 9 * 96, ninth);
    }
    Reader reader() {
        return [this](std::uint64_t address, void* output, std::size_t size) {
            accesses.push_back({'r', address, size});
            if (address == denied) return false;
            auto* target = static_cast<unsigned char*>(output);
            for (std::size_t i = 0; i < size; ++i) {
                const auto it = bytes.find(address + i);
                if (it == bytes.end()) return false; // Deliberately permits a partial write.
                target[i] = it->second;
            }
            return true;
        };
    }
    RangeCheck range() {
        return [this](std::uint64_t address, std::size_t size) {
            accesses.push_back({'q', address, size});
            if (!allow_range) return false;
            for (std::size_t i = 0; i < size; ++i)
                if (bytes.find(address + i) == bytes.end()) return false;
            return true;
        };
    }
};

constexpr std::uint64_t pawn = 0x10000, root = 0x20000, mesh = 0x30000;
constexpr std::uint64_t data = 0x40000, other_data = 0x50000;
const double nan = std::numeric_limits<double>::quiet_NaN();
const double infinity = std::numeric_limits<double>::infinity();

void positions() {
    require(!scalar(nan) && !scalar(infinity) && !scalar(-infinity) &&
            !scalar(1e10) && !scalar(-1e10), "scalar rejects NaN/Inf and strict endpoints");
    require(scalar(std::nextafter(1e10, 0.0)) && scalar(std::nextafter(-1e10, 0.0)),
            "scalar rejects inside endpoint");
    require(!position(Position{}) && !position(Position{-0.0, 0, -0.0}), "zero position accepted");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (double bad : {nan, infinity, -infinity, 1e10, -1e10}) {
            Position value{1, 2, 3};
            value[axis] = bad;
            require(!position(value), "position invalid component accepted");
        }
    }
    Fixture f;
    f.put(pawn + 0x1c0, root);
    f.put(pawn + 0x3a0, mesh);
    f.put(root + 0x230, Position{1, 2, 3});
    f.put(root + 0x210, Position{4, 5, 6});
    f.put(mesh + 0x230, Position{7, 8, 9});
    auto chosen = choose_position(f.reader(), pawn);
    require(chosen.accepted && chosen.address == root + 0x230 && chosen.value == Position{1, 2, 3},
            "primary position selection");
    require(f.accesses == std::vector<Access>{{'r', pawn + 0x1c0, 8}, {'r', root + 0x230, 24}},
            "primary did not short circuit");
    f.put(root + 0x230, Position{});
    f.accesses.clear();
    chosen = choose_position(f.reader(), pawn);
    require(chosen.accepted && chosen.address == root + 0x210 && f.accesses.size() == 3,
            "secondary fallback or short circuit");
    f.denied = root + 0x210;
    f.accesses.clear();
    chosen = choose_position(f.reader(), pawn);
    require(chosen.accepted && chosen.value == Position{7, 8, 9}, "mesh fallback");
    require(f.accesses == std::vector<Access>{{'r', pawn + 0x1c0, 8}, {'r', root + 0x230, 24},
            {'r', root + 0x210, 24}, {'r', pawn + 0x3a0, 8}, {'r', mesh + 0x230, 24}},
            "three-stage read order");
    f.put(mesh + 0x230, Position{nan, 8, 9});
    chosen = choose_position(f.reader(), pawn);
    require(!chosen.accepted && chosen.address == 0 && chosen.value == Position{},
            "new interface failed output is not empty");
    f.put(mesh + 0x230, Position{7, 8, 9});
    f.denied = pawn + 0x1c0;
    f.accesses.clear();
    require(choose_position(f.reader(), pawn).accepted && f.accesses.size() == 3,
            "missing root did not skip its candidates");
    std::puts("PASS reference position: strict predicates, three candidates and short-circuit order");
}

void transform_rules() {
    const Transform identity;
    require(transform(identity), "identity rejected");
    for (std::size_t index = 0; index < 10; ++index) {
        Transform value;
        if (index < 4) value.rotation[index] = nan;
        else if (index < 7) value.translation[index - 4] = nan;
        else value.scale[index - 7] = nan;
        require(!transform(value), "NaN component accepted");
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (double accepted : {-100.0, -1e-6, 1e-6, 100.0}) {
            Transform value;
            value.scale[axis] = accepted;
            require(transform(value) && value.scale[axis] == accepted, "scale endpoint changed or rejected");
        }
        for (double rejected : {0.0, -0.0, 0.5e-6, -0.5e-6,
             std::nextafter(1e-6, 0.0), std::nextafter(-1e-6, 0.0),
             std::nextafter(100.0, infinity), std::nextafter(-100.0, -infinity)}) {
            Transform value;
            value.scale[axis] = rejected;
            require(!transform(value), "scale outside accepted intervals");
        }
    }
    for (double boundary : {0.8, 1.2}) {
        const double center = std::sqrt(boundary);
        for (double w : {std::nextafter(center, 0.0), center, std::nextafter(center, infinity)}) {
            Transform value;
            value.rotation[3] = w;
            const double squared = w * w;
            require(transform(value) == (squared >= 0.8 && squared <= 1.2),
                    "quaternion squared-norm threshold");
        }
    }
    for (double w : {0.0, 0.8, 1.1}) {
        Transform value;
        value.rotation[3] = w;
        require(!transform(value), "invalid quaternion norm");
    }
    Transform unnormalized;
    unnormalized.rotation[3] = 0.9;
    require(transform(unnormalized) && unnormalized.rotation[3] == 0.9,
            "accepted rotation was normalized");
    Fixture f;
    Transform original;
    original.translation = {2, 3, 4};
    original.scale = {-1, 2, 3};
    f.pose(data, original);
    const auto copied = read_transform(f.reader(), data);
    require(copied.read && copied.valid && copied.value.rotation == original.rotation &&
            copied.value.translation == original.translation && copied.value.scale == original.scale,
            "96-to-80 groups or padding copied incorrectly");
    require(f.accesses == std::vector<Access>{{'r', data, 96}}, "transform read is not 96 bytes");
    f.bytes.erase(data + 95);
    const auto partial = read_transform(f.reader(), data);
    require(!partial.read && !partial.valid && partial.value.rotation == identity.rotation &&
            partial.value.translation == identity.translation && partial.value.scale == identity.scale,
            "partial transform did not retain identity");
    f.accesses.clear();
    const auto absent = read_transform(f.reader(), 0);
    require(!absent.read && !absent.valid && f.accesses.empty() && absent.value.rotation == identity.rotation,
            "zero source transform performed a read");
    original.scale[0] = 0;
    f.pose(data, original);
    const auto rejected = read_transform(f.reader(), data);
    require(rejected.read && !rejected.valid && rejected.value.scale[0] == 0 &&
            rejected.value.translation == original.translation, "invalid read lost original values");
    std::puts("PASS reference transform: 96-byte read, identity failure, ten NaN components and numeric boundaries");
}

paths::Descriptor description(std::uint64_t address = data, std::int32_t count = 21,
                              std::int32_t capacity = 21) {
    return {{0, true, address}, {0, true, count}, {0, true, capacity}};
}

void descriptions() {
    require(bone_description(description()), "baseline description rejected");
    for (std::int32_t count : {21, 1200})
        require(bone_description(description(data, count, count)), "count inclusive endpoint");
    for (std::int32_t count : {20, 1201, -1, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max()})
        require(!bone_description(description(data, count, 2400)), "invalid count accepted");
    require(!bone_description(description(data, 21, 20)) &&
            bone_description(description(data, 21, 2400)) &&
            !bone_description(description(data, 21, 2401)), "capacity boundaries");
    require(!bone_description(description(0xffff)) && bone_description(description(0x10000)) &&
            bone_description(description(0x7fffffffffffULL)) &&
            !bone_description(description(0x800000000000ULL)), "data address boundaries");
    auto missing = description();
    missing.data.read = false;
    require(!bone_description(missing), "missing data accepted");
    missing = description(); missing.count.read = false;
    require(!bone_description(missing), "missing count accepted");
    missing = description(); missing.capacity.read = false;
    require(!bone_description(missing), "missing capacity accepted");
    Fixture f;
    f.bones(mesh + 0x620, data);
    f.put(mesh + 0x628, std::int32_t{20});
    require(!bone_candidate(f.reader(), f.range(), mesh, 0x620), "invalid description passed candidate");
    require(f.accesses == std::vector<Access>{{'r', mesh + 0x620, 8},
            {'r', mesh + 0x628, 4}, {'r', mesh + 0x62c, 4}},
            "description predicates ran before all three reads or range ran after rejection");
    std::puts("PASS reference descriptions: exact count/capacity/address bounds and read-status requirements");
}

void roots_and_candidates() {
    for (double input : {-1.0, 0.0, -0.0, -infinity, nan})
        require(distance_root(input) == 0, "0718 ordered-positive entry changed");
    require(distance_root(1) == 1 && distance_root(4) == 2 && distance_root(0.25) == 0.5,
            "0718 ordinary results");
    require(distance_root(1e-300) == 1.0 / 65536 && std::isnan(distance_root(infinity)),
            "0718 was replaced by standard sqrt or changed iteration count");
    const std::vector<Access> prefix{{'r', mesh + 0x620, 8}, {'r', mesh + 0x628, 4},
                                     {'r', mesh + 0x62c, 4}, {'q', data, 21 * 96}};
    Fixture f;
    f.bones(mesh + 0x620, data);
    f.allow_range = false;
    require(!bone_candidate(f.reader(), f.range(), mesh, 0x620) && f.accesses == prefix,
            "rejected full range still read samples");
    for (std::size_t failing = 0; failing < 3; ++failing) {
        Fixture sample;
        sample.bones(mesh + 0x620, data);
        const std::array<std::uint64_t, 3> indices{0, 1, 9};
        Transform invalid;
        invalid.scale[0] = 0;
        sample.pose(data + indices[failing] * 96, invalid);
        require(!bone_candidate(sample.reader(), sample.range(), mesh, 0x620),
                "invalid sample valid flag ignored");
        auto expected = prefix;
        for (std::size_t i = 0; i <= failing; ++i) expected.push_back({'r', data + indices[i] * 96, 96});
        require(sample.accesses == expected, "sample 0/1/9 short-circuit order");
    }
    for (double separation : {27.0, 28.0, 145.0, 146.0}) {
        Fixture sample;
        sample.bones(mesh + 0x620, data, separation);
        Transform unrelated;
        unrelated.translation = {500, 600, 700};
        sample.pose(data, unrelated);
        require(bone_candidate(sample.reader(), sample.range(), mesh, 0x620) ==
                    (separation == 28 || separation == 145), "sample separation or wrong sample pair");
    }
    std::puts("PASS reference bone candidates: range probe, valid flags, 0/1/9 order and 28..145 boundaries");
}

void descriptor_pair() {
    Fixture f;
    f.bones(mesh + 0x620, data);
    require(bone_pair_base(f.reader(), f.range(), mesh) == 0x620, "first description rejected");
    for (const auto& access : f.accesses)
        require(access.address != mesh + 0x630, "second description read after first accepted");
    f.bones(mesh + 0x630, other_data);
    f.put(mesh + 0x628, std::int32_t{20});
    f.accesses.clear();
    require(bone_pair_base(f.reader(), f.range(), mesh) == 0x620, "second description did not return pair base");
    require(f.accesses.size() == 10 && f.accesses[0] == Access{'r', mesh + 0x620, 8} &&
            f.accesses[2] == Access{'r', mesh + 0x62c, 4} &&
            f.accesses[3] == Access{'r', mesh + 0x630, 8} &&
            f.accesses[6] == Access{'q', other_data, 21 * 96}, "pair fallback order");
    f.put(mesh + 0x638, std::int32_t{20});
    require(bone_pair_base(f.reader(), f.range(), mesh) == 0, "two invalid descriptions returned base");
    std::puts("PASS reference descriptor pair: first short-circuit, second returns 0x620, both fail");
}
} // namespace

int main() {
    try {
        positions();
        transform_rules();
        descriptions();
        roots_and_candidates();
        descriptor_pair();
        std::puts("PASS static reference rule fixtures; live reads, cache and final pose selection remain NOT_TESTED");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
