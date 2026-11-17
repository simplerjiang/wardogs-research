/* chams.c — external read-only player chams. See chams.h for the API contract. */
#include "chams.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_PI 3.14159265358979323846
#define CH_D2R (CH_PI / 180.0)

/* ------------------------------------------------------------------ geometry */

static float*    g_pos;        /* nv*3  bind-space positions        */
static uint8_t*  g_bone;       /* nv*8  GLOBAL bone index per influence */
static float*    g_wgt;        /* nv*8  influence weights           */
static uint32_t* g_idx;        /* ni    triangle indices            */
static int       g_nv, g_ni, g_ns, g_nb;

static char     g_bone_names[1024][128];
static int*      g_parent;     /* nb    parent index, -1 = root     */
static CH_Q4*    g_bindR;      /* nb    local rotation              */
static CH_V3*    g_bindT;      /* nb    local translation           */
static CH_Q4*    g_bindCSR;    /* nb    component-space rotation    */
static CH_V3*    g_bindCST;    /* nb    component-space translation */
static int*      g_map;        /* nb    mesh bone -> live bone index */

/* ------------------------------------------------------------- render state */

static uint32_t* g_px;
static int       g_w, g_h, g_stride;

/* Undo log: every pixel the chams touch is recorded with its previous value so
 * the next frame can erase itself. The cheat clears the DIB only on its dirty
 * rectangle, which does not include a moving player's body, so without this the
 * chams would leave a trail of frozen poses behind them. */
typedef struct { uint32_t off, old; } CH_UNDO;
static CH_UNDO*  g_undo;
static int       g_undo_n, g_undo_cap, g_undo_on, g_undo_lost;
static float*    g_zbuf;
static size_t    g_zcap;

static CH_CFG    g_cfg;
static CH_V3     g_cam, g_camrot;
static double    g_fov;
static double    g_aspect;        /* game aspect from the cheat; 0 = derive from sw/sh */
static int       g_sw, g_sh;

static double*   g_sx;         /* per-vertex screen x  (scratch)    */
static double*   g_sy;         /* per-vertex screen y  (scratch)    */
static double*   g_sz;         /* per-vertex view depth (scratch)   */
static double*   g_vx, *g_vy, *g_vz;   /* per-vertex world pos (scratch) */
static size_t    g_vcap;

static int       g_last_skipped;
static double    g_cluster_cm = 0.0;   /* vertex-cluster cell size, 0 = off */

/* ------------------------------------------------------------------- helpers */

static CH_V3 v3(double x, double y, double z) { CH_V3 r; r.x = x; r.y = y; r.z = z; return r; }

static CH_Q4 qnorm(CH_Q4 q)
{
    double n = sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (n < 1e-12) { q.x = q.y = q.z = 0.0; q.w = 1.0; return q; }
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    return q;
}

static CH_Q4 qconj(CH_Q4 q) { CH_Q4 r; r.x = -q.x; r.y = -q.y; r.z = -q.z; r.w = q.w; return r; }

static CH_Q4 qmul(CH_Q4 a, CH_Q4 b)
{
    CH_Q4 r;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    return r;
}

static CH_V3 qrot(CH_Q4 q, CH_V3 v)
{
    double tx = 2.0*(q.y*v.z - q.z*v.y);
    double ty = 2.0*(q.z*v.x - q.x*v.z);
    double tz = 2.0*(q.x*v.y - q.y*v.x);
    return v3(v.x + q.w*tx + (q.y*tz - q.z*ty),
              v.y + q.w*ty + (q.z*tx - q.x*tz),
              v.z + q.w*tz + (q.x*ty - q.y*tx));
}

/* UE rotator (pitch, yaw, roll in degrees) -> rows {forward, right, up}. */
static void rot_matrix(const CH_V3* r, double m[3][3])
{
    double sp = sin(r->x*CH_D2R), cp = cos(r->x*CH_D2R);
    double sy = sin(r->y*CH_D2R), cy = cos(r->y*CH_D2R);
    double sr = sin(r->z*CH_D2R), cr = cos(r->z*CH_D2R);
    m[0][0] =  cp*cy;                m[0][1] =  cp*sy;                m[0][2] = sp;
    m[1][0] =  sr*sp*cy - cr*sy;     m[1][1] =  sr*sp*sy + cr*cy;     m[1][2] = -sr*cp;
    m[2][0] = -(cr*sp*cy + sr*sy);   m[2][1] =  cy*sr - cr*sp*sy;     m[2][2] = cr*cp;
}

