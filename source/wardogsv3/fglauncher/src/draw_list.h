#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "math3d.h"

struct Rgba {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct ScreenBox {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    float CenterX() const { return (left + right) * 0.5f; }
    float CenterY() const { return (top + bottom) * 0.5f; }
};

struct DrawQuad {
    float cx = 0.0f;
    float cy = 0.0f;
    float hw = 0.0f;
    float hh = 0.0f;
    float angle = 0.0f;
    float pad = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct DrawList {
    std::vector<DrawQuad> quads;
    std::size_t capacity = 16384;
    std::size_t dropped = 0;

    void Clear() {
        quads.clear();
        dropped = 0;
    }

    void Push(const DrawQuad& quad) {
        if (quads.size() >= capacity) {
            ++dropped;
            return;
        }
        quads.push_back(quad);
    }

    void FilledRect(float left, float top, float width, float height, const Rgba& color) {
        if (!(width > 0.0f) || !(height > 0.0f)) return;
        DrawQuad q;
        q.cx = left + width * 0.5f;
        q.cy = top + height * 0.5f;
        q.hw = width * 0.5f;
        q.hh = height * 0.5f;
        q.r = color.r;
        q.g = color.g;
        q.b = color.b;
        q.a = color.a;
        Push(q);
    }

    void Line(float x0, float y0, float x1, float y1, float thickness, const Rgba& color) {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0f)) return;
        DrawQuad q;
        q.cx = (x0 + x1) * 0.5f;
        q.cy = (y0 + y1) * 0.5f;
        q.hw = length * 0.5f;
        q.hh = thickness > 0.0f ? thickness * 0.5f : 0.5f;
        q.angle = std::atan2(dy, dx);
        q.r = color.r;
        q.g = color.g;
        q.b = color.b;
        q.a = color.a;
        Push(q);
    }

    void BoxOutline(const ScreenBox& box, float thickness, const Rgba& color) {
        const float t = thickness > 0.0f ? thickness : 1.0f;
        Line(box.left, box.top, box.right, box.top, t, color);
        Line(box.right, box.top, box.right, box.bottom, t, color);
        Line(box.right, box.bottom, box.left, box.bottom, t, color);
        Line(box.left, box.bottom, box.left, box.top, t, color);
    }

    void BoxCorners(const ScreenBox& box, float arm_ratio, float thickness, const Rgba& color) {
        const float t = thickness > 0.0f ? thickness : 1.0f;
        float arm = arm_ratio > 0.0f && arm_ratio < 0.5f ? arm_ratio : 0.22f;
        const float ax = box.Width() * arm;
        const float ay = box.Height() * arm;
        Line(box.left, box.top, box.left + ax, box.top, t, color);
        Line(box.left, box.top, box.left, box.top + ay, t, color);
        Line(box.right, box.top, box.right - ax, box.top, t, color);
        Line(box.right, box.top, box.right, box.top + ay, t, color);
        Line(box.left, box.bottom, box.left + ax, box.bottom, t, color);
        Line(box.left, box.bottom, box.left, box.bottom - ay, t, color);
        Line(box.right, box.bottom, box.right - ax, box.bottom, t, color);
        Line(box.right, box.bottom, box.right, box.bottom - ay, t, color);
    }
};
