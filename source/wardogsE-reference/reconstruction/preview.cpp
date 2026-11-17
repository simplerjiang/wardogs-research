#include "preview_scene.h"
#include "snapshot.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace reconstruction;
namespace fs = std::filesystem;
constexpr UINT advance_test = WM_APP + 1;
constexpr Color background{0.035f, 0.05f, 0.08f};

void require(bool value, const char* operation) {
    if (!value) throw std::runtime_error(operation);
}

struct Preview {
    HWND window = nullptr;
    unsigned width = 0, height = 0, painted_width = 0, painted_height = 0;
    unsigned test_step = 0, cycle = 0;
    bool running = true, testing = false, failed = false, minimized = false;
    bool awaiting_timer = false;
    unsigned before_timer = 0;
    MenuConfig menu = MenuConfig::defaults();
    Snapshot<PreviewFrame> frames;
    PreviewFrame produced, current;
    std::unique_ptr<Canvas> canvas;
    std::vector<unsigned char> pixels;
    fs::path config_path, output;
    std::wstring status = L"内置样例";

    bool load_config() {
        std::ifstream input(config_path, std::ios::binary);
        if (!input) return false;
        char data[511];
        input.read(data, sizeof(data));
        return apply_menu(std::string_view(data, static_cast<size_t>(input.gcount())), menu);
    }
    bool save_config() const {
        std::ofstream file(config_path, std::ios::binary | std::ios::trunc);
        file << serialize_menu(menu);
        file.close();
        return !file.fail();
    }
    void tick() {
        if (!running) return;
        ++produced.sequence;
        produced.displacement = 0.035f * std::sin(produced.sequence * 0.025f);
        frames.publish(produced);
        InvalidateRect(window, nullptr, FALSE);
    }
    void key(WPARAM value) {
        int slot = -1;
        switch (value) {
        case 'B': slot = 12; break;
        case 'S': slot = 13; break;
        case 'H': slot = 14; break;
        case 'N': slot = 15; break;
        case 'D': slot = 16; break;
        case 'R': slot = 1; break;
        case VK_SPACE: running = !running; break;
        case VK_F5: status = load_config() ? L"配置已读取" : L"配置读取失败"; break;
        case VK_F6: status = save_config() ? L"配置已导出" : L"配置导出失败"; break;
        case VK_ESCAPE: PostMessageW(window, WM_CLOSE, 0, 0); break;
        }
        if (slot >= 0) menu.values[slot] = !menu.values[slot];
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint() {
        if (!width || !height || minimized) {
            ValidateRect(window, nullptr);
            return;
        }
        if (!canvas || painted_width != width || painted_height != height) {
            canvas = std::make_unique<Canvas>(width, height);
            painted_width = width;
            painted_height = height;
        }
        frames.try_copy(current);
        canvas->render(preview_scene(width, height, current, menu, running, status), background);
        pixels = canvas->pixels();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        const int lines = StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height,
                                       pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(window, &paint);
        require(lines != static_cast<int>(GDI_ERROR) && lines != 0, "Present client bitmap");
    }
    void verify_client(const wchar_t* label) {
        UpdateWindow(window);
        require(!failed && canvas != nullptr, "Window paint failed");
        HDC dc = GetDC(window);
        require(dc != nullptr, "Get client DC");
        bool equal = true;
        // Read actual window pixels, not just the offscreen source. Test an
        // evenly spaced grid including text, geometry, and background regions.
        for (unsigned y = 2; y < height && equal; y += 7) {
            for (unsigned x = 2; x < width; x += 7) {
                const COLORREF color = GetPixel(dc, static_cast<int>(x), static_cast<int>(y));
                const auto* expected = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                if (color == CLR_INVALID || GetRValue(color) != expected[2] ||
                    GetGValue(color) != expected[1] || GetBValue(color) != expected[0]) {
                    equal = false;
                    break;
                }
            }
        }
        ReleaseDC(window, dc);
        require(equal, "Presented window pixels differ from rendered frame");
        canvas->save_png((output / (std::to_wstring(cycle) + L"-" + label + L".png")).c_str());
    }
    void resize_client(unsigned w, unsigned h) {
        RECT bounds{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        require(AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0) != 0, "Adjust bounds");
        require(SetWindowPos(window, HWND_TOP, 40, 40, bounds.right - bounds.left,
                             bounds.bottom - bounds.top, testing ? SWP_NOACTIVATE : 0) != 0, "Resize preview");
        if (testing) require(GetForegroundWindow() != window, "Self-test resize took foreground focus");
        InvalidateRect(window, nullptr, FALSE);
    }
    void test() {
        switch (test_step++) {
        case 0: {
            running = false;
            InvalidateRect(window, nullptr, FALSE);
            verify_client(L"initial");
            const auto before = pixels;
            SendMessageW(window, WM_KEYDOWN, 'B', 0);
            verify_client(L"box-off");
            require(menu.values[12] == 0 && before != pixels, "Box toggle had no effect");
            SendMessageW(window, WM_KEYDOWN, 'B', 0);
            for (const WPARAM key_value : {'S', 'H', 'N', 'D', 'R'}) {
                UpdateWindow(window);
                const auto old = pixels;
                SendMessageW(window, WM_KEYDOWN, key_value, 0);
                UpdateWindow(window);
                require(old != pixels, "Feature toggle had no pixel effect");
                SendMessageW(window, WM_KEYDOWN, key_value, 0);
            }
            std::puts("PASS window: native pixel sampling and six display toggles");
            break;
        }
        case 1: {
            menu.values[4] = 7;
            menu.values[19] = 0x25dca0;
            menu.values[10] = 100;
            menu.values[11] = 100;
            menu.values[25] = 400;
            SendMessageW(window, WM_KEYDOWN, VK_F6, 0);
            require(status == L"配置已导出", "Export config failed");
            menu.values[4] = 1;
            menu.values[19] = 0;
            SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
            require(menu.values[4] == 7 && menu.values[19] == 0x25dca0, "Config reload mismatch");
            verify_client(L"config");
            const auto* line_pixel = &pixels[(200 * width + 240) * 4];
            require(line_pixel[2] == 0x25 && line_pixel[1] == 0xdc && line_pixel[0] == 0xa0,
                    "Reloaded box color did not reach rendered pixels");
            const auto accepted = menu.values;
            { std::ofstream invalid(config_path); invalid << "invalid"; }
            SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
            require(status == L"配置读取失败" && menu.values == accepted, "Invalid file changed configuration");
            SendMessageW(window, WM_KEYDOWN, VK_F6, 0);
            std::puts("PASS window: export and reload controlled configuration");
            resize_client(640, 480);
            break;
        }
        case 2:
            require(width == 640 && height == 480, "Small client size mismatch");
            verify_client(L"640x480");
            resize_client(1280, 720);
            break;
        case 3:
            require(width == 1280 && height == 720, "Large client size mismatch");
            verify_client(L"1280x720");
            ShowWindow(window, SW_MINIMIZE);
            require(minimized, "Minimize did not reach window state");
            ShowWindow(window, SW_SHOWNOACTIVATE);
            require(GetForegroundWindow() != window, "Self-test restore took foreground focus");
            InvalidateRect(window, nullptr, FALSE);
            break;
        case 4: {
            require(!minimized, "Restore failed");
            verify_client(L"restored");
            const unsigned before = produced.sequence;
            SendMessageW(window, WM_TIMER, 1, 0);
            require(produced.sequence == before, "Paused frame advanced");
            SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
            SendMessageW(window, WM_TIMER, 1, 0);
            require(produced.sequence == before + 1, "Resume did not publish a frame");
            SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
            verify_client(L"resumed");
            std::puts("PASS window: resize, minimize/restore, pause/resume");
            before_timer = produced.sequence;
            awaiting_timer = true;
            running = true;
            require(SetTimer(window, 1, 33, nullptr) != 0, "Start real timer test");
            return;
        }
        case 5:
            std::printf("TIMER observed: running=%d before=%u produced=%u consumed=%u\n",
                        running, before_timer, produced.sequence, current.sequence);
            require(running, "Self-test paused while awaiting the real timer");
            running = false;
            require(KillTimer(window, 1) != 0, "Stop owned preview timer");
            require(produced.sequence > before_timer, "Real timer did not advance controlled data");
            InvalidateRect(window, nullptr, FALSE);
            verify_client(L"timer");
            require(current.sequence == produced.sequence, "Rendered snapshot did not receive timer frame");
            std::puts("PASS window: real Windows timer delivered and rendered controlled frame");
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        PostMessageW(window, advance_test, 0, 0);
    }
};

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Preview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, message, wparam, lparam);
    try {
        switch (message) {
        case WM_SIZE:
            app->minimized = wparam == SIZE_MINIMIZED;
            app->width = LOWORD(lparam);
            app->height = HIWORD(lparam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: app->paint(); return 0;
        case WM_TIMER:
            app->tick();
            if (app->awaiting_timer) {
                app->awaiting_timer = false;
                PostMessageW(hwnd, advance_test, 0, 0);
            }
            return 0;
        case WM_KEYDOWN: app->key(wparam); return 0;
        case advance_test: app->test(); return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            app->canvas.reset();
            PostQuitMessage(app->failed ? 1 : 0);
            return 0;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL preview: %s\n", error.what());
        app->failed = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    fs::path output, config;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--self-test" && i + 1 < argc) output = argv[++i];
        else if (argument == L"--config" && i + 1 < argc) config = argv[++i];
        else { std::fputs("Usage: preview.exe [--config path] [--self-test output-directory]\n", stderr); return 2; }
    }
    if (!output.empty()) {
        fs::create_directories(output);
        config = output / L"controlled-menu.cfg";
    } else if (config.empty()) config = fs::path(argv[0]).parent_path() / L"preview-menu.cfg";
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    SetProcessDPIAware();
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"RouteBControlledPreview";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    int result = 0;
    if (!RegisterClassW(&type)) result = 1;
    DWORD warm_gdi = 0, warm_user = 0;
    for (unsigned cycle = 0; result == 0 && cycle < (output.empty() ? 1u : 3u); ++cycle) {
        Preview app;
        app.testing = !output.empty();
        app.cycle = cycle;
        app.output = output;
        app.config_path = config;
        app.menu.values[1] = 1;
        app.menu.values[10] = 66;
        app.menu.values[11] = 28;
        if (!app.testing && app.load_config()) app.status = L"配置已读取";
        app.frames.publish(app.current);
        HWND window = CreateWindowExW(app.testing ? WS_EX_NOACTIVATE : 0, type.lpszClassName, L"Route B — Controlled Preview",
            WS_OVERLAPPEDWINDOW, 40, 40, 980, 590, nullptr, nullptr, type.hInstance, &app);
        if (!window) { result = 1; break; }
        try {
            app.resize_client(960, 540);
            ShowWindow(window, app.testing ? SW_SHOWNOACTIVATE : SW_SHOW);
            if (app.testing) require(GetForegroundWindow() != window, "Self-test show took foreground focus");
            UpdateWindow(window);
            if (app.testing) PostMessageW(window, advance_test, 0, 0);
            else require(SetTimer(window, 1, 33, nullptr) != 0, "Start preview timer");
            MSG message{};
            int received;
            while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (received < 0 || app.failed || message.wParam != 0) result = 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL startup: %s\n", error.what());
            result = 1;
        }
        if (IsWindow(window)) DestroyWindow(window);
        if (app.testing && result == 0) {
            if (IsWindow(window) || app.canvas) { result = 1; std::puts("FAIL owned window/canvas still live"); }
            EnumThreadWindows(GetCurrentThreadId(), [](HWND remaining, LPARAM) -> BOOL {
                wchar_t name[256]{};
                GetClassNameW(remaining, name, 256);
                std::printf("REMAINING thread window: %ls\n", name);
                return TRUE;
            }, 0);
            const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            std::printf("CYCLE %u closed: GDI=%lu USER=%lu\n", cycle, gdi, user);
            if (cycle == 0) { warm_gdi = gdi; warm_user = user; }
            else {
                if (gdi != warm_gdi) { result = 1; std::puts("FAIL GDI count changed after warm cycle"); }
                if (user != warm_user)
                    std::puts("DIAGNOSTIC process USER count changed; includes framework-owned objects");
            }
            if (result == 0) std::puts("PASS lifecycle: owned timer stopped, window destroyed, Canvas released");
        }
    }
    UnregisterClassW(type.lpszClassName, type.hInstance);
    CoUninitialize();
    return result;
}
