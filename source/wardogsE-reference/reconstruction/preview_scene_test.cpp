#include "preview_scene.h"

#include <cstdio>
#include <stdexcept>

using namespace reconstruction;

void check_radar(unsigned width, unsigned height, int position, float x, float y, float side) {
    auto menu = MenuConfig::defaults();
    for (unsigned i = 12; i <= 18; ++i) menu.values[i] = 0;
    menu.values[1] = 1;
    menu.values[10] = position;
    menu.values[11] = position;
    menu.values[25] = 400;
    const auto list = preview_scene(width, height, {}, menu, false, L"Test");
    for (const auto& primitive : list.items) {
        if (primitive.kind == Kind::rectangle) {
            if (primitive.from.x != x || primitive.from.y != y ||
                primitive.to.x != x + side || primitive.to.y != y + side)
                throw std::runtime_error("Radar bounds differ from expected usable-area coordinates");
            return;
        }
    }
    throw std::runtime_error("Radar rectangle missing");
}

int main() {
    try {
        check_radar(960, 540, 0, 0, 0, 400);
        check_radar(960, 540, 100, 560, 140, 400);
        check_radar(200, 150, 100, 50, 0, 150);
        std::puts("PASS scene: radar 0/100 percent endpoints and size clamped to small viewport");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
