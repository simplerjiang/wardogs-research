#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "draw_list.h"
#include "esp_projection.h"
#include "math3d.h"

enum class TargetKind { kCharacter = 0, kVehicle = 1 };

struct EspTarget {
    Vec3 feet;
    float height_cm = 180.0f;
    TargetKind kind = TargetKind::kCharacter;
    std::string label;
};

struct EspSettings {
    float scale = 1.0f;
    float width_ratio = 0.46f;
    float min_height = 3.0f;
    float min_width = 2.0f;
    float thickness = 1.0f;
    bool corners = false;
    float corner_ratio = 0.22f;
    Rgba character{0.431f, 0.608f, 1.0f, 1.0f};
    Rgba vehicle{0.745f, 0.627f, 1.0f, 1.0f};
    std::size_t max_boxes = 512;
    float max_distance = 0.0f;
    float margin = 64.0f;
};

struct SceneStats {
    std::size_t targets = 0;
    std::size_t drawn = 0;
    std::size_t skipped = 0;
    std::size_t outside = 0;
    std::size_t too_far = 0;
    std::size_t limited = 0;
    float nearest = 0.0f;
    float farthest = 0.0f;
};

void BuildScene(const Camera& camera, const std::vector<EspTarget>& targets, const EspSettings& settings,
                DrawList* list, SceneStats* stats);
