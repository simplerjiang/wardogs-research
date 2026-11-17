#include "esp_live.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

template <typename T>
bool ReadAt(const EspReadFn& read, u64 va, T* out, std::string* error) {
    if (!out) return false;
    if (!read) {
        if (error) *error = "no reader installed";
        return false;
    }
    return read(va, out, sizeof(T), error);
}

bool PlausiblePointer(u64 value) {
    return value >= 0x10000ull && value < 0x0000800000000000ull;
}

double ParseNumber(const char* start, char** end) {
    while (*start == ' ' || *start == '\t' || *start == '"') ++start;
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        char* stop = nullptr;
        const unsigned long long value = std::strtoull(start, &stop, 16);
        *end = stop;
        return static_cast<double>(value);
    }
    return std::strtod(start, end);
}

bool JsonNumber(const std::string& text, const char* key, double* out) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = text.find(pattern);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + pattern.size());
    if (pos == std::string::npos) return false;
    char* end = nullptr;
    const double value = ParseNumber(text.c_str() + pos + 1, &end);
    if (end == text.c_str() + pos + 1) return false;
    *out = value;
    return true;
}

u32 NumberKey(const std::string& text, const char* key, u32 fallback) {
    double value = 0.0;
    if (!JsonNumber(text, key, &value)) return fallback;
    if (!(value >= 0.0) || value > 4294967295.0) return fallback;
    return static_cast<u32>(value);
}

u64 NumberKey64(const std::string& text, const char* key, u64 fallback) {
    double value = 0.0;
    if (!JsonNumber(text, key, &value)) return fallback;
    if (!(value >= 0.0) || value > 1.0e19) return fallback;
    return static_cast<u64>(value);
}

float FloatKey(const std::string& text, const char* key, float fallback) {
    double value = 0.0;
    if (!JsonNumber(text, key, &value)) return fallback;
    if (!std::isfinite(value)) return fallback;
    return static_cast<float>(value);
}

std::string FormatVector(const float* values) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.2f,%.2f,%.2f", values[0], values[1], values[2]);
    return buffer;
}

bool VectorSane(const float* values) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(values[i])) return false;
        if (std::fabs(values[i]) > 1.0e7f) return false;
    }
    return true;
}

struct PointerArray {
    u64 data = 0;
    u32 count = 0;
    u32 capacity = 0;
};

}  

bool LoadOffsets(const std::wstring& path, EspOffsets* out, std::string* error) {
    if (!out) return false;
    std::vector<u8> bytes;
    if (!ReadWholeFile(path, &bytes)) {
        if (error) *error = "cannot read profile: " + Narrow(path);
        return false;
    }
    std::string text(bytes.begin(), bytes.end());
    EspOffsets parsed;
    parsed.world_slot = NumberKey64(text, "world_slot", 0);
    parsed.owning_game_instance = NumberKey(text, "owning_game_instance", 0);
    parsed.local_players = NumberKey(text, "local_players", 0);
    parsed.local_player_controller = NumberKey(text, "local_player_controller", 0);
    parsed.player_camera_manager = NumberKey(text, "player_camera_manager", 0);
    parsed.camera_cache = NumberKey(text, "camera_cache", 0);
    parsed.pov_location = NumberKey(text, "pov_location", 0x10);
    parsed.pov_rotation = NumberKey(text, "pov_rotation", 0x1C);
    parsed.pov_fov = NumberKey(text, "pov_fov", 0x28);
    parsed.persistent_level = NumberKey(text, "persistent_level", 0);
    parsed.world_levels = NumberKey(text, "world_levels", 0);
    parsed.level_actors = NumberKey(text, "level_actors", 0);
    parsed.actor_root_component = NumberKey(text, "actor_root_component", 0);
    parsed.component_location = NumberKey(text, "component_location", 0);
    parsed.character_height_cm = FloatKey(text, "character_height_cm", 180.0f);
    parsed.vehicle_height_cm = FloatKey(text, "vehicle_height_cm", 300.0f);
    *out = parsed;
    return true;
}

bool ReadPov(const EspReadFn& read, u64 image_base, const EspOffsets& offsets, EspPov* out, std::string* error) {
    if (!out) return false;
    if (!offsets.camera_ready()) {
        if (error) *error = "camera offsets are not calibrated in the profile";
        return false;
    }
    u64 world = 0;
    if (!ReadAt(read, image_base + offsets.world_slot, &world, error)) return false;
    if (!PlausiblePointer(world)) {
        if (error) *error = "world pointer is not plausible: " + Hex(world);
        return false;
    }
    u64 instance = 0;
    if (!ReadAt(read, world + offsets.owning_game_instance, &instance, error)) return false;
    if (!PlausiblePointer(instance)) {
        if (error) *error = "game instance pointer is not plausible: " + Hex(instance);
        return false;
    }
    PointerArray players{};
    if (!ReadAt(read, instance + offsets.local_players, &players, error)) return false;
    if (!PlausiblePointer(players.data) || players.count == 0) {
        if (error) *error = "no local player in the game instance";
        return false;
    }
    u64 player = 0;
    if (!ReadAt(read, players.data, &player, error)) return false;
    if (!PlausiblePointer(player)) {
        if (error) *error = "local player pointer is not plausible: " + Hex(player);
        return false;
    }
    u64 controller = 0;
    if (!ReadAt(read, player + offsets.local_player_controller, &controller, error)) return false;
    if (!PlausiblePointer(controller)) {
        if (error) *error = "player controller pointer is not plausible: " + Hex(controller);
        return false;
    }
    u64 manager = 0;
    if (!ReadAt(read, controller + offsets.player_camera_manager, &manager, error)) return false;
    if (!PlausiblePointer(manager)) {
        if (error) *error = "camera manager pointer is not plausible: " + Hex(manager);
        return false;
    }
    const u64 cache = manager + offsets.camera_cache;
    float location[3]{};
    float rotation[3]{};
    float fov = 0.0f;
    if (!ReadAt(read, cache + offsets.pov_location, location, error)) return false;
    if (!ReadAt(read, cache + offsets.pov_rotation, rotation, error)) return false;
    if (!ReadAt(read, cache + offsets.pov_fov, &fov, error)) return false;
    if (!VectorSane(location) || !VectorSane(rotation)) {
        if (error) *error = "camera view is not finite";
        return false;
    }
    if (!(fov > 1.0f) || !(fov < 179.0f)) {
        if (error) *error = "camera fov out of range: " + std::to_string(fov);
        return false;
    }
    out->location = Vec3{location[0], location[1], location[2]};
    out->pitch = rotation[0];
    out->yaw = rotation[1];
    out->roll = rotation[2];
    out->fov = fov;
    out->ok = true;
    return true;
}

