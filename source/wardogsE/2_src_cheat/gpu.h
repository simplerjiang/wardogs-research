/* gpu.h - GPU render backend for the WarDogs overlay.
 *
 * Replaces the GDI path: a 32-bit DIB drawn on the CPU, a full-surface alpha
 * fix-up pass over 3.7M pixels and UpdateLayeredWindow copying 14 MB to DWM
 * every frame. Here the overlay is a DirectComposition visual fed by a flip
 * model swap chain: DWM composites it on the GPU, per-pixel alpha comes free,
 * and nothing is copied through the CPU.
 *
 * Drawing goes through Direct2D (anti-aliased lines, real alpha, DirectWrite
 * text). The chams layer stays on the CPU rasteriser and is uploaded as a
 * texture; its outline is produced by a dilate pass in the pixel shader, which
 * is what makes a 1-2 px border around a silhouette cheap.
 */
#ifndef WDE_GPU_H
#define WDE_GPU_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0xAARRGGBB, straight (not premultiplied) - the backend premultiplies. */
typedef uint32_t RGBA;

#define RRGB(r,g,b)      (0xFF000000u | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define RARGB(a,r,g,b)   (((uint32_t)(a)<<24) | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define RCOLORREF(c)     (0xFF000000u | ((uint32_t)GetRValue(c)<<16) | ((uint32_t)GetGValue(c)<<8) | (uint32_t)GetBValue(c))
#define RA(c)            ((int)(((c)>>24)&0xFF))

/* ---- lifecycle ---- */
int  gpu_init(HWND hwnd, int w, int h);   /* 1 on success, 0 = caller falls back */
void gpu_shutdown(void);
int  gpu_ready(void);
void gpu_size(int w, int h);

/* ---- frame ---- */
void gpu_begin(void);        /* clear to transparent, bind D2D target */
void gpu_present(void);      /* end D2D batch, swap chain Present */

/* ---- state ---- */
void gpu_pen(RGBA c, float width);
void gpu_font(float px);     /* text size for the next r_text calls */

/* ---- primitives (screen space, pixels) ---- */
void r_line(float x1, float y1, float x2, float y2);
void r_line_c(float x1, float y1, float x2, float y2, RGBA c, float w);
void r_rect(float x, float y, float w, float h, RGBA c, float stroke);
void r_fill(float x, float y, float w, float h, RGBA c);
void r_ellipse(float cx, float cy, float rx, float ry, RGBA c, float stroke);
void r_fill_ellipse(float cx, float cy, float rx, float ry, RGBA c);
void r_polyline(const float* xy, int n, RGBA c, float w);

/* text: utf-8, (x,y) is the top-left of the line box unless align says
   otherwise. outline draws a dark halo so labels stay readable over snow,
   sky and muzzle flash - the same reason the GDI path drew 8 offsets. */
#define RT_LEFT    0
#define RT_CENTER  1
#define RT_RIGHT   2
#define RT_OUTLINE 4
void r_text(float x, float y, const char* utf8, RGBA c, int flags);
void r_text_w(float x, float y, const wchar_t* s, int len, RGBA c, int flags);
float r_text_width(const char* utf8);
float r_text_height(void);

/* ---- chams layer ----
 * The CPU rasteriser draws into its own buffer; only the box that changed is
 * uploaded. outline_px dilates the silhouette in the shader, which is what a
 * chams border is: the silhouette grown by N pixels, drawn behind the fill. */
void gpu_chams_alloc(int w, int h);
void gpu_chams_upload(const uint32_t* px, int w, int h, int stride,
                      int x0, int y0, int x1, int y1);
void gpu_chams_draw(int outline_px, RGBA outline_col, float opacity);

#ifdef __cplusplus
}
#endif

#endif