/* ---------------------------------------------------------------- mesh load */

static void ch_free(void)
{
    free(g_pos);   g_pos = NULL;
    free(g_bone);  g_bone = NULL;
    free(g_wgt);   g_wgt = NULL;
    free(g_idx);   g_idx = NULL;
    free(g_parent);g_parent = NULL;
    free(g_bindR); g_bindR = NULL;
    free(g_bindT); g_bindT = NULL;
    free(g_bindCSR); g_bindCSR = NULL;
    free(g_bindCST); g_bindCST = NULL;
    free(g_map);   g_map = NULL;
}

static uint8_t* slurp(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t* b = (uint8_t*)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return b;
}

/* Vertex-cluster decimation. Snapping vertices to a grid and merging each cell
 * into one representative cuts LOD0's 110k triangles down to a few thousand,
 * which is what makes per-frame skinning affordable with a dozen players on
 * screen. Collapsed triangles are dropped; the cell is small enough (2cm) that
 * the representative's bone weights still describe the whole cell. */
static void ch_decimate(double cell)
{
    if (cell <= 0.0 || g_nv <= 0 || !g_pos) return;

    int cap = 1;
    while (cap < g_nv * 2) cap <<= 1;
    int*   table = (int*)malloc((size_t)cap * sizeof(int));
    int*   remap = (int*)malloc((size_t)g_nv * sizeof(int));
    int*   first = (int*)malloc((size_t)g_nv * sizeof(int));
    int*   cnt   = (int*)malloc((size_t)g_nv * sizeof(int));
    float* npos  = (float*)malloc((size_t)g_nv * 3 * sizeof(float));
    uint8_t* nbone = NULL;
    float*   nwgt  = NULL;
    uint32_t* nidx = NULL;
    if (!table || !remap || !first || !cnt || !npos) {
        free(table); free(remap); free(first); free(cnt); free(npos);
        return;
    }
    for (int i = 0; i < cap; i++) table[i] = -1;

    double inv = 1.0 / cell;
    int nnew = 0;
    for (int i = 0; i < g_nv; i++) {
        long long cx = (long long)floor(g_pos[i*3+0] * inv);
        long long cy = (long long)floor(g_pos[i*3+1] * inv);
        long long cz = (long long)floor(g_pos[i*3+2] * inv);
        unsigned long long h = (unsigned long long)(cx * 73856093LL)
                             ^ (unsigned long long)(cy * 19349663LL)
                             ^ (unsigned long long)(cz * 83492791LL);
        unsigned mask = (unsigned)(cap - 1);
        unsigned slot = (unsigned)(h & mask);
        int found = -1;
        while (table[slot] != -1) {
            int ci = table[slot];
            int s = first[ci];
            long long ox = (long long)floor(g_pos[s*3+0] * inv);
            long long oy = (long long)floor(g_pos[s*3+1] * inv);
            long long oz = (long long)floor(g_pos[s*3+2] * inv);
            if (ox == cx && oy == cy && oz == cz) { found = ci; break; }
            slot = (slot + 1) & mask;
        }
        if (found < 0) {
            found = nnew++;
            table[slot] = found;
            first[found] = i;
            cnt[found] = 0;
            npos[found*3+0] = npos[found*3+1] = npos[found*3+2] = 0.f;
        }
        remap[i] = found;
        npos[found*3+0] += g_pos[i*3+0];
        npos[found*3+1] += g_pos[i*3+1];
        npos[found*3+2] += g_pos[i*3+2];
        cnt[found]++;
    }

    nbone = (uint8_t*)malloc((size_t)nnew * 8);
    nwgt  = (float*)malloc((size_t)nnew * 8 * sizeof(float));
    nidx  = (uint32_t*)malloc((size_t)(g_ni ? g_ni : 3) * sizeof(uint32_t));
    if (!nbone || !nwgt || !nidx) {
        free(table); free(remap); free(first); free(cnt); free(npos);
        free(nbone); free(nwgt); free(nidx);
        return;
    }
    for (int c = 0; c < nnew; c++) {
        int s = first[c];
        memcpy(nbone + c*8, g_bone + s*8, 8);
        memcpy(nwgt  + c*8, g_wgt  + s*8, 8 * sizeof(float));
        npos[c*3+0] /= (float)cnt[c];
        npos[c*3+1] /= (float)cnt[c];
        npos[c*3+2] /= (float)cnt[c];
    }

    int nni = 0;
    for (int t = 0; t + 2 < g_ni; t += 3) {
        uint32_t a = (uint32_t)remap[g_idx[t]];
        uint32_t b = (uint32_t)remap[g_idx[t+1]];
        uint32_t c = (uint32_t)remap[g_idx[t+2]];
        if (a == b || b == c || a == c) continue;
        nidx[nni++] = a; nidx[nni++] = b; nidx[nni++] = c;
    }

    int ov = g_nv, ot = g_ni / 3;
    free(g_pos); free(g_bone); free(g_wgt); free(g_idx);
    g_pos = npos; g_bone = nbone; g_wgt = nwgt; g_idx = nidx;
    g_nv = nnew; g_ni = nni;
    g_vcap = 0;                                    /* scratch reallocated on demand */

    printf("[chams] decimate %.2f cm: %d verts / %d tris -> %d verts / %d tris\n",
           cell, ov, ot, g_nv, g_ni / 3);

    free(table); free(remap); free(first); free(cnt);
}

