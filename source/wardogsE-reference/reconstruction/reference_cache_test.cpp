#include "reference_cache.h"

#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace reconstruction::reference;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void put(PoseCache& cache, std::uint64_t pawn, std::uint64_t mesh = 400) {
    cache.write(pawn, mesh, 1, 148, 1, 0, 52);
}
void storage_rules() {
    require(sizeof(PoseCacheEntry) == 40 && offsetof(PoseCacheEntry, score) == 32,
            "Observed slot layout");
    PoseCache cache;
    require(!cache.lookup(0, 0).key_matches, "Zeroed slots are not hits");
    cache.write(1, 2, 0, 148, 1, 0, 7);
    require(cache.entries()[0].pawn == 0, "Score seven does not insert");
    cache.write(1, 2, 0, 148, 1, 0, 8);
    require(cache.lookup(1, 2).key_matches && cache.lookup(1, 2).score == 8,
            "Score eight inserts");
    cache.write(1, 9, 1, 356, 2, 1, std::numeric_limits<int>::min());
    require(cache.lookup(1, 2).key_matches && !cache.lookup(1, 9).key_matches,
            "Low score preserves an existing entry");
    put(cache, 2);
    cache.clear_entity(1);
    put(cache, 2, 401);
    require(cache.entries()[0].pawn == 0 && cache.entries()[1].mesh == 401,
            "Existing pawn takes priority over an earlier empty slot");
    require(!cache.lookup(2, 400).key_matches && cache.lookup(2, 401).key_matches,
            "Write identity is pawn-only; lookup also needs mesh");
    cache.write(3, 402, -9, -17, 77, 99, 8);
    const auto unchecked = cache.lookup(3, 402);
    require(unchecked.key_matches && unchecked.descriptor == -9 && unchecked.bone_count == -17 &&
            unchecked.mapping_kind == 77 && unchecked.mode == 99,
            "Storage must not invent field gates delegated to pose selection");
    cache.clear_entity(3);
    const auto& cleared = cache.entries()[0];
    require(cleared.pawn == 0 && cleared.mesh == 0 && cleared.score == 0 &&
            cleared.descriptor == -9 && cleared.bone_count == -17 &&
            cleared.mapping_kind == 77 && cleared.mode == 99,
            "Entity clear preserves non-key metadata");
    cache.clear_entity(999);
    require(cache.lookup(2, 401).key_matches, "Absent clear leaves other entities");

    PoseCache zero;
    zero.write(0, 0, 0, 148, 1, 0, 8);
    require(zero.lookup(0, 0).key_matches, "Do not add an unobserved nonzero-key gate");
    zero.clear_entity(0);
    require(!zero.lookup(0, 0).key_matches, "Clear uses the supplied key literally");
}
void replacement_rules() {
    PoseCache cache;
    for (std::size_t i = 0; i < PoseCache::capacity; ++i) put(cache, i + 1);
    require(cache.entries().back().pawn == 192 && cache.replacement_cursor() == 0,
            "All empty slots fill before rotation advances");
    put(cache, 193);
    require(!cache.lookup(1, 400).key_matches && cache.entries()[0].pawn == 193 &&
            cache.replacement_cursor() == 1, "Full table replaces slot zero");
    put(cache, 192, 500);
    require(cache.replacement_cursor() == 1 && cache.lookup(192, 500).key_matches,
            "Existing updates do not advance rotation");
    cache.clear_entity(50);
    put(cache, 194);
    require(cache.entries()[49].pawn == 194 && cache.replacement_cursor() == 1,
            "A new hole wins over the replacement cursor");
    (void)cache.lookup(2, 400);
    put(cache, 195);
    require(!cache.lookup(2, 400).key_matches && cache.entries()[1].pawn == 195 &&
            cache.replacement_cursor() == 2, "Lookup does not refresh replacement order");
    for (std::size_t i = 2; i < PoseCache::capacity; ++i) put(cache, 1000 + i);
    require(cache.replacement_cursor() == 192 && cache.entries()[191].pawn == 1191,
            "Rotation reaches the last slot");
    put(cache, 2000);
    require(cache.entries()[0].pawn == 2000 && cache.replacement_cursor() == 193,
            "Replacement modulo wraps to zero");
    require(cache.observe_context(600), "Changed context observed");
    for (const auto& entry : cache.entries())
        require(entry.pawn == 0 && entry.mesh == 0 && entry.score == 0 && entry.descriptor == 0 &&
                entry.bone_count == 0 && entry.mapping_kind == 0 && entry.mode == 0 && entry.reserved == 0,
                "Context changes clear the whole table");
    require(cache.replacement_cursor() == 0, "Context changes reset rotation");
    put(cache, 7);
    require(!cache.observe_context(600) && cache.lookup(7, 400).key_matches,
            "Same context does not expire entries");
    require(cache.observe_context(0) && !cache.lookup(7, 400).key_matches,
            "A changed context of zero also clears");
    put(cache, 8);
    cache.reset_local_session();
    require(cache.context() == 0 && cache.replacement_cursor() == 0 &&
            !cache.lookup(8, 400).key_matches, "Explicit local session reset");
    cache.reset_local_session();
    put(cache, 8);
    require(cache.lookup(8, 400).key_matches, "Local model can restart after repeated reset");
}

