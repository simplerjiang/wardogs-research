#include "render2d.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace reconstruction {
namespace {
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << ": HRESULT 0x" << std::hex
                << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}
D2D1_COLOR_F native(Color c) { return {c.r, c.g, c.b, c.a}; }
}

void DrawList::line(Point from, Point to, Color c, float width) {
    items.push_back({Kind::line, from, to, c, width, {}});
}
void DrawList::rectangle(Point from, Point to, Color c) {
    items.push_back({Kind::rectangle, from, to, c, 0, {}});
}
void DrawList::outline(Point from, Point to, Color c, float width) {
    line(from, {to.x, from.y}, c, width);
    line({to.x, from.y}, to, c, width);
    line(to, {from.x, to.y}, c, width);
    line({from.x, to.y}, from, c, width);
}
void DrawList::text(Point from, Point to, std::wstring value, Color c, float size) {
    items.push_back({Kind::text, from, to, c, size, std::move(value)});
}
void DrawList::circle(Point center, float radius, Color c, float width) {
    constexpr double tau = 6.283185307179586;
    Point previous{center.x + radius, center.y};
    for (int i = 1; i <= 48; ++i) {
        const double angle = tau * i / 48;
        const Point next{center.x + radius * static_cast<float>(std::cos(angle)),
                         center.y - radius * static_cast<float>(std::sin(angle))};
        line(previous, next, c, width);
        previous = next;
    }
}

Canvas::Canvas(unsigned width, unsigned height) : width_(width), height_(height) {
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(wic_.GetAddressOf())), "Create WIC factory");
    check(wic_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapCacheOnLoad, &bitmap_), "Create bitmap");
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()),
          "Create D2D factory");
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(write_.GetAddressOf())),
          "Create text factory");
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    check(d2d_->CreateWicBitmapRenderTarget(bitmap_.Get(), properties, &target_),
          "Create bitmap render target");
    check(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_), "Create brush");
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void Canvas::render(const DrawList& list, Color background) {
    // Prepare fallible text resources before BeginDraw so an exception cannot
    // leave the render target inside a drawing transaction.
    std::vector<Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats(list.items.size());
    for (size_t i = 0; i < list.items.size(); ++i) {
        const auto& item = list.items[i];
        if (item.kind == Kind::text) {
            check(write_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, item.size, L"en-us",
                &formats[i]), "Create text format");
            check(formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Set wrapping");
        }
    }
    target_->BeginDraw();
    target_->Clear(native(background));
    for (size_t i = 0; i < list.items.size(); ++i) {
        const auto& item = list.items[i];
        brush_->SetColor(native(item.color));
        const auto bounds = D2D1::RectF(item.from.x, item.from.y, item.to.x, item.to.y);
        switch (item.kind) {
        case Kind::line:
            target_->DrawLine(D2D1::Point2F(item.from.x, item.from.y),
                              D2D1::Point2F(item.to.x, item.to.y), brush_.Get(), item.size);
            break;
        case Kind::rectangle:
            target_->FillRectangle(bounds, brush_.Get());
            break;
        case Kind::text:
            target_->DrawText(item.text.data(), static_cast<UINT32>(item.text.size()),
                              formats[i].Get(), bounds, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            break;
        }
    }
    check(target_->EndDraw(), "Render primitives");
}

std::vector<unsigned char> Canvas::pixels() const {
    std::vector<unsigned char> data(static_cast<size_t>(width_) * height_ * 4);
    check(bitmap_->CopyPixels(nullptr, width_ * 4, static_cast<UINT>(data.size()), data.data()),
          "Read bitmap pixels");
    return data;
}

void Canvas::save_png(const wchar_t* path) const {
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    check(wic_->CreateStream(&stream), "Create PNG stream");
    check(stream->InitializeFromFilename(path, GENERIC_WRITE), "Open PNG output");
    check(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
    check(frame->Initialize(nullptr), "Initialize PNG frame");
    check(frame->SetSize(width_, height_), "Set PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Set PNG format");
    check(frame->WriteSource(bitmap_.Get(), nullptr), "Write PNG pixels");
    check(frame->Commit(), "Commit PNG frame");
    check(encoder->Commit(), "Commit PNG file");
}

} // namespace reconstruction
