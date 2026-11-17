/* chams_hook.h — bridges the chams engine into wd_esp_full.c.
 *
 * Included from wd_esp_full.c right after pal_bind(), so every symbol it needs
 * (ENT, rd/rd64/rd32/pl, OFF_MESH_*, g_pal_v, g_rcam/g_rcamrot/g_rfov/g_raspect,
 * g_render/g_nrender, g_front/g_write/g_render_gen) is already declared.
 *
 * Three call sites to wire up:
 *   1. chams_init()             once at startup
 *   2. chams_capture(e, slot)   on the read thread, next to read_bones(e)
 *   3. ch_undo()                top of DrawOverlay, BEFORE its FillRect clear
 *      chams_draw_all(vw, vh)   right after that clear
 *
 * Read-only: the mesh comes from mesh.bin (unpacked offline from the .pak), live
 * memory supplies nothing but the bone transforms. Nothing is written to the
 * game process, and no overlay hook beyond the cheat's own DIB is used. */

/* The game's skeletons carry 350-450 bones. 384 silently rejected every one of
   them and dropped into the 0x620 fallback, which is NOT a bone array (it is
   the TArray<FSkelMeshComponentLODInfo> LOD cache) — that read back 1-5 garbage
   transforms and drew smeared triangles all over the screen, changing every
   frame. 512 covers the real range; 512*0x60 = 48 KB stays under the driver's
   64 KB MAX_TRANSFER. */
#define CHAMS_MAX_BONES 512

static CH_XF g_chb[2][MAX_PLAYERS][CHAMS_MAX_BONES];
static int   g_chbn[2][MAX_PLAYERS];
static CH_XF g_chr[MAX_PLAYERS][CHAMS_MAX_BONES];
/* Render-thread-owned composition buffer. chams_draw_all rasterises HERE;
   the paint thread blits this onto the DIB after its clear. Cross-thread
   access is unsafe per-pixel but self-healing: a torn frame lasts one paint
   and the next draw overwrites it. */
static uint32_t* g_chams_bb   = NULL;   /* w*h, 0 = transparent */
static int       g_chams_bb_w = 0, g_chams_bb_h = 0;
static int   g_chrn[MAX_PLAYERS];
static int   g_chmap[2][MAX_PLAYERS][CHAMS_MAX_BONES];
static int   g_chrmap[MAX_PLAYERS][CHAMS_MAX_BONES];
/* chams no longer draw into the overlay surface: they own g_chams_bb and the
   paint thread uploads the dirty box to a GPU texture. */

static int    g_chams_on          = 1;
static int    g_chams_mode        = CH_WIRE;   /* CH_SOLID | CH_WIRE | CH_SOLID_WIRE */
static int    g_chams_depth       = 0;          /* 0 = сквозь стены, 1 = self-occluding */
static int    g_chams_max_dist_cm = 20000;      /* 200 m */
static double g_chams_cluster_cm  = 3.0;        /* decimation cell; 3 cm -> ~16k tris */

static int chams_init(void)
{
    /* 3 cm vertex clustering takes LOD0 from 110k triangles to ~16k, which is
       what keeps a dozen skinned players inside the frame budget. */
    if (!ch_load_ex("E:\\Project\\wardogs\\meshdump\\mesh.bin", g_chams_cluster_cm))
        return 0;
    ch_undo_log(1 << 22);                      /* 4M pixels: 30+ players on a full server */
    return 1;
}

/* Resolve the extracted mesh's bone names against each live asset. */
typedef struct { uint64_t asset, data; int count, map[512]; DWORD tick; } ChamsMap;
static ChamsMap g_chmaps[512];
static int g_chmaps_count;
static int* chams_resolve_map(uint64_t mesh) {
    uint64_t asset = rd64(mesh + OFF_MESH_SKINNEDASSET);
    uint64_t data = pl(asset) ? rd64(asset + 0x2E0) : 0;
    int count = pl(asset) ? (int)rd32(asset + 0x2E8) : 0;
    if (!pl(data) || count <= 0 || count > 640 || ch_bone_count() > 512) return NULL;
    ChamsMap* m = NULL;
    for (int i=0;i<g_chmaps_count;i++)
        if (g_chmaps[i].asset == asset) { m=&g_chmaps[i]; break; }
    if (!m) {
        if (g_chmaps_count >= 512) return NULL;
        m=&g_chmaps[g_chmaps_count++];
    }
    DWORD now = GetTickCount();
    if (m->asset == asset && m->data == data && m->count == count && now-m->tick < 20000) return m->map;
    uint8_t records[640*12];
    if (!rd(data, records, (size_t)count*12)) return NULL;
    for (int b=0;b<ch_bone_count();b++) m->map[b]=-1;
    for (int i=0;i<count;i++) {
        uint32_t id; memcpy(&id, records+i*12, 4);
        const char* name = fname_str(id);
        for (int b=0;b<ch_bone_count();b++)
            if (!_stricmp(name, ch_bone_name(b))) m->map[b]=i;
    }
    m->asset=asset; m->data=data; m->count=count; m->tick=now;
    return m->map;
}

