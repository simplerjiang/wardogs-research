#include "reference_pose.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace reconstruction::reference;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr std::uint32_t bit(int i) { return std::uint32_t{1} << i; }

Pose human() {
    Pose pose;
    pose.mask = (1U << 21) - 1;
    pose.points = {{{0,0,90},{0,0,100},{0,0,115},{0,0,130},{0,0,145},{0,0,160},{0,0,180},
        {-20,0,160},{-35,0,145},{-40,0,125},{-40,0,115},
        {20,0,160},{35,0,145},{40,0,125},{40,0,115},
        {-10,0,90},{-10,0,60},{-10,0,20},{10,0,90},{10,0,60},{10,0,20}}};
    return pose;
}
PoseSource source(const Pose& pose, std::size_t count = 148, double x = 0) {
    PoseSource result;
    result.descriptor_accepted = true;
    result.bones.resize(count);
    const auto mapping = fixed_bone_mapping(count);
    for (auto& bone : result.bones) { bone.read = true; bone.valid = true; }
    for (int i = 0; i < 21; ++i) {
        auto& bone = result.bones[static_cast<std::size_t>(mapping.indices[i])];
        bone.value.translation = pose.points[i];
        bone.value.translation[0] += x;
        bone.valid = (pose.mask & bit(i)) != 0;
    }
    return result;
}
PoseInput baseline() {
    PoseInput input;
    input.sources[0] = source(human());
    input.component.read = input.component.valid = true;
    input.component.value.translation = {1000,0,0};
    input.actor_position = {1000,0,0};
    input.preferred_descriptor = 0;
    return input;
}

void mathematics() {
    require(fixed_bone_mapping(148).indices[6] == 9 && fixed_bone_mapping(148).kind == 1 &&
        fixed_bone_mapping(356).kind == 2 && fixed_bone_mapping(357).indices[20] == 304 &&
        fixed_bone_mapping(448).indices[20] == 396 && fixed_bone_mapping(149).kind == 0,
        "fixed mapping count or source table changed");
    Transform parent;
    parent.scale = {-2,3,4}; parent.translation = {10,20,30};
    require(transform_point(parent, {1,2,3}) == Position{8,26,42}, "scale and translation composition");
    parent = {};
    parent.rotation = {0,0,1,0};
    require(transform_point(parent, {2,3,4}) == Position{-2,-3,4}, "quaternion rotation composition");
    require(pose_distance({0,0,0},{28,0,0}) == 28, "distance did not use recovered helper");
    Pose pose = human();
    require(pose_shape(pose) && pose_limb(pose,15,false,false) && pose_limb(pose,18,false,false),
        "controlled human shape or limb rejected");
    pose.mask &= ~bit(6);
    require(!pose_shape(pose) && !pose_limb(pose,15,false,false), "missing shape bit accepted");
    pose = human(); pose.points[6][2] = 128;
    require(pose_shape(pose), "inclusive torso minimum");
    pose.points[6][2] = 127;
    require(!pose_shape(pose), "torso below minimum accepted");
    pose.points[6][2] = 245;
    require(pose_shape(pose), "inclusive torso maximum");
    pose.points[6][2] = 246;
    require(!pose_shape(pose), "torso above maximum accepted");
    std::puts("PASS pose math: fixed maps, signed scale, quaternion point transform and shape predicates");
}