int ch_load_ex(const char* mesh_bin_path, double cluster_cm)
{
    g_cluster_cm = cluster_cm;
    return ch_load(mesh_bin_path);
}

int ch_load(const char* mesh_bin_path)
{
    size_t size = 0;
    uint8_t* d = slurp(mesh_bin_path, &size);
    if (!d) return 0;

    uint32_t hdr[6];
    memcpy(hdr, d, sizeof(hdr));
    int nv = (int)hdr[2], ni = (int)hdr[3], ns = (int)hdr[4], nb = (int)hdr[5];
    if (nv <= 0 || ni <= 0 || nb <= 0) { free(d); return 0; }

    ch_free();
    g_nv = nv; g_ni = ni; g_ns = ns; g_nb = nb;

    g_pos  = (float*)malloc((size_t)nv*3*sizeof(float));
    g_bone = (uint8_t*)malloc((size_t)nv*8);
    g_wgt  = (float*)malloc((size_t)nv*8*sizeof(float));
    g_idx  = (uint32_t*)malloc((size_t)ni*sizeof(uint32_t));
    if (!g_pos || !g_bone || !g_wgt || !g_idx) { free(d); ch_free(); return 0; }

    size_t o = 24;
    memcpy(g_pos, d + o, (size_t)nv*3*sizeof(float));      o += (size_t)nv*12;
    o += (size_t)nv*8;                                     /* uv      */
    o += (size_t)nv*12;                                    /* normals */

    /* Raw influences: the bone index is SECTION-LOCAL. Resolve it here, once,
       through FSkelMeshRenderSection::BoneMap so the per-frame loop is a plain
       lookup. Reading them as global indices is the classic "mesh explodes and
       only bones 0..66 get any weight" bug. */
    {
        uint8_t* raw_bone = (uint8_t*)malloc((size_t)nv*8);
        float*   raw_wgt  = (float*)malloc((size_t)nv*8*sizeof(float));
        if (!raw_bone || !raw_wgt) { free(raw_bone); free(raw_wgt); free(d); ch_free(); return 0; }
        for (int v = 0; v < nv; v++) {
            for (int k = 0; k < 8; k++) {
                raw_bone[v*8+k] = d[o];
                memcpy(&raw_wgt[v*8+k], d + o + 1, 4);
                o += 5;
            }
        }
        memcpy(g_idx, d + o, (size_t)ni*4);
        o += (size_t)ni*4;

        /* sections */
        int* sec_bv = (int*)malloc((size_t)ns*sizeof(int));
        int* sec_nv = (int*)malloc((size_t)ns*sizeof(int));
        int** sec_map = (int**)calloc((size_t)ns, sizeof(int*));
        int*  sec_mlen = (int*)calloc((size_t)ns, sizeof(int));
        if (!sec_bv || !sec_nv || !sec_map || !sec_mlen) {
            free(raw_bone); free(raw_wgt); free(d);
            free(sec_bv); free(sec_nv); free(sec_map); free(sec_mlen);
            ch_free(); return 0;
        }
        for (int s = 0; s < ns; s++) {
            int fields[7];
            memcpy(fields, d + o, 28); o += 28;
            sec_bv[s]   = fields[3];
            sec_nv[s]   = fields[4];
            sec_mlen[s] = fields[6];
            if (sec_mlen[s] > 0) {
                sec_map[s] = (int*)malloc((size_t)sec_mlen[s]*sizeof(int));
                if (sec_map[s]) memcpy(sec_map[s], d + o, (size_t)sec_mlen[s]*4);
            }
            o += (size_t)sec_mlen[s]*4;
        }

        memset(g_bone, 0, (size_t)nv*8);
        memset(g_wgt,  0, (size_t)nv*8*sizeof(float));
        for (int s = 0; s < ns; s++) {
            int v0 = sec_bv[s], v1 = sec_bv[s] + sec_nv[s];
            if (v1 > nv) v1 = nv;
            for (int v = v0; v < v0 + (v1 - v0); v++) {
                if (v < 0) continue;
                for (int k = 0; k < 8; k++) {
                    int loc = raw_bone[v*8+k];
                    int g = loc;
                    if (sec_map[s] && loc < sec_mlen[s]) g = sec_map[s][loc];
                    g_bone[v*8+k] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
                    g_wgt[v*8+k]  = raw_wgt[v*8+k];
                }
            }
        }
        for (int s = 0; s < ns; s++) free(sec_map[s]);
        free(sec_map); free(sec_mlen); free(sec_bv); free(sec_nv);
        free(raw_bone); free(raw_wgt);
    }

    /* bones: names + parents, then local transforms */
    g_parent = (int*)malloc((size_t)nb*sizeof(int));
    g_bindR  = (CH_Q4*)malloc((size_t)nb*sizeof(CH_Q4));
    g_bindT  = (CH_V3*)malloc((size_t)nb*sizeof(CH_V3));
    if (!g_parent || !g_bindR || !g_bindT) { free(d); ch_free(); return 0; }

    for (int i = 0; i < nb; i++) {
        int ln = d[o++];
        if (i >= 1024 || o + (size_t)ln + 4 > size) { free(d); ch_free(); return 0; }
        int copy = ln < 127 ? ln : 127;
        memcpy(g_bone_names[i], d + o, copy);
        g_bone_names[i][copy] = 0;
        o += (size_t)ln;
        memcpy(&g_parent[i], d + o, 4); o += 4;
    }
    for (int i = 0; i < nb; i++) {
        float t[3], r[4];
        memcpy(t, d + o, 12); o += 12;
        memcpy(r, d + o, 16); o += 16;
        o += 12;                                   /* local scale, unused */
        g_bindT[i] = v3(t[0], t[1], t[2]);
        CH_Q4 bind_q = {r[0], r[1], r[2], r[3]};
        g_bindR[i] = qnorm(bind_q);
    }
    free(d);

    /* bind pose in COMPONENT space: parent transform applied to the local one */
    g_bindCSR = (CH_Q4*)malloc((size_t)nb*sizeof(CH_Q4));
    g_bindCST = (CH_V3*)malloc((size_t)nb*sizeof(CH_V3));
    g_map     = (int*)malloc((size_t)nb*sizeof(int));
    if (!g_bindCSR || !g_bindCST || !g_map) { ch_free(); return 0; }
    for (int i = 0; i < nb; i++) {
        int p = g_parent[i];
        if (p < 0 || p >= i) {
            g_bindCSR[i] = g_bindR[i];
            g_bindCST[i] = g_bindT[i];
        } else {
            g_bindCSR[i] = qnorm(qmul(g_bindCSR[p], g_bindR[i]));
            g_bindCST[i] = v3(g_bindCST[p].x + qrot(g_bindCSR[p], g_bindT[i]).x,
                              g_bindCST[p].y + qrot(g_bindCSR[p], g_bindT[i]).y,
                              g_bindCST[p].z + qrot(g_bindCSR[p], g_bindT[i]).z);
        }
        g_map[i] = i;                              /* default: same order */
    }
    ch_decimate(g_cluster_cm);                     /* 0 = keep full detail */
    return 1;
}

