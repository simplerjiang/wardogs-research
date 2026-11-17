#include "reference_pose.h"

#include <algorithm>
#include <cmath>

namespace reconstruction::reference {
namespace {
constexpr std::uint32_t bit(int i) { return std::uint32_t{1} << i; }
bool numbers(const Position& p) { return scalar(p[0]) && scalar(p[1]) && scalar(p[2]); }
bool has(const Pose& pose, int i) { return (pose.mask & bit(i)) != 0; }
int point_count(const Pose& pose) {
    int count = 0;
    for (int i = 0; i < 21; ++i) if (has(pose, i)) ++count;
    return count;
}
double horizontal(const Position& a, const Position& b) {
    const double x = a[0] - b[0], y = a[1] - b[1];
    return distance_root(x * x + y * y);
}
// ae58: whether the component translation replaces the actor's anchor.
bool world_position(const Position& p) {
    return numbers(p) && std::fabs(p[0]) <= 8000000 && std::fabs(p[1]) <= 8000000 &&
           std::fabs(p[2]) <= 800000 &&
           (std::fabs(p[0]) >= 50 || std::fabs(p[1]) >= 50 || std::fabs(p[2]) >= 50);
}
// 1e70; indices are internal constants, not an external index API.
bool bent_pair(const Pose& pose, int a, int b) {
    if (!has(pose, 1) || !has(pose, a) || !has(pose, b)) return false;
    const auto& root = pose.points[1];
    const auto& end = pose.points[b];
    if (!numbers(root) || !numbers(end)) return false;
    const double segment = pose_distance(pose.points[a], end);
    const double dx = end[0] - root[0], dy = end[1] - root[1];
    const double horizontal_squared = dy * dy + dx * dx;
    const double z = end[2] - root[2];
    return pose_distance(root, pose.points[a]) <= 36 && segment >= 15 && segment <= 80 &&
           horizontal_squared >= 400 && horizontal_squared <= 8100 && z >= -30 && z <= 35;
}
bool special_state(const Pose& pose, bool state20) {
    return state20 || ((pose.mask & 0x42) == 0x42 &&
        pose.points[6][2] - pose.points[1][2] >= 40 &&
        bent_pair(pose, 15, 16) && bent_pair(pose, 18, 19));
}
double segment_distance(const Position& a, const Position& b, const Position& p) {
    const double x = b[0] - a[0], y = b[1] - a[1], z = b[2] - a[2];
    const double length = (y * y + x * x) + z * z;
    if (length < 1) return pose_distance(a, p);
    const double projection = (((p[1] - a[1]) * y + (p[0] - a[0]) * x) +
                               (p[2] - a[2]) * z) / length;
    const double t = std::min(1.0, std::max(0.0, projection));
    return pose_distance(Position{a[0] + t * x, a[1] + t * y, a[2] + t * z}, p);
}
// 9bd8: removes outlying torso points and overlong arm segments.
void clean_chains(Pose& pose) {
    if ((pose.mask & 0x42) != 0x42) return;
    for (int i = 2; i <= 5; ++i)
        if (has(pose, i) && segment_distance(pose.points[1], pose.points[6], pose.points[i]) > 26)
            pose.mask &= ~bit(i);
    for (int start : {7, 11}) {
        int previous = has(pose, 4) ? 4 : 6;
        for (int i = start; i < start + 4; ++i) {
            if (!has(pose, i)) continue;
            if (pose_distance(pose.points[previous], pose.points[i]) > 52) pose.mask &= ~bit(i);
            else previous = i;
        }
    }
}
// 23f8 runs after radius clipping, and does not recount all surviving points.
void clean_feet(Pose& pose, bool dynamic) {
    if (dynamic || !has(pose, 1)) return;
    for (int i : {17, 20}) {
        if (!has(pose, i)) continue;
        const double z = pose.points[i][2] - pose.points[1][2];
        const double h = horizontal(pose.points[i], pose.points[1]);
        const double d = distance_root(z * z + h * h);
        if (z > 35 || h > 200 || d > 210 || d < 8) pose.mask &= ~bit(i);
    }
}

// 9fec: the shoulder-to-shoulder axis, with a minimum observed separation.
bool side_axis(const Pose& pose, Position& axis) {
    if ((pose.mask & 0x880) != 0x880) return false;
    for (int i = 0; i < 3; ++i) axis[i] = pose.points[11][i] - pose.points[7][i];
    const double length = distance_root((axis[1] * axis[1] + axis[0] * axis[0]) + axis[2] * axis[2]);
    if (!(length >= 8)) return false;
    for (double& n : axis) n /= length;
    return true;
}
// 650c: synthesized joint positions at the recovered 0.46/0.72 fractions.
void interpolate_leg(Pose& pose, int first, const Position& end) {
    const auto root = pose.points[1];
    int index = first;
    for (double fraction : {0.46, 0.72}) {
        for (int i = 0; i < 3; ++i) pose.points[index][i] = (end[i] - root[i]) * fraction + root[i];
        pose.mask |= bit(index++);
    }
    pose.points[first + 2] = end;
    pose.mask |= bit(first + 2);
}
void mirror_leg(Pose& pose, int from, int to) {
    Position axis{};
    const bool directional = side_axis(pose, axis);
    const auto root = pose.points[1];
    pose.mask &= ~(bit(to) | bit(to + 1) | bit(to + 2));
    for (int i = 0; i < 3; ++i) {
        if (!has(pose, from + i)) continue;
        Position p = pose.points[from + i];
        const double x = p[0] - root[0], y = p[1] - root[1], z = p[2] - root[2];
        if (directional) {
            double projection = z * axis[2] + (axis[1] * y + axis[0] * x);
            projection += projection;
            for (int j = 0; j < 3; ++j) p[j] -= projection * axis[j];
        } else {
            p[0] = root[0] - x;
            p[1] = root[1] - y;
        }
        pose.points[to + i] = p;
        pose.mask |= bit(to + i);
    }
}
bool foot_candidate(const Position& root, const Position& foot, bool flat) {
    const double x = foot[0] - root[0], y = foot[1] - root[1], z = foot[2] - root[2];
    const double h = distance_root(y * y + x * x);
    const double distance = distance_root(z * z + h * h);
    if (!(distance > 30)) return false;
    if (flat) return h >= 28 && h <= 120 && z >= -35 && z <= 20;
    return z <= -40 && h <= 55 && distance >= 45 && distance <= 140;
}
void repair_legs(Pose& pose, const PoseSource& source, const Transform& parent,
                 bool dynamic, bool state20, bool allow_fallback) {
    if (dynamic || special_state(pose, state20)) return;
    bool left = pose_limb(pose, 15, false, state20);
    bool right = pose_limb(pose, 18, false, state20);
    if ((left && right) || !has(pose, 1)) return;
    const bool flat = has(pose, 6) && pose.points[6][2] - pose.points[1][2] < 40;
    Position axis{}, negative{}, positive{};
    const bool directional = side_axis(pose, axis);
    bool have_negative = false, have_positive = false;
    if (source.bones.size() > 20 && source.repair_range_readable) {
        const auto count = std::min<std::size_t>(640, source.bones.size());
        for (std::size_t i = 0; i < count; ++i) {
            // The owned input represents readable whole records; live partial
            // translation-only reads are outside this materialized-data API.
            if (!source.bones[i].read) break;
            const auto& raw = source.bones[i].value.translation;
            if (!numbers(raw)) continue;
            const Position p = transform_point(parent, raw);
            if (!numbers(p) || !foot_candidate(pose.points[1], p, flat) || !directional) continue;
            const double dot = ((p[1] - pose.points[1][1]) * axis[1] +
                                (p[0] - pose.points[1][0]) * axis[0]) +
                                (p[2] - pose.points[1][2]) * axis[2];
            if (dot > 4 && (!have_positive || p[2] < positive[2])) {
                positive = p; have_positive = true;
            } else if (dot < -4 && (!have_negative || p[2] < negative[2])) {
                negative = p; have_negative = true;
            }
        }
    }
    if (!left && have_negative) {
        pose.mask &= ~(bit(15) | bit(16) | bit(17));
        interpolate_leg(pose, 15, negative);
        left = pose_limb(pose, 15, false, state20);
    }
    if (!right && have_positive) {
        pose.mask &= ~(bit(18) | bit(19) | bit(20));
        interpolate_leg(pose, 18, positive);
        right = pose_limb(pose, 18, false, state20);
    }
    if (left && !right) mirror_leg(pose, 15, 18);
    else if (right && !left) mirror_leg(pose, 18, 15);
    else if (!left && !right && allow_fallback) {
        if (!side_axis(pose, axis)) axis = {1, 0, 0};
        pose.mask &= ~(bit(15) | bit(16) | bit(17) | bit(18) | bit(19) | bit(20));
        const auto& root = pose.points[1];
        interpolate_leg(pose, 15, Position{root[0] - axis[0] * 14, root[1] - axis[1] * 14, root[2] - 80});
        interpolate_leg(pose, 18, Position{root[0] + axis[0] * 14, root[1] + axis[1] * 14, root[2] - 80});
    }
}
} // namespace

BoneMapping fixed_bone_mapping(std::size_t count) {
    switch (count) {
    case 148: return {{{0,1,2,3,5,6,9,64,65,66,70,10,11,12,16,122,123,124,145,146,147}}, 1};
    case 356: return {{{0,1,2,3,4,6,9,64,65,66,76,10,11,12,22,122,123,124,145,146,147}}, 2};
    case 357: return {{{0,1,2,3,4,6,9,66,67,68,78,124,125,126,136,272,273,274,302,303,304}}, 3};
    case 448: return {{{0,1,2,3,4,6,9,216,217,218,228,158,159,160,170,364,365,366,394,395,396}}, 4};
    default: return {};
    }
}

Position transform_point(const Transform& t, const Position& p) {
    const double x = t.scale[0] * p[0], y = t.scale[1] * p[1], z = t.scale[2] * p[2];
    const auto& q = t.rotation;
    double a = q[1] * z - q[2] * y; a += a;
    double b = q[2] * x - q[0] * z; b += b;
    double c = q[0] * y - q[1] * x; c += c;
    // The parentheses retain the scalar assembly's addition order.
    return {((c * q[1] - b * q[2]) + (a * q[3] + x)) + t.translation[0],
            ((a * q[2] - c * q[0]) + (b * q[3] + y)) + t.translation[1],
            ((b * q[0] - a * q[1]) + (c * q[3] + z)) + t.translation[2]};
}

double pose_distance(const Position& a, const Position& b) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return distance_root((y * y + x * x) + z * z);
}

