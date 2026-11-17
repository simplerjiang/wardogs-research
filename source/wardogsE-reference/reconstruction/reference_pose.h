#pragma once

#include "reference_rules.h"
#include <vector>

namespace reconstruction::reference {

// Materialized-data model. It has no process, driver, memory-write or OS cache API.

struct Pose {
    std::array<Position, 21> points{};
    std::uint32_t mask = 0;
};

struct BoneMapping {
    std::array<int, 21> indices{};
    int kind = 0; // 1..4: recovered fixed tables; 5: caller-supplied dynamic mapping.
};

struct PoseSource {
    bool descriptor_accepted = false; // Result of the separately tested ab80 stage.
    std::vector<TransformRead> bones;
    BoneMapping mapping; // kind=0 asks for the fixed table matching bones.size().
    bool repair_range_readable = true; // Owned fixture's second range probe for 8dc4.
};

struct PoseCacheHint {
    bool key_matches = false; // Caller supplies pawn/mesh identity comparison.
    int descriptor = -1;
    int bone_count = 0;
    int mapping_kind = 0;
    int mode = 0;
    int score = 0;
};

struct PoseInput {
    std::array<PoseSource, 2> sources;
    TransformRead component;
    Position actor_position{};
    int preferred_descriptor = -1; // -1 (unavailable), 0 or 1.
    PoseCacheHint cache;
    bool state20 = false; // Existing snapshot +0x20 input, not a reconstructed game field.
};

enum class CacheAction { keep, clear, write };
struct PoseSelection {
    Pose pose;
    bool selected = false; // A candidate was selected; final mask may still be zero.
    bool postprocess_complete = false; // This owned-data pipeline ran, not live/reference parity.
    int descriptor = -1;
    int mode = -1;
    int pass = -1;
    int score = -1;
    int mapping_kind = 0;
    CacheAction cache_action = CacheAction::keep; // Early descriptor/component failure leaves the slot alone.
};

BoneMapping fixed_bone_mapping(std::size_t bone_count);
Position transform_point(const Transform&, const Position&); // 1158
double pose_distance(const Position&, const Position&); // 23c0 -> 0718
bool pose_shape(const Pose&); // a290: bits 1/6 and their distance
bool pose_limb(const Pose&, int first, bool dynamic_mapping, bool state20); // 5378; first=15 or 18
void trim_pose(Pose&, const Position& component_position, bool dynamic_mapping);
PoseSelection select_pose(const PoseInput&);

} // namespace reconstruction::reference
