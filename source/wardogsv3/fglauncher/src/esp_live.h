#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "math3d.h"

using EspReadFn = std::function<bool(u64 va, void* dst, size_t n, std::string* error)>;

struct EspOffsets {
    u64 world_slot = 0;
    u32 owning_game_instance = 0;
    u32 local_players = 0;
    u32 local_player_controller = 0;
    u32 player_camera_manager = 0;
    u32 camera_cache = 0;
    u32 pov_location = 0x10;
    u32 pov_rotation = 0x1C;
    u32 pov_fov = 0x28;
    u32 persistent_level = 0;
    u32 world_levels = 0;
    u32 level_actors = 0;
    u32 actor_root_component = 0;
    u32 component_location = 0;
    float character_height_cm = 180.0f;
    float vehicle_height_cm = 300.0f;

    bool camera_ready() const {
        return world_slot != 0 && owning_game_instance != 0 && local_players != 0 &&
               local_player_controller != 0 && player_camera_manager != 0;
    }
    bool actors_ready() const { return actor_root_component != 0 && component_location != 0; }
};

bool LoadOffsets(const std::wstring& path, EspOffsets* out, std::string* error);

struct EspPov {
    Vec3 location;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    float fov = 90.0f;
    bool ok = false;
};

bool ReadPov(const EspReadFn& read, u64 image_base, const EspOffsets& offsets, EspPov* out, std::string* error);
bool ReadActorOrigin(const EspReadFn& read, const EspOffsets& offsets, u64 actor_va, Vec3* out, std::string* error);
std::vector<u64> ReadActorList(const EspReadFn& read, u64 image_base, const EspOffsets& offsets, std::size_t limit,
                               std::string* error);
std::string ProbeActor(const EspReadFn& read, u64 actor_va, const EspOffsets& offsets);