/* Bulk-read the whole component-space transform array for one player. */
static void chams_capture(ENT* e, int slot)
{
    if (!e || !pl(e->mesh) || slot < 0 || slot >= MAX_PLAYERS) return;
    g_chbn[g_write][slot] = 0;

    uint64_t arr = rd64(e->mesh + OFF_MESH_BONES_SDK);
    int32_t  cnt = (int32_t)rd32(e->mesh + OFF_MESH_BONES_SDK + 8);
    if (!pl(arr) || cnt <= 0 || cnt > CHAMS_MAX_BONES) {
        /* Fallback only if the alternate really looks like a skeleton. A count
           of 1-5 means we are looking at the LOD cache, not bones — reading it
           as FTransforms is what produced the random filled blobs. */
        uint64_t a2 = rd64(e->mesh + OFF_MESH_BONES);
        int32_t  c2 = (int32_t)rd32(e->mesh + OFF_MESH_BONES_N);
        if (!pl(a2) || c2 < 32 || c2 > CHAMS_MAX_BONES) return;
        arr = a2; cnt = c2;
    }

    /* FTransform is stride 0x60: quat 4x double @0x00, translation 3x double @0x20,
       scale 3x double @0x38. One burst covers the whole skeleton. */
    static uint8_t raw[CHAMS_MAX_BONES * 0x60];
    if (!rd(arr, raw, (size_t)cnt * 0x60)) return;

    CH_XF* dst = g_chb[g_write][slot];
    for (int i = 0; i < cnt; i++) {
        const uint8_t* s = raw + (size_t)i * 0x60;
        double q[4], t[3], sc[3];
        memcpy(q,  s + 0x00, 32);
        memcpy(t,  s + 0x20, 24);
        memcpy(sc, s + 0x40, 24);
        dst[i].rot.x = q[0]; dst[i].rot.y = q[1]; dst[i].rot.z = q[2]; dst[i].rot.w = q[3];
        dst[i].pos.x = t[0]; dst[i].pos.y = t[1]; dst[i].pos.z = t[2];
        dst[i].scl.x = sc[0]; dst[i].scl.y = sc[1]; dst[i].scl.z = sc[2];
    }
    int* map = chams_resolve_map(e->mesh);
    if (!map) return;
    memcpy(g_chmap[g_write][slot], map, (size_t)ch_bone_count()*sizeof(int));
    g_chbn[g_write][slot] = cnt;
}

/* Цвет по фракции из палитры меню (g_pal_v): fac_cls 0 = синий (Alpha),
   1 = красный (Bravo), 2 = зелёный (Charlie), -1 = неизвестный. Свои
   (fac_cls==0) рисуются только при cham_team=1 — решает вызывающий гард. */
static uint32_t chams_colour(int cls)
{
    const unsigned char* c =
        (cls == 1) ? g_pal_v.red   :
        (cls == 2) ? g_pal_v.green :
        (cls == 0) ? g_pal_v.blue  : g_pal_v.unknown;
    if (!c) return 0xFFFF3A3A;
    return 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
}

