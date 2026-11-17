#include "esp_scene.h"

#include <algorithm>

void BuildScene(const Camera& camera, const std::vector<EspTarget>& targets, const EspSettings& settings,
                DrawList* list, SceneStats* stats) {
    SceneStats local;
    if (!list) {
        if (stats) *stats = local;
        return;
    }
    local.targets = targets.size();

    for (const auto& target : targets) {
        const float height_cm = target.height_cm > 1.0f ? target.height_cm : 180.0f;
        const Vec3 head{target.feet.x, target.feet.y, target.feet.z + height_cm};

        ProjectionStyle style;
        style.width_ratio = settings.width_ratio;
        style.min_height = settings.min_height;
        style.min_width = settings.min_width;
        style.scale = settings.scale;
        style.nominal_height_cm = height_cm;

        const ProjectionResult projection = ProjectBox(camera, head, target.feet, style);
        if (projection.status == BoxStatus::kSkipped) {
            ++local.skipped;
            continue;
        }
        if (settings.max_distance > 0.0f && projection.distance > settings.max_distance) {
            ++local.too_far;
            continue;
        }
        const ScreenBox box = projection.box;
        const float margin = settings.margin;
        if (box.right < -margin || box.left > camera.width + margin || box.bottom < -margin ||
            box.top > camera.height + margin) {
            ++local.outside;
            continue;
        }
        if (local.drawn >= settings.max_boxes) {
            ++local.limited;
            continue;
        }

        const Rgba color = target.kind == TargetKind::kVehicle ? settings.vehicle : settings.character;
        const float thickness = settings.thickness > 0.0f ? settings.thickness : 1.0f;
        if (settings.corners) {
            list->BoxCorners(box, settings.corner_ratio, thickness, color);
        } else {
            list->BoxOutline(box, thickness, color);
        }

        if (local.drawn == 0 || projection.distance < local.nearest) local.nearest = projection.distance;
        if (projection.distance > local.farthest) local.farthest = projection.distance;
        ++local.drawn;
    }

    if (local.drawn == 0) {
        local.nearest = 0.0f;
        local.farthest = 0.0f;
    }
    if (stats) *stats = local;
}
