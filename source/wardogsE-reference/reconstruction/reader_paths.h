#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace reconstruction::paths {

enum class Source { reference, local };
using Reader = std::function<bool(std::uint64_t, void*, std::size_t)>;

// Raw observations only. Readability does not establish semantic validity.
// Failed reads discard any partial write. This is our interface contract,
// not a reproduction of the reference's failure-output behavior.
// read=false also covers an unattempted field with a missing dependency.
// Fixture callers provide addresses whose uint64_t additions do not overflow.
template<class T> struct Field {
    std::uint64_t address = 0;
    bool read = false;
    T value{};
};

template<class T>
Field<T> field(const Reader& reader, std::uint64_t address) {
    Field<T> result;
    result.address = address;
    T value{};
    result.read = reader(address, &value, sizeof(value));
    if (result.read) result.value = value;
    return result;
}

template<class T>
Field<T> through(const Reader& reader, const Field<std::uint64_t>& pointer,
                 std::uint64_t offset) {
    if (!pointer.read || pointer.value == 0) return {};
    return field<T>(reader, pointer.value + offset);
}

using Position = std::array<double, 3>;
struct Positions {
    Source source;
    Field<std::uint64_t> root, mesh;
    Field<Position> root_primary, root_secondary, mesh_position;
};

// Collect candidates independently: no fallback selection or coordinate repair.
// The local original's bulk read, occupant override and update order are not modeled.
inline Positions positions(const Reader& reader, Source source, std::uint64_t pawn) {
    Positions result{source, {}, {}, {}, {}, {}};
    result.root = field<std::uint64_t>(reader, pawn + 0x1c0);
    result.mesh = field<std::uint64_t>(reader, pawn + 0x3a0);
    result.root_primary = through<Position>(reader, result.root,
        source == Source::reference ? 0x230 : 0x168);
    if (source == Source::reference)
        result.root_secondary = through<Position>(reader, result.root, 0x210);
    result.mesh_position = through<Position>(reader, result.mesh, 0x230);
    return result;
}

struct Association {
    Source source;
    Field<std::uint64_t> component, seat, candidate;
};

// A three-link candidate, without class, distance or ownership acceptance rules.
inline Association association(const Reader& reader, Source source, std::uint64_t pawn) {
    Association result{source, {}, {}, {}};
    result.component = field<std::uint64_t>(reader, pawn + 0x768);
    result.seat = through<std::uint64_t>(reader, result.component, 0x188);
    result.candidate = through<std::uint64_t>(reader, result.seat,
        source == Source::reference ? 0x20 : 0xc8);
    return result;
}

struct Descriptor {
    Field<std::uint64_t> data;
    Field<std::int32_t> count, capacity;
};

inline Descriptor split_descriptor(const Reader& reader, std::uint64_t address,
                                   bool capacity) {
    Descriptor result;
    result.data = field<std::uint64_t>(reader, address);
    if (!result.data.read) return result;
    result.count = field<std::int32_t>(reader, address + 8);
    if (result.count.read && capacity)
        result.capacity = field<std::int32_t>(reader, address + 12);
    return result;
}

inline Descriptor packed_descriptor(const Reader& reader, std::uint64_t address) {
    std::array<unsigned char, 16> bytes{};
    Descriptor result;
    result.data.address = address;
    result.count.address = address + 8;
    result.capacity.address = address + 12;
    if (!reader(address, bytes.data(), bytes.size())) return result;
    std::memcpy(&result.data.value, bytes.data(), 8);
    std::memcpy(&result.count.value, bytes.data() + 8, 4);
    std::memcpy(&result.capacity.value, bytes.data() + 12, 4);
    result.data.read = result.count.read = result.capacity.read = true;
    return result;
}

struct Bones {
    Source source;
    Descriptor first, second;
};

// Both raw descriptors are observed; this does not run either pose selector.
inline Bones bones(const Reader& reader, Source source, std::uint64_t mesh) {
    if (source == Source::reference)
        return {source, split_descriptor(reader, mesh + 0x620, true),
                split_descriptor(reader, mesh + 0x630, true)};
    return {source, split_descriptor(reader, mesh + 0xa18, false),
            split_descriptor(reader, mesh + 0x620, false)};
}

struct Levels {
    Source source;
    Descriptor list;
    Field<std::uint64_t> single;
};

inline Levels levels(const Reader& reader, Source source, std::uint64_t world) {
    if (source == Source::reference)
        return {source, packed_descriptor(reader, world + 0x1c8), {}};
    return {source, {}, field<std::uint64_t>(reader, world + 0x30)};
}

// Fixture callers supply valid indices; count/capacity are not acceptance checks here.
// These observations neither scan nor publish a cache.
inline Field<std::uint64_t> item(const Reader& reader, const Descriptor& list,
                                 std::uint32_t index) {
    return through<std::uint64_t>(reader, list.data, std::uint64_t(index) * 8);
}

inline Descriptor actors(const Reader& reader, Source source,
                         const Field<std::uint64_t>& level) {
    if (!level.read || level.value == 0) return {};
    if (source == Source::reference)
        return packed_descriptor(reader, level.value + 0xa0);
    return split_descriptor(reader, level.value + 0xa0, false);
}

} // namespace reconstruction::paths
