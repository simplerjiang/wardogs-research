#include "menu_config.h"

#include <cstdio>
#include <stdexcept>

using reconstruction::MenuConfig;
using reconstruction::apply_menu;
using reconstruction::serialize_menu;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string prefix(const MenuConfig& config, std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) text += std::to_string(config.values[i]) + ' ';
    return text;
}

void grammar_and_bounds() {
    const auto defaults = MenuConfig::defaults();
    auto state = defaults;
    require(!apply_menu(prefix(defaults, 9), state) && state.values == defaults.values,
            "Short input mutated configuration");
    for (const auto invalid : {"+1 ", "\xEF\xBB\xBF", "\v", "\f", "x", ","}) {
        require(!apply_menu(std::string(invalid) + prefix(defaults, 38), state), "Invalid prefix accepted");
        require(state.values == defaults.values, "Rejected input mutated state");
    }
    require(apply_menu("1-2 300 300 1 3 1 70 2 1", state) && state.values[0] == 1 && state.values[1] == 1,
            "Adjacent signed values differ from recovered parser");
    require(apply_menu(" \t\r\n" + prefix(defaults, 10) + "bad 9 9", state), "Valid prefix discarded");
    require(state.values[10] == 2 && state.values[11] == 78, "Invalid tail changed optional group");
    const std::string embedded = prefix(defaults, 9) + std::string(1, '\0') + "1";
    require(!apply_menu(embedded, state), "Embedded NUL did not stop parsing");
    const auto nine = prefix(defaults, 9);
    require(!apply_menu(std::string(511 - nine.size(), ' ') + nine + "1", state), "Read crossed byte 511");
    require(apply_menu(std::string(510 - nine.size(), ' ') + nine + "123", state) && state.values[9] == 1,
            "Token cut at byte 511 did not retain digit prefix");
    state = defaults;
    require(apply_menu(serialize_menu(defaults) + "9999 9999", state) && state.values == defaults.values,
            "Fields after 38 changed configuration");
    std::puts("PASS grammar: invalid/short input, whitespace, signed adjacency, NUL, 511-byte and 38-field limits");
}

void partial_groups() {
    const auto defaults = MenuConfig::defaults();
    auto input = defaults;
    input.values[10] = 45; input.values[11] = 55;
    for (std::size_t i = 12; i <= 18; ++i) input.values[i] = 0;
    for (std::size_t i = 19; i <= 24; ++i) input.values[i] = 123;
    input.values[35] = 101; input.values[36] = 202; input.values[37] = 303;
    for (const auto count : {10u, 11u, 12u, 18u, 19u, 24u, 25u, 35u, 36u, 37u, 38u}) {
        auto state = defaults;
        state.values[35] = 5; state.values[36] = 6; state.values[37] = 7;
        require(apply_menu(prefix(input, count), state), "Valid partial input rejected");
        require(state.values[10] == (count >= 12 ? 45 : 2) && state.values[11] == (count >= 12 ? 55 : 78),
                "Position group applied partially");
        for (std::size_t i = 12; i <= 18; ++i)
            require(state.values[i] == (count >= 19 ? 0 : defaults.values[i]), "Boolean group applied partially");
        for (std::size_t i = 19; i <= 24; ++i)
            require(state.values[i] == (count >= 25 ? 123 : defaults.values[i]), "Color group applied partially");
        for (std::size_t i = 35; i < 38; ++i)
            require(state.values[i] == (count > i ? input.values[i] : 300), "Missing distance did not reset independently");
    }
    auto state = defaults;
    state.values[10] = -10; state.values[11] = 110; state.values[25] = 222;
    require(apply_menu(prefix(defaults, 10), state) && state.values[10] == 0 && state.values[11] == 100 &&
            state.values[25] == 222, "Old positions not clamped or unrelated optional field lost");
    std::puts("PASS partial groups: thresholds 10/12/19/25, optional retention, independent final defaults");
}

void clamps_and_export() {
    const std::array<std::int32_t, 38> lows{
        1,1,25,0,1,1,1,30,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,100,25,0,0,0,0,0,1,1,25,0,0,0};
    const std::array<std::int32_t, 38> highs{
        1,1,1000,1000,8,14,8,180,7,7,100,100,1,1,1,1,1,1,1,
        0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,400,1000,
        0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,1,1,1000,1000,1000,1000};
    auto input = MenuConfig::defaults(), state = input;
    input.values.fill(-1);
    require(apply_menu(prefix(input, 38), state) && state.values == lows, "Lower bounds or negative booleans wrong");
    input.values.fill(2147483647);
    require(apply_menu(prefix(input, 38), state) && state.values == highs, "Upper bounds wrong");
    auto restored = MenuConfig::defaults();
    require(apply_menu(serialize_menu(state), restored) && restored.values == state.values, "Normalized roundtrip failed");
    const auto defaults = MenuConfig::defaults();
    require(apply_menu(serialize_menu(defaults), restored) && restored.values == defaults.values, "Save-template roundtrip failed");
    input = defaults; input.values[8] = -3;
    require(serialize_menu(input).find("70 0 1 ") != std::string::npos, "Export did not convert negative to zero");
    require(serialize_menu(input).back() == '\n', "Export lacks final LF");
    std::puts("PASS normalization: all lower/upper limits, negative booleans, template/current-state roundtrip, export format");
}

int main() {
    try { grammar_and_bounds(); partial_groups(); clamps_and_export(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
