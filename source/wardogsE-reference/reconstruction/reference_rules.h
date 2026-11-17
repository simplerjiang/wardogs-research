#pragma once

#include "reader_paths.h"

namespace reconstruction::reference {

using paths::Position;
using paths::Reader;
using RangeCheck = std::function<bool(std::uint64_t, std::size_t)>;

// 0570 is stricter than isfinite. Position combines 25c4 with 86fc's nonzero rule.
inline bool scalar(double value) { return value > -1e10 && value < 1e10; }
inline bool position(const Position& value) {
    return scalar(value[0]) && scalar(value[1]) && scalar(value[2]) &&
           (value[0] != 0 || value[1] != 0 || value[2] != 0);
}

struct PositionChoice {
    bool accepted = false;
    std::uint64_t address = 0;
    Position value{};
};

// 86fc acceptance and short-circuit order. Our failed result is empty;
// the original may leave the last rejected candidate in its output buffer.
inline PositionChoice choose_position(const Reader& reader, std::uint64_t pawn) {
    const auto root = paths::field<std::uint64_t>(reader, pawn + 0x1c0);
    if (root.read && root.value != 0) {
        for (std::uint64_t offset : {0x230ULL, 0x210ULL}) {
            const auto candidate = paths::field<Position>(reader, root.value + offset);
            if (candidate.read && position(candidate.value))
                return {true, candidate.address, candidate.value};
        }
    }
    const auto mesh = paths::field<std::uint64_t>(reader, pawn + 0x3a0);
    if (mesh.read && mesh.value != 0) {
        const auto candidate = paths::field<Position>(reader, mesh.value + 0x230);
        if (candidate.read && position(candidate.value))
            return {true, candidate.address, candidate.value};
    }
    return {};
}

struct Transform {
    std::array<double, 4> rotation{0, 0, 0, 1};
    Position translation{};
    Position scale{1, 1, 1};
};

// aa0c: reject only; negative scale is allowed and rotation is not normalized.
inline bool transform(const Transform& value) {
    for (double n : value.rotation) if (!scalar(n)) return false;
    for (double n : value.translation) if (!scalar(n)) return false;
    for (double n : value.scale) if (!scalar(n)) return false;
    const auto& q = value.rotation;
    const double norm = ((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3];
    if (norm < 0.8 || norm > 1.2) return false;
    for (double n : value.scale)
        if ((n > -1e-6 && n < 1e-6) || n < -100 || n > 100) return false;
    return true;
}

struct TransformRead {
    Transform value;
    bool read = false;
    bool valid = false;
};

// 8b30: one 96-byte read; padding at 0x38/0x58 is not copied.
// Failed reads retain identity, successful but invalid reads retain their values.
inline TransformRead read_transform(const Reader& reader, std::uint64_t address) {
    TransformRead result;
    if (address == 0) return result;
    std::array<unsigned char, 96> source{};
    if (!reader(address, source.data(), source.size())) return result;
    result.read = true;
    std::memcpy(result.value.rotation.data(), source.data(), 32);
    std::memcpy(result.value.translation.data(), source.data() + 32, 24);
    std::memcpy(result.value.scale.data(), source.data() + 64, 24);
    result.valid = transform(result.value);
    return result;
}

inline bool bone_description(const paths::Descriptor& value) {
    return value.data.read && value.count.read && value.capacity.read &&
           value.count.value >= 21 && value.count.value <= 1200 &&
           value.count.value <= value.capacity.value && value.capacity.value <= 2400 &&
           value.data.value >= 0x10000 && value.data.value < 0x800000000000ULL;
}

// 0718 uses sixteen Newton steps, not the standard library square root.
// The assembly's initial ordered comparison also returns zero for NaN.
inline double distance_root(double value) {
    if (!(value > 0)) return 0;
    double estimate = value > 1 ? value * 0.5 + 0.5 : 1;
    for (int i = 0; i < 16; ++i) {
        if (estimate < 1e-18) break;
        estimate = (estimate + value / estimate) * 0.5;
    }
    return estimate;
}

// ab80: range predicate is supplied by the owned-byte fixture. This does not
// implement VirtualQuery, its eight-slot cache, or a live memory provider.
inline bool bone_candidate(const Reader& reader, const RangeCheck& range,
                           std::uint64_t mesh, std::uint32_t offset) {
    const auto descriptor = paths::split_descriptor(reader, mesh + offset, true);
    if (!bone_description(descriptor)) return false;
    const auto address = descriptor.data.value;
    if (!range(address, std::size_t(descriptor.count.value) * 96)) return false;
    if (!read_transform(reader, address).valid) return false;
    const auto first = read_transform(reader, address + 96);
    if (!first.valid) return false;
    const auto ninth = read_transform(reader, address + 9 * 96);
    if (!ninth.valid) return false;
    const double dx = first.value.translation[0] - ninth.value.translation[0];
    const double dy = first.value.translation[1] - ninth.value.translation[1];
    const double dz = first.value.translation[2] - ninth.value.translation[2];
    const double distance = distance_root((dy * dy + dx * dx) + dz * dz);
    return distance >= 28 && distance <= 145;
}

// 2054 identifies a descriptor pair. It does not select the eventual pose array.
inline std::uint32_t bone_pair_base(const Reader& reader, const RangeCheck& range,
                                    std::uint64_t mesh) {
    if (bone_candidate(reader, range, mesh, 0x620)) return 0x620;
    return bone_candidate(reader, range, mesh, 0x630) ? 0x620 : 0;
}

} // namespace reconstruction::reference