void ch_bone_map(const int* live_index, int n)
{
    if (!g_map || !live_index) return;
    for (int i = 0; i < g_nb && i < n; i++) g_map[i] = live_index[i];
}

void ch_cfg(const CH_CFG* c) { if (c) g_cfg = *c; }

void ch_surface(uint32_t* pixels, int w, int h, int stride)
{
    if (pixels != g_px || w != g_w || h != g_h || stride != g_stride) {
        g_undo_n = 0;                       /* old offsets are meaningless now */
    }
    g_px = pixels; g_w = w; g_h = h; g_stride = stride > 0 ? stride : w;
    size_t need = (size_t)g_h * (size_t)g_stride;
    if (need > g_zcap) {
        float* z = (float*)realloc(g_zbuf, need*sizeof(float));
        if (!z) return;
        g_zbuf = z; g_zcap = need;
    }
}

/* Dirty box since the last ch_bbox() call. The overlay uploads only this
 * rectangle to the GPU texture instead of the whole 1440p surface, so the cost
 * of a chams frame is proportional to the pixels actually drawn. */
static int g_bx0 = 0, g_by0 = 0, g_bx1 = -1, g_by1 = -1;

static void bbox_touch(int p)
{
    if (!g_w || p < 0) return;
    int x = p % g_w, y = p / g_w;
    if (g_bx1 < 0) { g_bx0 = g_bx1 = x; g_by0 = g_by1 = y; return; }
    if (x < g_bx0) g_bx0 = x;
    if (x > g_bx1) g_bx1 = x;
    if (y < g_by0) g_by0 = y;
    if (y > g_by1) g_by1 = y;
}