bool ReadActorOrigin(const EspReadFn& read, const EspOffsets& offsets, u64 actor_va, Vec3* out, std::string* error) {
    if (!out) return false;
    if (!offsets.actors_ready()) {
        if (error) *error = "actor offsets are not calibrated in the profile";
        return false;
    }
    u64 root = 0;
    if (!ReadAt(read, actor_va + offsets.actor_root_component, &root, error)) return false;
    if (!PlausiblePointer(root)) {
        if (error) *error = "root component pointer is not plausible: " + Hex(root);
        return false;
    }
    float location[3]{};
    if (!ReadAt(read, root + offsets.component_location, location, error)) return false;
    if (!VectorSane(location)) {
        if (error) *error = "component location is not finite";
        return false;
    }
    out->x = location[0];
    out->y = location[1];
    out->z = location[2];
    return true;
}

std::vector<u64> ReadActorList(const EspReadFn& read, u64 image_base, const EspOffsets& offsets, std::size_t limit,
                               std::string* error) {
    std::vector<u64> actors;
    if (!offsets.world_slot) {
        if (error) *error = "world slot is not calibrated in the profile";
        return actors;
    }
    u64 world = 0;
    if (!ReadAt(read, image_base + offsets.world_slot, &world, error)) return actors;
    if (!PlausiblePointer(world)) {
        if (error) *error = "world pointer is not plausible: " + Hex(world);
        return actors;
    }
    std::vector<u64> levels;
    if (offsets.persistent_level) {
        u64 level = 0;
        if (!ReadAt(read, world + offsets.persistent_level, &level, error)) return actors;
        if (PlausiblePointer(level)) levels.push_back(level);
    }
    if (offsets.world_levels) {
        PointerArray array{};
        if (!ReadAt(read, world + offsets.world_levels, &array, error)) return actors;
        if (PlausiblePointer(array.data)) {
            const u32 count = array.count > 64u ? 64u : array.count;
            for (u32 i = 0; i < count; ++i) {
                u64 level = 0;
                std::string ignored;
                if (!ReadAt(read, array.data + static_cast<u64>(i) * 8u, &level, &ignored)) continue;
                if (PlausiblePointer(level)) levels.push_back(level);
            }
        }
    }
    for (u64 level : levels) {
        if (!offsets.level_actors) break;
        PointerArray array{};
        std::string ignored;
        if (!ReadAt(read, level + offsets.level_actors, &array, &ignored)) continue;
        if (!PlausiblePointer(array.data)) continue;
        const u32 count = array.count > 65536u ? 65536u : array.count;
        for (u32 i = 0; i < count; ++i) {
            if (actors.size() >= limit) break;
            u64 actor = 0;
            if (!ReadAt(read, array.data + static_cast<u64>(i) * 8u, &actor, &ignored)) continue;
            if (PlausiblePointer(actor)) actors.push_back(actor);
        }
        if (actors.size() >= limit) break;
    }
    return actors;
}

std::string ProbeActor(const EspReadFn& read, u64 actor_va, const EspOffsets& offsets) {
    std::string report;
    report += "actor=" + Hex(actor_va) + "\n";
    const u32 location_offset = offsets.component_location ? offsets.component_location : 0x130u;
    report += "location_offset_probe=" + Hex(location_offset, 4) + "\n";
    int found = 0;
    for (u32 offset = 0; offset < 0x800u; ++offset) {
        if ((offset % 8u) != 0u) continue;
        u64 candidate = 0;
        std::string ignored;
        if (!ReadAt(read, actor_va + offset, &candidate, &ignored)) continue;
        if (!PlausiblePointer(candidate)) continue;
        float location[3]{};
        if (!ReadAt(read, candidate + location_offset, location, &ignored)) continue;
        if (!VectorSane(location)) continue;
        report += "root_slot=" + Hex(offset, 4) + " root=" + Hex(candidate) +
                  " location=" + FormatVector(location) + "\n";
        if (++found >= 16) break;
    }
    if (found == 0) {
        int floats = 0;
        for (u32 offset = 0; offset < 0x400u; offset += 4) {
            float values[3]{};
            std::string ignored;
            if (!ReadAt(read, actor_va + offset, values, &ignored)) continue;
            if (!VectorSane(values)) continue;
            if (std::fabs(values[0]) < 1.0f && std::fabs(values[1]) < 1.0f && std::fabs(values[2]) < 1.0f) continue;
            report += "inline_vector=" + Hex(offset, 4) + " value=" + FormatVector(values) + "\n";
            if (++floats >= 16) break;
        }
        if (floats == 0) report += "no candidate root and no inline vector found\n";
    }
    return report;
}