static void chams_draw_all(int vw, int vh)
{
    /* Erase last frame first: the undo log restores every pixel the rasteriser
       touched, which is also what the GPU upload box is built from. Runs even
       when chams are switched off, otherwise the last frame stays on screen. */
    ch_undo();

    /* Тумблер меню (Champs/Glow) — единственный выключатель. Дефолт конфига
       cham_on=0, включается тумблером или F10. */
    g_chams_on = g_cfg_cham_on ? 1 : 0;
    if (!g_chams_on || g_nrender <= 0) return;

    CH_CFG cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.mode       = g_chams_mode;
    cfg.depth      = g_chams_depth;
    cfg.backface   = (g_chams_mode == CH_WIRE) ? 0 : 1;
    cfg.color_mode = 1;
    /* Дистанция — из max_distance меню (0 = без лимита), а не из отдельного
       knob'а врезки: один источник правды. */
    cfg.max_dist   = (g_cfg_max_distance > 0) ? (double)g_cfg_max_distance
                                              : 0.0;
    cfg.fill_alpha = 128;
    cfg.wire_alpha = 128;
    /* Scale triangle sampling with player count: wireframe cost is linear in
       players x triangles. step=1 to 10 players, 2 to 25, 3 beyond - keeps a
       full-server frame under ~20 ms instead of 95 ms. */
    cfg.tri_step   = (g_nrender > 25) ? 3 : (g_nrender > 10) ? 2 : 1;
    ch_cfg(&cfg);

    /* rasterise into OUR back buffer, never the paint-owned DIB */
    if (!g_chams_bb || g_chams_bb_w != vw || g_chams_bb_h != vh) {
        uint32_t* nb = (uint32_t*)realloc(g_chams_bb, (size_t)vw*vh*4);
        if (!nb) return;
        memset(nb, 0, (size_t)vw*vh*4);      /* once, on (re)allocation only */
        g_chams_bb = nb; g_chams_bb_w = vw; g_chams_bb_h = vh;
    }
    /* No memset: ch_undo() above already restored every pixel the previous
       frame touched, and the buffer is calloc'd on first allocation. */
    ch_surface(g_chams_bb, vw, vh, vw);
    CH_V3 cl = { g_rcam.x,   g_rcam.y,   g_rcam.z   };
    CH_V3 cr = { g_rcamrot.p, g_rcamrot.y, g_rcamrot.r };
    ch_camera(&cl, &cr, g_rfov, vw, vh);
    ch_aspect((double)g_raspect);   /* same value project_all() gets */
    ch_begin();

    /* g_render was memcpy'd from g_ents[g_render_gen] under g_cs. Re-reading
       g_front here raced the read thread's swap and paired one frame's entities
       with another frame's bones — that alone flickered and smeared. */
    int front = g_render_gen;
    int drawn = 0;
    for (int i = 0; i < g_nrender; i++) {
        ENT* e = &g_render[i];
        if (e->is_self) continue;
        /* Свои (fac_cls==0) — только при включённом cham_team. */
        if (e->fac_slot == 0 && !g_cfg_cham_team) continue;
        /* Same own-player guard the markers use: the local-player chain can read
           as zero at scan time, so is_self is not enough. Your own skeleton sits
           at the camera and projects as huge smeared triangles. */
        if (e->dist > 0.0 && e->dist < 250.0) continue;
        if (!e->have_c2w) continue;      /* stale/zero c2w -> mesh at the origin */
        int n = g_chrn[i];
        if (n <= 0) continue;
        ch_bone_map(g_chrmap[i], ch_bone_count());
        CH_Q4 q = { e->c2w_rot.x, e->c2w_rot.y, e->c2w_rot.z, e->c2w_rot.w };
        CH_V3 p = { e->c2w_pos.x, e->c2w_pos.y, e->c2w_pos.z };
        CH_V3 s = { e->c2w_scl.x, e->c2w_scl.y, e->c2w_scl.z };
        if (ch_player(g_chr[i], n, &q, &p, &s, chams_colour(e->fac_cls)) > 0)
            drawn++;
    }
    ch_end();
    br_get()->telem_cham_n = drawn;
}

/* Paint-thread side: stamp the latest finished chams frame onto the DIB.
   Runs after the black clear, before the marker pass, so markers stay on top.
   A pixel the rasteriser wrote without an alpha byte gets 50% here, matching
   the wire/fill alpha. */
static void chams_blit_latest(uint32_t* dib, int w, int h)
{
    if (!g_chams_bb || !dib) return;
    int cw = g_chams_bb_w < w ? g_chams_bb_w : w;
    int ch2 = g_chams_bb_h < h ? g_chams_bb_h : h;
    for (int y = 0; y < ch2; y++) {
        const uint32_t* src = g_chams_bb + (size_t)y * g_chams_bb_w;
        uint32_t* dst = dib + (size_t)y * w;
        for (int x = 0; x < cw; x++) {
            uint32_t px = src[x];
            if (!px) continue;
            if ((px >> 24) == 0) px |= 0x80000000u;
            dst[x] = px;
        }
    }
}