void ch_bbox(int* x0, int* y0, int* x1, int* y1)
{
    if (x0) *x0 = g_bx0;
    if (y0) *y0 = g_by0;
    if (x1) *x1 = g_bx1 + 1;      /* exclusive */
    if (y1) *y1 = g_by1 + 1;
    g_bx1 = -1;                   /* start a new frame */
}

void ch_undo(void)
{
    if (!g_px || g_undo_n <= 0) return;
    for (int i = g_undo_n - 1; i >= 0; i--) {    /* reverse: last write wins */
        g_px[g_undo[i].off] = g_undo[i].old;
        bbox_touch((int)g_undo[i].off);          /* erased pixels must reach the GPU too */
    }
    g_undo_n = 0;
}

int ch_undo_log(int entries)
{
    if (entries < 1024) entries = 1024;
    CH_UNDO* p = (CH_UNDO*)realloc(g_undo, (size_t)entries * sizeof(CH_UNDO));
    if (!p) return 0;
    g_undo = p; g_undo_cap = entries; g_undo_n = 0;
    g_undo_on = 1; g_undo_lost = 0;
    return entries;
}

int ch_undo_lost(void) { return g_undo_lost; }

void ch_camera(const CH_V3* loc, const CH_V3* rot_deg, double fov, int sw, int sh)
{
    if (loc) g_cam = *loc;
    if (rot_deg) g_camrot = *rot_deg;
    g_fov = fov;
    g_sw = sw; g_sh = sh;
}

/* The game's own aspect (UWorld/g_aspect), not the window's. The cheat projects
   with this exact value, so anything else lands the mesh somewhere else. */
void ch_aspect(double a)
{
    g_aspect = (a > 1.0 && a < 3.5) ? a : 0.0;
}

void ch_begin(void)
{
    if (g_zbuf && g_zcap) {
        for (size_t i = 0; i < (size_t)g_h*(size_t)g_stride; i++) g_zbuf[i] = 1e30f;
    }
}

int ch_vertex_count(void)   { return g_nv; }
int ch_triangle_count(void) { return g_ni / 3; }
int ch_bone_count(void)     { return g_nb; }
const char* ch_bone_name(int i) { return i >= 0 && i < g_nb && i < 1024 ? g_bone_names[i] : ""; }
int ch_last_skipped(void)   { return g_last_skipped; }

/* --------------------------------------------------------------- projection */

