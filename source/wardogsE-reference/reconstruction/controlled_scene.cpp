#include "controlled_scene.h"
#include "reference_projection.h"

#include <cmath>

namespace reconstruction {
namespace {
using namespace reference;
constexpr std::uint64_t pawn_address = 0x10000, root_address = 0x20000;
constexpr std::uint64_t mesh_address = 0x30000, bones_address = 0x50000;
constexpr std::uint64_t asset_address = 0x70000, names_address = 0x80000;
constexpr std::array<int,21> dynamic_indices{
    0,1,2,3,4,6,9,20,21,22,23,30,31,32,33,40,41,42,50,51,52};
constexpr std::array<int,21> changed_indices{
    0,1,2,3,4,6,9,24,25,26,27,34,35,36,37,44,45,46,54,55,56};
constexpr std::array<int,21> parent_joints{
    -1,0,1,2,3,4,5,5,7,8,9,5,11,12,13,1,15,16,1,18,19};

// All addresses identify owned byte arrays. Tokens are invented fixture IDs,
// not recovered game names. Training and target share the same fixture IDs.
struct Fixture {
    std::array<unsigned char,0x800> pawn{};
    std::array<unsigned char,0x248> root{};
    std::array<unsigned char,0x700> mesh{};
    std::array<unsigned char,0x310> asset{};
    std::array<std::vector<unsigned char>,2> bones;
    std::vector<unsigned char> names;
    std::uint64_t names_base = names_address;
    unsigned name_reads = 0;