PoseInput pose_fixture() {
    PoseInput input;
    input.component.read = input.component.valid = true;
    input.component.value.translation = {500, 0, 0};
    input.actor_position = {500, 0, 0};
    input.preferred_descriptor = 0;
    const std::array<Position, 21> points{{
        {0,0,90},{0,0,100},{0,0,115},{0,0,130},{0,0,145},{0,0,160},{0,0,180},
        {0,-20,160},{0,-35,145},{0,-40,125},{0,-40,115},
        {0,20,160},{0,35,145},{0,40,125},{0,40,115},
        {0,-10,90},{0,-10,60},{0,-10,20},{0,10,90},{0,10,60},{0,10,20}
    }};
    for (auto& source : input.sources) {
        source.descriptor_accepted = true;
        source.bones.resize(148);
        const auto mapping = fixed_bone_mapping(148);
        for (auto& bone : source.bones) bone.read = bone.valid = true;
        for (std::size_t i = 0; i < points.size(); ++i)
            source.bones[static_cast<std::size_t>(mapping.indices[i])].value.translation = points[i];
    }
    return input;
}
void selection_integration() {
    PoseCache cache;
    auto input = pose_fixture();
    auto selected = select_pose(input);
    require(selected.selected && selected.pass == 1 && selected.cache_action == CacheAction::write,
            "Cold selection uses the preferred descriptor");
    cache.write(100, 200, selected.descriptor, 148, selected.mapping_kind, selected.mode, selected.score);
    input.cache = cache.lookup(100, 200);
    selected = select_pose(input);
    require(selected.selected && selected.pass == 0, "Stored hint reaches the cached pass");
    cache.write(100, 200, 0, 147, 1, 0, 52);
    input.cache = cache.lookup(100, 200);
    selected = select_pose(input);
    require(selected.selected && selected.pass == 1, "Count mismatch bypasses cached pass");
    cache.write(100, 200, 0, 148, 1, 0, 52);
    input.cache = cache.lookup(100, 201);
    require(!input.cache.key_matches && select_pose(input).pass == 1, "Mesh mismatch bypasses cache");
    input.cache = cache.lookup(100, 200);
    input.preferred_descriptor = 1;
    selected = select_pose(input);
    require(selected.selected && selected.descriptor == 1 && selected.pass == 1,
            "Changed preferred descriptor bypasses old hint");
    input.preferred_descriptor = -1;
    selected = select_pose(input);
    require(selected.selected && selected.pass == 0 && selected.cache_action == CacheAction::clear,
            "Cached selection without a preferred descriptor asks to clear");
    cache.clear_entity(100);
    require(!cache.lookup(100, 200).key_matches, "Caller applies the observed clear action");
}
}

int main() {
    try {
        storage_rules();
        replacement_rules();
        selection_integration();
        std::puts("PASS reference_cache: storage, rotation, context/local reset and pose-selection integration");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL reference_cache: %s\n", error.what());
        return 1;
    }
}