static int project(const CH_V3* w, double* ox, double* oy, double* odepth)
{
    CH_V3 d = v3(w->x - g_cam.x, w->y - g_cam.y, w->z - g_cam.z);
    double m[3][3];
    rot_matrix(&g_camrot, m);
    double fwd = d.x*m[0][0] + d.y*m[0][1] + d.z*m[0][2];
    double rgt = d.x*m[1][0] + d.y*m[1][1] + d.z*m[1][2];
    double up  = d.x*m[2][0] + d.y*m[2][1] + d.z*m[2][2];
    if (fwd <= 1.0) return 0;
    /* FOV convention MUST mirror w2s_a(): UE's GetFOVAngle() is HORIZONTAL, so
       tanH = tan(fov/2) and tanV = tanH/aspect. Treating fov as vertical (the
       old code) scaled both axes by `aspect` and squashed every silhouette. */
    double tanH = tan(g_fov*CH_D2R*0.5);
    if (tanH <= 0.0) return 0;
    double aspect = g_aspect;
    if (!(aspect > 1.0 && aspect < 3.5))
        aspect = (g_sh > 0) ? (double)g_sw/(double)g_sh : 1.7777778;
    if (!(aspect > 1.0 && aspect < 3.5)) aspect = 1.7777778;
    double tanV = tanH / aspect;
    if (fabs(rgt) > fwd*tanH*4.0) return 0;
    if (fabs(up)  > fwd*tanV*4.0) return 0;
    double cx = g_sw*0.5, cy = g_sh*0.5;
    *ox = cx + (rgt/fwd/tanH)*cx;
    *oy = cy - (up /fwd/tanV)*cy;
    *odepth = fwd;
    return 1;
}

/* -------------------------------------------------------------- rasterisers */

static void bbox_touch(int p);

static void undo_push(int p)
{
    bbox_touch(p);            /* every pixel we write is a pixel to upload */
    if (!g_undo_on) return;
    if (g_undo_n >= g_undo_cap) { g_undo_lost++; return; }
    g_undo[g_undo_n].off = (uint32_t)p;
    g_undo[g_undo_n].old = g_px[p];
    g_undo_n++;
}

static void blend_px(int p, uint32_t c, int alpha)
{
    if (!g_px || p < 0) return;
    uint32_t sa = (uint32_t)((c >> 24) & 0xFF);
    if (alpha >= 0) sa = (sa * (uint32_t)alpha) / 255u;
    if (sa == 0) return;
    undo_push(p);
    uint32_t sr = ((c >> 16) & 0xFF) * sa / 255u;
    uint32_t sg = ((c >>  8) & 0xFF) * sa / 255u;
    uint32_t sb = ( c        & 0xFF) * sa / 255u;
    if (sa >= 255u) { g_px[p] = 0xFF000000u | (sr << 16) | (sg << 8) | sb; return; }
    uint32_t d = g_px[p];
    uint32_t da = (d >> 24) & 0xFF;
    uint32_t dr = ((d >> 16) & 0xFF) * (255u - sa) / 255u;
    uint32_t dg = ((d >>  8) & 0xFF) * (255u - sa) / 255u;
    uint32_t db = ( d        & 0xFF) * (255u - sa) / 255u;
    uint32_t oa = sa + da*(255u - sa)/255u;
    g_px[p] = (oa << 24) | ((sr + dr) << 16) | ((sg + dg) << 8) | (sb + db);
}

