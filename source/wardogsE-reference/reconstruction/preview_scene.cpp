#include "preview_scene.h"

#include <algorithm>

namespace reconstruction {
namespace {
Color rgb(std::int32_t value) {
    const auto bits = static_cast<unsigned>(value);
    return {((bits >> 16) & 255) / 255.0f, ((bits >> 8) & 255) / 255.0f,
            (bits & 255) / 255.0f};
}
}

DrawList preview_scene(unsigned width, unsigned height, const PreviewFrame& frame,
                       const MenuConfig& menu, bool running, const std::wstring& status) {
    DrawList list;
    const auto& v = menu.values;
    const Color white{0.9f, 0.94f, 1}, muted{0.52f, 0.61f, 0.72f};
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    list.text({24, 16}, {w - 16, 52}, L"路线 B · 受控数据预览", white, 24);
    list.text({24, 56}, {w - 16, 80},
              L"B 方框  S 连线  H 血条  N 名称  D 距离  R 雷达  空格 暂停/继续", muted, 14);
    list.text({24, 82}, {w - 16, 106}, L"F5 读取配置  F6 导出配置  Esc 关闭", muted, 14);
    const float cx = w * (0.34f + frame.displacement), top = h * 0.28f;
    const float bottom = h * 0.79f, span = bottom - top;
    const float half = w * 0.09f;
    const Point from{cx - half, top}, to{cx + half, bottom};
    if (v[12]) list.outline(from, to, rgb(v[19]), static_cast<float>(v[4]));
    if (v[13]) {
        const Color color = rgb(v[20]);
        const float stroke = static_cast<float>(v[6]);
        list.circle({cx, top + span * 0.12f}, span * 0.065f, color, stroke);
        const Point neck{cx, top + span * 0.19f}, chest{cx, top + span * 0.34f};
        const Point waist{cx, top + span * 0.61f};
        list.line(neck, waist, color, stroke);
        for (const float direction : {-1.0f, 1.0f}) {
            const Point elbow{cx + direction * half * 0.6f, top + span * 0.28f};
            list.line(chest, elbow, color, stroke);
            list.line(elbow, {cx + direction * half * 0.85f, top + span * 0.48f}, color, stroke);
            const Point knee{cx + direction * half * 0.4f, top + span * 0.8f};
            list.line(waist, knee, color, stroke);
            list.line(knee, {cx + direction * half * 0.55f, bottom - 4}, color, stroke);
        }
    }
    if (v[14]) {
        const float bar = static_cast<float>(v[5]);
        list.rectangle({from.x - bar - 6, top}, {from.x - 6, bottom}, {0.18f, 0.22f, 0.28f});
        list.rectangle({from.x - bar - 6, bottom - span * frame.health},
                       {from.x - 6, bottom}, rgb(v[21]));
    }
    // This fixture uses fixed display text, not game-derived labels or distance.
    if (v[15]) list.text({from.x, bottom + 8}, {to.x + 80, bottom + 32}, L"样例 A / Fixture A", rgb(v[22]));
    if (v[16]) list.text({from.x, bottom + 34}, {to.x + 80, bottom + 58}, L"100 m（合成）", rgb(v[23]));
    if (v[1]) {
        const float side = std::min({static_cast<float>(v[25]), w, h});
        const float x = (w - side) * static_cast<float>(v[10]) / 100;
        const float y = (h - side) * static_cast<float>(v[11]) / 100;
        list.rectangle({x, y}, {x + side, y + side}, {0.08f, 0.11f, 0.16f});
        list.outline({x, y}, {x + side, y + side}, muted);
        list.line({x + side / 2, y}, {x + side / 2, y + side}, muted);
        list.line({x, y + side / 2}, {x + side, y + side / 2}, muted);
        list.rectangle({x + side * 0.3f, y + side * 0.3f},
                       {x + side * 0.3f + 6, y + side * 0.3f + 6}, {0.4f, 0.9f, 0.7f});
    }
    list.text({24, h - 48}, {w - 16, h - 24},
              (running ? L"运行中 | 帧 " : L"已暂停 | 帧 ") + std::to_wstring(frame.sequence) + L" | " + status,
              muted, 14);
    list.text({24, h - 24}, {w - 16, h}, L"全部数据由本程序生成；未连接游戏或驱动。", muted, 13);
    return list;
}
} // namespace reconstruction
