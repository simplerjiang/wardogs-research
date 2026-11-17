#pragma once

#include "menu_config.h"
#include "render2d.h"

namespace reconstruction {
// Owned synthetic data, deliberately independent of the reference binary ABI.
struct PreviewFrame {
    unsigned sequence = 0;
    float displacement = 0;
    float health = 0.75f;
};

DrawList preview_scene(unsigned width, unsigned height, const PreviewFrame& frame,
                       const MenuConfig& menu, bool running, const std::wstring& status);
} // namespace reconstruction
