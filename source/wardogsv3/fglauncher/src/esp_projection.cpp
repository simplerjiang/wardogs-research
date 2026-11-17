#include "esp_projection.h"

#include <algorithm>
#include <cmath>

float PixelsPerCentimeter(const Camera& camera, float distance) {
    if (!(camera.width >= 1.0f) || !(camera.height >= 1.0f)) return 0.0f;
    if (!(camera.fov > 1.0f) || !(camera.fov < 179.0f)) return 0.0f;
    if (!(distance > 1.0f)) distance = 1.0f;
    const float aspect = camera.width / camera.height;
    const float tan_half = std::tan(camera.fov * 0.5f * kDegToRad);
    const float tan_v = camera.fov_horizontal ? tan_half / aspect : tan_half;
    if (!(tan_v > 0.0f)) return 0.0f;
    return (camera.height * 0.5f) / (tan_v * distance);
}

ProjectionResult ProjectBox(const Camera& camera, const Vec3& head, const Vec3& feet,
                            const ProjectionStyle& style) {
    ProjectionResult result;
    Vec2 top{}, bottom{};
    const bool head_ok = ProjectToScreen(camera, head, &top);
    const bool feet_ok = ProjectToScreen(camera, feet, &bottom);

    float height = 0.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;

    if (head_ok && feet_ok) {
        height = std::fabs(bottom.y - top.y);
        center_x = (top.x + bottom.x) * 0.5f;
        center_y = (top.y + bottom.y) * 0.5f;
        result.status = BoxStatus::kTwoPoints;
        result.distance = Length(feet - camera.location);
    } else if (head_ok || feet_ok) {
        const Vec3 anchor = head_ok ? head : feet;
        const Vec2 point = head_ok ? top : bottom;
        result.distance = Length(anchor - camera.location);
        height = style.nominal_height_cm * PixelsPerCentimeter(camera, result.distance);
        center_x = point.x;
        center_y = head_ok ? point.y + height * 0.5f : point.y - height * 0.5f;
        result.status = BoxStatus::kOnePoint;
    } else {
        return result;
    }

    if (!std::isfinite(height) || !std::isfinite(center_x) || !std::isfinite(center_y)) {
        result.status = BoxStatus::kSkipped;
        return result;
    }

    float box_height = std::max(height, style.min_height > 0.0f ? style.min_height : 3.0f);
    const float ratio = style.width_ratio > 0.0f ? style.width_ratio : 0.46f;
    float box_width = std::max(style.min_width > 0.0f ? style.min_width : 2.0f, ratio * box_height);

    float scale = style.scale > 0.0f ? style.scale : 1.0f;
    if (scale < 0.2f) scale = 0.2f;
    if (scale > 4.0f) scale = 4.0f;
    box_height *= scale;
    box_width *= scale;

    result.projected_height = height;
    result.applied_scale = scale;
    result.box.left = center_x - box_width * 0.5f;
    result.box.right = center_x + box_width * 0.5f;
    result.box.top = center_y - box_height * 0.5f;
    result.box.bottom = center_y + box_height * 0.5f;
    return result;
}