void selection() {
    auto input = baseline();
    auto result = select_pose(input);
    require(result.selected && result.postprocess_complete && result.pass == 1 && result.mode == 0 &&
        result.descriptor == 0 && result.score == 52 && result.pose.mask == human().mask &&
        result.pose.points[1] == Position{1000,0,100} && result.cache_action == CacheAction::write,
        "preferred descriptor or mode tie selection");
    input.cache = {true,0,148,1,0,52};
    result = select_pose(input);
    require(result.pass == 0, "eligible cached pass ignored");
    input.cache.bone_count = 147;
    require(select_pose(input).pass == 1, "cache count mismatch accepted");
    input.cache = {true,0,148,1,0,52};
    const auto fixed = fixed_bone_mapping(148);
    input.sources[0].bones[fixed.indices[17]].valid = false;
    require(select_pose(input).pass == 1, "cached pass accepted score below 48");
    input = baseline();
    input.cache = {};
    input.preferred_descriptor = -1;
    result = select_pose(input);
    require(result.pass == 2 && result.cache_action == CacheAction::clear, "general pass/cache clear");
    input = baseline();
    input.sources[0] = source(human(),148,5000);
    input.component.value.translation = {5000,0,0};
    input.actor_position = {5000,0,0};
    result = select_pose(input);
    require(result.selected && result.mode == 1 && result.pose.points[1] == Position{5000,0,100},
        "identity/world-space mode not selected");
    input = baseline();
    input.sources[1] = source(human(),357);
    input.preferred_descriptor = -1;
    result = select_pose(input);
    require(result.descriptor == 1 && result.mapping_kind == 3, "mapping kind tie priority");
    input = baseline();
    Pose weak = human(); weak.mask &= ~(bit(15)|bit(16)|bit(17)|bit(18)|bit(19)|bit(20));
    input.sources[0] = source(weak);
    input.sources[1] = source(human(),357);
    result = select_pose(input);
    require(result.pass == 1 && result.descriptor == 0 && result.score == 8,
        "preferred accepted pass incorrectly continued to stronger general candidate");
    input.component.valid = false;
    require(!select_pose(input).selected && select_pose(input).cache_action == CacheAction::keep,
        "invalid component transform accepted or early return cleared cache");
    input = baseline(); input.sources[0].descriptor_accepted = false;
    require(!select_pose(input).selected && select_pose(input).cache_action == CacheAction::keep,
        "failed descriptor accepted or early return cleared cache");
    input = baseline(); input.sources[0].bones[9].valid = false;
    require(!select_pose(input).selected && select_pose(input).cache_action == CacheAction::clear,
        "failed pose accepted or selection failure retained cache");
    std::puts("PASS pose selection: cached/preferred/general passes, tie order, two transform modes and failures");
}

void repair_and_filters() {
    auto input = baseline();
    const auto mapping = fixed_bone_mapping(148);
    for (int i : {15,16,17,18,19,20}) input.sources[0].bones[mapping.indices[i]].valid = false;
    // Raw translations remain readable despite invalid complete transforms.
    auto result = select_pose(input);
    require(result.selected && result.postprocess_complete && (result.pose.mask & 0x1f8000) == 0x1f8000 &&
        result.pose.points[17] == Position{990,0,20} && result.pose.points[20] == Position{1010,0,20},
        "raw translation rescan did not restore both legs");
    input.sources[0].repair_range_readable = false;
    result = select_pose(input);
    require((result.pose.mask & 0x1f8000) == 0, "preferred source synthesized when fallback is disabled");
    input.preferred_descriptor = -1;
    result = select_pose(input);
    require((result.pose.mask & 0x1f8000) == 0x1f8000 &&
        result.pose.points[17] == Position{986,0,20} && result.pose.points[20] == Position{1014,0,20},
        "fallback leg interpolation constants");
    input = baseline();
    for (int i : {18,19,20}) input.sources[0].bones[mapping.indices[i]].valid = false;
    input.sources[0].repair_range_readable = false;
    result = select_pose(input);
    require(result.pose.points[20] == Position{1010,0,20} && (result.pose.mask & bit(20)),
        "one-sided leg reflection");
    input = baseline();
    input.sources[0].bones[mapping.indices[3]].value.translation = {100,0,130};
    input.sources[0].bones[mapping.indices[9]].value.translation = {-150,0,125};
    result = select_pose(input);
    require(result.selected && !(result.pose.mask & bit(3)) && !(result.pose.mask & bit(9)),
        "torso/arm postprocess did not clear outliers");
    input = baseline(); input.state20 = true;
    input.sources[0].bones[mapping.indices[17]].value.translation = {-10,0,140};
    result = select_pose(input);
    require(!(result.pose.mask & bit(17)), "final feet cleanup did not reject foot above root+35");
    Pose pose = human();
    pose.points[2] = {108,0,100};
    pose.points[3] = {std::nextafter(108.0,std::numeric_limits<double>::infinity()),0,100};
    trim_pose(pose,{},false);
    require((pose.mask & bit(2)) && !(pose.mask & bit(3)), "radius inclusive endpoint");
    pose = human(); pose.points[3] = {170,0,100};
    trim_pose(pose,{},true);
    require(pose.mask & bit(3), "dynamic-mapping radius170 endpoint");
    pose = human(); pose.mask = bit(0)|bit(1)|bit(2)|bit(3)|bit(6);
    trim_pose(pose,{},false);
    require(pose.mask == 0, "fewer-than-six mask not cleared");
    std::puts("PASS pose postprocess: raw rescan, synthesis, reflection, chain filtering and radius clipping");
}
} // namespace

int main() {
    try {
        mathematics(); selection(); repair_and_filters();
        std::puts("PASS controlled pose pipeline; dynamic mapping discovery, live state and original runtime remain NOT_TESTED");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what());
        return 1;
    }
}
