#pragma once

#include <string>
#include <vector>
#include <wincodec.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace reconstruction {

struct Point { float x, y; };
struct Color { float r, g, b, a = 1; };
enum class Kind { line, rectangle, text };

struct Primitive {
    Kind kind;
    Point from, to;
    Color color;
    float size; // Line width or text size in pixels at 96 DPI.
    std::wstring text;
};

// Screen coordinates; the producer owns projection and application semantics.
struct DrawList {
    std::vector<Primitive> items;
    void line(Point from, Point to, Color color, float width = 1);
    void rectangle(Point top_left, Point bottom_right, Color color);
    void outline(Point top_left, Point bottom_right, Color color, float width = 1);
    void text(Point top_left, Point bottom_right, std::wstring value,
              Color color, float size = 16);
    void circle(Point center, float radius, Color color, float width = 1);
};

// Caller initializes COM. This backend renders only into its own WIC bitmap.
class Canvas {
public:
    Canvas(unsigned width, unsigned height);
    void render(const DrawList& list, Color background);
    std::vector<unsigned char> pixels() const; // Top-down premultiplied BGRA.
    void save_png(const wchar_t* path) const;

private:
    unsigned width_, height_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
};

} // namespace reconstruction