bool pose_shape(const Pose& pose) {
    if ((pose.mask & 0x42) != 0x42 || !numbers(pose.points[1]) || !numbers(pose.points[6])) return false;
    const double distance = pose_distance(pose.points[1], pose.points[6]);
    return distance >= 28 && distance <= 145;
}

bool pose_limb(const Pose& pose, int first, bool dynamic, bool state20) {
    const int middle = first + 1, end = first + 2;
    const auto mask = 0x42U | bit(first) | bit(middle) | bit(end);
    if ((pose.mask & mask) != mask) return false;
    for (int i : {1, 6, first, middle, end}) if (!numbers(pose.points[i])) return false;
    const auto& root = pose.points[1];
    const auto& tip = pose.points[end];
    const double a = pose_distance(pose.points[first], pose.points[middle]);
    const double b = pose_distance(pose.points[middle], tip);
    if (pose_distance(root, pose.points[first]) > 36 || a < 6 || a > 80 || b < 6 || b > 80) return false;
    const double x = root[0] - tip[0], y = root[1] - tip[1], z = tip[2] - root[2];
    const double distance = distance_root((x * x + y * y) + z * z);
    if (dynamic) return distance <= 170;
    const double h = distance_root(x * x + y * y);
    if (special_state(pose, state20)) return z >= -130 && z <= 35 && h <= 145 && distance >= 15 && distance <= 150;
    if (pose.points[6][2] - root[2] < 40) return h >= 28 && h <= 130 && z >= -35 && z <= 20 && distance >= 28;
    return z <= -40 && h <= 60 && distance >= 45 && distance <= 140;
}

