#pragma once

#include "reference_rules.h"
#include <vector>

namespace reconstruction::reference {

struct Camera {
    bool valid = true;
    Position location{};
    Position rotation_degrees{}; // pitch, yaw, roll; snapshot +0x48
    float fov_degrees = 90;      // snapshot +0x60
    float aspect_hint = 16.0f / 9.0f; // snapshot +0x64
};
struct Viewport { float width = 0, height = 0; };
struct ScreenPoint { float x = 0, y = 0; };
struct ScreenSegment { ScreenPoint from, to; };

// 49dc / 2098: no viewport clipping. Failure may write rejected screen values.
// Controlled camera angles use the normal finite range; CPU exception flags and
// out-of-range CVTTSD2SI behavior in the original trig helper are not modeled.
bool project(const Camera&, Viewport, const Position&, ScreenPoint&);
// 12d18: world length, near-camera disparity, projection, screen-length filter.
bool project_segment(const Camera&, Viewport, const Position&, const Position&,
                     ScreenSegment&);
// dbb4/c85c/b7e8/d584: skipped invalid joints bridge their valid neighbors.
// Flags correspond to the internal entity's +0x24 and +0x20 fields, not games.
std::vector<ScreenSegment> project_skeleton(const Camera&, Viewport,
    const std::array<Position, 21>&, std::uint32_t mask,
    bool largeProfile = false, bool profile20 = false);

} // namespace reconstruction::reference