static void tri_fill(double x0, double y0, double z0,
                     double x1, double y1, double z1,
                     double x2, double y2, double z2,
                     uint32_t col, int depth_on, int cull)
{
    double area = (x1 - x0)*(y2 - y0) - (x2 - x0)*(y1 - y0);
    if (cull && area >= 0.0) return;               /* screen y grows downward */
    if (fabs(area) < 1e-9) return;
    int minx = (int)floor(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int maxx = (int)ceil (x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
    int miny = (int)floor(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int maxy = (int)ceil (y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > g_w - 1) maxx = g_w - 1;
    if (maxy > g_h - 1) maxy = g_h - 1;
    if (minx > maxx || miny > maxy) return;
    double inv = 1.0/area;
    for (int y = miny; y <= maxy; y++) {
        double py = y + 0.5;
        for (int x = minx; x <= maxx; x++) {
            double px = x + 0.5;
            double w0 = ((x1 - px)*(y2 - py) - (x2 - px)*(y1 - py)) * inv;
            double w1 = ((x2 - px)*(y0 - py) - (x0 - px)*(y2 - py)) * inv;
            double w2 = 1.0 - w0 - w1;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            int p = y*g_stride + x;
            if (depth_on) {
                double z = w0*z0 + w1*z1 + w2*z2;
                if (z >= g_zbuf[p]) continue;
                g_zbuf[p] = (float)z;
            }
            blend_px(p, col, g_cfg.fill_alpha);
        }
    }
}

static void line_z(double x0, double y0, double z0,
                   double x1, double y1, double z1,
                   uint32_t col, int depth_on)
{
    double dx = x1 - x0, dy = y1 - y0;
    int n = (int)(fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy));
    if (n <= 0) n = 1;
    if (n > 4096) n = 4096;
    double sx = dx/n, sy = dy/n, sz = (z1 - z0)/n;
    double x = x0, y = y0, z = z0;
    for (int i = 0; i <= n; i++, x += sx, y += sy, z += sz) {
        int ix = (int)(x + 0.5), iy = (int)(y + 0.5);
        if (ix < 0 || iy < 0 || ix >= g_w || iy >= g_h) continue;
        int p = iy*g_stride + ix;
        if (depth_on) {
            if (z >= g_zbuf[p]) continue;
            g_zbuf[p] = (float)z;
        }
        blend_px(p, col, g_cfg.wire_alpha);
    }
}

/* ---------------------------------------------------------------- per player */

static void ensure_scratch(int nv)
{
    if (g_vcap >= (size_t)nv) return;
    g_sx = (double*)realloc(g_sx, (size_t)nv*sizeof(double));
    g_sy = (double*)realloc(g_sy, (size_t)nv*sizeof(double));
    g_sz = (double*)realloc(g_sz, (size_t)nv*sizeof(double));
    g_vx = (double*)realloc(g_vx, (size_t)nv*sizeof(double));
    g_vy = (double*)realloc(g_vy, (size_t)nv*sizeof(double));
    g_vz = (double*)realloc(g_vz, (size_t)nv*sizeof(double));
    if (g_sx && g_sy && g_sz && g_vx && g_vy && g_vz) g_vcap = (size_t)nv;
}

int ch_player(const CH_XF* bones, int count,
              const CH_Q4* c2w_rot, const CH_V3* c2w_pos, const CH_V3* c2w_scl,
              uint32_t colour)
{
    g_last_skipped = 0;
    if (!g_pos || !g_px || !bones || count <= 0) return 0;
    if (g_cfg.mode == CH_OFF) return 0;

    /* skin matrices for every mesh bone: M = liveCS * inverse(bindCS) */
    static CH_Q4* sr = NULL;
    static CH_V3* st = NULL;
    static int    sc = 0;
    if (sc < g_nb) {
        CH_Q4* nr = (CH_Q4*)realloc(sr, (size_t)g_nb*sizeof(CH_Q4));
        CH_V3* nt = (CH_V3*)realloc(st, (size_t)g_nb*sizeof(CH_V3));
        if (!nr || !nt) return 0;
        sr = nr; st = nt; sc = g_nb;
    }
    for (int i = 0; i < g_nb; i++) {
        int li = g_map ? g_map[i] : i;
        if (li < 0 || li >= count) {
            sr[i] = (CH_Q4){0,0,0,1}; st[i] = v3(0,0,0); continue;
        }
        CH_Q4 lq = qnorm(bones[li].rot);
        CH_V3 lt = bones[li].pos;
        CH_Q4 iq = qconj(g_bindCSR[i]);
        CH_V3 negt = v3(-g_bindCST[i].x, -g_bindCST[i].y, -g_bindCST[i].z);
        CH_V3 it = qrot(iq, negt);
        sr[i] = qnorm(qmul(lq, iq));
        CH_V3 rt = qrot(lq, it);
        st[i] = v3(lt.x + rt.x, lt.y + rt.y, lt.z + rt.z);
    }

    ensure_scratch(g_nv);
    if (!g_sx || g_vcap < (size_t)g_nv) return 0;

    CH_Q4 cq = c2w_rot ? qnorm(*c2w_rot) : (CH_Q4){0,0,0,1};
    CH_V3 cp = c2w_pos ? *c2w_pos : v3(0,0,0);
    CH_V3 cs = c2w_scl ? *c2w_scl : v3(1,1,1);
    if (g_cfg.max_dist > 0.0) {
        double dx = cp.x - g_cam.x, dy = cp.y - g_cam.y, dz = cp.z - g_cam.z;
        if (sqrt(dx*dx + dy*dy + dz*dz) > g_cfg.max_dist) { g_last_skipped = 1; return 0; }
    }

    /* skin, transform to world, project */
    int ok = 0;
    double miny = 1e30, maxy = -1e30;
    for (int v = 0; v < g_nv; v++) {
        double vx = g_pos[v*3+0], vy = g_pos[v*3+1], vz = g_pos[v*3+2];
        double ax = 0, ay = 0, az = 0, ws = 0;
        for (int k = 0; k < 8; k++) {
            float w = g_wgt[v*8+k];
            if (w <= 0.0f) continue;
            int b = g_bone[v*8+k];
            if (b >= g_nb) continue;
            int live_index = g_map ? g_map[b] : b;
            if (live_index < 0 || live_index >= count) continue;
            CH_V3 r = qrot(sr[b], v3(vx, vy, vz));
            ax += w*(r.x + st[b].x);
            ay += w*(r.y + st[b].y);
            az += w*(r.z + st[b].z);
            ws += w;
        }
        if (ws > 0) { ax /= ws; ay /= ws; az /= ws; }
        else        { ax = vx; ay = vy; az = vz; }
        CH_V3 l = v3(ax*cs.x, ay*cs.y, az*cs.z);
        CH_V3 wr = qrot(cq, l);
        wr.x += cp.x; wr.y += cp.y; wr.z += cp.z;
        g_vx[v] = wr.x; g_vy[v] = wr.y; g_vz[v] = wr.z;
        if (!project(&wr, &g_sx[v], &g_sy[v], &g_sz[v])) { g_sz[v] = -1.0; continue; }
        if (g_sy[v] < miny) miny = g_sy[v];
        if (g_sy[v] > maxy) maxy = g_sy[v];
        ok++;
    }
    if (ok < 3) { g_last_skipped = 2; return 0; }
    if (g_cfg.min_h_px > 0 && (maxy - miny) < (double)g_cfg.min_h_px) { g_last_skipped = 3; return 0; }

    int depth_on = g_cfg.depth ? 1 : 0;
    int step = g_cfg.tri_step > 0 ? g_cfg.tri_step : 1;
    int drawn = 0;
    for (int t = 0; t + 2 < g_ni; t += 3*step) {
        int a = (int)g_idx[t], b = (int)g_idx[t+1], c = (int)g_idx[t+2];
        if (a >= g_nv || b >= g_nv || c >= g_nv) continue;
        if (g_sz[a] < 0 || g_sz[b] < 0 || g_sz[c] < 0) continue;
        if (g_cfg.mode == CH_SOLID || g_cfg.mode == CH_SOLID_WIRE) {
            tri_fill(g_sx[a], g_sy[a], g_sz[a],
                     g_sx[b], g_sy[b], g_sz[b],
                     g_sx[c], g_sy[c], g_sz[c],
                     colour, depth_on, g_cfg.backface);
        }
        if (g_cfg.mode == CH_WIRE || g_cfg.mode == CH_SOLID_WIRE) {
            line_z(g_sx[a], g_sy[a], g_sz[a], g_sx[b], g_sy[b], g_sz[b], colour, depth_on);
            line_z(g_sx[b], g_sy[b], g_sz[b], g_sx[c], g_sy[c], g_sz[c], colour, depth_on);
            line_z(g_sx[c], g_sy[c], g_sz[c], g_sx[a], g_sy[a], g_sz[a], colour, depth_on);
        }
        drawn++;
    }
    return drawn;
}

void ch_end(void) { /* rasterisation is immediate; nothing to flush */ }

int ch_bind_pose(CH_XF* out, int cap)
{
    if (!out || !g_bindCSR || cap < g_nb) return 0;
    for (int i = 0; i < g_nb; i++) {
        out[i].rot = g_bindCSR[i];
        out[i].pos = g_bindCST[i];
        out[i].scl = v3(1,1,1);
    }
    return g_nb;
}
