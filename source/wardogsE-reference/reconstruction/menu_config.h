#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace reconstruction {

struct MenuConfig {
    std::array<std::int32_t, 38> values;
    // Recovered save-buffer template, not verified runtime startup globals.
    static MenuConfig defaults();
};

// Pure memory decoder: no file polling or access to the reference application.
bool apply_menu(std::string_view text, MenuConfig& state);
// Local export of all current fields, not the reference radar-position saver.
std::string serialize_menu(const MenuConfig& state);

} // namespace reconstruction
