#include "controlled_scene.h"
#include "snapshot.h"

#include <dwmapi.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

using namespace reconstruction;
namespace fs = std::filesystem;
constexpr COLORREF backing_color = RGB(24, 42, 64);
constexpr DWORD overlay_flags = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    Surface(unsigned width, unsigned height) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (dc) bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap) previous = SelectObject(dc, bitmap);
    }
    ~Surface() {
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

void save_composite(const fs::path& path, unsigned width, unsigned height, Surface& captured) {
    auto* bytes = static_cast<unsigned char*>(captured.pixels);
    for (std::size_t i = 3; i < std::size_t(width) * height * 4; i += 4) bytes[i] = 255;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()))), "Create capture encoder factory");
    require(SUCCEEDED(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppBGRA,
        width*4,width*height*4,bytes,&bitmap)), "Create captured bitmap");
    require(SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width,height)), "Initialize capture PNG");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    require(SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(),nullptr)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "Write capture PNG");
}

struct Overlay {
    HWND window = nullptr, backing = nullptr;
    unsigned width = 0, height = 0, sequence = 0, step = 0;
    unsigned timer_baseline = 0;
    bool testing = false, failed = false, hotkey = false;
    fs::path output;
    Snapshot<ControlledScene> frames;
    ControlledSession session;
    ControlledScene current;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Surface> surface;
    std::vector<unsigned char> pixels, normal_pixels;

