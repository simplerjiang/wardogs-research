#pragma once

#include "render2d.h"
#include "reference_cache.h"
#include "reference_mapping.h"
#include "reference_consistency.h"

namespace reconstruction {
enum class ControlledCase {
    normal, alternate, behind_camera, missing_joints, rejected,
    mapping_contents_changed, mapping_pointer_changed, invalid_mapping, invalid_pose,
    short_read_before, short_read_after, transform_short_read, pose_short_read, source_changed_once,
    source_changing, mesh_changed, address_aba, content_changed_midread
};
struct ControlledScene {
    DrawList drawing;
    bool selected = false, postprocessed = false;
    int descriptor = -1;
    unsigned mask = 0;
    std::size_t segments = 0;
    int mapping_kind = 0, mapping_joint7 = -1, pose_pass = -1;
    unsigned name_reads = 0;
    bool mapping_hit = false;
    reference::CacheAction cache_action = reference::CacheAction::keep;
    reference::ConsistencyStatus consistency = reference::ConsistencyStatus::no_mesh;
    unsigned attempts = 0, consistency_clears = 0, source_changes = 0;
};
// Owns only controlled-data caches. stop/start is a local session contract,
// not a reconstruction of the reference DLL's shutdown/unload protocol.
class ControlledSession {
public:
    void start(std::uint64_t selected_context);
    void stop();
    ControlledScene frame(unsigned width, unsigned height, unsigned sequence,
                          ControlledCase scenario = ControlledCase::normal);
    bool running() const { return running_; }
    const reference::MappingCache& mappings() const { return mappings_; }
    const reference::PoseCache& poses() const { return poses_; }
private:
    bool running_ = false;
    reference::MappingCache mappings_;
    reference::PoseCache poses_;
};
}
