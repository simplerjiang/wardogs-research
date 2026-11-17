#pragma once

#include "draw_list.h"
#include "math3d.h"

struct ProjectionStyle {
    float width_ratio = 0.46f;
    float min_height = 3.0f;
    float min_width = 2.0f;
    float scale = 1.0f;
    float nominal_height_cm = 180.0f;
};

enum class BoxStatus { kSkipped = 0, kTwoPoints = 1, kOnePoint = 2 };

struct ProjectionResult {
    ScreenBox box;
    BoxStatus status = BoxStatus::kSkipped;
    float projected_height = 0.0f;
    float applied_scale = 1.0f;
    float distance = 0.0f;
};

float PixelsPerCentimeter(const Camera& camera, float distance);

ProjectionResult ProjectBox(const Camera& camera, const Vec3& head, const Vec3& feet,
                            const ProjectionStyle& style);
