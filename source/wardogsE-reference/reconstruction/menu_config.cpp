#include "menu_config.h"

#include <algorithm>
#include <cstring>

namespace reconstruction {

MenuConfig MenuConfig::defaults() {
    return {{0, 0, 300, 300, 1, 3, 1, 70, 2, 1, 2, 78,
             1, 1, 1, 1, 1, 1, 1,
             0xE24B4B, 0xF0E2C8, 0xE2A84F, 0xF4ECE4, 0xD7E6FF, 0xFFB4A8,
             168, 150, 0x63DF9B, 0xE24B4B, 0x64C8FF, 0xFFB347, 0xBEA0FF,
             1, 0, 300, 300, 300, 300}};
}

bool apply_menu(std::string_view text, MenuConfig& state) {
    text = text.substr(0, 511);
    std::array<std::int32_t, 38> parsed{};
    std::size_t position = 0, count = 0;
    while (position < text.size() && count < parsed.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
               text[position] == '\r' || text[position] == '\n')) ++position;
        if (position == text.size()) break;
        const bool negative = text[position] == '-';
        if (negative) ++position;
        if (position == text.size() || text[position] < '0' || text[position] > '9') break;
        std::uint32_t value = 0;
        do {
            value = value * 10 + static_cast<std::uint32_t>(text[position++] - '0');
        } while (position < text.size() && text[position] >= '0' && text[position] <= '9');
        if (negative) value = 0u - value;
        // Defined local 32-bit wrap; reference overflow equivalence is unverified.
        std::memcpy(&parsed[count++], &value, sizeof(value));
    }
    if (count < 10) return false;

    auto& values = state.values;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t required = i < 10 ? 10 : i < 12 ? 12 : i < 19 ? 19 : i < 25 ? 25 : i + 1;
        if (count >= required) values[i] = parsed[i];
        else if (i >= 35) values[i] = 300;
        else if (i != 10 && i != 11) continue;

        const bool boolean = i < 2 || (i >= 12 && i <= 18) || i == 32 || i == 33;
        if (boolean) { values[i] = values[i] != 0; continue; }
        constexpr std::array<std::int32_t, 38> low{
            0, 0, 25, 0, 1, 1, 1, 30, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            100, 25, 0, 0, 0, 0, 0, 0, 0, 25, 0, 0, 0};
        constexpr std::array<std::int32_t, 38> high{
            1, 1, 1000, 1000, 8, 14, 8, 180, 7, 7, 100, 100,
            1, 1, 1, 1, 1, 1, 1,
            0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF,
            400, 1000, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF,
            1, 1, 1000, 1000, 1000, 1000};
        values[i] = std::clamp(values[i], low[i], high[i]);
    }
    return true;
}

std::string serialize_menu(const MenuConfig& state) {
    std::string text;
    for (std::size_t i = 0; i < state.values.size(); ++i) {
        text += std::to_string(std::max(state.values[i], std::int32_t{0}));
        text += i + 1 == state.values.size() ? '\n' : ' ';
    }
    return text;
}

} // namespace reconstruction