void trim_pose(Pose& pose, const Position& component, bool dynamic) {
    const Position anchor = has(pose, 1) ? pose.points[1] : component;
    const double radius = dynamic ? 170 : 108;
    int remaining = 0;
    for (int i = 0; i < 21; ++i) {
        if (!has(pose, i)) continue;
        const double x = pose.points[i][0] - anchor[0], y = pose.points[i][1] - anchor[1];
        const double z = pose.points[i][2] - anchor[2];
        if ((y * y + x * x) + z * z <= radius * radius) ++remaining;
        else pose.mask &= ~bit(i);
    }
    if (remaining < 6) pose.mask = 0;
}

PoseSelection select_pose(const PoseInput& input) {
    PoseSelection result;
    if (!input.component.valid ||
        (!input.sources[0].descriptor_accepted && !input.sources[1].descriptor_accepted)) return result;
    result.cache_action = CacheAction::clear;
    const auto& cache = input.cache;
    const auto available = [&input](int slot) {
        return slot >= 0 && slot < 2 && input.sources[slot].descriptor_accepted;
    };
    const int preferred = available(input.preferred_descriptor) ? input.preferred_descriptor : -1;
    const bool cached = cache.key_matches && cache.score >= 8 && available(cache.descriptor) &&
        input.sources[cache.descriptor].bones.size() == static_cast<std::size_t>(cache.bone_count) &&
        (input.preferred_descriptor < 0 || cache.descriptor == preferred);
    const Position anchor = world_position(input.component.value.translation)
        ? input.component.value.translation : input.actor_position;
    int best_tie = -1;
    for (int pass = 0; pass < 3 && !result.selected; ++pass) {
        if ((pass == 0 && !cached) || (pass == 1 && preferred < 0)) continue;
        for (int slot = 0; slot < 2; ++slot) {
            if (!available(slot) || (pass == 1 && slot != preferred)) continue;
            const auto& source = input.sources[slot];
            const BoneMapping mapping = source.mapping.kind ? source.mapping : fixed_bone_mapping(source.bones.size());
            if (mapping.kind == 0) continue;
            for (int mode = 0; mode < 2; ++mode) {
                if (pass == 0 && (slot != cache.descriptor || mode != cache.mode || mapping.kind != cache.mapping_kind)) continue;
                const Transform parent = mode == 0 ? input.component.value : Transform{};
                Pose candidate;
                for (int i = 0; i < 21; ++i) {
                    const int index = mapping.indices[i];
                    if (index < 0 || static_cast<std::size_t>(index) >= source.bones.size()) continue;
                    const auto& bone = source.bones[index];
                    if (!bone.valid) continue;
                    const Position p = transform_point(parent, bone.value.translation);
                    if (numbers(p) && pose_distance(p, anchor) <= 2500) {
                        candidate.points[i] = p;
                        candidate.mask |= bit(i);
                    }
                }
                if (point_count(candidate) < 6 || !pose_shape(candidate)) continue;
                const bool dynamic = mapping.kind == 5;
                const bool left = pose_limb(candidate, 15, dynamic, input.state20);
                const bool right = pose_limb(candidate, 18, dynamic, input.state20);
                int score = 8 + (left ? 20 : 0) + (right ? 20 : 0);
                if (left && right) {
                    const double feet = pose_distance(candidate.points[17], candidate.points[20]);
                    score += feet < 6 || feet > 110 ? -12 : 4;
                }
                if (score < 8 || (pass == 0 && score < 48)) continue;
                const int tie = mapping.kind + (mode == 0 ? 2 : 0) + (slot == preferred ? 3 : 0);
                if (result.selected && (score < result.score || (score == result.score && tie <= best_tie))) continue;
                result.pose = candidate;
                result.selected = true;
                result.descriptor = slot;
                result.mode = mode;
                result.pass = pass;
                result.score = score;
                result.mapping_kind = mapping.kind;
                best_tie = tie;
            }
        }
    }
    if (!result.selected) return result;
    result.cache_action = result.descriptor == preferred ? CacheAction::write : CacheAction::clear;
    const bool dynamic = result.mapping_kind == 5;
    const Transform parent = result.mode == 0 ? input.component.value : Transform{};
    repair_legs(result.pose, input.sources[result.descriptor], parent,
        dynamic, input.state20, result.descriptor != preferred);
    clean_chains(result.pose);
    trim_pose(result.pose, input.component.value.translation, dynamic);
    clean_feet(result.pose, dynamic);
    if (!pose_shape(result.pose)) result.pose.mask = 0;
    result.postprocess_complete = true;
    return result;
}

} // namespace reconstruction::reference
