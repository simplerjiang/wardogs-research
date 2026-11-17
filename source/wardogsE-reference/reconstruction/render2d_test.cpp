#include "render2d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace reconstruction;
constexpr Color black{0, 0, 0}, white{1, 1, 1};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void pixel(const std::vector<unsigned char>& data, unsigned width, unsigned x,
           unsigned y, int r, int g, int b, int a = 255) {
    const auto* p = &data[(static_cast<size_t>(y) * width + x) * 4];
    require(std::abs(p[0] - b) <= 1 && std::abs(p[1] - g) <= 1 &&
            std::abs(p[2] - r) <= 1 && std::abs(p[3] - a) <= 1, "Unexpected pixel");
}

void check_geometry() {
    Canvas canvas(128, 96);
    DrawList list;
    list.rectangle({-10, -10}, {20, 20}, {1, 0, 0});
    list.rectangle({40, 10}, {60, 30}, {0, 0, 1});
    list.rectangle({40, 10}, {60, 30}, {1, 0, 0, 0.5f});
    list.line({10, 50}, {110, 50}, {0, 1, 0}, 4);
    canvas.render(list, black);
    auto data = canvas.pixels();
    pixel(data, 128, 0, 0, 255, 0, 0); // Viewport clipping.
    pixel(data, 128, 21, 21, 0, 0, 0);
    pixel(data, 128, 50, 20, 128, 0, 127); // Alpha and painter order.
    pixel(data, 128, 60, 49, 0, 255, 0);
    pixel(data, 128, 60, 53, 0, 0, 0); // Width does not fill outside the line.
    canvas.render({}, {0, 0, 0, 0});
    data = canvas.pixels();
    require(std::all_of(data.begin(), data.end(), [](unsigned char c) { return c == 0; }),
            "Previous frame leaked into transparent clear");
    std::puts("PASS pixels: clipping, alpha, painter order, line width, frame clear");
}

void check_text_and_circle() {
    Canvas canvas(160, 96);
    DrawList text;
    text.text({8, 8}, {90, 40}, L"Text 123", white, 20);
    canvas.render(text, black);
    auto data = canvas.pixels();
    unsigned lit = 0;
    for (unsigned y = 0; y < 96; ++y) {
        for (unsigned x = 0; x < 160; ++x) {
            const bool changed = data[(y * 160 + x) * 4] != 0;
            if (changed) {
                ++lit;
                require(x >= 8 && x < 90 && y >= 8 && y < 40, "Text exceeded its clip");
            }
        }
    }
    require(lit > 100, "Text produced no readable glyph area");
    DrawList ring;
    ring.circle({80, 48}, 30, white, 4);
    require(ring.items.size() == 48, "Expected 48-segment circle");
    const auto& first = ring.items.front();
    const auto& last = ring.items.back();
    require(std::abs(first.from.x - last.to.x) < 0.001f &&
            std::abs(first.from.y - last.to.y) < 0.001f, "Circle did not close");
    canvas.render(ring, black);
    data = canvas.pixels();
    pixel(data, 160, 80, 48, 0, 0, 0);
    pixel(data, 160, 109, 48, 255, 255, 255);
    pixel(data, 160, 80, 18, 255, 255, 255);
    std::puts("PASS pixels: clipped glyphs, hollow 48-segment circle");
}

DrawList sample() {
    DrawList list;
    const Color muted{0.52f, 0.59f, 0.68f}, blue{0.35f, 0.7f, 1}, green{0.35f, 0.85f, 0.65f};
    list.text({32, 24}, {928, 66}, L"Route B / 2D rendering fixture", white, 28);
    list.text({32, 67}, {928, 100}, L"Synthetic screen coordinates | No game or driver connection", muted, 16);
    list.rectangle({32, 118}, {620, 474}, {0.08f, 0.11f, 0.16f});
    list.outline({220, 150}, {400, 438}, blue, 2);
    list.circle({310, 196}, 20, white, 2);
    const Point joints[] = {{310,216}, {310,265}, {310,330}, {265,245}, {240,290},
                            {355,245}, {380,290}, {280,375}, {268,425}, {340,375}, {352,425}};
    const int edges[][2] = {{0,1},{1,2},{1,3},{3,4},{1,5},{5,6},{2,7},{7,8},{2,9},{9,10}};
    for (const auto& edge : edges) list.line(joints[edge[0]], joints[edge[1]], white, 2);
    list.rectangle({206, 150}, {211, 438}, {0.2f, 0.25f, 0.3f});
    list.rectangle({206, 222}, {211, 438}, green);
    list.text({236, 445}, {450, 470}, L"Fixture A / 75%", blue);
    list.rectangle({648, 118}, {928, 398}, {0.08f, 0.11f, 0.16f});
    list.circle({788, 258}, 110, muted);
    list.line({678,258}, {898,258}, muted);
    list.line({788,148}, {788,368}, muted);
    list.rectangle({738, 208}, {745, 215}, green);
    list.rectangle({838, 288}, {845, 295}, blue);
    list.rectangle({784, 254}, {792, 262}, white);
    list.text({648, 417}, {928, 468}, L"Lines / quads / text / alpha", muted, 16);
    list.text({32, 493}, {928, 524}, L"Module test only. Reference visual parity has not been verified.", muted, 16);
    return list;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fputs("Usage: render2d_test.exe output.png\n", stderr);
        return 2;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    int result = 0;
    try {
        check_geometry();
        check_text_and_circle();
        Canvas canvas(960, 540);
        canvas.render(sample(), {0.035f, 0.05f, 0.08f});
        canvas.save_png(argv[1]);
        std::puts("PASS synthetic fixture exported (not game/reference acceptance)");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    CoUninitialize();
    return result;
}