    template<class T> static void put(unsigned char* destination, const T& value) {
        std::memcpy(destination,&value,sizeof(value));
    }
    static void pose(unsigned char* destination, const Transform& value) {
        std::memcpy(destination,value.rotation.data(),32);
        std::memcpy(destination+32,value.translation.data(),24);
        std::memcpy(destination+64,value.scale.data(),24);
    }
    Fixture(int count, const std::array<int,21>& mapping, unsigned sequence, ControlledCase scenario) {
        if (scenario == ControlledCase::mapping_pointer_changed || scenario == ControlledCase::invalid_mapping)
            names_base += 0x10000;
        put(pawn.data()+0x1c0,root_address);
        put(pawn.data()+0x3a0,mesh_address);
        put(mesh.data()+0x5a8,asset_address);
        put(asset.data()+0x300,names_base);
        put(asset.data()+0x308,count);
        put(asset.data()+0x30c,count);
        names.resize(std::size_t(count)*12);
        for (int i=0;i<count;++i) {
            put(names.data()+std::size_t(i)*12,std::uint64_t{0x9988000000000000ULL}+i);
            put(names.data()+std::size_t(i)*12+8,std::int32_t{i==0 ? -1 : 0});
        }
        for (std::size_t i=0;i<mapping.size();++i) {
            auto* record = names.data()+std::size_t(mapping[i])*12;
            put(record,std::uint64_t{0x1122000000000100ULL}+i);
            put(record+8,std::int32_t{parent_joints[i]<0 ? -1 : mapping[parent_joints[i]]});
        }
        if (scenario == ControlledCase::invalid_mapping)
            put(names.data()+std::size_t(mapping[7])*12,std::uint64_t{0x1122000000000106ULL});
        Transform component;
        component.translation = {500,0,90};
        pose(mesh.data()+0x210,component);
        put(root.data()+0x230,component.translation);
        const std::array<Position,21> joints{{
            {0,0,85},{0,0,90},{0,0,105},{0,0,120},{0,0,135},{0,0,150},{0,0,170},
            {0,-18,135},{0,-35,120},{0,-45,105},{0,-50,100},
            {0,18,135},{0,35,120},{0,45,105},{0,50,100},
            {0,-10,85},{0,-10,45},{0,-10,5},{0,10,85},{0,10,45},{0,10,5}
        }};
        for (std::size_t source=0;source<2;++source) {
            auto* descriptor = mesh.data()+0x620+source*16;
            put(descriptor,bones_address+source*0x10000);
            put(descriptor+8,count); put(descriptor+12,count);
            bones[source].resize(std::size_t(count)*96);
            for (int i=0;i<count;++i) pose(bones[source].data()+std::size_t(i)*96,Transform{});
            for (std::size_t i=0;i<joints.size();++i) {
                Transform value;
                value.translation = joints[i];
                value.translation[1] += double(source)*30.0 + 10.0*std::sin(sequence*0.03);
                value.translation[2] -= 90;
                if ((scenario == ControlledCase::missing_joints && (i==16 || i==19)) ||
                    (scenario == ControlledCase::invalid_pose && i!=0 && i!=1 && i!=6)) value.scale[0] = 0;
                pose(bones[source].data()+std::size_t(mapping[i])*96,value);
            }
            if (scenario == ControlledCase::rejected) {
                Transform bad; bad.scale[0] = 0;
                pose(bones[source].data(),bad);
            }
        }
        put(mesh.data()+0x688,std::int32_t{scenario == ControlledCase::alternate ? 1 : 0});
    }
    const unsigned char* locate(std::uint64_t address, std::size_t size) const {
        const std::array<std::uint64_t,9> bases{pawn_address,root_address,mesh_address,
            bones_address,bones_address+0x10000,asset_address,names_base,0xa0000,0xb0000};
        const std::array<const unsigned char*,9> arrays{pawn.data(),root.data(),mesh.data(),
            bones[0].data(),bones[1].data(),asset.data(),names.data(),bones[1].data(),mesh.data()};
        const std::array<std::size_t,9> lengths{pawn.size(),root.size(),mesh.size(),
            bones[0].size(),bones[1].size(),asset.size(),names.size(),bones[1].size(),mesh.size()};
        for (std::size_t i=0;i<bases.size();++i)
            if (address>=bases[i] && address-bases[i]<=lengths[i] &&
                size<=lengths[i]-std::size_t(address-bases[i])) return arrays[i]+std::size_t(address-bases[i]);
        return nullptr;
    }
    Reader reader() {
        return [this](std::uint64_t address, void* output, std::size_t size) {
            if (address==names_base && size==names.size()) ++name_reads;
            const auto* bytes = locate(address,size);
            if (!bytes) return false;
            std::memcpy(output,bytes,size); return true;
        };
    }
    RangeCheck range() const {
        return [this](std::uint64_t address, std::size_t size) { return locate(address,size)!=nullptr; };
    }
};
}

void ControlledSession::start(std::uint64_t selected_context) {
    if (poses_.observe_context(selected_context)) mappings_.clear_context();
    running_ = true;
}
void ControlledSession::stop() {
    running_ = false;
    poses_.reset_local_session();
    mappings_.reset();
}
ControlledScene ControlledSession::frame(unsigned width, unsigned height, unsigned sequence,
                                         ControlledCase scenario) {
    ControlledScene result;
    if (!running_) return result;
    if (!mappings_.signature_ready()) {
        Fixture training(356,fixed_bone_mapping(356).indices,0,ControlledCase::normal);
        mappings_.learn_from_pawn(training.reader(),training.range(),pawn_address);
        result.name_reads += training.name_reads;
    }
    const bool changed = scenario == ControlledCase::mapping_contents_changed ||
        scenario == ControlledCase::mapping_pointer_changed || scenario == ControlledCase::invalid_mapping;
    Fixture fixture(96,changed ? changed_indices : dynamic_indices,sequence,scenario);
    const auto raw_reader = fixture.reader();
    const auto range = fixture.range();
    bool collecting = false;
    unsigned marker_reads = 0, collections = 0;
    std::array<unsigned,2> head_reads{};
    // Deterministic interleavings in owned arrays, never writes to a process.
    const Reader reader = [&](std::uint64_t address, void* bytes, std::size_t size) {
        if (!collecting && address == mesh_address+0x688 && size == 4) ++marker_reads;
        const bool failed_snapshot = !collecting && address == mesh_address+0x620 && size == 8 &&
            ((scenario == ControlledCase::short_read_before && marker_reads == 1) ||
             (scenario == ControlledCase::short_read_after && marker_reads > 0 && marker_reads%2 == 0));
        bool failed_transform = false;
        for (std::size_t source=0;collecting && source<2;++source) {
            if (size != 96 || address != bones_address+source*0x10000+9*96) continue;
            ++head_reads[source];
            failed_transform = scenario == ControlledCase::transform_short_read ||
                (scenario == ControlledCase::pose_short_read && head_reads[source] == 2);
        }
        if (failed_snapshot || failed_transform) {
            raw_reader(address,bytes,failed_snapshot ? 4 : 40); // Prefix written, then failure.
            return false;
        }
        if (collecting && scenario == ControlledCase::content_changed_midread &&
            result.source_changes == 0 && address == bones_address+20*96 && size == 96) {
            double y = -50;
            Fixture::put(fixture.bones[0].data()+21*96+40,y);
            ++result.source_changes;
        }
        return raw_reader(address,bytes,size);
    };
    PoseInput input;
    const auto collect = [&]() {
        collecting = true;
        ++collections;
        head_reads = {};
        if (scenario == ControlledCase::address_aba) {
            Fixture::put(fixture.mesh.data()+0x620,std::uint64_t{0xa0000});
            ++result.source_changes;
        }
        // 76e4 rereads mesh inside each collection; the outer wrapper keeps its baseline.
        const auto mesh = paths::field<std::uint64_t>(reader,pawn_address+0x3a0).value;
        input = {};
        input.actor_position = choose_position(reader,pawn_address).value;
        input.component = read_transform(reader,mesh+0x210);
        input.preferred_descriptor = paths::field<std::int32_t>(reader,mesh+0x688).value;
        input.cache = poses_.lookup(pawn_address,mesh);
        for (std::size_t source=0;source<2;++source) {
            auto& destination = input.sources[source];
            destination.descriptor_accepted = bone_candidate(reader,range,mesh,std::uint32_t(0x620+source*16));
            if (!destination.descriptor_accepted) continue;
            const auto descriptor = paths::split_descriptor(reader,mesh+0x620+source*16,true);
            const auto mapping = mappings_.discover(reader,mesh,descriptor.count.value);
            destination.mapping = mapping.mapping;
            if (source==0) {
                result.mapping_hit = mapping.cache_hit;
                if (mapping.accepted) result.mapping_joint7 = mapping.mapping.indices[7];
            }
            destination.repair_range_readable = range(descriptor.data.value,std::size_t(descriptor.count.value)*96);
            for (std::int32_t i=0;i<descriptor.count.value;++i)
                destination.bones.push_back(read_transform(reader,descriptor.data.value+std::uint64_t(i)*96));
        }
        const auto selection = select_pose(input);
        switch (selection.cache_action) {
        case CacheAction::keep: break;
        case CacheAction::clear: poses_.clear_entity(pawn_address); break;
        case CacheAction::write:
            poses_.write(pawn_address,mesh,selection.descriptor,
                int(input.sources[selection.descriptor].bones.size()),selection.mapping_kind,selection.mode,selection.score);
            break;
        }
        if ((scenario == ControlledCase::source_changed_once && collections == 1) ||
            scenario == ControlledCase::source_changing) {
            Fixture::put(fixture.mesh.data()+0x620,collections%2 ? std::uint64_t{0xa0000} : bones_address);
            ++result.source_changes;
        }
        if (scenario == ControlledCase::mesh_changed && collections == 1) {
            Fixture::put(fixture.pawn.data()+0x3a0,std::uint64_t{0xb0000});
            ++result.source_changes;
        }
        if (scenario == ControlledCase::address_aba) {
            Fixture::put(fixture.mesh.data()+0x620,bones_address);
            ++result.source_changes;
        }
        collecting = false;
        return selection;
    };
    const auto checked = read_consistent_pose(reader,range,pawn_address,poses_,collect);
    const auto& selection = checked.selection;
    result.consistency = checked.status;
    result.attempts = checked.attempts;
    result.consistency_clears = checked.clears;
    result.name_reads += fixture.name_reads;
    Camera camera;
    camera.location = {0,0,90};
    camera.aspect_hint = float(width)/float(height);
    if (scenario == ControlledCase::behind_camera) camera.rotation_degrees[1] = 180;
    const auto segments = project_skeleton(camera,{float(width),float(height)},
        selection.pose.points,selection.pose.mask,selection.mapping_kind==5,input.state20);
    result.selected = checked.status == ConsistencyStatus::stable && selection.selected;
    result.postprocessed = checked.status == ConsistencyStatus::stable && selection.postprocess_complete;
    result.descriptor = selection.descriptor;
    result.mask = selection.pose.mask;
    result.segments = segments.size();
    result.mapping_kind = selection.mapping_kind;
    result.pose_pass = selection.pass;
    result.cache_action = checked.clears && checked.status != ConsistencyStatus::stable ?
        CacheAction::clear : selection.cache_action;
    for (const auto& segment : segments)
        result.drawing.line({segment.from.x,segment.from.y},{segment.to.x,segment.to.y},{0.2f,1,0.55f},2.5f);
    return result;
}
}
