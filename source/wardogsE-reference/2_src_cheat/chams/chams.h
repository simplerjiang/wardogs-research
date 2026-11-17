/* chams.h — external read-only player chams for WarDogs.
 *
 * Geometry is unpacked OFFLINE from the game's .pak (CUE4Parse -> mesh.bin).
 * At runtime only the bone transforms are read from live memory, and the
 * skinning + rasterisation happen on OUR side, straight into the overlay
 * surface. Nothing is written to the game.
 *
 * Typical use inside the overlay's render loop:
 *
 *     ch_load("meshdump\\mesh.bin");
 *     ...
 *     ch_surface(dib_pixels, g_sw, g_sh, g_sw);
 *     ch_camera(&g_cam, &g_camrot, g_fov, g_sw, g_sh);
 *     ch_begin();
 *     for each player:
 *         ch_player(bones, bone_cnt, &c2w_rot, &c2w_pos, &c2w_scl, colour);
 *     ch_end();
 */
#ifndef CHAMS_H
#define CHAMS_H

#include <stdint.h>

typedef struct { double x, y, z; } CH_V3;
typedef struct { double x, y, z, w; } CH_Q4;

/* One live FTransform as stored in ComponentSpaceTransforms (stride 0x60). */
typedef struct { CH_Q4 rot; CH_V3 pos; CH_V3 scl; } CH_XF;

/* mode */
#define CH_OFF      0
#define CH_SOLID    1
#define CH_WIRE     2
#define CH_SOLID_WIRE 3

typedef struct {
    int      mode;        /* CH_OFF / CH_SOLID / CH_WIRE / CH_SOLID_WIRE */
    int      depth;       /* 1 = z-buffer, hidden faces stay hidden
                             0 = through walls (whole silhouette) */
    int      backface;    /* 1 = cull back faces (solid only) */
    int      color_mode;  /* 0 = fixed `col`, 1 = colour passed per player */
    uint32_t col;         /* 0xAARRGGBB, used when color_mode == 0 */
    double   max_dist;    /* metres; 0 = no limit */
    int      min_h_px;    /* skip players shorter than this on screen */
    int      wire_alpha;  /* 0..255, wireframe line alpha */
    int      fill_alpha;  /* 0..255, solid fill alpha */
    int      tri_step;    /* draw every Nth triangle, 1 = all (perf knob) */
} CH_CFG;

/* Load geometry once. Returns 1 on success. */
int  ch_load(const char* mesh_bin_path);
/* Same, with vertex-cluster decimation: cluster_cm is the grid cell size in
 * centimetres (2.0 is a good live value, 0 keeps full detail). */
int  ch_load_ex(const char* mesh_bin_path, double cluster_cm);

/* Optional override: live skeleton index for each of the mesh's bones.
 * Without it the module assumes the mesh bone order matches the live one
 * (verified 177/177 on the Manticore preset). */
void ch_bone_map(const int* live_index, int n);

void ch_cfg(const CH_CFG* c);

/* Overlay target: 32-bit 0xAARRGGBB pixels, `stride` in pixels. */
void ch_surface(uint32_t* pixels, int w, int h, int stride);
/* Erase the previous frame's chams from the surface. The cheat clears only its
 * dirty rectangle, so call this once per frame BEFORE that clear. */
void ch_undo(void);
/* Size the undo log (entries; 2M is ~1.5 ms of work per frame). */
int  ch_undo_log(int entries);
int  ch_undo_lost(void);

/* Camera, same convention as the rest of the overlay (degrees). */
void ch_camera(const CH_V3* loc, const CH_V3* rot_deg, double fov, int sw, int sh);
/* Optional: the game's aspect. MUST match the value the host overlay projects
   with, otherwise the mesh lands in the wrong place. fov is HORIZONTAL
   (UE GetFOVAngle), same convention as the host's own world-to-screen. */
void ch_aspect(double a);

void ch_begin(void);   /* clears the depth buffer for a new frame */

/* Queue-and-draw one player.
 *   bones  — live ComponentSpaceTransforms (count entries, stride 0x60)
 *   c2w_*  — the mesh component's ComponentToWorld
 *   colour — 0xAARRGGBB, used when cfg.color_mode == 1
 * Returns the number of triangles rasterised (0 = skipped). */
int  ch_player(const CH_XF* bones, int count,
               const CH_Q4* c2w_rot, const CH_V3* c2w_pos, const CH_V3* c2w_scl,
               uint32_t colour);

void ch_end(void);

/* Dirty rectangle since the previous call, exclusive on x1/y1. The overlay
 * uploads only this box to the GPU texture; call it once per presented frame. */
void ch_bbox(int* x0, int* y0, int* x1, int* y1);

/* Test/support helper: fill `out` with the mesh's bind pose in component space
 * (count = ch_bone_count()). Feeding it back through ch_player renders the
 * model undeformed, which is how the offline preview is produced. */
int  ch_bind_pose(CH_XF* out, int cap);

/* Diagnostics */
int  ch_vertex_count(void);
int  ch_triangle_count(void);
int  ch_bone_count(void);
const char* ch_bone_name(int i);
int  ch_last_skipped(void);

#endif /* CHAMS_H */