    void resize(unsigned w, unsigned h) {
        canvas.reset();
        surface.reset();
        width = w; height = h;
        canvas = std::make_unique<Canvas>(w, h);
        surface = std::make_unique<Surface>(w, h);
        require(surface->dc && surface->bitmap && surface->previous &&
                surface->previous != HGDI_ERROR, "Create layered bitmap");
        if (backing) {
            require(SetWindowPos(backing, HWND_TOPMOST, 40, 40, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0, "Resize controlled background");
            InvalidateRect(backing, nullptr, FALSE);
            UpdateWindow(backing);
        }
        require(SetWindowPos(window, HWND_TOPMOST, testing ? 40 : 0, testing ? 40 : 0,
                            w, h, SWP_NOACTIVATE) != 0, "Resize topmost overlay");
    }
    void present(ControlledCase scenario) {
        frames.publish(session.frame(width, height, testing && step < 7 ? 0 : sequence, scenario));
        require(frames.try_copy(current), "Copy controlled scene");
        canvas->render(current.drawing, {0, 0, 0, 0});
        pixels = canvas->pixels();
        std::memcpy(surface->pixels, pixels.data(), pixels.size());
        RECT bounds{};
        require(GetWindowRect(window, &bounds) != 0, "Read overlay bounds");
        POINT destination{bounds.left, bounds.top}, source{};
        SIZE size{static_cast<LONG>(width), static_cast<LONG>(height)};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        require(UpdateLayeredWindow(window, nullptr, &destination, &size, surface->dc,
            &source, 0, &blend, ULW_ALPHA) != 0, "Present alpha overlay");
    }
    void verify(const wchar_t* label) {
        require(SUCCEEDED(DwmFlush()), "Flush desktop composition");
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        require((style & (WS_CAPTION | WS_THICKFRAME)) == 0 && (style & WS_POPUP), "Overlay has a frame");
        require((extended & overlay_flags) == overlay_flags, "Overlay flags changed");
        require(GetForegroundWindow() != window && GetForegroundWindow() != backing,
                "Controlled window took foreground focus");
        RECT outer{}, client{};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        POINT origin{};
        ClientToScreen(window, &origin);
        require(origin.x == outer.left && origin.y == outer.top &&
                client.right == outer.right - outer.left && client.bottom == outer.bottom - outer.top,
                "Client does not cover the full borderless window");
        require(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(origin.x + 20, origin.y + 20)) == HTTRANSPARENT,
                "Overlay hit-test did not pass through");
        HDC desktop = GetDC(nullptr);
        require(desktop != nullptr, "Read desktop pixels");
        Surface captured(width, height);
        const bool capture_ok = captured.bitmap && captured.previous && captured.previous != HGDI_ERROR &&
            BitBlt(captured.dc, 0, 0, width, height, desktop, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, desktop);
        require(capture_ok, "Capture composited desktop region");
        require(GdiFlush() != 0, "Flush captured DIB writes");
        const auto* captured_pixels = static_cast<const unsigned char*>(captured.pixels);
        unsigned clear = 0, opaque = 0, blended = 0;
        bool matching = true;
        POINT clear_point{}, opaque_point{};
        // Read back the desktop compositor over our own solid background.
        // Canvas stores premultiplied BGRA, including antialiased edges.
        for (unsigned y = 3; y < height && matching; y += 3) {
            for (unsigned x = 3; x < width; x += 3) {
                const auto* expected = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                const auto* observed = &captured_pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                const COLORREF got = RGB(observed[2], observed[1], observed[0]);
                const unsigned inverse_alpha = 255 - expected[3];
                const COLORREF wanted = RGB(
                    expected[2] + ((backing_color & 255) * inverse_alpha + 127) / 255,
                    expected[1] + (((backing_color >> 8) & 255) * inverse_alpha + 127) / 255,
                    expected[0] + (((backing_color >> 16) & 255) * inverse_alpha + 127) / 255);
                if (got == CLR_INVALID || std::abs(int(GetRValue(got)) - int(GetRValue(wanted))) > 2 ||
                    std::abs(int(GetGValue(got)) - int(GetGValue(wanted))) > 2 ||
                    std::abs(int(GetBValue(got)) - int(GetBValue(wanted))) > 2) {
                    std::printf("PIXEL mismatch (%u,%u): got=%08lx wanted=%08lx alpha=%u\n", x,y,got,wanted,expected[3]);
                    matching = false;
                    break;
                }
                if (expected[3] == 0) { ++clear; clear_point = {origin.x + LONG(x), origin.y + LONG(y)}; }
                else if (expected[3] == 255) { ++opaque; opaque_point = {origin.x + LONG(x), origin.y + LONG(y)}; }
                else ++blended;
            }
        }
        require(matching && clear > 0, "Composited overlay differs from controlled frame");
        require(current.segments == 0 || opaque > 0, "No opaque projected line samples");
        require(WindowFromPoint(clear_point) == backing, "Transparent area intercepts mouse targeting");
        require(opaque == 0 || WindowFromPoint(opaque_point) == backing, "Drawn line intercepts mouse targeting");
        canvas->save_png((output / (std::wstring(label) + L".png")).c_str());
        save_composite(output / (std::wstring(label) + L"-desktop.png"), width, height, captured);
        std::printf("PASS overlay %ls: selected=%d descriptor=%d mask=%08x segments=%zu clear=%u opaque=%u blended=%u hit=backing\n",
            label, current.selected, current.descriptor, current.mask, current.segments, clear, opaque, blended);
    }
    void test() {
        switch (step++) {
        case 0:
            present(ControlledCase::normal);
            require(current.selected && current.postprocessed && current.descriptor == 0 && current.segments > 0,
                    "Normal fixture did not traverse pose and projection");
            verify(L"normal");
            normal_pixels = pixels;
            break;
        case 1:
            present(ControlledCase::alternate);
            require(current.descriptor == 1 && pixels != normal_pixels, "Preferred pose did not change the frame");
            verify(L"alternate");
            break;
        case 2:
            present(ControlledCase::behind_camera);
            require(current.selected && current.segments == 0, "Behind-camera pose produced lines");
            verify(L"behind-camera");
            break;
        case 3:
            present(ControlledCase::missing_joints);
            require(current.selected && current.segments > 0 && pixels != normal_pixels,
                    "Missing-joint fixture did not exercise a different visible result");
            verify(L"missing-joints");
            break;
        case 4:
            present(ControlledCase::rejected);
            require(!current.selected && current.mask == 0 && current.segments == 0,
                    "Rejected pose retained old drawing");
            verify(L"rejected");
            break;
        case 5:
            resize(960, 540);
            present(ControlledCase::normal);
            verify(L"resized");
            timer_baseline = sequence;
            break;
        case 6:
            require(sequence > timer_baseline, "Real WM_TIMER did not advance");
            present(ControlledCase::normal);
            verify(L"timer");
            require(KillTimer(window, 1) != 0, "Stop owned overlay timer");
            DestroyWindow(window);
            break;
        }
    }
    void test_desktop(bool blank = false, bool close = true) {
        require(!backing, "Desktop test must have no backing window");
        const HWND foreground = GetForegroundWindow();
        Surface earlier(width, height), before(width, height), visible(width, height), after(width, height);
        const auto capture = [&](Surface& image, DWORD settling = 300) {
            require(image.bitmap && image.previous && image.previous != HGDI_ERROR, "Create desktop capture");
            // Show/hide can animate desktop surfaces such as the taskbar.
            Sleep(settling);
            require(SUCCEEDED(DwmFlush()), "Flush desktop test composition");
            HDC dc = GetDC(nullptr);
            require(dc != nullptr, "Acquire desktop test DC");
            const bool ok = BitBlt(image.dc, 0, 0, width, height, dc, 0, 0, SRCCOPY | CAPTUREBLT) != 0;
            ReleaseDC(nullptr, dc);
            require(ok && GdiFlush(), "Read desktop test pixels");
        };
        ShowWindow(window, SW_HIDE);
        capture(earlier);
        capture(before, 83); // Unequal spacing avoids aliasing periodic loading indicators.
        POINT line_point{};
        bool found_line = false;
        for (unsigned y = 0; y < height && !found_line; ++y)
            for (unsigned x = 0; x < width; ++x)
                if (pixels[(std::size_t(y) * width + x) * 4 + 3] == 255) {
                    line_point = {LONG(x), LONG(y)}; found_line = true; break;
                }
        require(blank ? !found_line : found_line, "Unexpected desktop test line state");
        if (blank) line_point = {LONG(width/2), LONG(height/2)};
        const HWND underlying = WindowFromPoint(line_point);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        capture(visible);
        require(WindowFromPoint(line_point) == underlying && underlying != window,
                "Desktop line changed mouse target");
        ShowWindow(window, SW_HIDE);
        capture(after);
        const auto* a = static_cast<const unsigned char*>(before.pixels);
        const auto* previous = static_cast<const unsigned char*>(earlier.pixels);
        const auto* b = static_cast<const unsigned char*>(visible.pixels);
        const auto* c = static_cast<const unsigned char*>(after.pixels);
        // A moving sprite can cover a pixel only in the middle capture. Exclude
        // neighborhoods of observed changes, not just changed pixel centers.
        const std::size_t stride = std::size_t(width) + 1;
        std::vector<unsigned> changes(stride * (std::size_t(height) + 1));
        for (unsigned y = 0; y < height; ++y) {
            unsigned row = 0;
            for (unsigned x = 0; x < width; ++x) {
                const auto i = (std::size_t(y) * width + x) * 4;
                row += a[i] != c[i] || a[i+1] != c[i+1] || a[i+2] != c[i+2] ||
                       a[i] != previous[i] || a[i+1] != previous[i+1] || a[i+2] != previous[i+2];
                changes[(std::size_t(y)+1)*stride+x+1] = changes[std::size_t(y)*stride+x+1] + row;
            }
        }
        std::size_t clear = 0, opaque = 0, blended = 0, changed = 0, mismatch = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned x = unsigned((i/4) % width), y = unsigned((i/4) / width);
            const unsigned left = x > 32 ? x-32 : 0, top = y > 32 ? y-32 : 0;
            const unsigned right = x+33 < width ? x+33 : width, bottom = y+33 < height ? y+33 : height;
            if (changes[std::size_t(bottom)*stride+right] + changes[std::size_t(top)*stride+left] !=
                changes[std::size_t(bottom)*stride+left] + changes[std::size_t(top)*stride+right]) {
                ++changed; continue;
            }
            const unsigned alpha = pixels[i+3];
            if (blank) require(alpha == 0, "Stopped overlay retained alpha");
            bool equal = true;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned expected = pixels[i+channel] + (a[i+channel] * (255-alpha) + 127) / 255;
                if (std::abs(int(b[i+channel]) - int(expected)) > 2) equal = false;
                if (alpha == 0) require(pixels[i+channel] == 0, "Transparent source contains background color");
            }
            if (!equal) ++mismatch;
            if (alpha == 0) ++clear;
            else if (alpha == 255) ++opaque;
            else ++blended;
        }
        save_composite(output / L"desktop-earlier.png", width, height, earlier);
        save_composite(output / L"desktop-before.png", width, height, before);
        save_composite(output / L"desktop-visible.png", width, height, visible);
        save_composite(output / L"desktop-after.png", width, height, after);
        canvas->save_png((output / L"overlay-alpha.png").c_str());
        std::printf("DESKTOP no backing: %ux%u clear=%zu opaque=%zu blended=%zu volatile_neighborhood=%zu mismatch=%zu hit_unchanged=1\n",
            width,height,clear,opaque,blended,changed,mismatch);
        require(clear > std::size_t(width) * height / 2 && (blank || opaque > 0) && mismatch == 0,
                "Desktop transparency comparison failed or insufficient stable pixels");
        require(GetForegroundWindow() == foreground, "Desktop test changed foreground");
        if (close) {
            // Briefly show the same verified frame without a test background.
            ShowWindow(window, SW_SHOWNOACTIVATE);
            DwmFlush();
            Sleep(3000);
            DestroyWindow(window);
        }
        std::puts("PASS desktop transparency, alpha composition and unchanged mouse target; no backing window");
    }
    void test_mapping() {
        require(!backing, "Mapping test must have no backing window");
        session.stop();
        session.start(1);
        const auto stage = [&](const wchar_t* label, ControlledCase scenario, bool visible,
                               unsigned reads, int joint, int pass, bool hit) {
            present(scenario);
            require(current.selected == visible && current.name_reads == reads &&
                    current.mapping_joint7 == joint && current.pose_pass == pass &&
                    current.mapping_hit == hit, "Mapping window stage contract");
            std::size_t colored = 0;
            for (std::size_t i = 3; i < pixels.size(); i += 4) colored += pixels[i] != 0;
            require(visible ? colored > 0 : colored == 0, "Mapping window retained or lost drawing");
            ShowWindow(window, SW_SHOWNOACTIVATE);
            require(SUCCEEDED(DwmFlush()), "Flush mapping stage");
            const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
            require((style & (WS_CAPTION | WS_THICKFRAME)) == 0 && (style & WS_POPUP) &&
                    (extended & overlay_flags) == overlay_flags, "Mapping overlay window style");
            canvas->save_png((output / (std::wstring(label) + L".png")).c_str());
            std::printf("PASS mapping window %ls: kind=%d joint7=%d pass=%d hit=%d name_reads=%u segments=%zu alpha_pixels=%zu entries=%zu\n",
                label,current.mapping_kind,current.mapping_joint7,current.pose_pass,current.mapping_hit,
                current.name_reads,current.segments,colored,session.mappings().stats().entries);
        };
        stage(L"cold",ControlledCase::normal,true,2,20,1,false);
        stage(L"warm",ControlledCase::normal,true,0,20,0,true);
        stage(L"new-array",ControlledCase::mapping_pointer_changed,true,1,24,0,false);
        session.start(2);
        stage(L"new-context",ControlledCase::mapping_contents_changed,true,1,24,1,false);
        stage(L"invalid-mapping",ControlledCase::invalid_mapping,false,2,-1,-1,false);
        session.stop();
        stage(L"stopped",ControlledCase::normal,false,0,-1,-1,false);
        const auto root = output;
        output /= L"stopped-desktop";
        fs::create_directories(output);
        test_desktop(true,false);
        output = root;
        session.start(2);
        stage(L"restarted",ControlledCase::normal,true,2,20,1,false);
        test_desktop();
    }
    void test_consistency() {
        using reference::ConsistencyStatus;
        struct Case {
            const wchar_t* label;
            ControlledCase scenario;
            ConsistencyStatus status;
            unsigned attempts, clears;
            bool visible;
        };
        const Case cases[]{
            {L"short-before",ControlledCase::short_read_before,ConsistencyStatus::initial_read_failed,0,0,false},
            {L"short-after",ControlledCase::short_read_after,ConsistencyStatus::unstable,2,2,false},
            {L"short-transform",ControlledCase::transform_short_read,ConsistencyStatus::stable,1,0,false},
            {L"short-pose",ControlledCase::pose_short_read,ConsistencyStatus::stable,1,0,false},
            {L"changed-once",ControlledCase::source_changed_once,ConsistencyStatus::stable,2,1,true},
            {L"changing",ControlledCase::source_changing,ConsistencyStatus::unstable,2,2,false},
            {L"mesh-changed",ControlledCase::mesh_changed,ConsistencyStatus::unstable,2,2,false},
            {L"aba-undetected",ControlledCase::address_aba,ConsistencyStatus::stable,1,0,true},
            {L"content-undetected",ControlledCase::content_changed_midread,ConsistencyStatus::stable,1,0,true}
        };
        require(!backing, "Consistency test must have no backing window");
        const HWND foreground = GetForegroundWindow();
        for (const auto& item : cases) {
            session.stop(); session.start(1);
            present(ControlledCase::normal);
            require(current.selected && current.segments == 19, "Consistency window baseline");
            present(item.scenario);
            require(current.consistency == item.status && current.attempts == item.attempts &&
                    current.consistency_clears == item.clears && current.selected == item.visible,
                    "Consistency window stage contract");
            std::size_t colored = 0;
            for (std::size_t i=3;i<pixels.size();i+=4) colored += pixels[i] != 0;
            require(item.visible ? colored > 0 : colored == 0, "Consistency failure retained old pixels");
            require(session.mappings().stats().entries == 1, "Wrapper cleared the independent mapping cache");
            ShowWindow(window, SW_SHOWNOACTIVATE);
            require(SUCCEEDED(DwmFlush()), "Flush consistency stage");
            canvas->save_png((output / (std::wstring(item.label)+L".png")).c_str());
            std::printf("PASS consistency window %ls: attempts=%u clears=%u changes=%u selected=%d segments=%zu alpha_pixels=%zu mapping_entries=%zu\n",
                item.label,current.attempts,current.consistency_clears,current.source_changes,current.selected,
                current.segments,colored,session.mappings().stats().entries);
            if (item.scenario == ControlledCase::short_read_after) {
                const auto root = output;
                output /= L"short-read-desktop";
                fs::create_directories(output);
                test_desktop(true,false);
                output = root;
            }
        }
        require(GetForegroundWindow() == foreground, "Consistency stages changed foreground");
        present(ControlledCase::normal);
        require(current.selected && current.segments == 19, "Consistency window recovery");
        test_desktop();
    }
};

LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case WM_TIMER:
            ++app->sequence;
            if (app->testing) app->test(); else app->present(ControlledCase::normal);
            return 0;
        case WM_HOTKEY: DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            app->session.stop();
            RemovePropW(window, L"NonRudeHWND");
            if (app->hotkey) UnregisterHotKey(window, 1);
            app->surface.reset(); app->canvas.reset();
            PostQuitMessage(app->failed ? 1 : 0);
            return 0;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL overlay: %s\n", error.what());
        app->failed = true;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK backing_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        SetDCBrushColor(dc, backing_color);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    fs::path output;
    const bool desktop_test = argc == 3 && std::wstring(argv[1]) == L"--desktop-test";
    const bool mapping_test = argc == 3 && std::wstring(argv[1]) == L"--mapping-test";
    const bool consistency_test = argc == 3 && std::wstring(argv[1]) == L"--consistency-test";
    const bool no_backing_test = desktop_test || mapping_test || consistency_test;
    if (argc == 3 && (std::wstring(argv[1]) == L"--self-test" || no_backing_test)) output = argv[2];
    else if (argc != 1) { std::puts("Usage: overlay.exe [--self-test|--desktop-test|--mapping-test|--consistency-test output-directory]"); return 2; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    SetProcessDPIAware();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW overlay_type{};
    overlay_type.lpfnWndProc = overlay_proc;
    overlay_type.hInstance = instance;
    overlay_type.lpszClassName = L"RouteBControlledOverlay";
    WNDCLASSW backing_type{};
    backing_type.lpfnWndProc = backing_proc;
    backing_type.hInstance = instance;
    backing_type.lpszClassName = L"RouteBControlledBacking";
    int result = 0;
    DWORD warm_gdi = 0;
    try {
        require(RegisterClassW(&overlay_type) != 0 && RegisterClassW(&backing_type) != 0, "Register overlay classes");
        if (!output.empty()) fs::create_directories(output);
        for (unsigned cycle = 0; cycle < (output.empty() || no_backing_test ? 1u : 3u); ++cycle) {
            Overlay app;
            app.testing = !output.empty() && !no_backing_test;
            app.output = no_backing_test ? output : output / std::to_wstring(cycle);
            if (app.testing) fs::create_directories(app.output);
            app.window = CreateWindowExW(overlay_flags, overlay_type.lpszClassName,
                L"Controlled overlay", WS_POPUP, 0,0,1,1,nullptr,nullptr,instance,&app);
            try {
                require(app.window != nullptr, "Create overlay");
                // A screen-sized transparent overlay must not trigger Shell's
                // fullscreen detection and push the taskbar behind other apps.
                require(SetPropW(app.window, L"NonRudeHWND", reinterpret_cast<HANDLE>(1)) != 0,
                        "Keep overlay out of Shell fullscreen detection");
                if (app.testing) {
                    app.backing = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                        backing_type.lpszClassName, L"Controlled background", WS_POPUP,
                        40,40,640,480,nullptr,nullptr,instance,nullptr);
                    require(app.backing != nullptr, "Create controlled background");
                } else if (!no_backing_test) {
                    app.hotkey = RegisterHotKey(app.window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10) != 0;
                    require(app.hotkey, "Register Ctrl+Alt+F10 close key");
                    std::puts("Controlled data only. Ctrl+Alt+F10 closes the transparent overlay.");
                }
                app.resize(app.testing ? 640u : static_cast<unsigned>(GetSystemMetrics(SM_CXSCREEN)),
                           app.testing ? 480u : static_cast<unsigned>(GetSystemMetrics(SM_CYSCREEN)));
                app.session.start(1);
                app.present(ControlledCase::normal);
                ShowWindow(app.window, SW_SHOWNOACTIVATE);
                if (consistency_test) app.test_consistency();
                else if (mapping_test) app.test_mapping();
                else if (desktop_test) app.test_desktop();
                else require(SetTimer(app.window, 1, app.testing ? 180 : 33, nullptr) != 0, "Start overlay timer");
                MSG message{};
                int received;
                while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                require(received == 0 && !app.failed && message.wParam == 0, "Overlay message loop");
            } catch (...) {
                if (IsWindow(app.window)) DestroyWindow(app.window);
                if (app.backing) DestroyWindow(app.backing);
                throw;
            }
            if (app.backing) DestroyWindow(app.backing);
            require(!IsWindow(app.window) && !app.surface && !app.canvas, "Owned overlay objects still live");
            if (app.testing) {
                const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
                std::printf("CYCLE %u closed: GDI=%lu USER=%lu\n", cycle,gdi,user);
                if (cycle == 0) warm_gdi = gdi;
                else require(gdi == warm_gdi, "Overlay GDI count grew after warm cycle");
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); result = 1;
    }
    UnregisterClassW(overlay_type.lpszClassName, instance);
    UnregisterClassW(backing_type.lpszClassName, instance);
    CoUninitialize();
    return result;
}
