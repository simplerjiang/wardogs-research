#pragma once

#include <cmath>

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline bool IsFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

struct Camera {
    Vec3 location;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    float fov = 90.0f;
    float width = 1920.0f;
    float height = 1080.0f;
    bool fov_horizontal = true;
};

inline bool CameraBasis(const Camera& camera, Vec3* forward, Vec3* right, Vec3* up) {
    if (!forward || !right || !up) return false;
    const float p = camera.pitch * kDegToRad;
    const float y = camera.yaw * kDegToRad;
    const float r = camera.roll * kDegToRad;
    const float cp = std::cos(p), sp = std::sin(p);
    const float cy = std::cos(y), sy = std::sin(y);
    const float cr = std::cos(r), sr = std::sin(r);
    const Vec3 f{cp * cy, cp * sy, sp};
    const Vec3 right0{-sy, cy, 0.0f};
    const Vec3 up0{-sp * cy, -sp * sy, cp};
    *forward = f;
    *right = right0 * cr + up0 * sr;
    *up = up0 * cr - right0 * sr;
    return true;
}

inline bool ProjectToScreen(const Camera& camera, const Vec3& world, Vec2* screen) {
    if (!screen) return false;
    if (!(camera.width >= 1.0f) || !(camera.height >= 1.0f)) return false;
    if (!(camera.fov > 1.0f) || !(camera.fov < 179.0f)) return false;
    if (!IsFinite(world) || !IsFinite(camera.location)) return false;
    if (!std::isfinite(camera.pitch) || !std::isfinite(camera.yaw) || !std::isfinite(camera.roll)) return false;

    Vec3 forward, right, up;
    CameraBasis(camera, &forward, &right, &up);
    const Vec3 rel = world - camera.location;
    const float depth = Dot(rel, forward);
    if (!(depth > 1.0f)) return false;

    const float across = Dot(rel, right);
    const float upward = Dot(rel, up);
    const float aspect = camera.width / camera.height;
    const float tan_half = std::tan(camera.fov * 0.5f * kDegToRad);
    const float tan_h = camera.fov_horizontal ? tan_half : tan_half * aspect;
    const float tan_v = camera.fov_horizontal ? tan_half / aspect : tan_half;
    if (!(tan_h > 0.0f) || !(tan_v > 0.0f)) return false;

    const float ndc_x = across / (depth * tan_h);
    const float ndc_y = upward / (depth * tan_v);
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) return false;
    if (std::fabs(ndc_x) > 16.0f || std::fabs(ndc_y) > 16.0f) return false;

    screen->x = (ndc_x + 1.0f) * 0.5f * camera.width;
    screen->y = (1.0f - ndc_y) * 0.5f * camera.height;
    return true;
}
