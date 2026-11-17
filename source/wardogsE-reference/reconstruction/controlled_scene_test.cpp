#include "controlled_scene.h"

#include <cstdio>
#include <stdexcept>

using namespace reconstruction;
using reconstruction::reference::CacheAction;
using reconstruction::reference::ConsistencyStatus;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void consistency_cases() {
    struct Case {
        ControlledCase scenario;
        ConsistencyStatus status;
        unsigned attempts, clears, mutations;
        bool visible, cached;
    };
    const Case cases[]{
        {ControlledCase::short_read_before,ConsistencyStatus::initial_read_failed,0,0,0,false,true},
        {ControlledCase::short_read_after,ConsistencyStatus::unstable,2,2,0,false,false},
        {ControlledCase::transform_short_read,ConsistencyStatus::stable,1,0,0,false,true},
        {ControlledCase::pose_short_read,ConsistencyStatus::stable,1,0,0,false,false},
        {ControlledCase::source_changed_once,ConsistencyStatus::stable,2,1,1,true,true},
        {ControlledCase::source_changing,ConsistencyStatus::unstable,2,2,2,false,false},
        {ControlledCase::mesh_changed,ConsistencyStatus::unstable,2,2,1,false,false},
        {ControlledCase::address_aba,ConsistencyStatus::stable,1,0,2,true,true},
        {ControlledCase::content_changed_midread,ConsistencyStatus::stable,1,0,1,true,true}
    };
    for (const auto& item : cases) {
        ControlledSession session;
        session.start(1);
        const auto before = session.frame(960,540,0);
        const auto frame = session.frame(960,540,0,item.scenario);
        require(frame.consistency == item.status && frame.attempts == item.attempts &&
                frame.consistency_clears == item.clears && frame.source_changes == item.mutations,
                "Consistency scenario did not reach the expected read/clear sequence");
        require(frame.selected == item.visible && frame.drawing.items.empty() != item.visible &&
                session.poses().lookup(0x10000,0x30000).key_matches == item.cached,
                "Consistency result publication or pose cache state differs");
        require(session.mappings().stats().entries == 1 && frame.name_reads == 0,
                "Consistency failure cleared or reread the independent mapping cache");
        if (item.scenario == ControlledCase::source_changed_once || item.scenario == ControlledCase::address_aba ||
            item.scenario == ControlledCase::content_changed_midread) {
            bool different = before.drawing.items.size() != frame.drawing.items.size();
            for (std::size_t i=0;!different && i<frame.drawing.items.size();++i) {
                const auto& a = before.drawing.items[i]; const auto& b = frame.drawing.items[i];
                different = a.from.x != b.from.x || a.from.y != b.from.y || a.to.x != b.to.x || a.to.y != b.to.y;
            }
            require(different, "Owned source mutation did not affect the collected drawing");
        }
        require(session.frame(960,540,0).selected, "Stable frame did not recover after a fault fixture");
    }
    std::puts("PASS controlled consistency: partial reads, bounded retry, mesh change, independent caches, ABA/content detection limits");
}
int main() {
    try {
        ControlledSession session;
        require(session.frame(960,540,0).drawing.items.empty(), "Stopped session drew a frame");
        session.start(1);
        auto frame = session.frame(960,540,0);
        require(frame.selected && frame.mapping_kind==5 && frame.mapping_joint7==20 &&
                frame.pose_pass==1 && frame.name_reads==2 && !frame.mapping_hit && frame.segments==19,
                "Cold learning, dynamic discovery, pose and projection integration");
        require(session.mappings().stats().entries==1 && session.poses().lookup(0x10000,0x30000).key_matches,
                "Successful frame did not populate both caches");
        session.start(1);
        frame = session.frame(960,540,0);
        require(frame.selected && frame.pose_pass==0 && frame.mapping_hit && frame.name_reads==0,
                "Warm frame reread names or failed to use cached pose");
        frame = session.frame(960,540,0,ControlledCase::mapping_contents_changed);
        require(frame.mapping_hit && frame.mapping_joint7==20 && frame.name_reads==0 &&
                frame.consistency==ConsistencyStatus::stable && frame.consistency_clears==0,
                "Same-key content change unexpectedly refreshed the reference cache");
        frame = session.frame(960,540,0,ControlledCase::mapping_pointer_changed);
        require(frame.selected && !frame.mapping_hit && frame.mapping_joint7==24 &&
                frame.name_reads==1 && frame.segments==19 && session.mappings().stats().entries==2,
                "Changed array key did not discover the new joint layout");
        session.start(2);
        require(session.mappings().signature_ready() && session.mappings().stats().entries==0 &&
                !session.poses().lookup(0x10000,0x30000).key_matches, "Context change cache lifetime");
        frame = session.frame(960,540,0,ControlledCase::mapping_contents_changed);
        require(frame.selected && frame.name_reads==1 && frame.mapping_joint7==24 && frame.pose_pass==1,
                "Context clear did not refresh same-address records using retained signature");
        session.stop(); session.stop();
        require(!session.running() && !session.mappings().signature_ready() &&
                session.mappings().stats().entries==0 && !session.poses().lookup(0x10000,0x30000).key_matches &&
                session.frame(960,540,0).drawing.items.empty(), "Local stop left state or drawing");
        session.start(2);
        frame = session.frame(960,540,0);
        require(frame.selected && frame.pose_pass==1 && frame.name_reads==2,
                "Local restart failed to relearn or reused a stopped session");
        frame = session.frame(960,540,0,ControlledCase::rejected);
        require(!frame.selected && frame.drawing.items.empty() && frame.cache_action==CacheAction::keep &&
                session.poses().lookup(0x10000,0x30000).key_matches, "Early reject incorrectly cleared pose cache");
        frame = session.frame(960,540,0,ControlledCase::invalid_pose);
        require(!frame.selected && frame.drawing.items.empty() && frame.cache_action==CacheAction::clear &&
                !session.poses().lookup(0x10000,0x30000).key_matches, "Selection failure retained pose cache");
        frame = session.frame(960,540,0,ControlledCase::invalid_mapping);
        require(!frame.selected && frame.drawing.items.empty() && frame.name_reads==2 &&
                session.mappings().stats().entries==1, "Invalid mapping published a pose or replaced cached mapping");
        frame = session.frame(960,540,0);
        require(frame.selected && frame.mapping_hit && frame.name_reads==0 && frame.pose_pass==1,
                "Restoring the old mapping key did not reuse the surviving entry after pose clear");
        std::puts("PASS controlled dynamic integration: learned tokens, shuffled indices, both caches, same-key staleness, changed key/context, stop/restart, early vs late failure");
        consistency_cases();
        std::puts("NOT_TESTED live game, source races, reference DLL stop/unload equivalence");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
