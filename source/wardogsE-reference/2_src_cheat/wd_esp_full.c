// language: C, file: wd_esp_full.c, target Windows 11 x64, MSVC
// WarDogs ESP: settings menu + transparent overlay. Reads via mapped driver.
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib,"winmm.lib")
#include <winhttp.h>
#pragma comment(lib,"winhttp.lib")
#include <shellapi.h>
#pragma comment(lib,"shell32.lib")
#include <winternl.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Мост к ImGui-меню: одна структура BR_Config на весь процесс.
 * Чит читает CONFIG-поля (тумблеры, цвета, дистанции) и пишет TELEMETRY.
 * Ниже, после объявления g_cfg/g_pal, макросы подменяют их обращения на
 * br_get()->... — поэтому весь существующий код работает через мост
 * без единой правки в местах использования. */
#include "menu_bridge.h"
#include "gpu.h"              /* D3D11 + DirectComposition + Direct2D backend */
#include "chams/chams.h"      /* external chams engine (geometry from mesh.bin) */

/* ---- Mirror: stream the finished overlay to a second PC -----------------
   Off unless MIRROR_HOST is defined at build time, e.g.
       cl ... /DMIRROR_HOST=\"192.168.1.50\" ...
   The sender only reads the DIB the overlay already produced and puts dirty
   tiles on UDP; the game process, the driver and the render path are not
   touched, and with the switch off the code compiles out entirely. */
#ifdef MIRROR_HOST
#include "mirror/wd_stream.h"
int  ws_send_init(const char* host, int port);
void ws_send_frame(const uint32_t* src, int w, int h);
void ws_send_shutdown(void);
#define MIRROR_ON 1
#else
#define MIRROR_ON 0
#endif

/* ---- Bridge: publish WORLD SPACE so a remote PC renders it itself ----------
 * Different from MIRROR_* above: that one ships finished pixels, so the far end
 * sees the local player's view. This one ships positions and skeletons, so the
 * far end builds its own view with its own camera and keeps seeing everyone no
 * matter where the local player looks. Enable with
 *     /DBRIDGE_HOST=\"relay.example.com\"        (or an IP)
 *     /DBRIDGE_TOKEN=\"secret\"                  (only for the relay case)
 *     /DBRIDGE_TCP                              (TCP instead of UDP)
 */
#ifdef BRIDGE_HOST
#include "mirror/wd_state.h"
#define BRIDGE_ON 1
#else
#define BRIDGE_ON 0
#endif


#ifndef TH32CS_SNAPPROCESS
#define TH32CS_SNAPPROCESS 0x00000002
#endif

/* ================= offsets (UC 759678, verified live) ================== */
#define RVA_UWORLD              0xD2AFC08u   /* GWorld, new season 2026-10-15 (was 0xD0E0E98) */
#define RVA_GENGINE             0xD2B2EC0u   /* UEngine* — not walked yet, kept for the next pass */
#define OFF_UW_LEVEL            0x30u
#define OFF_UW_GAMESTATE        0x1B0u
#define OFF_UW_GAMEINSTANCE     0x228u
#define OFF_GS_PLAYERARRAY      0x2D0u
#define OFF_GI_LOCALPLAYERS     0x38u
#define OFF_LP_PLAYERCONTROLLER 0x30u
#define OFF_PC_PAWN             0x360u
#define OFF_PC_PLAYERSTATE      0x2C0u
#define OFF_PC_CAMMGR           0x370u
#define OFF_CAM_CACHE           0x1560u
#define OFF_POV                 0x10u
#define POV_LOCATION            0x00u
#define POV_ROTATION            0x18u
#define POV_FOV                 0x30u
#define OFF_PS_PAWN             0x330u
#define OFF_PS_NAME             0x360u   /* PlayerNamePrivate moved +0x10. 0x350 now holds a
                                            string that is only filled for the LOCAL player, so
                                            reading it names yourself and shows "?" for everyone
                                            else. Verified live: 5/5 remote names read at 0x360. */
#define OFF_PS_FACTIONCOMP      0x5A0u
#define OFF_PS_SQUADCOMP        0x5A8u
#define OFF_SQUAD_ID            0x140u
#define POV_ASPECT              0x48u
#define OFF_MESH_BONES_N        0x628u
#define OFF_VEH_VARIANT         0x3A8u
#define OFF_VEH_SEATCOMP        0x758u   /* +0x10 by pattern; NOT live-verified (no vehicles on
                                            the server during the update) */   /* UBHVehicleSeatComponent* on ABHBaseVehiclePawn (SDK 1109) */
#define OFF_VEH_MESH            0x760u   /* USkeletalMeshComponentVehicle* on ABHBaseVehiclePawn.
                                            +0x10 by pattern; NOT live-verified (no vehicles live). */
#define OFF_SEAT_OCCUPANTS      0x108u
#define OFF_VARIANT_ID          0x34u
#define MAX_VEHICLES            128
#define OFF_FACTION_TAG         0x110u
#define OFF_ACTOR_ROOT          0x1C0u
#define OFF_PAWN_MESH           0x3A0u   /* verified: WDSkeletalMeshComponentBudgeted "CharacterMesh" */
#define OFF_PAWN_VITALITY       0x748u   /* verified: WDCharacterVitalityComponent */
#define OFF_PAWN_INVENTORY      0x0758u   /* AWDMoverCharacter::UWDPlayerInventoryComponent* — verified live */
#define OFF_INV_ARMOR           0x0830u   /* +0x18, NOT +0x10: this class shifts further than the
                                            pawn classes. Anchored on two SDK members
                                            (InputMappingContext 0x5C8->0x5E0, ABHItem* 0x8C8->0x8E0).
                                            NOT live-verified: nobody wore plates to read. */   /* TArray<FWDReplicatedArmorDurability> */
#define ARMOR_STRIDE            0x18u     /* struct is 0x14, aligned to 0x18 */
#define ARMOR_OFF_DUR           0x08u     /* float durability inside the element */
#define OFF_PAWN_WEAPONCOMP     0x780u   /* UWDWeaponBehaviorComponent* — verified live */
#define OFF_ITEM_ITEMID         0x328u   /* ABHItem::ItemId (FGameplayTag) */
#define OFF_MESH_ANIMINST       0x958u   /* USkeletalMeshComponent::AnimInstance */
#define OFF_ANIM_HELDITEM       0xC54u   /* UWDCharacterAnimInstance held item (replicated) */
#define OFF_WBC_HELDITEM        0x42Cu   /* TWeakObjectPtr<ABHItem> current held item */
#define OFF_PAWN_VITSTATE       0x804u   /* FWDCharacterVitalityState inline struct — verified live
                                            (two prone players read stance=2 at 0x804+0x1D). */
/* Vehicle occupancy. A seated player carries a UWDVehicleOperatorComponent on
   the pawn at +0x758; inside it the seat component sits at +0x188 (+0x198 is a
   second replica of the same pointer), and that seat's owner (+0xC8, the
   UActorComponent::OwnerActor slot) is the vehicle actor itself. Comparing the
   owner's class name is what tells a mortar apart from a jeep or a helicopter.
   Verified live: pawn+0x758 -> WDVehicleOperatorComponent -> +0x188 seat ->
   +0xC8 owner == AWDStationaryVehicle for every mortar gunner on the map. */
#define OFF_PAWN_OPERATOR       0x768u   /* verified: WDVehicleOperatorComponent */
#define OFF_OP_SEAT             0x188u
#define OFF_SEAT_OWNER          0x0C8u
#define OFF_VS_BLEEDOUT         0x1Cu   /* EWDBleedoutState: 0=none, 1=HoldingOn, 2=GivingUp */
#define OFF_VS_STANCE          0x1Du   /* EBHPlayerStance: 0=Stand 1=Crouch 2=Prone (NOT downed!) */    /* EWDBleedoutState byte */
#define OFF_SC_RELLOC           0x168u
#define OFF_SC_C2W              0x210u
#define OFF_VIT_MAXHEALTH       0x134u   /* UWDCharacterVitalityComponent::MaxHealth (f32) */
#define OFF_VIT_HEALTHATTR      0x140u   /* -> attribute object holding the live value */
#define OFF_ATTR_CURVALUE       0x30u    /* double CurrentValue inside the attribute */
#define OFF_VIT_CURHEALTH_ALT   0x174u   /* local-pawn mirror (f64); 0 for remote players */
#define OFF_MESH_SKINNEDASSET   0x5B0u
#define OFF_MESH_BONES          0x620u
#define OFF_MESH_BONES_SDK      0xA18u   /* reads 0 on the new build — the live array is at
                                            OFF_MESH_BONES (0x620); the fallback in the bone
                                            reader still covers it. */   /* USkeletalMeshComponent::CachedComponentSpaceTransforms (0.1.2 dump) */
#define OFF_MESH_LASTRENDER     0x35Cu
/* Chams через OverlayMaterial (форум + SDK):
 *   UMeshComponent::OverrideMaterials @ 0x550 (TArray<UMaterialInterface*>)
 *   UMeshComponent::OverlayMaterial  @ 0x560 (UMaterialInterface*)
 *   OverlayMaterialMaxDrawDistance    @ 0x568 (float)
 * Метод: один write per mesh — mesh+0x560 = glow-материал.
 * Базовые материалы (0x550) НЕ трогаем: оверлей снимается записью 0,
 * оригинал восстанавливать не нужно. Сквозь стены светит только если
 * у материала выключен depth-test, поэтому поиск предпочитает
 * Unlit/Wireframe/Emissive/BasicShape/Debug. */

/* --- ballistics: verified live on the shooting range (SV98) ---------------
   The local pawn carries the whole shoot solution:
     +0x0770  WDWeaponBehaviorComponent -> +0x440 WDWeaponStatsData
                    +0x38 FWDWeaponStats -> +0x0A4 TypicalSpeed (cm/s)
                                         -> +0x0AD8 gravity    (10 m/s^2)
     +0x08B8  zeroing distance in METRES (float) - what the player dialled in
   Speed is re-read every tick, so swapping rifles re-solves immediately. */
#define OFF_PAWN_WEAPBEHAV      0x780u
#define OFF_WBC_STATSDATA       0x440u
#define OFF_STATS_STRUCT        0x38u
#define OFF_STATS_TYPICALSPEED  0xA4u
#define OFF_STATS_INITIALSPD    0xDD8u   /* UWDWeaponStatsData::InitialBulletSpeed (f32, muzzle velocity) —
                                             shorter chain than TypicalSpeed and the ballistically correct
                                             value: what the bullet leaves the barrel at. Adapted from the
                                             UC prediction source (predictionStats_ + 0xDD8). */
#define OFF_STATS_GRAVITY       0xAD8u
#define OFF_PAWN_ZEROING        0x8B8u
#define LEAD_DEFAULT_SPEED      41500.0   /* cm/s - SV98 fallback */
#define LEAD_GRAVITY            1000.0    /* cm/s^2 - the game uses 10 m/s^2 */
#define FT_STRIDE               0x60u
#define FT_ROTATION             0x00u
#define FT_TRANSLATION          0x20u
#define FT_SCALE3D              0x40u   /* verified: translation 0x20, scale 0x40, stride 0x60 */
#define GN_RVA                  0xD050CC0u
#define GN_BLOCKS               0x10u   /* block table inline at RVA+0x10 (0.1.2) */
#define GN_ENTRY_SZ             8u
#define GN_HEADER               0x08u
#define GN_KIND                 0x00u
#define GN_DATA                 0x0Cu
#define GN_KIND_FIXED           0u
#define GN_FIXED_LEN            0x00u

#define MAX_PLAYERS 128

/* ===================== kernel read backend ============================= */
typedef NTSTATUS (WINAPI* fNtCF)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
typedef NTSTATUS (WINAPI* fNtDIOC)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,
    ULONG,PVOID,ULONG,PVOID,ULONG);
#define MKCTL(f) ((ULONG)((0x22u<<16)|(0<<14)|((f)<<2)|0))
#define IOCTL_READ  MKCTL(0x801u)
#define IOCTL_BASE  MKCTL(0x802u)
#define IOCTL_WRITE MKCTL(0x804u)   /* 0x222010 — ABI_IOCTL_WRITE_MEMORY в driver.c */

#pragma pack(push,1)
typedef struct { ULONG magic_key; ULONG process_id; ULONG_PTR address; SIZE_T size; } DRVCMD;
#pragma pack(pop)

typedef struct _PE32A { DWORD dwSize,cntUsage,th32ProcessID; ULONG_PTR th32DefaultHeapID;
  DWORD th32ModuleID,cntThreads,th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags;
  CHAR szExeFile[260]; } PE32A;

static HANDLE   g_dev = NULL;
static ULONG    g_drvkey = 0x4c9ab131UL;
static DWORD    g_pid = 0;
static uint64_t g_base = 0;

static int drv_open(void) {
    HMODULE nd = GetModuleHandleW(L"ntdll.dll");
    fNtCF pCF = (fNtCF)GetProcAddress(nd, "NtCreateFile");
    if (!pCF) return 0;
    for (int n = 0; n < 5 && !g_dev; n++) {
        wchar_t nt[64];
        const unsigned char h[] = {0x5C,'D','e','v','i','c','e',0x5C,'A','b','i','C','o','m','m',0};
        int i = 0; for (; h[i]; i++) nt[i] = (wchar_t)h[i];
        nt[i++] = (wchar_t)(L'0' + n); nt[i] = 0;
        UNICODE_STRING un; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK ios;
        un.Buffer = (PWSTR)nt; un.Length = (USHORT)(wcslen(nt)*2); un.MaximumLength = un.Length + 2;
        InitializeObjectAttributes(&oa,&un,0,NULL,NULL);
        HANDLE h2 = NULL;
        NTSTATUS st = pCF(&h2, GENERIC_READ|GENERIC_WRITE, &oa, &ios, NULL, 0,
                          FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
        if (st >= 0 && h2) g_dev = h2;
    }
    return g_dev != NULL;
}

static fNtDIOC g_pdioc = NULL;
/* instrumentation: counted per second to size the real driver load */

/* ---- IOCTL attribution counters (temporary, for the perf hunt) ---- */
static volatile long g_c_span=0, g_c_relloc=0, g_c_bones=0, g_c_wep=0, g_c_sweep=0,
                     g_c_objclass=0, g_c_fname=0, g_c_parr=0, g_c_scan=0;
static volatile long g_ioctl_count = 0;
static volatile long g_ioctl_bytes = 0;
static int rd(uint64_t a, void* o, size_t sz) {
    if (!g_dev || sz > 0x10000u) return 0;
    g_ioctl_count++; g_ioctl_bytes += (long)sz;
    fNtDIOC p = g_pdioc;
    if(!p){
        p = (fNtDIOC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
        if(!p) return 0;
        g_pdioc = p;
    }
    DRVCMD c;
    c.magic_key = g_drvkey; c.process_id = g_pid; c.address = (ULONG_PTR)a; c.size = sz;
    IO_STATUS_BLOCK ios; memset(&ios,0,sizeof(ios));
    NTSTATUS st = p(g_dev,NULL,NULL,NULL,&ios,IOCTL_READ,&c,(ULONG)sizeof(c),o,(ULONG)sz);
    return (st >= 0) && (ios.Information == sz);
}
static uint64_t drv_base(void) {
    if(!g_pdioc) g_pdioc=(fNtDIOC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
    fNtDIOC p = g_pdioc;
    if (!p || !g_dev) return 0;
    DRVCMD c; c.magic_key = g_drvkey; c.process_id = g_pid; c.address = 0; c.size = 0;
    uint64_t b = 0; IO_STATUS_BLOCK ios; memset(&ios,0,sizeof(ios));
    NTSTATUS st = p(g_dev,NULL,NULL,NULL,&ios,IOCTL_BASE,&c,(ULONG)sizeof(c),&b,8);
    return (st >= 0) ? b : 0;
}
static int pl(uint64_t v){return v>=0x10000ULL && v<0x00007FFFFFFFFFFFULL;}
static uint64_t rd64(uint64_t a){uint64_t v=0;rd(a,&v,8);return v;}
static uint32_t rd32(uint64_t a){uint32_t v=0;rd(a,&v,4);return v;}
static float    rdf (uint64_t a){float v=0;rd(a,&v,4);return v;}
static double   rdd (uint64_t a){double v=0;rd(a,&v,8);return v;}
/* ===================== kernel write backend (chams) ====================== */
/* Driver: METHOD_BUFFERED, input = ABI_COMMAND + payload bytes.
 * В usermode NtDeviceIoControlFile берёт SystemBuffer из input при
 * inputLength > outputLength, поэтому шлём один combined-буфер. */
static int wr(uint64_t a, const void* data, size_t sz) {
    if (!g_dev || !data || sz == 0 || sz > 0x10000u) return 0;
    fNtDIOC p = g_pdioc;
    if (!p) {
        p = (fNtDIOC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
        if (!p) return 0;
        g_pdioc = p;
    }
    size_t total = sizeof(DRVCMD) + sz;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return 0;
    DRVCMD* c = (DRVCMD*)buf;
    c->magic_key = g_drvkey; c->process_id = g_pid;
    c->address = (ULONG_PTR)a; c->size = sz;
    memcpy(buf + sizeof(DRVCMD), data, sz);
    uint64_t dummy = 0; IO_STATUS_BLOCK ios; memset(&ios,0,sizeof(ios));
    NTSTATUS st = p(g_dev,NULL,NULL,NULL,&ios,IOCTL_WRITE,
                    buf,(ULONG)total,&dummy,(ULONG)sizeof(dummy));
    free(buf);
    g_ioctl_count++;
    g_ioctl_bytes += (long)sz;
    return (st >= 0);
}
static int wr64(uint64_t a, uint64_t v){ return wr(a,&v,8); }
static int wr32(uint64_t a, uint32_t v){ return wr(a,&v,4); }
static int wrfloat(uint64_t a, float v){ return wr(a,&v,4); }

/* ============================= math ==================================== */
typedef struct { double x,y,z; } V3;
typedef struct { double x,y,z,w; } Q4;
typedef struct { double p,y,r; } ROT;
#define PI_ 3.14159265358979323846

static void rot_matrix(const ROT* ro, double m[3][3]) {
    double rp=ro->p*PI_/180.0, ry=ro->y*PI_/180.0, rr=ro->r*PI_/180.0;
    double sp=sin(rp),cp=cos(rp),sy=sin(ry),cy=cos(ry),sr=sin(rr),cr=cos(rr);
    m[0][0]=cp*cy;             m[0][1]=cp*sy;             m[0][2]=sp;
    m[1][0]=sr*sp*cy-cr*sy;    m[1][1]=sr*sp*sy+cr*cy;    m[1][2]=-sr*cp;
    m[2][0]=-(cr*sp*cy+sr*sy); m[2][1]=cy*sr-cr*sp*sy;    m[2][2]=cr*cp;
}
static V3 qrot(const Q4* q, V3 v){
    double tx=2.0*(q->y*v.z-q->z*v.y), ty=2.0*(q->z*v.x-q->x*v.z), tz=2.0*(q->x*v.y-q->y*v.x);
    V3 o;
    o.x=v.x+q->w*tx+(q->y*tz-q->z*ty);
    o.y=v.y+q->w*ty+(q->z*tx-q->x*tz);
    o.z=v.z+q->w*tz+(q->x*ty-q->y*tx);
    return o;
}
static int w2s_a(const V3* w,const V3* cp,const ROT* crt,double fov,float aspect,int sw,int sh,double*ox,double*oy){
    V3 d={w->x-cp->x,w->y-cp->y,w->z-cp->z};
    double dist=sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
    if(!(dist>0.0)||dist>1e7)return 0;
    double m[3][3]; rot_matrix(crt,m);
    double fwd=d.x*m[0][0]+d.y*m[0][1]+d.z*m[0][2];
    double rgt=d.x*m[1][0]+d.y*m[1][1]+d.z*m[1][2];
    double up =d.x*m[2][0]+d.y*m[2][1]+d.z*m[2][2];
    /* Reject on the ANGLE off the view axis, before dividing. Clamping fwd to
       1.0 (a common shortcut) is what scattered markers millions of pixels
       away: a player 20 km off to the side with fwd=0.5 projects to
       (-25000000, 4500000) instead of being culled.
       The cut is the half-diagonal FOV, so anything outside the frustum (plus
       a small margin) never reaches the division. */
    double tanH = tan(fov*PI_/180.0*0.5);
    if(tanH<=0.0) return 0;
    if(!(aspect>1.0f&&aspect<3.5f)) aspect = (sh > 0) ? (float)sw / (float)sh : 1.7777778f;
    double tanV = tanH / aspect;
    double ft = tanH;
    double fovSlope = tanH * 1.6;
    if(fwd <= 0.0) return 0;
    if(fabs(rgt) > fwd * fovSlope * 2.5) return 0;
    if(fabs(up)  > fwd * (tanV * 1.6) * 2.5) return 0;

    double cx=sw*0.5, cy=sh*0.5;
    *ox=cx+(rgt/fwd/tanH)*cx;
    *oy=cy-(up /fwd/tanV)*cy;
    if(*ox!=*ox||*oy!=*oy) return 0;
    if(*ox<-sw*1.5||*ox>sw*2.5||*oy<-sh*1.5||*oy>sh*2.5) return 0;
    return 1;
}
static int w2s(const V3* w,const V3* cp,const ROT* crt,double fov,int sw,int sh,double*ox,double*oy){
    float a = (sh > 0) ? (float)sw / (float)sh : 1.7777778f;
    return w2s_a(w,cp,crt,fov,a,sw,sh,ox,oy);
}

/* ============================ FName ==================================== */
static char g_nb[256];
static const char* fname_str(uint32_t idx) {
    g_c_fname++;
    g_nb[0] = 0;
    if (!idx || !g_base) return g_nb;
    /* FNamePool (live-verified 30.09 on 0.1.2 by fname_probe.py): the block
       table is INLINE in the pool object at RVA+0x10 — no pointer hop. The
       14.09 build needed rd64(base+RVA) first; that dereference now reads
       garbage, so EVERY name (incl. nicknames) resolved empty → grey boxes,
       no weapon labels. Block N covers indices N<<16 .. (N<<16)|0xFFFF.
       Entries: 16-bit header at +0x08 (len in high bits), text at +0x0C. */
    uint64_t table = g_base + GN_RVA + GN_BLOCKS;
    uint64_t chunk = rd64(table + (uint64_t)(idx >> 16) * 8);
    if (!pl(chunk)) return g_nb;
    uint64_t ent = chunk + (uint64_t)(uint16_t)idx * GN_ENTRY_SZ;
    uint16_t hdr = 0;
    if (!rd(ent + GN_HEADER, &hdr, 2)) return g_nb;
    if (!hdr) return g_nb;
    uint32_t len = (uint32_t)(hdr >> 6);
    if (!len || len > 255) return g_nb;
    char b[256]; memset(b,0,sizeof(b));
    if (!rd(ent + GN_DATA, b, len)) return g_nb;
    memcpy(g_nb,b,len); g_nb[len]=0;
    return g_nb;
}
static void obj_class(uint64_t obj, char* out, size_t n) {
    g_c_objclass++;
    out[0]=0;
    uint64_t c = rd64(obj + 0x10);
    if (!pl(c)) return;
    strncpy(out, fname_str(rd32(c + 0x18)), n-1);
    out[n-1]=0;
}
/* Faction tag -> color class, resolved by the NAME in GNames (patch-proof:
   FName indices move every build, the string suffix does not). Cache by tag
   value; resolve lazily; unknown tags stay grey. */
static struct { uint64_t tag; int cls; } g_fcache[32];
static int g_nfcache = 0;
static int faction_class(uint64_t tag) {
    if (!tag) return -1;
    for (int i = 0; i < g_nfcache; i++) if (g_fcache[i].tag == tag) return g_fcache[i].cls;
    int cls = -1;
    const char* nm = fname_str((uint32_t)tag);
    if (nm[0]) {
        char low[96]; size_t j = 0;
        for (; nm[j] && j + 1 < sizeof(low); j++)
            low[j] = (char)((nm[j] >= 'A' && nm[j] <= 'Z') ? nm[j] + 32 : nm[j]);
        low[j] = 0;
        /* Release faction tags (logged from a live raid):
           Meta.Alignment.Faction.Alpha   = blue
           Meta.Alignment.Faction.Bravo   = red
           Meta.Alignment.Faction.Charlie = green
           (beta 0x22951/56/5B -> release 0x229A3/A8/AD, consistent +0x52;
            confirmed by dj in game: his Charlie mates drew green.)
           Match on the suffix so the tree prefix can change freely. */
        const char* dot = NULL;
        for (const char* q = low; *q; q++) if (*q == '.') dot = q;
        if (dot) {
            if (!strncmp(dot, ".alpha", 6))   cls = 0;
            else if (!strncmp(dot, ".bravo", 6))   cls = 1;
            else if (!strncmp(dot, ".charlie", 8)) cls = 2;
        }
        /* color-named tags if the devs ever rename to them */
        if (cls < 0) {
            if (strstr(low, "blue"))      cls = 0;
            else if (strstr(low, "red"))  cls = 1;
            else if (strstr(low, "green")) cls = 2;
        }
    }
    /* Log every freshly resolved tag name so the real faction->color map can
       be read from disk after one match (the release tag names are unknown;
       guessing painted a green faction blue). No hash fallback: a wrong
       color is worse than grey. */
    {
        FILE* f = fopen("E:\\Project\\wardogs\\WarDogsESP\\factions.log", "a");
        if (f) { fprintf(f, "tag=0x%llX name=%s cls=%d\n",
                         (unsigned long long)tag, nm, cls); fclose(f); }
    }
    if (g_nfcache < 32) { g_fcache[g_nfcache].tag = tag; g_fcache[g_nfcache].cls = cls; g_nfcache++; }
    return cls;
}

static int read_fs(uint64_t fs, char* out, size_t osz) {
    out[0]=0;
    uint64_t data = rd64(fs);
    int32_t len = (int32_t)rd32(fs + 8);
    if (!pl(data) || len <= 0 || len > 63) return 0;
    wchar_t w[64]; memset(w,0,sizeof(w));
    if (!rd(data, w, (size_t)len*2)) return 0;
    char t[128]; memset(t,0,sizeof(t));
    int got = WideCharToMultiByte(CP_UTF8,0,w,len,t,(int)sizeof(t)-1,NULL,NULL);
    if (got <= 0) return 0;
    t[got]=0;
    /* Keep the full UTF-8 text: an ASCII-only filter blanked every non-English
       nickname ("Илюша Шуруп", "Résztvevő", "†⸸ MadB0nzai ⸸†"), so those players
       showed an empty label even though their data read fine. Control bytes are
       dropped, everything from 0x80 up is a UTF-8 lead/continuation byte and
       must survive. The copy is bounded because out[] is caller-sized. */
    size_t o=0;
    for (size_t i=0;t[i]&&o+1<osz;i++)
        if ((unsigned char)t[i]>=0x20) t[o++]=t[i];
    t[o]=0;
    size_t c=0;
    for (; t[c] && c+1<osz; c++) out[c]=t[c];
    out[c]=0;
    return out[0]!=0;
}

/* ============================ bones ==================================== */
enum { B_PELVIS=0,B_SPINE1,B_SPINE3,B_NECK,B_HEAD,
       B_CLAV_L,B_UPPERARM_L,B_LOWERARM_L,B_HAND_L,
       B_CLAV_R,B_UPPERARM_R,B_LOWERARM_R,B_HAND_R,
       B_THIGH_L,B_CALF_L,B_FOOT_L,
       B_THIGH_R,B_CALF_R,B_FOOT_R, B__COUNT };
static const char* BONE_NAMES[B__COUNT] = {
    "Pelvis","spine_01","spine_03","neck_01","Head",
    "clavicle_l","upperarm_l","lowerarm_l","hand_l",
    "clavicle_r","upperarm_r","lowerarm_r","hand_r",
    "thigh_l","calf_l","foot_l","thigh_r","calf_r","foot_r" };
/* Verified against the running game: 0=Root 1=Pelvis 2=spine_01 3=spine_02
   4=spine_03 7=neck_01 9=Head 10=clavicle_l 11=upperarm_l 12=lowerarm_l
   22=hand_l. The previous fallbacks sat a few centimetres apart, which made
   every bone project to the same pixel and the skeleton vanish. */
/* Exact indices, dumped live from this game's ReferenceSkeleton:
     0=Root 1=Pelvis 2=spine_01 4=spine_03 7=neck_01 9=Head
    10=clavicle_l 11=upperarm_l 12=lowerarm_l 22=hand_l
    64=clavicle_r 65=upperarm_r 66=lowerarm_r 76=hand_r
   122=thigh_r 123=calf_r 124=foot_r 145=thigh_l 146=calf_l 147=foot_l
   The previous guesses (33/34/35, 51/52/53 ...) landed on finger and twist
   bones a few centimetres apart, so every joint projected to the same pixel
   and the skeleton disappeared. */
static const int BONE_FALLBACK[B__COUNT] = {
    1,2,4,7,9, 10,11,12,22, 64,65,66,76, 145,146,147, 122,123,124 };
/* Skeleton graph. The clavicles hang off the NECK, not off spine_03: drawing
   spine_03 -> clavicle_* put two lines into the middle of the back, which read
   as the spine forking and continuing into the arms. Anatomically (and as in
   the reference screenshot) the spine runs pelvis -> spine_01 -> spine_03 ->
   neck -> head, and the shoulders branch from the neck. */
static const int BONE_EDGES[][2] = {
    {B_PELVIS,B_SPINE1},{B_SPINE1,B_SPINE3},{B_SPINE3,B_NECK},{B_NECK,B_HEAD},
    {B_NECK,B_CLAV_L},{B_CLAV_L,B_UPPERARM_L},{B_UPPERARM_L,B_LOWERARM_L},{B_LOWERARM_L,B_HAND_L},
    {B_NECK,B_CLAV_R},{B_CLAV_R,B_UPPERARM_R},{B_UPPERARM_R,B_LOWERARM_R},{B_LOWERARM_R,B_HAND_R},
    {B_PELVIS,B_THIGH_L},{B_THIGH_L,B_CALF_L},{B_CALF_L,B_FOOT_L},
    {B_PELVIS,B_THIGH_R},{B_THIGH_R,B_CALF_R},{B_CALF_R,B_FOOT_R} };
#define N_EDGES (sizeof(BONE_EDGES)/sizeof(BONE_EDGES[0]))
#define MAX_BONES 640   /* the heaviest character model carries 583 bones (indices up to 582) */

typedef struct { uint64_t asset; int idx[B__COUNT]; DWORD refreshed; } BONEMAP;
static BONEMAP g_bm[512]; static int g_nbm = 0;   /* one raid exposed 187 distinct character assets; never evict, just grow */

/* Dump every bone of one skeleton: index -> name. This is the diagnostic the
   UC thread recommends - character models have different bone orderings, so a
   fixed index table only lines up for whichever model it was dumped from.
   Comparing dumps side by side shows which index holds which joint per model. */
static void dump_skeleton(uint64_t asset) {
    FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\bones.txt","a");
    if (!lf) return;
    fprintf(lf, "\n=== asset %llX ===\n", (unsigned long long)asset);
    uint64_t data = rd64(asset + 0x2C0 + 0x20);
    int32_t cnt = (int32_t)rd32(asset + 0x2C0 + 0x28);
    fprintf(lf, "  refskeleton data=%llX cnt=%d\n", (unsigned long long)data, cnt);
    if (pl(data) && cnt > 0 && cnt <= 1024) {
        uint8_t* recs = (uint8_t*)malloc((size_t)cnt * 12);
        if (recs && rd(data, recs, (size_t)cnt * 12)) {
            for (int i = 0; i < cnt; i++) {
                uint32_t id = 0; memcpy(&id, recs + (size_t)i*12, 4);
                const char* nm = fname_str(id);
                fprintf(lf, "  %3d  %s\n", i, nm[0] ? nm : "?");
            }
        }
        free(recs);
    }
    fclose(lf);
}

static BONEMAP* bm_get(uint64_t asset) {
    BONEMAP* m = NULL;
    DWORD now = GetTickCount();
    for (int i=0;i<g_nbm;i++) if (g_bm[i].asset==asset) {
        m=&g_bm[i];
        if (now-m->refreshed < 20000) return m;
        break;
    }
    if (!m) {
        if (g_nbm>=512) return NULL;
        m=&g_bm[g_nbm++];
    }
    m->asset=asset; m->refreshed=now;
    for (int b=0;b<B__COUNT;b++) m->idx[b]=-1;
    uint64_t data=rd64(asset+0x2C0+0x20);
    int32_t cnt=(int32_t)rd32(asset+0x2C0+0x28);
    if(pl(data)&&cnt>0){
        if(cnt>640)cnt=640;
        uint8_t* recs=(uint8_t*)malloc((size_t)cnt*12);
        if(recs&&rd(data,recs,(size_t)cnt*12)){
            uint32_t* ids=(uint32_t*)malloc((size_t)cnt*4);
            if(ids){
                for(int i=0;i<cnt;i++) memcpy(&ids[i],recs+(size_t)i*12,4);
                /* Resolve every name once, then match. Calling fname_str per
                   bone per name was ~4800 IOCTLs the first time a skeleton was
                   seen and stalled the whole read loop. */
                char** names=(char**)malloc((size_t)cnt*sizeof(char*));
                if(names){
                    for(int i=0;i<cnt;i++){
                        const char* n0=fname_str(ids[i]);
                        size_t L=strlen(n0);
                        names[i]=(char*)malloc(L+1);
                        memcpy(names[i],n0,L+1);
                    }
                    for(int b=0;b<B__COUNT;b++)
                        for(int i=0;i<cnt;i++){
                            if(names[i][0]&&!_stricmp(names[i],BONE_NAMES[b])){m->idx[b]=i;break;}
                        }
                    for(int i=0;i<cnt;i++) free(names[i]);
                    free(names);
                } else {
                    for(int b=0;b<B__COUNT;b++)
                        for(int i=0;i<cnt;i++){
                            const char* nm=fname_str(ids[i]);
                            if(nm[0]&&!_stricmp(nm,BONE_NAMES[b])){m->idx[b]=i;break;}
                        }
                }
                free(ids);
            }
        }
        free(recs);
    }
    /* Log the resolution result once per skeleton: how many of our joints were
       matched by name. A low count means the name table and the model disagree
       (different taxonomy), which is what makes bones land on the wrong joint. */
    {
        FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\bmres.txt","a");
        if (lf) {
            int matched = 0;
            for (int b = 0; b < B__COUNT; b++)
                if (m->idx[b] != BONE_FALLBACK[b]) matched++;
            fprintf(lf, "asset=%llX bones=%d matched=%d/%d\n",
                    (unsigned long long)asset, cnt, matched, (int)B__COUNT);
            fclose(lf);
        }
    }
    return m;
}

/* ============================ entities ================================= */
typedef struct {
    uint64_t pawn, mesh, ps;
    V3 root;
    V3 proot;              /* previous read position (for interpolation) */
    unsigned long pos_tick, ppos_tick;
    V3 bones[B__COUNT];
    int bone_ok[B__COUNT];
    int have_c2w;
    V3 c2w_pos; Q4 c2w_rot; V3 c2w_scl;
    char name[96];
    double hp, maxhp;
    int    hp_valid;   /* 1 = health is real; 0 = game does not replicate it */
    uint8_t downed;    /* FWDCharacterVitalityState +0x1C: 2 = knocked down */
    double armor;      /* summed armor durability across equipped armour pieces */
    double armor_max;  /* this player's own full-plate value (>= 200) */
    double armor_base; /* first sample, shown as an absolute number next to the bar */
    int    armor_n;    /* number of armour pieces */
    uint64_t faction;
    int fac_slot;
    int fac_cls;      /* 0 blue 1 red 2 green -1 unk; resolved on read thread */
    uint8_t dbno;     /* EWDBleedoutState: 0 none, 1 bleeding-out/holding, 2 giving up */
    char wep[32];     /* weapon-in-hands short label, from the held item object name */
    /* vehicle occupancy, resolved on the read thread. veh_kind: 0 = on foot,
       1 = mortar (AWDStationaryVehicle), 2 = car/truck, 3 = helicopter/plane,
       4 = something else with a seat. The L81 Users panel keys off veh_kind==1;
       the plain ESP keeps drawing the normal box for every seated player too. */
    int    veh_kind;
    uint64_t veh_actor;   /* the vehicle actor the player is seated in */
    /* human-readable vehicle variant, resolved from the vehicle's
       ModularVehicleVariant object at actor+0x3A8:
       "VV_WHL_05_Humvee_Default" -> "Humvee". Filled for the vehicle
       itself and for the crew sitting inside it. */
    char   veh_name[40];
    int    veh_armed;    /* 1 = the variant carries a mounted weapon */
    /* --- lead marker -------------------------------------------------------
       Target velocity in world cm/s, via windowed linear regression over the
       last VELWIN position samples (least-squares slope per axis). A single
       raw delta was hopeless: the read loop jitters 15-50 ms between ticks,
       so consecutive deltas differed by 30%+ and the lead dot visibly shook
       on an evenly running player. The regression averages that tick noise
       out while VELWIN_TURNOVER (0.3) keeps reaction faster than a human
       can accelerate. Adapted after reviewing the UC prediction reader. */
#define VELWIN 5
    double vx, vy, vz;
    int    vel_ok;
    unsigned long vel_tick;
    V3     vel_prev;
    double hp_[VELWIN][4];  /* history: t(ms),x,y,z */
    int    hp_n;
    V3     lead_aim;     /* world aim point: lead + drop - zeroing */
    int    lead_ok;
    double lead_dist;    /* flight distance, metres */
    double lead_time;    /* flight time, seconds */
    double lead_drop;    /* signed vertical hold, metres (+ = aim high) */
    int    lead_veh;     /* 1 = a vehicle entry, for the Vehicles filter */
    int bone_cnt;
    uint64_t squad_lo, squad_hi;
    int is_self, visible;
    double dist;
    double sx[B__COUNT], sy[B__COUNT];
    V3  bcam[B__COUNT];
    ROT bcamrot[B__COUNT];
    double bfov[B__COUNT];
    int bok[B__COUNT];
    int smooth_init;
    int miss[B__COUNT];
    int last_tick;
    int need_bones;
    int bone_tick;
    int bone_slot;     /* per-entity phase so refreshes spread across ticks */
    /* Anchor for frozen poses. UE leaves bone transforms stale for meshes it
       does not render (VisibilityBasedAnimTickOption), so a distant skeleton
       keeps the pose from the last time the engine ticked it - but the actor's
       root keeps moving. Recording the root at the moment the bones were read
       lets the draw step translate the whole frozen skeleton by the root delta,
       so it rides along with the player instead of standing at the old spot. */
    V3  bone_root;
    int bone_live;     /* 1 = bone data updated on the last read */
    double bone_sum;   /* checksum of the raw translations at the last read */
    V3  bone_shift;    /* accumulated translation applied to a frozen pose */
} ENT;

static ENT g_ents[2][MAX_PLAYERS];
/* Render copy: the render thread copies the front snapshot + camera under the
   lock, then projects and draws from the copy with no lock held. The reader
   never blocks on projection and the painter never sees a half-written row. */
static ENT g_render[MAX_PLAYERS];
static int g_nrender = 0;
typedef struct {
    uint64_t act; V3 pos;
    char label[48];      /* human name from the variant table */
    char raw[48];        /* raw variant tag, kept for unknown content */
    int occ, seats;
    double dist;
    int armed;           /* 1 = turret / minigun / rockets */
    int kind;            /* 2 = wheeled, 3 = rotary, 4 = tracked */
    /* the lead marker treats a vehicle the same way it treats a player */
    double vx, vy, vz;
    int    vel_ok;
    unsigned long vel_tick;
    V3     vel_prev;
    double vhp_[VELWIN][4];
    int    vhp_n;
    V3     lead_aim;
    int    lead_ok;
    double lead_dist, lead_time, lead_drop;
} VENT;
static VENT g_veh[2][MAX_VEHICLES];
/* Shoot solution, refreshed on the read thread. Defaults are the SV98 values
   measured live, so the marker still shows a sane circle before the stats asset
   for the equipped weapon has loaded. */
static double g_bullet_speed = LEAD_DEFAULT_SPEED;  /* cm/s   */
static double g_gravity      = LEAD_GRAVITY;        /* cm/s^2 */
static double g_zeroing      = 100.0;               /* metres */
static int    g_shoot_valid  = 0;
/* Weapon-speed cache (adapted from the UC prediction reader): when the stats
   read fails for a tick but the weapon object is UNCHANGED and the cache is
   younger than 150 ms, the cached speed keeps the marker from flickering.
   Any weapon swap invalidates it immediately — different rifles, different
   muzzle velocities. */
static uint64_t g_wbc_cache_ptr   = 0;    /* WeaponBehaviorComponent we cached from */
static uint64_t g_stats_cache_ptr = 0;    /* stats object the speed came from */
static DWORD    g_speed_cache_tick = 0;

static int g_nveh[2] = {0,0};
static VENT g_vehrender[MAX_VEHICLES];
static int g_nvehrender = 0;
/* ======================= WORLD-POSITION INTERPOLATION ======================
   Every marker is a projection of a world position, and those positions only
   change when the read thread publishes a new snapshot. Projecting the raw
   sample makes each marker step at the read rate while the screen presents
   faster - that stepping is what reads as "the box lags / the circle stutters",
   and it is worst on vehicles because their sweep is slower than the player
   roster.

   The fix is NOT a smoothing filter: a filter trails the target by a constant
   amount and makes the marker permanently late. Instead the two most recent
   real samples are kept with their timestamps and the position is interpolated
   between them by wall-clock alpha. Both endpoints are measurements, so the
   result is exact at the sample instants and moves at the target's true speed
   in between - smooth with zero added lag.

   A pair older than STALE_MS is dropped rather than extrapolated: extrapolation
   overshoots badly the instant a target turns or stops. */
#define IP_STALE_MS 300

typedef struct {
    V3 prev, cur;
    unsigned long t_prev, t_cur;
    int have;
} IPOS;

static void ipos_push(IPOS* ip, const V3* p, DWORD now) {
    if (!ip->have) {
        ip->prev = *p; ip->cur = *p;
        ip->t_prev = ip->t_cur = now;
        ip->have = 1;
        return;
    }
    /* only a genuinely new sample shifts the window */
    if (p->x != ip->cur.x || p->y != ip->cur.y || p->z != ip->cur.z) {
        ip->prev = ip->cur; ip->t_prev = ip->t_cur;
        ip->cur  = *p;      ip->t_cur  = now;
    }
}

static int ipos_get(const IPOS* ip, DWORD now, V3* out) {
    if (!ip->have) return 0;
    if (now - ip->t_cur > IP_STALE_MS) return 0;      /* too old: do not extrapolate */
    double span = (double)(ip->t_cur - ip->t_prev);
    double a = 1.0;
    if (span > 1.0) {
        a = (double)(now - ip->t_prev) / span;
        if (a > 1.0) a = 1.0;
        if (a < 0.0) a = 0.0;
    }
    out->x = ip->prev.x + (ip->cur.x - ip->prev.x) * a;
    out->y = ip->prev.y + (ip->cur.y - ip->prev.y) * a;
    out->z = ip->prev.z + (ip->cur.z - ip->prev.z) * a;
    return 1;
}

/* Per-vehicle interpolation state, parallel to g_vehrender and keyed by actor.
   Vehicles need this more than players do: their positions are refreshed on the
   deep sweep, which is slower than the player roster, so a raw projection steps
   visibly. Interpolating between the two most recent real samples makes the box
   glide at the vehicle's true speed with no added lag. */
typedef struct { uint64_t act; IPOS ip; V3 smooth; int ok; } VEH_IP;
static VEH_IP g_vehip[MAX_VEHICLES];
static int    g_nvehip = 0;

/* Advance every vehicle's interpolator and publish the smoothed position. */
static void veh_interp_tick(void) {
    DWORD now = GetTickCount();
    for (int i = 0; i < g_nvehrender; i++) {
        VENT* v = &g_vehrender[i];
        VEH_IP* s = 0;
        for (int k = 0; k < g_nvehip; k++) if (g_vehip[k].act == v->act) { s = &g_vehip[k]; break; }
        if (!s) {
            if (g_nvehip >= MAX_VEHICLES) continue;
            s = &g_vehip[g_nvehip++];
            memset(s, 0, sizeof(*s));
            s->act = v->act;
        }
        ipos_push(&s->ip, &v->pos, now);
        s->ok = ipos_get(&s->ip, now, &s->smooth);
    }
    /* drop entries whose vehicle is gone, so the table cannot grow forever */
    int d = 0;
    for (int k = 0; k < g_nvehip; k++) {
        int alive = 0;
        for (int i = 0; i < g_nvehrender; i++) if (g_vehrender[i].act == g_vehip[k].act) { alive = 1; break; }
        if (alive) { if (d != k) g_vehip[d] = g_vehip[k]; d++; }
    }
    g_nvehip = d;
}

static const V3* veh_smooth_pos(const VENT* v) {
    for (int k = 0; k < g_nvehip; k++)
        if (g_vehip[k].act == v->act && g_vehip[k].ok) return &g_vehip[k].smooth;
    return &v->pos;
}


/* ===================== L81 MORTAR TRAJECTORY =============================
   Live-measured 03.10 (shooting range, 8 rounds tracked at 50 ms):
   projectile actor class "BP_120x800mm_C" spawns at the muzzle the tick you
   fire, position via RootComponent+0x168 (double), and the spent round KEEPS
   lying at the impact spot (never despawns during the session) - so the impact
   marker below stays up on its own.
   Ballistic fit over the whole track: LINEAR drag a = -v/tau - g beats both
   pure parabola (600+ m off) and quadratic drag (670 m off) by an order of
   magnitude. Best fit: tau_xy=3.2 s, tau_z=2.8 s, g=1100 cm/s2; 5-35 m
   deviation across the whole arc. */
#define PROJ_MAX      16
#define PROJ_ARC_PTS  48        /* solved arc points per round */
#define PROJ_TAU_XY   3.2
#define PROJ_TAU_Z    2.8
#define PROJ_G        1100.0
#define PROJ_POST_IMPACT_MS 2000

typedef struct {
    uint64_t act;               /* BP_120x800mm_C actor */
    int      used;
    int      solved;            /* 1 once the arc reaches the ground */
    unsigned long born_ms;
    unsigned long last_ms;
    V3       p0;
    double   vx, vy, vz;
    int      n_samples;
    V3       last_pos;          /* live shell position */
    unsigned long t0;
    unsigned long land_ms;
    int      still_samples;      /* consecutive stationary samples */
    unsigned long still_since_ms;
    int      flying;
    /* full flight from spawn, not a sliding window */
    double   hist[96][4];       /* t,x,y,z */
    int      n_hist;
    V3       arc[PROJ_ARC_PTS];
    int      arc_n;
    V3       impact;
    int      impact_ok;
    double   range_m;
    double   locked_range_m;     /* distance frozen at projectile spawn */
    int      range_locked;
    double   flight_s;
} PROJ;

static PROJ g_proj[PROJ_MAX];
static int  g_nproj = 0;
static int  g_proj_rescan_req = 0;   /* T: полный проход за минами после своего выстрела */
static void proj_sight(uint64_t a, const char* cn);
static void proj_full_sweep(uint64_t world) {
    uint64_t lv = rd64(world + OFF_UW_LEVEL);
    if (!pl(lv)) return;
    uint64_t arr = rd64(lv + 0xA0);
    int32_t cnt = (int32_t)rd32(lv + 0xA8);
    if (!pl(arr) || cnt <= 0 || cnt >= 200000) return;
    if (cnt > 20000) cnt = 20000;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = rd64(arr + (uint64_t)i * 8);
        if (!pl(a)) continue;
        char cn[64]; obj_class(a, cn, sizeof(cn));
        if (cn[0] && strstr(cn, "120x800")) proj_sight(a, cn);
    }
}

typedef struct { uint64_t act; VENT v; int miss; } VEHHOLD;
static VEHHOLD g_vhold[MAX_VEHICLES];
static int g_nvhold = 0;
static V3  g_rcam = {0};
static ROT g_rcamrot = {0};
static double g_rfov = 120.0;
static float g_raspect = 1.7777778f;



typedef struct {
    double sx[B__COUNT], sy[B__COUNT];
    int bok[B__COUNT];
    int miss[B__COUNT];
    int init;
    uint64_t pawn;
    /* --- lead marker screen cache -------------------------------------------
       The aim point is a world position that only changes when the read thread
       publishes a new sample, so projecting it raw makes the circle step at the
       read rate while the screen presents faster - that stepping is what reads
       as "the circle stutters". Instead the two most recent WORLD samples are
       kept with their timestamps and the circle is projected from the position
       interpolated between them by wall-clock alpha. The circle then moves at
       the target's true speed, smoothly, and WITHOUT the lag a smoothing filter
       would add: interpolation between two known samples is exact, not a
       trailing average. */
    /* Interpolated world root: every marker (box, skeleton anchor, name row)
       is derived from this, so smoothing it here smooths them all at once and
       without the per-element lag a screen-space filter would add. */
    IPOS root_ip;
    V3   root_smooth;
    int  root_ok;
    V3   lead_prev;        /* previous published aim point */
    V3   lead_cur;         /* current published aim point  */
    unsigned long lead_t_prev, lead_t_cur;
    int  lead_have;
    double lead_sx, lead_sy;   /* last projected screen position, for the fade */
    int  lead_sok;
} SCR;
static SCR g_scr[MAX_PLAYERS];
static int g_nents[2] = {0,0};
static int g_front = 0;
static int g_write = 1;
/* Which generation g_render/g_nrender were snapshotted from. The chams engine
   must use this, not g_front: the read thread can swap between the snapshot and
   the draw, pairing one frame's entities with another frame's bones. */
static int g_render_gen = 0;
static CRITICAL_SECTION g_cs;
static int g_menu_open = 0;
static int g_menu_sel = 0;
/* Menu window state. Position is user-draggable and persisted so it comes back
   where it was left instead of dead centre every session. */
static int  g_menu_x = -1, g_menu_y = -1;   /* -1 = not placed yet -> centre */
static int  g_menu_drag = 0;                /* mouse held on the title bar */
static int  g_menu_dx = 0, g_menu_dy = 0;   /* grab offset inside the title */
static DWORD g_menu_anim = 0;               /* TickCount when the menu opened */
static int  g_menu_hover = -1;
static int  g_menu_page = 0;                /* 0 = ESP, 1 = Visuals, 2 = System */
static float g_menu_alpha = 1.0f;

#define MENU_W 400
#define MENU_TITLE_H 34
#define MENU_ROW_H 26

/* ---------------------------------------------------------------------
   Configurable palette. Every marker colour used to be a compile-time
   constant, so a lobby full of identical-looking markers could not be told
   apart. These are runtime values now, edited on the Colors page and saved
   with the rest of the config.

   ХРАНЕНИЕ ПЕРЕЕХАЛО В МОСТ. g_pal больше не отдельная переменная —
   это вид на BR_Config.color[] из menu_bridge.h. Чит и ImGui-меню
   смотрят в ОДНУ память, поэтому правка цвета в меню меняет маркер
   в том же кадре. Индексы pal_row() совпадают с индексами BR_ColorSlot.
   --------------------------------------------------------------------- */
typedef struct {
    /* marker colours by faction class */
    unsigned char blue[3], red[3], green[3], own[3], unknown[3];
    int dim_pct;          /* brightness of the "not visible" variant, percent */
    /* per-element HUD colours. use_faction=1 keeps the element tied to the
       faction colour (the old behaviour); 0 uses the custom colour below. */
    unsigned char name[3],  weapon[3], distance[3], armor[3], head[3];
    unsigned char downed[3];   /* colour for a knocked-down player */
    unsigned char hp_hi[3], hp_mid[3], hp_lo[3];
    int name_use_fac, distance_use_fac, head_use_fac;
} PALETTE;

/* Вид на палитру внутри моста. Поля-указатели, а не копии: запись через
 * g_pal_v.blue[i] попадает прямо в br_get()->color[0].rgb[i]. */
typedef struct {
    unsigned char* blue; unsigned char* red;  unsigned char* green;
    unsigned char* own;  unsigned char* unknown;
    unsigned char* name; unsigned char* weapon;
    unsigned char* distance; unsigned char* armor;
    unsigned char* head; unsigned char* downed;
    unsigned char* hp_hi; unsigned char* hp_mid; unsigned char* hp_lo;
    unsigned char* l81;
    unsigned char* aa_fov;    /* цвет круга FOV ассиста (слот 14) */
} PALVIEW;

static PALVIEW g_pal_v;
static int* g_dim_pct = NULL;
/* use_fac в BR_ColorSlot — ОДИН байт. Указатель на int читал бы четыре
 * байта: use_fac + reserved + первые два байта следующего слота. При
 * use_fac=0 и rgb[0..1]=255 это давало 0xFFFF0000 != 0, и ветка «цвет от
 * фракции» срабатывала всегда — правка цвета в меню игнорировалась.
 * Держим unsigned char* и сравниваем как байт. */
static unsigned char* g_name_fac = NULL, *g_dist_fac = NULL, *g_head_fac = NULL;

/* Указатели на слоты моста. Вызывается один раз из WinMain ПОСЛЕ того,
 * как br_get() создал экземпляр (иначе слоты ещё нулевые). */
static void pal_bind(void) {
    BR_Config* b = br_get();
    /* Один элемент = один слот. Флаги use_fac стоят рядом со своими
     * цветами — пишем их в тот же слот, что и RGB, иначе правка цвета
     * не доедет до маркера (флаг в соседнем слоте оставит приоритет
     * за цветом фракции). */
    g_pal_v.blue    = b->color[0].rgb;
    g_pal_v.red     = b->color[1].rgb;
    g_pal_v.green   = b->color[2].rgb;
    g_pal_v.own     = b->color[3].rgb;
    g_pal_v.unknown = b->color[4].rgb;
    g_pal_v.downed  = b->color[5].rgb;
    g_pal_v.name    = b->color[6].rgb;
    g_pal_v.weapon  = b->color[7].rgb;
    g_pal_v.distance= b->color[8].rgb;
    g_pal_v.armor   = b->color[9].rgb;
    g_pal_v.head    = b->color[10].rgb;
    g_pal_v.hp_hi   = b->color[11].rgb;
    g_pal_v.hp_mid  = b->color[12].rgb;
    g_pal_v.hp_lo   = b->color[12].rgb;
    g_pal_v.l81     = b->color[13].rgb;
    g_pal_v.aa_fov  = b->color[14].rgb;
    g_dim_pct   = &b->dim_pct;
    g_name_fac  = &b->color[6].use_fac;
    g_dist_fac  = &b->color[8].use_fac;
    g_head_fac  = &b->color[10].use_fac;
}

/* Старые имена становятся макросами поверх моста. Одна строка здесь —
 * и все ~170 обращений g_cfg./g_pal. в файле начинают читать мост.
 *
 * Именно пофайловые макросы на каждое поле, а не `#define g_cfg br_get()->`:
 * последнее дало бы `br_get()->.show_box` и не собралось бы. */
#define g_cfg_enabled         (br_get()->enabled)
#define g_cfg_show_box        (br_get()->show_box)
#define g_cfg_show_skeleton   (br_get()->show_skeleton)
#define g_cfg_show_name       (br_get()->show_name)
#define g_cfg_show_health     (br_get()->show_health)
#define g_cfg_show_distance   (br_get()->show_distance)
#define g_cfg_show_team       (br_get()->show_team)
#define g_cfg_only_visible    (br_get()->only_visible)
#define g_cfg_max_distance    (br_get()->max_distance)
#define g_cfg_skel_distance   (br_get()->skel_distance)
#define g_cfg_show_weapon     (br_get()->show_weapon)
#define g_cfg_show_armor      (br_get()->show_armor)
#define g_cfg_show_head       (br_get()->show_head)
#define g_cfg_show_l81_users  (br_get()->show_l81_users)
#define g_cfg_show_proj_trail (br_get()->show_proj_trail)
#define g_cfg_line_w          (br_get()->line_w)
#define g_cfg_show_vehicles   (br_get()->show_vehicles)
#define g_cfg_veh_armed_only  (br_get()->veh_armed_only)
#define g_cfg_veh_show_hp     (br_get()->veh_show_hp)
#define g_cfg_show_lead       (br_get()->show_lead)
#define g_cfg_lead_numbers    (br_get()->lead_numbers)
#define g_cfg_lead_vehicles   (br_get()->lead_vehicles)
#define g_cfg_lead_zeroing    (br_get()->lead_zeroing)
#define g_cfg_cham_on         (br_get()->cham_on)
#define g_cfg_cham_team       (br_get()->cham_team)
/* Aim assist: конфиг тот же BR_Config; поля в конце структуры. */
#define g_cfg_aa_on           (br_get()->aa_on)
#define g_cfg_aa_fov          (br_get()->aa_show_fov)
#define g_cfg_aa_fov_px       (br_get()->aa_fov_px)
#define g_cfg_aa_key          (br_get()->aa_key)
#define g_cfg_aa_smooth       (br_get()->aa_smooth)
#define g_cfg_aa_visible      (br_get()->aa_visible)
#define g_cfg_aa_dist         (br_get()->aa_dist)
#define g_cfg_aa_team         (br_get()->aa_team)
#define g_cfg_aa_head         (br_get()->aa_head)
#define g_cfg_aa_fov_world    (br_get()->aa_zoom_mode)   /* старое имя ключа */
#define g_cfg_aa_zoom_mode    (br_get()->aa_zoom_mode)
#define g_cfg_aa_gain_ref_x100 (br_get()->aa_gain_ref_x100)
#define g_cfg_aa_gain_x100    (br_get()->aa_gain_x100)
#define g_cfg_aa_gain_y100    (br_get()->aa_gain_y100)

/* Границы ползунка круга FOV. Объявлены здесь, а не рядом с движком: их
   проверяет ещё и cfg_load, который стоит выше по файлу. */
#define AA_FOV_PX_MIN       30
#define AA_FOV_PX_MAX      600
#include "chams_hook.h"

/* Config file beside the executable: keeps the menu position and every toggle
   across restarts, which is what "Save" and the auto-save on close write. */
static void cfg_path(char* out, size_t n) {
    char exe[MAX_PATH]; exe[0]=0;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash+1) = 0;
    snprintf(out, n, "%sCoreWD.cfg", exe);
}

static void cfg_save(void) {
    char p[MAX_PATH]; cfg_path(p, sizeof(p));
    FILE* f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "CoreWD 2\n");
    fprintf(f, "menu_x %d\nmenu_y %d\npage %d\n", g_menu_x, g_menu_y, g_menu_page);
    fprintf(f, "enabled %d\nshow_box %d\nshow_skeleton %d\nshow_name %d\nshow_health %d\n",
            g_cfg_enabled, g_cfg_show_box, g_cfg_show_skeleton, g_cfg_show_name, g_cfg_show_health);
    fprintf(f, "show_distance %d\nshow_team %d\nonly_visible %d\nshow_weapon %d\n",
            g_cfg_show_distance, g_cfg_show_team, g_cfg_only_visible, g_cfg_show_weapon);
    fprintf(f, "max_distance %d\nskel_distance %d\nshow_armor %d\n",
            g_cfg_max_distance, g_cfg_skel_distance, g_cfg_show_armor);
    fprintf(f, "show_head %d\ndim_pct %d\nshow_proj_trail %d\n", g_cfg_show_head, (*g_dim_pct), g_cfg_show_proj_trail);
    fprintf(f, "cham_on %d\ncham_team %d\n", g_cfg_cham_on, g_cfg_cham_team);
    /* Aim assist. Ключ пишется числом (VK), чтобы не зависеть от раскладки. */
    fprintf(f, "aa_on %d\naa_show_fov %d\naa_fov_px %d\naa_key %d\n", g_cfg_aa_on, g_cfg_aa_fov, g_cfg_aa_fov_px, g_cfg_aa_key);
    fprintf(f, "aa_smooth %d\naa_visible %d\naa_dist %d\naa_team %d\naa_head %d\naa_zoom_mode %d\n", g_cfg_aa_smooth, g_cfg_aa_visible, g_cfg_aa_dist, g_cfg_aa_team, g_cfg_aa_head, g_cfg_aa_zoom_mode);
    /* Измеренная калибровка и опорный масштаб: они не зависят от того,
       куда смотрит игрок, и следующая сессия стартует уже с ними —
       иначе на снайперском прицеле контур снова расходился бы с нуля. */
    fprintf(f, "aa_gain_x100 %d\naa_gain_y100 %d\naa_gain_ref_x100 %d\n",
            br_get()->aa_gain_x100, br_get()->aa_gain_y100, br_get()->aa_gain_ref_x100);
    for (int i = 0; i < 15; i++) {
        BR_ColorSlot* cs = &br_get()->color[i];
        fprintf(f, "pal %d %d %d %d %d %d\n",
                i, cs->rgb[0], cs->rgb[1], cs->rgb[2], cs->use_fac, 0);
    }
    fclose(f);
}

static void cfg_defaults(void) {
    g_cfg_enabled=1; g_cfg_show_box=1; g_cfg_show_skeleton=1; g_cfg_show_name=1;
    g_cfg_show_health=1; g_cfg_show_distance=1; g_cfg_show_weapon=1;
    g_cfg_show_team=0; g_cfg_only_visible=0;
    g_cfg_max_distance=30000; g_cfg_skel_distance=15000; g_cfg_show_armor=1; g_cfg_show_head=1; g_cfg_show_l81_users=1;
    g_cfg_show_vehicles=1; g_cfg_veh_armed_only=0; g_cfg_veh_show_hp=1;
    g_cfg_show_lead=1; g_cfg_lead_numbers=0; g_cfg_lead_vehicles=1; g_cfg_lead_zeroing=1;
    g_cfg_line_w=1; g_cfg_show_proj_trail=1;
    g_cfg_cham_on=0; g_cfg_cham_team=0;
    /* Aim assist: по умолчанию выключен, клавиша ALT, круг 150 px. */
    g_cfg_aa_on=0; g_cfg_aa_fov=1; g_cfg_aa_fov_px=150; g_cfg_aa_key=VK_MENU;
    g_cfg_aa_smooth=5; g_cfg_aa_visible=1; g_cfg_aa_dist=0; g_cfg_aa_team=0; g_cfg_aa_head=1;
    g_cfg_aa_zoom_mode=2;      /* AUTO: зона по измеренному масштабу */
    br_get()->aa_gain_x100=2000; br_get()->aa_gain_y100=2000;
    /* Палитра — те же значения, что k_pal_default в menu_imgui.cpp.
       Одна таблица на обе стороны: Reset в меню и Reset в чите дают
       одинаковый результат. */
    static const unsigned char kdef[15][3] = {
        { 90,170,255}, {255, 70, 70}, { 80,255,120}, { 90,220,255},
        {200,200,200}, { 90, 90, 90}, {255,255,255}, {190,190,190},
        {255,255,255}, {120,170,255}, {255, 80, 80}, { 60,230, 90},
        {240,200, 50}, {255,165,  0}, {220,220,220},
    };
    for (int i = 0; i < 15; i++) {
        BR_ColorSlot* cs = &br_get()->color[i];
        cs->rgb[0] = kdef[i][0]; cs->rgb[1] = kdef[i][1]; cs->rgb[2] = kdef[i][2];
        /* use_fac только для 6..10 (Name/Weapon/Distance/Armor/Head) и ВЫКЛ:
         * это настраиваемые текстовые элементы, пользователь красит их сам
         * и цвет не должен зависеть от фракции цели. Слоты 0..5 — сами
         * фракционные, у них флага нет. */
        cs->use_fac = 0;
    }    (*g_dim_pct) = 50;
}

static void cfg_load(void) {
    char p[MAX_PATH]; cfg_path(p, sizeof(p));
    FILE* f = fopen(p, "r");
    if (!f) return;
    char line[160]; char key[64];
    while (fgets(line, sizeof(line), f)) {
        /* Новый формат палитры: "pal <index> <r> <g> <b> <use_fac> <pad>".
           Индекс — позиция слота в BR_Config.color[], поэтому чит и меню
           всегда говорят об одном и том же цвете. */
        {
            int idx, r, g, b, uf, pad;
            if (sscanf(line, "pal %d %d %d %d %d %d", &idx, &r, &g, &b, &uf, &pad) == 6
                && idx >= 0 && idx < BR_COLOR_COUNT) {
                BR_ColorSlot* cs = &br_get()->color[idx];
                cs->rgb[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
                cs->rgb[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
                cs->rgb[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
                cs->use_fac = (unsigned char)(uf ? 1 : 0);
                continue;
            }
        }
        /* Старый формат: "pal_<name> r g b" — читаем, чтобы конфиг,
           созданный прошлой версией, не терялся при обновлении. */
        {
            int a, b2, c2;
            if (sscanf(line, "%63s %d %d %d", key, &a, &b2, &c2) == 4) {
                int idx = -1;
                if      (!strcmp(key,"pal_blue"))     idx = 0;
                else if (!strcmp(key,"pal_red"))      idx = 1;
                else if (!strcmp(key,"pal_green"))    idx = 2;
                else if (!strcmp(key,"pal_own"))      idx = 3;
                else if (!strcmp(key,"pal_unknown"))  idx = 4;
                else if (!strcmp(key,"pal_downed"))   idx = 5;
                else if (!strcmp(key,"pal_name"))     idx = 6;
                else if (!strcmp(key,"pal_weapon"))   idx = 7;
                else if (!strcmp(key,"pal_distance")) idx = 8;
                else if (!strcmp(key,"pal_armor"))    idx = 9;
                else if (!strcmp(key,"pal_head"))     idx = 10;
                else if (!strcmp(key,"pal_hphi"))     idx = 11;
                else if (!strcmp(key,"pal_hpmid"))    idx = 12;
                else if (!strcmp(key,"pal_hplo"))     idx = 12;
                if (idx >= 0) {
                    BR_ColorSlot* cs = &br_get()->color[idx];
                    cs->rgb[0]=(unsigned char)(a<0?0:a>255?255:a);
                    cs->rgb[1]=(unsigned char)(b2<0?0:b2>255?255:b2);
                    cs->rgb[2]=(unsigned char)(c2<0?0:c2>255?255:c2);
                    continue;
                }
            }
        }
        int val;
        if (sscanf(line, "%63s %d", key, &val) != 2) continue;
        if      (!strcmp(key,"menu_x"))        g_menu_x = val;
        else if (!strcmp(key,"menu_y"))        g_menu_y = val;
        else if (!strcmp(key,"page"))          g_menu_page = val;
        else if (!strcmp(key,"enabled"))       g_cfg_enabled = val;
        else if (!strcmp(key,"show_box"))      g_cfg_show_box = val;
        else if (!strcmp(key,"show_skeleton")) g_cfg_show_skeleton = val;
        else if (!strcmp(key,"show_name"))     g_cfg_show_name = val;
        else if (!strcmp(key,"show_health"))   g_cfg_show_health = val;
        else if (!strcmp(key,"show_distance")) g_cfg_show_distance = val;
        else if (!strcmp(key,"show_team"))     g_cfg_show_team = val;
        else if (!strcmp(key,"only_visible"))  g_cfg_only_visible = val;
        else if (!strcmp(key,"show_weapon"))   g_cfg_show_weapon = val;
        else if (!strcmp(key,"line_w"))         g_cfg_line_w = val;
        else if (!strcmp(key,"show_proj_trail")) g_cfg_show_proj_trail = val;
        else if (!strcmp(key,"max_distance"))  g_cfg_max_distance = val;
        else if (!strcmp(key,"skel_distance")) g_cfg_skel_distance = val;
        else if (!strcmp(key,"show_armor"))    g_cfg_show_armor = val;
        else if (!strcmp(key,"show_head"))     g_cfg_show_head = val;
        else if (!strcmp(key,"cham_on"))       g_cfg_cham_on = val;
        else if (!strcmp(key,"cham_team"))     g_cfg_cham_team = val;
        /* Aim assist */
        else if (!strcmp(key,"aa_on"))         g_cfg_aa_on = val;
        else if (!strcmp(key,"aa_show_fov"))   g_cfg_aa_fov = val;
        else if (!strcmp(key,"aa_fov_px"))     { if(val>=AA_FOV_PX_MIN&&val<=AA_FOV_PX_MAX) g_cfg_aa_fov_px = val; }
        else if (!strcmp(key,"aa_key"))        { if(val>=1&&val<=254) g_cfg_aa_key = val; }
        else if (!strcmp(key,"aa_smooth"))     { if(val>=1&&val<=10) g_cfg_aa_smooth = val; }
        else if (!strcmp(key,"aa_visible"))    g_cfg_aa_visible = val;
        else if (!strcmp(key,"aa_dist"))       { if(val>=0&&val<=5000) g_cfg_aa_dist = val; }
        else if (!strcmp(key,"aa_team"))       g_cfg_aa_team = val;
        else if (!strcmp(key,"aa_head"))       g_cfg_aa_head = val;
        else if (!strcmp(key,"aa_fov_world"))  { g_cfg_aa_zoom_mode = val ? 2 : 0; }
        else if (!strcmp(key,"aa_zoom_mode"))  { if(val>=0&&val<=2) g_cfg_aa_zoom_mode = val; }
        else if (!strcmp(key,"aa_gain_x100")) { if(val>=100&&val<=120000) br_get()->aa_gain_x100 = val; }
        else if (!strcmp(key,"aa_gain_y100")) { if(val>=100&&val<=120000) br_get()->aa_gain_y100 = val; }
        else if (!strcmp(key,"aa_gain_ref_x100")) { if(val>=100&&val<=120000) br_get()->aa_gain_ref_x100 = val; }
        else if (!strcmp(key,"dim_pct"))       { if(val>=10&&val<=100) (*g_dim_pct) = val; }
        else if (!strcmp(key,"name_use_fac"))  (*g_name_fac) = val;
        else if (!strcmp(key,"distance_use_fac")) (*g_dist_fac) = val;
        else if (!strcmp(key,"head_use_fac"))  (*g_head_fac) = val;
    }
    fclose(f);
}

/* Cached GDI objects: creating a pen/brush per entity per frame cost ms. */
static HPEN g_pen_team=NULL, g_pen_vis=NULL, g_pen_hid=NULL, g_pen_border=NULL;
static HPEN g_pen_blue=NULL, g_pen_red=NULL, g_pen_green=NULL, g_pen_grey=NULL;
static HPEN g_pen_blue_dim=NULL, g_pen_red_dim=NULL, g_pen_green_dim=NULL;
static HPEN g_pen_dbno=NULL, g_pen_dbno_dim=NULL;
static HBRUSH g_br_armor=NULL;
static HBRUSH g_br_panel=NULL, g_br_sel=NULL, g_br_hpbg=NULL,
             g_br_hpgr=NULL, g_br_hpyl=NULL, g_br_hprd=NULL;
static int g_scan_tick = 0;
static int g_sw=1920, g_sh=1080;   /* screen size, needed by the frustum pre-check in the read thread */
static V3  g_cam = {0};
static V3  g_cam_snapshot = {0};
static ROT g_camrot_snapshot = {0};
static double g_fov_snapshot = 120.0;
static float g_aspect_snapshot = 1.7777778f;
static ROT g_camrot = {0};
static double g_fov = 120.0;
static float g_aspect = 1.7777778f;
static int g_have_cam = 0;
static uint64_t g_myfac = 0;
static uint64_t g_mysq_lo = 0, g_mysq_hi = 0;   /* own squad id, 16 bytes */
static uint64_t g_fac_seen[16];
static int g_fac_n = 0;

static double eff_skel_dist(void);
/* Checksum of the raw translation block: unchanged between two reads means the
   engine is not ticking this mesh's pose (no animation update for unrendered
   actors). Used to decide whether the frozen-pose anchor is needed. */
static double g_last_sum[1024];
static uint64_t g_last_ptr[1024];
static int g_last_ok[1024];

/* Raw dump of FWDCharacterVitalityState (pawn+0x7F4, 0x28 bytes). The struct has
   no readable field names in the dump and its enum values are stripped, so the
   only way to find the downed flag is to watch which byte changes when a player
   goes down and comes back. Sampled from the read thread, not the menu. */
/* Transition log for FWDCharacterVitalityState (pawn+0x7F4, 0x28 bytes).
   The struct's fields are obfuscated in the dump and its enums come back empty,
   so the only honest way to identify the downed flag is to watch which byte
   flips at the exact moment a player goes down. Logging every sample produced
   16k lines with no way to tell WHEN something happened; this writes only
   transitions, with a wall-clock stamp, so the moment dj calls out maps
   directly onto a line. */
/* Local-player health block dump. The local pawn is the ONLY entity whose
   health is replicated (dj saw 0 hp while knocked, 10 after being revived),
   so the knockout flag has to live in the same component. Sweep 0x110..0x1A0
   of the vitality component on the local pawn and log byte transitions with a
   timestamp; that window brackets the live float at 0x174. */
void dump_local_health_block(void) {
    static DWORD last = 0;
    static uint8_t prev[0x90];
    static int have = 0;
    DWORD now = GetTickCount();
    if (now - last < 300) return;
    last = now;
    /* The local pawn is found from the published entity list (is_self), not a
       global - scan() keeps mypawn as a local. No self entity means no local
       pawn this frame. */
    uint64_t self_pawn = 0;
    for (int i = 0; i < g_nrender && i < 256; i++)
        if (g_render[i].is_self && pl(g_render[i].pawn)) { self_pawn = g_render[i].pawn; break; }
    if (!pl(self_pawn)) return;
    uint64_t vit = rd64(self_pawn + OFF_PAWN_VITALITY);
    if (!pl(vit)) return;
    uint8_t raw[0x90] = {0};
    if (!rd(vit + 0x110, raw, sizeof(raw))) return;
    if (!have) { memcpy(prev, raw, sizeof(raw)); have = 1; return; }
    for (int b = 0; b < 0x90; b++) {
        if (prev[b] == raw[b]) continue;
        FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\selfhealth.txt","a");
        if (lf) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(lf, "%02d:%02d:%02d.%03d  self  off=0x%03X  %02X -> %02X\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    0x110 + b, prev[b], raw[b]);
            fclose(lf);
        }
    }
    memcpy(prev, raw, sizeof(raw));
}

void dump_vitality_state(void) {
    static DWORD   last = 0;
    static uint8_t prev[256][0x28];
    static int     have[256];
    static int     init = 0;
    DWORD now = GetTickCount();
    if (now - last < 400) return;      /* 2.5 Hz: fast enough to catch a knockout */
    last = now;

    if (!init) { memset(prev, 0, sizeof(prev)); memset(have, 0, sizeof(have)); init = 1; }

    for (int i = 0; i < g_nrender && i < 256; i++) {
        ENT* e = &g_render[i];
        if (!pl(e->pawn)) continue;
        uint8_t raw[0x28] = {0};
        if (!rd(e->pawn + 0x7F4, raw, sizeof(raw))) continue;

        int slot = i;   /* entity slot is stable within a render frame */
        if (!have[slot]) { memcpy(prev[slot], raw, sizeof(raw)); have[slot] = 1; continue; }

        /* report every byte that changed, with its old and new value */
        for (int b = 0; b < 0x28; b++) {
            if (prev[slot][b] == raw[b]) continue;
            FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\vstate.txt","a");
            if (lf) {
                SYSTEMTIME st; GetLocalTime(&st);
                fprintf(lf, "%02d:%02d:%02d.%03d  %-18s hp=%6.1f  byte[0x%02X] %02X -> %02X\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        e->name, e->hp, b, prev[slot][b], raw[b]);
                fclose(lf);
            }
        }
        memcpy(prev[slot], raw, sizeof(raw));
    }
    /* drop slots that are no longer present so a re-used slot re-baselines */
    for (int i = g_nrender; i < 256; i++) have[i] = 0;
}

static void read_bones(ENT* e) {
    g_c_bones++;
    if (!pl(e->pawn)) return;
    /* Меш перечитываем, если он потерян. Запись e->mesh делается один раз при
       создании сущности, поэтому одна неудачная запись (или строка, попавшая под
       перезапись стека) убивала скелет до конца рейда: read_bones() выходил на
       !pl(mesh), а починить его было нечему. Перечитывание стоит один IOCTL и
       только для тех, у кого меша нет. */
    if (!pl(e->mesh)) {
        uint64_t m = rd64(e->pawn + OFF_PAWN_MESH);
        if (!pl(m)) return;
        e->mesh = m;
    }
    /* Bone transform array. The 06.09 build exposed ComponentSpaceTransforms
       at 0x620; the 11.09 SDK puts the two FTransform TArrays at 0xA08/0xA18.
       Reading the stale offset returned plausible-but-wrong data on part of
       the models (distorted skeletons). Try the SDK offsets first, fall back
       to the old one so both builds work. */
    /* 0xA08 is USkeletalMeshComponent::ComponentSpaceTransforms - the live
       array the animation system writes every tick regardless of visibility.
       0x620 belongs to the LOD cache (_aaaahi, TArray<FSkelMeshComponentLODInfo>
       sits at 0x7E0 and the bone cache lives inside it): it is only populated
       for meshes the renderer actually draws, so at range it holds a frozen
       pose - the "skeleton stays still while the player runs" bug, confirmed by
       a motion probe showing identical checksums for 60/60 distant players.
       Read the live array first and keep 0x620 only as a fallback. */
    uint64_t arr = rd64(e->mesh + OFF_MESH_BONES_SDK);
    int32_t  cnt = (int32_t)rd32(e->mesh + OFF_MESH_BONES_SDK + 8);
    if (!pl(arr) || cnt <= 0 || cnt > 2000) {
        arr = rd64(e->mesh + OFF_MESH_BONES);
        cnt = (int32_t)rd32(e->mesh + OFF_MESH_BONES_N);
    }
    if (!pl(arr) || cnt <= 0) return;
    e->bone_cnt = cnt;
    /* Bone transforms are in COMPONENT space (raw values like 82.1 cm around
       the model origin), so they must be composed with ComponentToWorld or the
       skeleton gets drawn at the map origin instead of on the player.
       Do not reject on scale: this build reports Scale3D.x == 0 with the other
       components at 1, and bailing out on that left every bone untransformed. */
    e->have_c2w = 0;
    if (rd(e->mesh+OFF_SC_C2W+FT_ROTATION,&e->c2w_rot,sizeof(Q4)) &&
        rd(e->mesh+OFF_SC_C2W+FT_TRANSLATION,&e->c2w_pos,sizeof(V3)) &&
        rd(e->mesh+OFF_SC_C2W+FT_SCALE3D,&e->c2w_scl,sizeof(V3))) {
        if (isfinite(e->c2w_pos.x) && isfinite(e->c2w_rot.w)) {
            /* normalise a bogus scale component to 1 instead of discarding */
            if (!isfinite(e->c2w_scl.x) || fabs(e->c2w_scl.x) < 1e-3) e->c2w_scl.x = 1.0;
            if (!isfinite(e->c2w_scl.y) || fabs(e->c2w_scl.y) < 1e-3) e->c2w_scl.y = 1.0;
            if (!isfinite(e->c2w_scl.z) || fabs(e->c2w_scl.z) < 1e-3) e->c2w_scl.z = 1.0;
            e->have_c2w = 1;
        }
    }
    uint64_t asset = rd64(e->mesh + OFF_MESH_SKINNEDASSET);
    BONEMAP* m = pl(asset) ? bm_get(asset) : NULL;
    /* Read the whole bone array. Capping the transfer at 64 KB only fetched 21
       transforms, but this game's skeletons carry 350-450 bones, so every bone
       past the cap fell back to a wrong index and the skeleton came out broken.
       Read in bursts and copy out just the bones we need. */
    static uint8_t buf[0x10000];
    static V3     bp[MAX_BONES];
    static int    bp_ok[MAX_BONES];
    /* Clamp to the COMPONENT count, not the reference count: LOD exposes
       fewer bones than the reference skeleton (w00t3d #62). Past-Num
       indices read arbitrary memory as positions. */
    int need = 0;
    static int use_idx[B__COUNT];
    for(int b=0;b<B__COUNT;b++){
        int bi = m ? m->idx[b] : BONE_FALLBACK[b];
        /* Out-of-range reference index = bone missing on this LOD, not the
           last valid bone. Clamping drew thigh-to-finger lines. */
        if(bi < 0 || (e->bone_cnt > 0 && bi >= e->bone_cnt)) bi = -1;
        use_idx[b] = bi;
        if(bi > need) need = bi;
    }
    if(need >= MAX_BONES) need = MAX_BONES - 1;

    for(int i=0;i<=need;i++) bp_ok[i]=0;

    /* Single bulk read of the bone range. Issuing one IOCTL per bone meant
       hundreds of round trips per frame; at 80 players the scan could not
       finish before the frame drew, which is why flicker scaled with the
       number of players. */
    size_t total = (size_t)(need + 1) * FT_STRIDE;
    /* static bulk buffer: malloc+free per entity per scan was a heap
       lock spike in the read thread. 148 bones * 96 B fits easily. */
    static uint8_t bulk[640*96];   /* 583-bone models need 55 KB */
    if(total > sizeof(bulk)) return;
    double bsum = 0;
    if(rd(arr, bulk, total)){
        for(int i=0;i<=need;i++){
            const uint8_t* p = bulk + (size_t)i*FT_STRIDE;
            double tx,ty,tz;
            memcpy(&tx,p+FT_TRANSLATION+0,8);
            memcpy(&ty,p+FT_TRANSLATION+8,8);
            memcpy(&tz,p+FT_TRANSLATION+16,8);
            if(!isfinite(tx)||!isfinite(ty)||!isfinite(tz)) continue;
            bsum += tx + ty*2.0 + tz*3.0;
            V3 v={tx,ty,tz};
            if(!e->have_c2w) continue;
            double sx,sy,sz;
            memcpy(&sx,p+FT_SCALE3D+0,8);
            memcpy(&sy,p+FT_SCALE3D+8,8);
            memcpy(&sz,p+FT_SCALE3D+16,8);
            /* Scale3D comes back as 0 on this build for EVERY character model
               (confirmed across 60 live skeletons), so the engine is not storing
               a usable scale here. Multiplying by it zeroed one word of every
               bone position: the skeleton collapsed into a plane. Seen from the
               side that plane faces the camera and looks fine; seen from front
               or back the character appears squashed - exactly the artefact
               reported. Treat 0 as "no scale info" and use 1. */
            if(!isfinite(sx) || fabs(sx) < 1e-4) sx = 1.0;
            if(!isfinite(sy) || fabs(sy) < 1e-4) sy = 1.0;
            if(!isfinite(sz) || fabs(sz) < 1e-4) sz = 1.0;
            V3 sc2={v.x*sx*e->c2w_scl.x, v.y*sy*e->c2w_scl.y, v.z*sz*e->c2w_scl.z};
            V3 r=qrot(&e->c2w_rot,sc2);
            v.x=r.x+e->c2w_pos.x; v.y=r.y+e->c2w_pos.y; v.z=r.z+e->c2w_pos.z;
            bp[i]=v; bp_ok[i]=1;
        }
    }

    /* Did the engine actually rewrite the pose since the last read? A checksum
       that repeats means this mesh is not being ticked (UE skips animation for
       actors it does not render), so the pose is stale even though the read
       succeeded. */
    extern int g_probe_disable_anchor;
    e->bone_live = (e->bone_sum == 0.0) || (fabs(bsum - e->bone_sum) > 1e-6);
    e->bone_sum = bsum;

    for(int b=0;b<B__COUNT;b++){
        int bi = use_idx[b];
        e->bone_ok[b]=0;
        if(bi<0||bi>need) continue;
        if(!bp_ok[bi]) continue;
        e->bones[b]=bp[bi];
        e->bone_ok[b]=1;
    }
    /* Anchor point for the frozen-pose correction. Updated ONLY while the pose
       is still live: once the engine stops ticking this mesh the anchor has to
       stay at the position where the pose was captured, otherwise the delta
       (root - anchor) is always zero and the correction never applies - which
       is exactly why the first attempt changed nothing. */
    if (e->bone_live) {
        e->bone_root = e->root;
    }
    e->bone_shift.x = e->bone_shift.y = e->bone_shift.z = 0;
    return;
}



/* ========================= weapon in hands ============================= */
#define GOBJECTS_RVA   0xD1234E0u
#define GOBJ_STRIDE    0x18u

/* ============================================================================
   L81 DETECTOR — миномёт L81 (STN_03): точная дальность + точка падения.
   ВСЕ ОФФСЕТЫ ПОДТВЕРЖДЕНЫ ЖИВЫМИ ЗАМЕРАМИ (билд 1109):
     орудие    : UObject класса "WDVehicleWeapon", у которого оружие зовётся
                 "W_STN_03_MainBarrel" (+0x330 -> UObject оружия, +0x18 -> FName idx)
     наведение : *(gun + 0x3D8) -> UWDVehicleWeaponRotationComponent
                 +0x1E8 double = текущее возвышение (град, вниз = минус)
                 +0x1F0 double = текущий азимут (град)
     позиция   : *(gun + 0x370) -> VehicleMesh; + OF_SC_RELLOC(0x168) -> 3x double (см)
     владелец  : WDStationaryVehicle  (поставленная миномётная точка)
   ИГРОВЫЕ ШКАЛЫ (со скриншота): RNG слева (метры), MIL справа (тысячные),
   азимут сверху (градусы). MIL = градусы * 17.7778.
   ============================================================================ */
#define L81_MAX_GUNS    8
#define L81_MIL_PER_DEG 17.7777778
#define L81_RAD         (PI_ / 180.0)
/* физика 81-мм миномёта: v0 ~ 250 м/с. Дальность R = v0^2*sin(2a)/g */
#define L81_V0_DEF      250.0
#define L81_G          9.80665

typedef struct {
    uint64_t gun;        /* WDVehicleWeapon*                 */
    uint64_t rotcomp;    /* UWDVehicleWeaponRotationComponent* */
    uint64_t mesh;       /* VehicleMesh                      */
    double   pitch;      /* возвышение, градусы (current)    */
    double   yaw;        /* азимут, градусы (current)        */
    double   ipos[3];    /* позиция ствола, см               */
    uint64_t reticle;    /* W_STN_03_Reticle_C* (HUD прицела) */
    double   rng_m;      /* дальность с прицела, метры (0=нет) */
    double   elev_deg;   /* возвышение с прицела, градусы     */
    double   table_m;    /* live ArtilleryRangeData interpolation */
    double   origin[3];  /* weapon root ComponentToWorld translation */
    float    range_units[32], range_metres[32];
    int      range_count;
    int      valid;
    char     wname[48];
} L81_GUN;

static L81_GUN g_l81[L81_MAX_GUNS];
static int     g_l81_n     = 0;
static int     g_l81_on    = 0;       /* тумблер фичи          */
static double  g_l81_k      = 0.0;    /* калибровка: R = k*sin(2a)+b (м) */
static double  g_l81_b      = 0.0;
static double  g_l81_v0     = 0.0;    /* 0 = L81_V0_DEF        */
static double  g_l81_gain   = 1.0;    /* ручная поправка       */
static double  g_l81_off    = 0.0;    /* сдвиг, м              */
static double  g_l81_lastrng= 0.0;    /* RNG, введённый игроком*/
static char    g_l81_msg[96]  = {0};
static DWORD   g_l81_msg_t   = 0;
static DWORD   g_l81_next    = 0;     /* throttle поиска       */

/* --- вспомогательное: имя класса объекта в локальный буфер --------------- */
static void l81_clsname(uint64_t obj, char* out, size_t n) {
    out[0] = 0;
    if (!pl(obj)) return;
    uint64_t c = rd64(obj + 0x10);
    if (!pl(c)) return;
    const char* s = fname_str(rd32(c + 0x18));
    if (!s) return;
    strncpy(out, s, n - 1); out[n - 1] = 0;
}
/* имя объекта (не класса) в локальный буфер */
static void l81_objname(uint64_t obj, char* out, size_t n) {
    out[0] = 0;
    if (!pl(obj)) return;
    const char* s = fname_str(rd32(obj + 0x18));
    if (!s) return;
    strncpy(out, s, n - 1); out[n - 1] = 0;
}

/* --- поиск живых миномётов в пуле объектов ------------------------------- */
/* ======================= SHOOT SOLUTION (lead marker) ======================
   Everything the marker needs is on the local pawn, so the whole solution is
   re-read once per read tick - the moment the player swaps rifles the flight
   time changes and the lead marker follows.

   Why the zeroing distance matters: the scope is dialled for a range, which
   physically tilts the barrel up by theta = atan(g*D_z / 2v^2). Aiming at a
   target nearer than D_z makes the shot fly HIGH, farther makes it fall SHORT.
   Ignoring it puts the marker metres off at mid range, so it is read and
   compensated, not assumed. */
static void read_shoot_solution(uint64_t mypawn) {
    if (!pl(mypawn)) return;
    /* speed from the weapon stats asset behind the behaviour component */
    uint64_t wbc = rd64(mypawn + OFF_PAWN_WEAPBEHAV);
    if (pl(wbc)) {
        uint64_t da = rd64(wbc + OFF_WBC_STATSDATA);
        if (pl(da)) {
            double sp = 0.0, gv = 0.0;
            /* InitialBulletSpeed (muzzle velocity) is the ballistically correct
               value — adapted from the UC prediction source. TypicalSpeed stays
               as the fallback: one hop deeper but always populated. */
            float ispd = 0.0f;
            if (rd(da + OFF_STATS_INITIALSPD, &ispd, sizeof(float)) && ispd > 2000.0f && ispd < 100000.0f)
                g_bullet_speed = ispd;
            else if (rd(da + OFF_STATS_STRUCT + OFF_STATS_TYPICALSPEED, &sp, sizeof(float)) && sp > 100.0 && sp < 400000.0)
                g_bullet_speed = sp;

            if (rd(da + OFF_STATS_STRUCT + OFF_STATS_GRAVITY, &gv, sizeof(float)) && gv > 1.0 && gv < 100.0)
                g_gravity = gv * 100.0;   /* m/s^2 -> cm/s^2 */

            /* cache bookkeeping: weapon unchanged + fresh cache = marker survives a failed read */
            if (wbc == g_wbc_cache_ptr && da == g_stats_cache_ptr)
                g_speed_cache_tick = GetTickCount();
            else {
                g_wbc_cache_ptr = wbc; g_stats_cache_ptr = da;
                g_speed_cache_tick = GetTickCount();
            }
        } else {
            /* stats read failed this tick: fall back to the 150 ms cache when
               the weapon component is still the same object (no swap happened) */
            if (wbc == g_wbc_cache_ptr && g_speed_cache_tick &&
                GetTickCount() - g_speed_cache_tick <= 150) {
                /* keep g_bullet_speed as-is */
            } else {
                g_wbc_cache_ptr = 0; g_stats_cache_ptr = 0; g_speed_cache_tick = 0;
            }
        }
    } else {
        /* behaviour component gone: weapon holstered or swapped — cache dies */
        g_wbc_cache_ptr = 0; g_stats_cache_ptr = 0; g_speed_cache_tick = 0;
    }
    /* zeroing: metres, straight off the pawn */
    double z = 0.0;
    if (rd(mypawn + OFF_PAWN_ZEROING, &z, sizeof(float)) && z >= 10.0 && z <= 3000.0)
        g_zeroing = z;

    g_shoot_valid = 1;
}

/* Where does a shot aimed at the CENTRE of (t) actually land, and where must
   the player aim instead?

   Model (UE world units are centimetres):
     t_flight = D / v                    - flat approximation, exact enough
                                           below ~1 km where the drop is small
                                           relative to D
     vertical compensation the scope already applies:
         theta_zero = atan(g*D_zero / (2*v^2))
         comp       = -D * tan(theta_zero)     (negative = barrel tilted up)
     and the drop the bullet suffers over the flight:
         drop       = g * t_flight^2 / 2
     so the true aim offset (in metres, in the vertical plane) is
         h = drop - D*tan(theta_zero)
   Horizontally the target keeps its velocity, so lead = v_target * t_flight. */
static void solve_lead(ENT* e, const V3* cam) {
    e->lead_ok = 0;
    if (!g_shoot_valid || g_bullet_speed < 100.0) return;
    if (!e->vel_ok) return;

    double dx = e->root.x - cam->x;
    double dy = e->root.y - cam->y;
    double dz = e->root.z - cam->z;
    double D = sqrt(dx*dx + dy*dy + dz*dz);      /* cm */
    if (D < 500.0 || D > 200000.0) return;       /* under 5 m or over 2 km: skip */

    const double V = g_bullet_speed;             /* cm/s */
    double tf = D / V;                           /* seconds of flight */
    if (tf > 6.0) return;                        /* nonsense guard */

    /* lead: target velocity times flight time, in world space */
    double lead_x = e->vx * tf;
    double lead_y = e->vy * tf;
    double lead_z = e->vz * tf;

    /* vertical: drop minus what the dialled zeroing already compensates */
    const double g = g_gravity;                  /* cm/s^2 */
    double drop = 0.5 * g * tf * tf;             /* cm */
    double dz_cm = (g_cfg_lead_zeroing ? g_zeroing : 0.0) * 100.0;  /* dialled range */
    double theta = atan((g * dz_cm) / (2.0 * V * V));
    double comp = D * tan(theta);                /* cm already corrected by the scope */
    double vert = drop - comp;                   /* + = must aim higher than the body */

    /* Lead in the horizontal plane only. The vertical part is NOT a lead: the
       target does not fly upward, so leading vertically would be wrong. Vertical
       is pure ballistics - drop versus the scope's own compensation - expressed
       as how much higher than the body the shot must be placed.
       Note vert>0 means "aim above the body". */
    V3 aim;
    aim.x = e->root.x + lead_x;
    aim.y = e->root.y + lead_y;
    aim.z = e->root.z + vert;        /* UE Z is up; +vert = aim above the body */

    e->lead_dist = D / 100.0;
    e->lead_time = tf;
    e->lead_drop = vert / 100.0;     /* metres, signed: + = hold high */
    e->lead_aim  = aim;
    e->lead_ok = 1;
}


/* Is this actor class a vehicle we want to mark? Wrecks (MDestroyedVehicle)
   and gameplay cues (GC_*) are excluded: they are scenery, not targets. */
static int is_vehicle_class(const char* cn) {
    if (!cn || !cn[0]) return 0;
    if (strstr(cn, "Destroyed")) return 0;      /* wreck: scenery, not a target */
    if (!strncmp(cn, "GC_", 3)) return 0;       /* gameplay cue (effects)      */
    if (!strncmp(cn, "MVehicle", 8)) return 0;  /* environment settings actor  */
    if (!strncmp(cn, "WDVehicleWeapon", 15)) return 0; /* weapon, not a chassis */
    if (strstr(cn, "Component")) return 0;      /* components are not actors   */
    /* Two different naming schemes reach us, and BOTH have to match:
         1. native classes  - WDRotaryVehicle, WDWheeledVehiclePawn, ...
         2. content Blueprints - B_TNK_01_C, B_PZH2000_C, B_WHL_07_C
       The tanks and the self-propelled howitzer are Blueprints (B_TNK_01_C)
       whose name contains no "Vehicle" substring at all, which is exactly why
       they never appeared: the helicopter family is native (WDRotaryVehicle)
       and matched, armour did not.
       Matching the content prefix (WHL_/TNK_/ROT_) tracks the asset family
       used by the variant tags, so new chassis are covered without a rebuild. */
    if (strstr(cn, "WheeledVehicle") || strstr(cn, "RotaryVehicle")
        || strstr(cn, "TrackedVehicle") || strstr(cn, "StationaryVehicle")
        || strstr(cn, "AirplaneVehicle") || strstr(cn, "BaseVehicle")
        || strstr(cn, "VehiclePawn")) return 1;
    /* Blueprint chassis: B_TNK_01_C, B_PZH2000_C, B_WHL_02_C, B_ROT_03_C.
       The leading 'B_' keeps environment meshes (SM_*, BP_*) out. */
    if (cn[0]=='B' && cn[1]=='_') {
        if (strstr(cn,"WHL_") || strstr(cn,"TNK_") || strstr(cn,"ROT_")
            || strstr(cn,"PZH")  || strstr(cn,"AA_")  || strstr(cn,"STN_"))
            return 1;
    }
    return 0;
}

/* ======================= VEHICLE VARIANT TABLE =============================
   The vehicle actor carries a UModularVehicleVariant object at +0x3A8 whose
   object name is the variant tag the game's own content pipeline uses:
       VV_WHL_01_Default            Bobcat
       VV_WHL_02_Default            Kodiak
       VV_WHL_02_Pickup             Kodiak [Pickup]
       VV_WHL_02_SUV_RingTurret     Kodiak [M249]
       VV_WHL_05_Humvee_Default     Humvee
       VV_WHL_05_Humvee_RingTurret  Humvee [M249]
       VV_WHL_05_Humvee_RingMinigun Humvee [Minigun]
       VV_WHL_06_DuneBuggy_Default  Buggy
       VV_WHL_07_Default            Ural
       VV_WHL_07_Battle             Ural Defender
       VV_WHL_07_ArmouredBattle     Ural Defender [M249]
       VV_TNK_01_Artillery          SPH-2   (self-propelled howitzer)
       VV_TNK_01_Heavy              L2A6
       VV_ROT_03_Default            MH-6
       VV_ROT_03_MachineGuns        AH-6M [Miniguns]
       VV_ROT_03_RocketPods         AH-6R [Rockets]
       VV_ROT_04_Default            Z20 Lakota
       VV_ROT_04_MountedMachineGuns Z20 Lakota [Miniguns]
       VV_Attack_ROT_02             Havoc

   Variant tags are stable content identifiers, so this table survives patches
   that shuffle struct offsets - only OFF_VEH_VARIANT needs a re-check.
   Verified live on the shooting range against all six vehicle kinds. */
typedef struct { const char* tag; const char* label; int armed; int kind; } VEHTAB;

static const VEHTAB g_vehtab[] = {
    /* --- wheeled --- */
    { "VV_WHL_01_Default",            "Bobcat",                0, 2 },
    { "VV_WHL_06_DuneBuggy",          "Buggy",                 0, 2 },
    { "VV_WHL_02_Default",            "Kodiak",                0, 2 },
    { "VV_WHL_02_Pickup",             "Kodiak [Pickup]",       0, 2 },
    { "VV_WHL_02_SUV_RingTurret",     "Kodiak [M249]",         1, 2 },
    { "VV_WHL_05_Humvee_Default",     "Humvee",                0, 2 },
    { "VV_WHL_05_Humvee_RingTurret",  "Humvee [M249]",         1, 2 },
    { "VV_WHL_05_Humvee_RingMinigun", "Humvee [Minigun]",      1, 2 },
    { "VV_WHL_07_Default",            "Ural",                  0, 2 },
    { "VV_WHL_07_Battle",             "Ural Defender",         0, 2 },
    { "VV_WHL_07_ArmouredBattle",     "Ural Defender [M249]",  1, 2 },
    /* --- tracked --- */
    { "VV_TNK_01_Artillery",          "SPH-2",                 1, 4 },
    { "VV_TNK_01_Heavy",              "L2A6",                  1, 4 },
    /* --- rotary --- */
    { "VV_ROT_03_Default",            "MH-6",                  0, 3 },
    { "VV_ROT_03_MachineGuns",        "AH-6M [Miniguns]",      1, 3 },
    { "VV_ROT_03_RocketPods",         "AH-6R [Rockets]",       1, 3 },
    { "VV_ROT_04_Default",            "Z20 Lakota",            0, 3 },
    { "VV_ROT_04_MountedMachineGuns", "Z20 Lakota [Miniguns]", 1, 3 },
    { "VV_Attack_ROT_02",             "Havoc",                 1, 3 },
};

/* Read the variant object name from the vehicle actor and match it against the
   table. Longest-prefix wins, so "[Minigun]" is never shadowed by "Humvee".
   Unknown variants fall back to the raw tag, so new content stays visible. */
static int veh_variant_label(uint64_t veh, char* label, int label_sz, int* armed, int* kind) {
    if (!pl(veh)) return 0;
    uint64_t var = rd64(veh + OFF_VEH_VARIANT);
    if (!pl(var)) return 0;
    char tag[96];
    l81_objname(var, tag, sizeof(tag));
    if (!tag[0]) return 0;

    const VEHTAB* best = 0; size_t best_len = 0;
    for (size_t i = 0; i < sizeof(g_vehtab) / sizeof(g_vehtab[0]); i++) {
        size_t L = strlen(g_vehtab[i].tag);
        if (L > best_len && !strncmp(tag, g_vehtab[i].tag, L)) { best = &g_vehtab[i]; best_len = L; }
    }
    if (!best) {
        strncpy(label, tag, label_sz - 1); label[label_sz - 1] = 0;
        *armed = 0; *kind = 2;
        return 1;
    }
    strncpy(label, best->label, label_sz - 1); label[label_sz - 1] = 0;
    *armed = best->armed; *kind = best->kind;
    return 1;
}

/* Run the lead solver over the freshly scanned entities. Called at the end of
   scan(), so every entry has a current root and (after a tick or two) a
   smoothed velocity. The camera is the local one being published. */
static void solve_leads(ENT* ents, int n, const V3* cam) {
    if (!g_shoot_valid) return;
    for (int i = 0; i < n; i++) {
        ENT* e = &ents[i];
        if (e->is_self) continue;
        solve_lead(e, cam);
    }
}

static int l81_read_range_table(L81_GUN* gg) {
    uint64_t ext_array=rd64(gg->gun+0x338);
    uint32_t count=rd32(gg->gun+0x340);
    if (!pl(ext_array) || count<1 || count>16) return 0;
    for (uint32_t i=0;i<count;i++) {
        uint64_t ext=rd64(ext_array+(uint64_t)i*8);
        if (!pl(ext)) continue;
        uint64_t da=rd64(ext+0x358);
        if (!pl(da) || rd32(da+0x2F0)!=2) continue;
        uint64_t rows=rd64(da+0x3C8);
        uint32_t n=rd32(da+0x3D0);
        if (!pl(rows) || n<2 || n>32) continue;
        float units[32], metres[32];
        int valid=1;
        float maxdist=0;
        for (uint32_t j=0;j<n;j++) {
            float row[3];
            if (!rd(rows+(uint64_t)j*12,row,sizeof(row)) ||
                !isfinite(row[0]) || !isfinite(row[1]) ||
                row[0]<-2000.f || row[0]>2000.f || row[1]<=0.f || row[1]>100000.f) {
                valid=0; break;
            }
            units[j]=row[0]; metres[j]=row[1];
            if (row[1]>maxdist) maxdist=row[1];
        }
        if (!valid) continue;
        float scale=maxdist>=5000.f?0.01f:1.f;
        for (uint32_t j=0;j<n;j++) gg->range_metres[j]=metres[j]*scale;
        memcpy(gg->range_units,units,n*sizeof(float));
        gg->range_count=(int)n;
        return 1;
    }
    return 0;
}

static double l81_table_range(const L81_GUN* gg,double pitch) {
    if (!gg || gg->range_count<2) return 0.0;
    const double unit=(pitch-2.0)*(1000.0*PI_/180.0);
    int n=gg->range_count;
    if (unit<=gg->range_units[0]) return gg->range_metres[0];
    if (unit>=gg->range_units[n-1]) return gg->range_metres[n-1];
    for (int i=0;i<n-1;i++) {
        double lo=gg->range_units[i], hi=gg->range_units[i+1];
        if (unit<lo || unit>hi || hi<=lo) continue;
        double t=(unit-lo)/(hi-lo);
        return gg->range_metres[i]+t*(gg->range_metres[i+1]-gg->range_metres[i]);
    }
    return 0.0;
}

static int l81_find(void) {
    uint64_t tbl = rd64(g_base + GOBJECTS_RVA);
    if (!pl(tbl)) return 0;
    int n = 0;
    for (int ci = 0; ci < 260 && n < L81_MAX_GUNS; ci++) {
        uint64_t chunk = rd64(tbl + (uint64_t)ci * 8);
        if (!pl(chunk)) continue;
        for (int oi = 0; oi < 0x10000 && n < L81_MAX_GUNS; oi++) {
            uint64_t o = rd64(chunk + (uint64_t)oi * GOBJ_STRIDE + 8);
            if (!pl(o)) continue;
            char cn[64]; l81_clsname(o, cn, sizeof(cn));
            if (!cn[0]) continue;
            if (strcmp(cn, "WDVehicleWeapon") && strcmp(cn, "WDMountedWeapon")) continue;

            uint64_t wn = rd64(o + 0x330);
            if (!pl(wn)) continue;
            char wnm[48]; l81_objname(wn, wnm, sizeof(wnm));
            if (!wnm[0]) continue;
            if (!strstr(wnm, "STN_03") && !strstr(wnm, "Mortar") && !strstr(wnm, "mortar")) continue;

            uint64_t rc = rd64(o + 0x3D8);
            if (!pl(rc)) continue;
            double p = rdd(rc + 0x260), y = rdd(rc + 0x268);
            if (p != p || y != y) continue;                 /* NaN  */
            if (p < -400.0 || p > 400.0 || y < -1000.0 || y > 1000.0) continue;

            uint64_t mesh = rd64(o + 0x370);
            double px = 0, py = 0, pz = 0;
            if (pl(mesh)) { px = rdd(mesh+OFF_SC_RELLOC); py = rdd(mesh+OFF_SC_RELLOC+8); pz = rdd(mesh+OFF_SC_RELLOC+16); }

            L81_GUN* gg = &g_l81[n];
            memset(gg, 0, sizeof(*gg));
            gg->gun = o; gg->rotcomp = rc; gg->mesh = mesh;
            gg->pitch = p; gg->yaw = y;
            gg->ipos[0] = px; gg->ipos[1] = py; gg->ipos[2] = pz;
            gg->valid = 1;
            uint64_t root=rd64(o+0x1C0);
            if (pl(root)) {
                gg->origin[0]=rdd(root+0x230);
                gg->origin[1]=rdd(root+0x238);
                gg->origin[2]=rdd(root+0x240);
            }
            l81_read_range_table(gg);
            gg->table_m=l81_table_range(gg,p);
            /* Ретикл HUD: единственный живой W_STN_03_Reticle_C. Дальность
               прицела и возвышение читаются ОТТУДА, а не считаются физикой —
               клиент хранит уже посчитанные игрой значения. */
            if (g_l81_on) {
                uint64_t tbl2 = rd64(g_base + GOBJECTS_RVA);
                if (pl(tbl2)) {
                    for (int cj = 0; cj < 260 && !gg->reticle; cj++) {
                        uint64_t ch2 = rd64(tbl2 + (uint64_t)cj * 8);
                        if (!pl(ch2)) continue;
                        for (int oj = 0; oj < 0x10000; oj++) {
                            uint64_t o2 = rd64(ch2 + (uint64_t)oj * GOBJ_STRIDE + 8);
                            if (!pl(o2)) continue;
                            char c2[64]; l81_clsname(o2, c2, sizeof(c2));
                            if (strcmp(c2, "W_STN_03_Reticle_C")) continue;
                            double e = rdf(o2 + 0x698);      /* возвышение, град */
                            double r = rdf(o2 + 0x6A8);      /* дальность, м     */
                            if (e != e || r != r) continue;
                            if (e <= 0.0 || e > 89.0) continue;
                            if (r <= 0.0 || r > 4000.0) continue;
                            gg->reticle = o2; gg->elev_deg = e; gg->rng_m = r;
                            break;
                        }
                    }
                }
            }
            strncpy(gg->wname, wnm, sizeof(gg->wname) - 1);
            n++;
        }
    }
    g_l81_n = n;
    {
        FILE* f = fopen("l81_runtime.log", "a");
        if (f) {
            fprintf(f, "find n=%d rows=%d pitch=%.3f range=%.2f origin=%.0f,%.0f,%.0f\n",
                n, n ? g_l81[0].range_count : 0, n ? g_l81[0].pitch : 0.0,
                n ? g_l81[0].table_m : 0.0, n ? g_l81[0].origin[0] : 0.0,
                n ? g_l81[0].origin[1] : 0.0, n ? g_l81[0].origin[2] : 0.0);
            fclose(f);
        }
    }
    return n;
}

/* --- обновление углов найденных орудий ----------------------------------- */
static double l81_range_m(double pitch_deg);
static void l81_update(void) {
    for (int i = 0; i < g_l81_n; i++) {
        L81_GUN* gg = &g_l81[i];
        double p = rdd(gg->rotcomp + 0x260);
        double y = rdd(gg->rotcomp + 0x268);
        if (p != p || y != y) { gg->valid = 0; continue; }
        gg->pitch = p; gg->yaw = y; gg->valid = 1;
        gg->table_m = l81_table_range(gg,p);
        if (pl(gg->reticle)) {
            double e = rdf(gg->reticle + 0x698);
            double r = rdf(gg->reticle + 0x6A8);
            if (e == e && e > 0.0 && e < 89.0)  gg->elev_deg = e;
            if (r == r && r > 0.0 && r < 4000.0) gg->rng_m = r;

        }
    }
}

/* --- дальность по углу возвышения, метры --------------------------------- */
static double l81_range_m(double pitch_deg);
static double l81_range_m_gun(const L81_GUN* gg) {
    if (gg && gg->rng_m > 1.0) return gg->rng_m;      /* ГЛАВНЫЙ путь: с HUD */
    return l81_range_m(gg ? gg->pitch : 0.0);         /* запасной: физика    */
}
static double l81_range_m(double pitch_deg) {
    /* Runtime ArtilleryRangeData from W_STN_03_MainBarrel.
       unitRaw is sight mils; rows are metres on this build. */
    static const float unit_raw[17] = {
        -500.f,-450.f,-400.f,-350.f,-300.f,-250.f,-200.f,-150.f,
        -100.f,-50.f,0.f,50.f,100.f,150.f,200.f,250.f,300.f
    };
    static const float dist_m[17] = {
        684.f,661.f,637.f,609.f,578.f,545.f,510.f,470.f,
        430.f,385.f,340.f,290.f,240.f,187.f,132.f,110.f,80.f
    };
    double unit = (pitch_deg - 2.0) * 17.45329252;
    if (unit <= unit_raw[0]) return dist_m[0];
    if (unit >= unit_raw[16]) return dist_m[16];
    for (int i=0; i<16; i++) {
        if (unit >= unit_raw[i] && unit <= unit_raw[i+1]) {
            double u=(unit-unit_raw[i])/(unit_raw[i+1]-unit_raw[i]);
            return dist_m[i] + (dist_m[i+1]-dist_m[i])*u;
        }
    }
    return 340.0;
}
static double l81_mil_of(double pitch_deg) { return fabs(pitch_deg) * L81_MIL_PER_DEG; }

/* --- точка падения в мировых см ----------------------------------------- */
static void l81_impact(const L81_GUN* gg, double* out) {
    double R = l81_range_m(gg->pitch) * 100.0;
    double y = gg->yaw * L81_RAD;
    out[0] = gg->ipos[0] + R * cos(y);
    out[1] = gg->ipos[1] + R * sin(y);
    out[2] = gg->ipos[2];
}

static void l81_calibrate(double rng_m) {
    if (g_l81_n <= 0 || rng_m <= 0.0) return;
    double a = fabs(g_l81[0].pitch) * L81_RAD;
    double s = sin(2.0 * a);
    if (s < 1e-4) {
        strcpy(g_l81_msg, "L81: angle too shallow to calibrate");
        g_l81_msg_t = GetTickCount();
        return;
    }
    g_l81_k  = rng_m / s;
    g_l81_b  = 0.0;
    g_l81_lastrng = rng_m;
    _snprintf(g_l81_msg, sizeof(g_l81_msg), "L81 calibrated: %.0f m @ %.1f deg", rng_m, fabs(g_l81[0].pitch));
    g_l81_msg_t = GetTickCount();
}

/* --- тумблер + сброс ---------------------------------------------------- */
static void l81_toggle(void) {
    g_l81_on = !g_l81_on;
    if (g_l81_on) { l81_find(); }
    else          { g_l81_n = 0; }
    _snprintf(g_l81_msg, sizeof(g_l81_msg), "L81 Detector: %s (%d gun%s)",
              g_l81_on ? "ON" : "OFF", g_l81_n, g_l81_n == 1 ? "" : "s");
    g_l81_msg_t = GetTickCount();
}

/* Resolve a TWeakObjectPtr index to the object address via the GObjects
   chunk table. Serial-number check skipped: a stale index yields a wrong
   name once, then the next meta tick rewrites it. */
static uint64_t weak_resolve(uint64_t weak_addr) {
    uint32_t idx = rd32(weak_addr);
    if (!idx || idx > 2000000u) return 0;
    uint64_t tbl = rd64(g_base + GOBJECTS_RVA);
    if (!pl(tbl)) return 0;
    uint64_t chunk = rd64(tbl + (uint64_t)(idx >> 16) * 8);
    if (!pl(chunk)) return 0;
    uint64_t obj = rd64(chunk + (uint64_t)(uint16_t)idx * GOBJ_STRIDE + 8);
    return pl(obj) ? obj : 0;
}
/* Short weapon label from the held item object: its FName usually mirrors
   the item id ("Id.Item.M4_C" style). Strip and cap. */
static void read_weapon(ENT* e, uint64_t pawn) {
    g_c_wep++;
    e->wep[0]=0;
    /* Weapon of ANY player (remote included): the held item pointer lives on
       the character ANIM INSTANCE, which replicates for every rendered pawn -
       the WeaponBehaviorComponent copy is only filled for the local player.
         pawn+0x390 -> SkeletalMeshComponent
         mesh+0x958 -> AnimInstance (UWDCharacterAnimInstance)
         anim+0xC54 -> TWeakObjectPtr<ABHItem>  (held item)
         item+0x328 -> ItemId (FGameplayTag/FName) */
    uint64_t item = 0;
    {
        uint64_t mesh = rd64(pawn + OFF_PAWN_MESH);
        if (pl(mesh)) {
            uint64_t anim = rd64(mesh + OFF_MESH_ANIMINST);
            if (pl(anim)) item = weak_resolve(anim + OFF_ANIM_HELDITEM);
        }
    }
    if (!pl(item)) {
        /* fallback: local-player path through the weapon behaviour component */
        uint64_t wbc = rd64(pawn + OFF_PAWN_WEAPONCOMP);
        if (!pl(wbc)) return;
        item = weak_resolve(wbc + OFF_WBC_HELDITEM);
    }
    if (!pl(item)) return;
    /* Primary source: ABHItem::ItemId (FGameplayTag = FName index) @ item+0x328.
       "Id.Item.M4" resolves through the FName pool - stable per build, unlike
       the object instance name (which is the archetype name on the CDO). */
    const char* n = NULL;
    uint32_t idIdx = rd32(item + OFF_ITEM_ITEMID);
    if (idIdx) n = fname_str(idIdx);
    if (!n || !n[0]) {
        n = fname_str(rd32(item + 0x18));
        if (!n[0]) {
            uint64_t cls = rd64(item + 0x10);
            if (pl(cls)) n = fname_str(rd32(cls + 0x18));
        }
        if (!n[0]) return;
    }
    /* The tag name is already human-readable ("Id.Item.SKS", "Id.Item.Bandage").
       Only the placeholder ids ("WEPN_035") need a lookup table - the real
       display names live in the item data table, which is a heavy chain
       (InventorySubsystem -> ItemDefinitions); a static map is cheaper and
       covers the weapons actually seen. */
    static const struct { const char* id; const char* name; } WEPN_MAP[] = {
        {"WEPN_028","MP5"},   {"WEPN_029","Galil"}, {"WEPN_030","FAL"},
        {"WEPN_033","Bushmaster"}, {"WEPN_035","A-91"},
        {"A91","A-91"}, {"KH2002","KH-2002"}, {"TAR21","T-21"},
        {"AK74M","AK-74"}, {"M4","M4"}, {"MP9","MP9"}, {"SMG_03","PP-19"},
        {"Vector","Vector"}, {"MP43","MP-43"}, {"M500","M500"},
        {"M249","M249"}, {"LMG_02","PKM"}, {"SKS","SKS"}, {"SVDM","SVD"},
        {"RFB","RFB"}, {0,0}
    };
    const char* base = n;
    for (const char* q = n; *q; q++) if (*q=='.') base = q+1;
    char tmp[32]; size_t j=0;
    for (; base[j] && j+1<sizeof(tmp) && j<20; j++) tmp[j]=base[j];
    tmp[j]=0;
    size_t L=strlen(tmp);
    if (L>2 && tmp[L-2]=='_' && (tmp[L-1]=='C'||tmp[L-1]=='c')) { tmp[L-2]=0; L-=2; }
    if (L>8 && !strncmp(tmp,"Id_Item_",8)) { memmove(tmp, tmp+8, L-7); }
    for (int k=0; WEPN_MAP[k].id; k++) {
        if (!_stricmp(tmp, WEPN_MAP[k].id)) { snprintf(tmp,sizeof(tmp),"%s",WEPN_MAP[k].name); break; }
    }
    L=strlen(tmp);
    if (L>14) tmp[14]=0;
    snprintf(e->wep,sizeof(e->wep),"%s",tmp);
}

/* ============================= scan ==================================== */
static uint64_t probe_world(uint64_t w) {
    if(!pl(w))return 0;
    uint64_t lv=rd64(w+OFF_UW_LEVEL);
    if(!pl(lv))return 0;
    uint64_t ar=rd64(lv+0xA0);
    int32_t cn=(int32_t)rd32(lv+0xA8);
    if(!pl(ar)||cn<200||cn>200000)return 0;
    return w;
}
static uint64_t find_world(void) {
    g_c_scan++;
    static const uint32_t cand[]={RVA_UWORLD,0xD095198u,0xD0CC818u,0xCE54098u,0xCDFCA58u,0xCFB1B00u};
    for(int i=0;i<7;i++){ uint64_t w=probe_world(rd64(g_base+cand[i])); if(w)return w; }
    /* Chunked .data sweep: the old full sweep fired 2M IOCTLs per call and
       ran EVERY 7.5s while no valid world existed (menus, loading) - that is
       the "lag right after kdmapper" spike. A rolling 16K-RVA window per
       call still covers the whole 0x2000000 range in ~32 calls (~4 min),
       finds GWorld within one window in practice, and never bursts. */
    static uint32_t sweep = 0xC000000u;
    uint32_t end = sweep + 0x10000u; if(end > 0xE000000u) end = 0xE000000u;
    for(uint32_t rva=sweep;rva<end;rva+=4){
        uint64_t w=probe_world(rd64(g_base+rva)); if(w){ sweep=rva+4; return w; }
    }
    sweep = (end >= 0xE000000u) ? 0xC000000u : end;
    return 0;
}

/* Classify what a player is sitting in.
   Chain (all verified live against a mortar, a jeep and a helicopter):
       pawn + 0x758  -> UWDVehicleOperatorComponent
       op   + 0x188  -> UWDVehicleSeatComponent (a second copy sits at +0x198)
       seat + 0x0C8  -> owning actor, i.e. the vehicle itself
   The vehicle's class name decides the kind. Reading the gun's own weapon list
   was tried and yields empty names, so the owner class is the only reliable
   signal - and it is enough, because mortars are exactly AWDStationaryVehicle.
   Returns the kind in *kind; the vehicle actor goes to *actor (0 if on foot). */
static void read_vehicle_kind(uint64_t pawn, int* kind, uint64_t* actor) {
    *kind = 0; *actor = 0;
    if (!pl(pawn)) return;
    uint64_t op = rd64(pawn + OFF_PAWN_OPERATOR);
    if (!pl(op)) return;
    /* only a real operator component counts: the field can hold a stale pointer
       (0xFF-filled) for players who are not seated at all. */
    uint64_t opcls = rd64(op + 0x10);
    if (pl(opcls)) {
        const char* cn = fname_str(rd32(opcls + 0x18));
        if (!cn || !strstr(cn, "Operator")) return;
    }
    uint64_t seat = rd64(op + OFF_OP_SEAT);
    if (!pl(seat)) return;
    uint64_t seatcls = rd64(seat + 0x10);
    if (pl(seatcls)) {
        const char* sc = fname_str(rd32(seatcls + 0x18));
        if (!sc || !strstr(sc, "SeatComponent")) return;
    }
    uint64_t own = rd64(seat + OFF_SEAT_OWNER);
    if (!pl(own)) return;
    uint64_t ocls = rd64(own + 0x10);
    if (!pl(ocls)) return;
    const char* on = fname_str(rd32(ocls + 0x18));
    if (!on || !on[0]) return;
    int k = 4;                                   /* seated, but an unknown frame */
    if (strstr(on, "StationaryVehicle"))      k = 1;   /* mortar / emplacement  */
    else if (strstr(on, "RotaryVehicle"))     k = 3;   /* helicopter            */
    else if (strstr(on, "AirplaneVehicle"))   k = 3;   /* plane                 */
    else if (strstr(on, "WheeledVehicle"))    k = 2;   /* car / truck           */
    else if (strstr(on, "TrackedVehicle"))    k = 2;   /* tracked               */
    *kind = k; *actor = own;
}

/* Windowed velocity regression: slope of (t -> pos) over the last n samples,
   least squares per axis. Tick-jitter immunity: one slow tick no longer
   produces a velocity spike — it is one point among several. */
static void velwin_push(double hp_[VELWIN][4], int* n, DWORD t, double x, double y, double z,
                        double* out_vx, double* out_vy, double* out_vz) {
    if (*n < VELWIN) {
        hp_[*n][0] = (double)t; hp_[*n][1] = x; hp_[*n][2] = y; hp_[*n][3] = z;
        (*n)++;
    } else {
        for (int i = 1; i < VELWIN; i++) memcpy(hp_[i-1], hp_[i], sizeof(hp_[0]));
        hp_[VELWIN-1][0] = (double)t; hp_[VELWIN-1][1] = x; hp_[VELWIN-1][2] = y; hp_[VELWIN-1][3] = z;
    }
    *out_vx = *out_vy = *out_vz = 0;
    if (*n < 3) return;
    /* least squares slope: v = cov(t,pos)/var(t), units cm per ms -> *1000 for cm/s */
    double st=0, sx=0, sy=0, sz=0, stt=0, stx=0, sty=0, stz=0;
    for (int i = 0; i < *n; i++) {
        double t2 = hp_[i][0], X = hp_[i][1], Y = hp_[i][2], Z = hp_[i][3];
        st += t2; sx += X; sy += Y; sz += Z;
        stt += t2*t2; stx += t2*X; sty += t2*Y; stz += t2*Z;
    }
    double nn = (double)*n;
    double denom = nn*stt - st*st;
    if (fabs(denom) < 1e-9) return;
    *out_vx = (nn*stx - st*sx) / denom * 1000.0;
    *out_vy = (nn*sty - st*sy) / denom * 1000.0;
    *out_vz = (nn*stz - st*sz) / denom * 1000.0;
}

static void add_ent(uint64_t pawn, uint64_t ps, uint64_t mypawn) {
    ENT* ents = g_ents[g_write];
    int* nent = &g_nents[g_write];
    if(!pl(pawn)||!pl(ps))return;
    if(*nent>=MAX_PLAYERS)return;
    /* Look for the entity in the buffer being filled AND in the published one:
       the double buffer alternates, so a naive search of the write buffer alone
       missed every player on every other tick, re-created the record, and ran
       the full meta path (weapon read, faction, name, health) again - measured
       at 10.4k weapon reads/s against 71 expected. */
    for(int i=0;i<*nent;i++) if(ents[i].pawn==pawn){
        ENT* ke=&ents[i];
        ke->last_tick=g_scan_tick;
        /* One span-read covers the whole pawn header: RootComponent 0x1C0,
           PlayerState 0x2D8, Mesh 0x390, VitalityComponent 0x738 and the
           WeaponBehaviorComponent 0x770 all live inside 0x600 bytes. Reading
           them in a single transaction removes ~5 IOCTLs per entity per tick -
           measured load was 68k IOCTL/s, which is what tanked the frame time. */
        g_c_span++;
        /* Span covers Root..WeaponComp and FWDCharacterVitalityState+0x1C
           (pawn+0x810). Folding the downed byte into this IOCTL means the
           knockout flag is assigned every tick with no extra round-trip -
           a throttled meta-tick left revived players grey until the next
           refresh. */
        uint8_t kspan[0x658];
        uint64_t kroot=0, kvit=0, kwbc=0;
        int kspan_ok = rd(pawn+OFF_ACTOR_ROOT, kspan, sizeof(kspan));
        if(kspan_ok){
            memcpy(&kroot, kspan, 8);
            memcpy(&ke->mesh, kspan + (OFF_PAWN_MESH - OFF_ACTOR_ROOT), 8);
            memcpy(&kvit,   kspan + (OFF_PAWN_VITALITY - OFF_ACTOR_ROOT), 8);
            memcpy(&kwbc,   kspan + (OFF_PAWN_WEAPONCOMP - OFF_ACTOR_ROOT), 8);
            uint8_t dv  = kspan[OFF_PAWN_VITSTATE + OFF_VS_BLEEDOUT - OFF_ACTOR_ROOT];
            uint8_t dst = kspan[OFF_PAWN_VITSTATE + OFF_VS_STANCE  - OFF_ACTOR_ROOT];
            /* The bleedout/stance bytes are kept only as diagnostic colour
               input. The authoritative knocked-down test is health == 0,
               computed later on the read thread (see the health block). */
            ke->dbno = dv ? dv : dst;
        } else {
            ke->mesh=rd64(pawn+OFF_PAWN_MESH);
            kroot=rd64(pawn+OFF_ACTOR_ROOT);
            kvit=rd64(pawn+OFF_PAWN_VITALITY);
            kwbc=rd64(pawn+OFF_PAWN_WEAPONCOMP);
            uint8_t vv[2] = {0,0};
            if (rd(pawn + OFF_PAWN_VITSTATE + OFF_VS_BLEEDOUT, vv, 2))
                ke->dbno = vv[0] ? vv[0] : vv[1];
            else ke->dbno = 0;
        }
        if(pl(kroot)){
            g_c_relloc++;
            V3 kr; if(rd(kroot+OFF_SC_RELLOC,&kr,sizeof(V3))&&isfinite(kr.x)){
                ke->proot = ke->root;
                ke->ppos_tick = ke->pos_tick;
                ke->root = kr;
                ke->pos_tick = GetTickCount();
            }
        }
        /* Seated players: once the character is attached to a seat the pawn root
           stops being world space - it reads the seat socket's local transform,
           (0,0,~166). Everything downstream uses e->root: the distance, the
           skeleton range gate and the marker anchor. A root at the origin is
           800 m away from the map, so the entity is culled and its skeleton
           never loads - which is exactly what "no skeleton in a vehicle" looked
           like. The mesh's ComponentToWorld keeps the real world position (the
           mesh is attached to the seat, not detached), so fall back to it. */
        if (pl(ke->mesh) && fabs(ke->root.x) < 50000.0 && fabs(ke->root.y) < 50000.0) {
            V3 mp;
            if (rd(ke->mesh + OFF_SC_C2W + FT_TRANSLATION, &mp, sizeof(V3)) &&
                isfinite(mp.x) && fabs(mp.x) > 50000.0) {
                ke->proot = ke->root;
                ke->ppos_tick = ke->pos_tick;
                ke->root = mp;
                ke->pos_tick = GetTickCount();
                if (!ke->veh_kind) {
                    int vk = 0; uint64_t va = 0;
                    read_vehicle_kind(pawn, &vk, &va);
                    ke->veh_kind = vk; ke->veh_actor = va;
                }
            }
        }
        if(pl(ke->mesh)){
            /* Bones only for players that will actually be DRAWN, and only at
               10 Hz. Measured: 83 entities x 7 IOCTLs x 30 Hz = 17k IOCTLs/s
               just for skeletons, including ones behind the camera that never
               reach the screen. A cheap screen-space test (2 projections) is
               free compared to the read it saves; positions still update at
               the full rate, only the bone list refreshes slower. */
            {
                double kbdx=ke->root.x-g_cam.x, kbdy=ke->root.y-g_cam.y, kbdz=ke->root.z-g_cam.z;
                double kbdist=sqrt(kbdx*kbdx+kbdy*kbdy+kbdz*kbdz);
                if(kbdist < eff_skel_dist() && ((ke->bone_slot + g_scan_tick) % 3) == 0){
                    /* Frustum pre-check with a generous margin. Testing only the
                       player's own root against the viewport marked anyone just
                       off-screen (or right behind the camera while it turns) as
                       "not worth reading", so their bones kept the position from
                       the last accepted frame - a frozen skeleton that slides
                       around with the player. Test the projected capsule with a
                       margin instead, and always refresh a skeleton that has
                       gone stale for more than ~0.5 s. */
                    V3 kf = ke->root, kh = ke->root; kh.z += 180.0;
                    int fok = 0, hok = 0;
                    {
                        V3 kfe = ke->root, khe = ke->root; khe.z += 180.0;
                        /* project centre with margin: accept if within 1.25x the
                           viewport half-extent, which covers the player partly
                           leaving frame */
                        double axL, ayL, bxL, byL;
                        fok = w2s(&kfe,&g_cam,&g_camrot,g_fov,g_sw,g_sh,&axL,&ayL);
                        hok = w2s(&khe,&g_cam,&g_camrot,g_fov,g_sw,g_sh,&bxL,&byL);
                        if (fok) fok = (axL > -g_sw*0.25 && axL < g_sw*1.25 && ayL > -g_sh*0.25 && ayL < g_sh*1.25);
                        if (hok) hok = (bxL > -g_sw*0.25 && bxL < g_sw*1.25 && byL > -g_sh*0.25 && byL < g_sh*1.25);
                    }
                    DWORD know2 = GetTickCount();
                    int stale = (!ke->bone_tick) || (know2 - (DWORD)ke->bone_tick > 500);
                    if(fok || hok || stale) {
                        read_bones(ke);
                        ke->bone_tick = (int)know2;
                    }
                }
                if (g_cfg_cham_on && kbdist < eff_skel_dist())
                    chams_capture(ke, (int)(ke - g_ents[g_write]));
                else if (!g_cfg_cham_on)
                    g_chbn[g_write][(int)(ke - g_ents[g_write])] = 0;
            }
        }
        if((((ke->pawn>>4)+g_scan_tick)%12==0)){
            if(pl(kvit)){
                float khmax=rdf(kvit+OFF_VIT_MAXHEALTH);
                if(khmax>0&&khmax<100000.0f&&isfinite(khmax)) ke->maxhp=khmax;
                /* Health. The live value is NOT in the component's own floats.
                   UWDCharacterVitalityComponent keeps MaxHealth at 0x134 and a
                   pointer to an attribute object at 0x140; the current value is
                   a DOUBLE at +0x30 inside that object. Verified on live data:
                   remote players read 100 / 70 / 35 / 10 / 61.01 / 89.16, so
                   health IS replicated for everyone - the old code only looked
                   at the local-pawn mirror at 0x174 (always 0 for others) and
                   therefore declared remote health unavailable. */
                double curval = -1.0;
                int    have   = 0;
                {
                    uint64_t attr = rd64(kvit + OFF_VIT_HEALTHATTR);
                    if (pl(attr)) {
                        double d = rdd(attr + OFF_ATTR_CURVALUE);
                        if (isfinite(d) && d >= 0.0 && d <= 1000000.0) {
                            curval = d; have = 1;
                        }
                    }
                    /* Fallback: the local pawn also mirrors its health as an
                       f64 at 0x174. Used only when the attribute pointer is
                       unavailable (e.g. the component is still streaming in). */
                    if (!have) {
                        double d2 = rdd(kvit + OFF_VIT_CURHEALTH_ALT);
                        if (isfinite(d2) && d2 >= 0.0 && d2 <= 1000000.0 && d2 > 0.0) {
                            curval = d2; have = 1;
                        }
                    }
                }
                if (have) {
                    ke->hp = curval;
                    ke->hp_valid = 1;
                } else {
                    ke->hp = (khmax > 0.0f) ? (double)khmax : 0.0;
                    ke->hp_valid = 0;
                }
                if (ke->maxhp > 0.0 && ke->hp > ke->maxhp) ke->hp = ke->maxhp;
                if (ke->hp < 0.0) ke->hp = 0.0;

                /* Knocked / dead state. A player at zero health is down; the
                   bar drops to empty and the marker switches to the downed
                   style, which is what the task asked for. */
                ke->downed = (ke->hp_valid && ke->hp <= 0.0) ? 1 : 0;

                /* One-shot diagnostic: dump the raw float grid so the real
                   health slot can be identified from live data instead of
                   guessed from the SDK layout a second time. */
                {
                    extern int g_dump_vit;
                    static DWORD lastdump = 0;
                    DWORD nd = GetTickCount();
                    if (g_dump_vit && nd - lastdump > 3000) {
                        lastdump = nd;
                        FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\vitality.txt","a");
                        if (lf) {
                            fprintf(lf, "\n%s max=%.1f  grid:", ke->name, khmax);
                            for (int off = 0x120; off <= 0x180; off += 4) {
                                float v = rdf(kvit + off);
                                fprintf(lf, " %X:%.1f", off, (double)v);
                            }
                            fprintf(lf, "\n");
                            fclose(lf);
                        }
                    }
                }

                /* Armour. UWDPlayerInventoryComponent holds a TArray of
                   FWDReplicatedArmorDurability entries, each carrying one
                   piece's remaining durability at +0x08 of a 0x18-byte stride.
                   Sum them and track the highest total seen for this player as
                   the 100% reference: the game does not expose a per-item
                   maximum, and the first full-health sample is the honest
                   baseline. */
                {
                    uint64_t inv = rd64(pawn + OFF_PAWN_INVENTORY);
                    ke->armor = 0; ke->armor_n = 0;
                    if (pl(inv)) {
                        uint64_t arr = rd64(inv + OFF_INV_ARMOR);
                        int32_t  n   = (int32_t)rd32(inv + OFF_INV_ARMOR + 8);
                        if (pl(arr) && n > 0 && n <= 16) {
                            double sum = 0; int cnt = 0;
                            for (int a = 0; a < n; a++) {
                                float d = rdf(arr + (uint64_t)a*ARMOR_STRIDE + ARMOR_OFF_DUR);
                                if (!isfinite(d) || d < 0.0f || d > 100000.0f) continue;
                                sum += d; cnt++;
                            }
                            /* The component exposes one meaningful slot (the
                               second is always 0). Full sets read 200-204, half
                               sets 100, damaged ones lower - so the scale is
                               fixed, not relative. Using a self-observed maximum
                               (the previous approach) made the bar drift: it
                               showed "full" for whoever happened to be at their
                               own peak. 200 is the game's own full-plate value. */
                            /* Full kits read 200-204 depending on the plate
                               set, so a hard 200 made anything above it clamp
                               to a permanently full bar. Track the highest
                               value seen for THIS player (that is their own
                               full plate) but never below the game's baseline
                               of 200, so a bar that starts mid-raid is not
                               reported as full. */
                            ke->armor = sum; ke->armor_n = cnt;
                            if (sum > ke->armor_max) ke->armor_max = sum;
                            if (ke->armor_max < 200.0) ke->armor_max = 200.0;
                            if (sum > 0.0 && ke->armor_base == 0.0) ke->armor_base = sum;
                            {
                                extern int g_dump_vit;
                                static DWORD lastad = 0;
                                DWORD na = GetTickCount();
                                if (g_dump_vit && na - lastad > 3000) {
                                    lastad = na;
                                    FILE* lf2 = fopen("E:\\Project\\wardogs\\WarDogsESP\\armor.txt","a");
                                    if (lf2) {
                                        fprintf(lf2, "%-20s n=%d sum=%.1f max=%.1f |", ke->name, cnt, sum, ke->armor_max);
                                        for (int a2 = 0; a2 < n; a2++) {
                                            float d2 = rdf(arr + (uint64_t)a2*ARMOR_STRIDE + ARMOR_OFF_DUR);
                                            fprintf(lf2, " %.1f", (double)d2);
                                        }
                                        fprintf(lf2, "\n");
                                        fclose(lf2);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            {
                uint64_t kfc=rd64(ps+OFF_PS_FACTIONCOMP);
                if(pl(kfc)){ uint64_t kf=rd64(kfc+OFF_FACTION_TAG); if(kf){ ke->faction=kf; ke->fac_cls=faction_class(kf); } }
            }

            /* Weapon label: refresh once per second per entity. The chain
               (mesh -> anim -> weak ptr -> item -> ItemId -> FName) costs ~6
               IOCTLs, and running it for every entity every tick measured
               11.6k calls/s = ~70k IOCTLs/s - the single biggest load in the
               whole read loop. Phased by entity slot so the cost is spread
               evenly across ticks instead of spiking on one frame. */
            if(((g_scan_tick / 30) % 1) == 0 && (g_scan_tick % 30) == ke->bone_slot)
                read_weapon(ke, pawn);
            {
                /* Downed state, authoritative source: the vitality state
                   object array at pawn+0x9A0 holds one of
                     UWDPlayerVitalityState_Alive / _Critical / _Dead.
                   _Critical is the bleedout/knocked-down state. Class name of
                   the state object tells us which - 2 reads per element, and
                   there is normally exactly one element. */
                /* Vitality-state array probe removed: it resolved the class
                   name of every state object for EVERY player on EVERY tick
                   (~10 IOCTLs each: 2 array reads plus a class-name lookup per
                   element) and the result was discarded - the names are
                   obfuscated, so no keyword matching was possible. Left over
                   from the downed-player work dj asked to be removed. Measured
                   37k class lookups/s from this one block. */
                /* raw dump moved out of the meta tick - see below (runs every
                   tick so a downed player is caught the moment it happens) */
            }
            {
                uint64_t ksc=rd64(ps+OFF_PS_SQUADCOMP);
                if(pl(ksc)){ ke->squad_lo=rd64(ksc+OFF_SQUAD_ID); ke->squad_hi=rd64(ksc+OFF_SQUAD_ID+8); }
            }
            if(ke->name[0]==0) read_fs(ps+OFF_PS_NAME,ke->name,sizeof(ke->name));
        }

        /* Seated players MOVE - the gun turns, the vehicle drives - so the
           occupancy has to be re-resolved every tick, not only when the record
           is created. Without this the entity kept veh_kind from its first
           sighting (0, taken while still on foot) and its root stayed at the
           pawn's (0,0,96), so the marker was never drawn at all. On foot this
           is one extra pointer read that returns 0 and changes nothing. */
        {
            int vk=0; uint64_t va=0;
            read_vehicle_kind(pawn,&vk,&va);
            ke->veh_kind=vk; ke->veh_actor=va;
            if(vk && pl(va)){
                uint64_t vr = rd64(va + OFF_ACTOR_ROOT);
                V3 vp;
                if(pl(vr) && rd(vr+OFF_SC_RELLOC,&vp,sizeof(V3)) && isfinite(vp.x) && isfinite(vp.y))
                    ke->root = vp;
            }
        }

        {
            double kdx=ke->root.x-g_cam.x, kdy=ke->root.y-g_cam.y, kdz=ke->root.z-g_cam.z;
            ke->dist=sqrt(kdx*kdx+kdy*kdy+kdz*kdz);
        }
        /* Target velocity for the lead marker: windowed least-squares slope
           over the last VELWIN position samples. The old single-delta method
           inherited the read loop's 15-50 ms tick jitter directly - on an
           evenly running player the dot walked back and forth. The regression
           averages tick noise; a human or vehicle cannot exceed 120 m/s, so
           anything beyond that (teleport/respawn) resets the window instead
           of poisoning the slope. */
        {
            DWORD now = GetTickCount();
            if (ke->vel_tick && now > ke->vel_tick) {
                double dt = (now - ke->vel_tick) / 1000.0;
                if (dt > 0.0 && dt < 1.0) {
                    double nvx = (ke->root.x - ke->vel_prev.x) / dt;
                    double nvy = (ke->root.y - ke->vel_prev.y) / dt;
                    double nvz = (ke->root.z - ke->vel_prev.z) / dt;
                    if (fabs(nvx) > 12000.0 || fabs(nvy) > 12000.0 || fabs(nvz) > 12000.0) {
                        ke->hp_n = 0;      /* teleport/respawn: window resets */
                    } else {
                        double rvx, rvy, rvz;
                        velwin_push(ke->hp_, &ke->hp_n, now, ke->root.x, ke->root.y, ke->root.z,
                                    &rvx, &rvy, &rvz);
                        if (ke->hp_n >= 3 &&
                            fabs(rvx) < 12000.0 && fabs(rvy) < 12000.0 && fabs(rvz) < 12000.0) {
                            const double a = 0.30;   /* gentle blend onto the regression */
                            if (!ke->vel_ok) { ke->vx = rvx; ke->vy = rvy; ke->vz = rvz; ke->vel_ok = 1; }
                            else {
                                ke->vx += (rvx - ke->vx) * a;
                                ke->vy += (rvy - ke->vy) * a;
                                ke->vz += (rvz - ke->vz) * a;
                            }
                        }
                    }
                } else {
                    ke->hp_n = 0;          /* gap too large: stale window */
                }
            }
            ke->vel_prev = ke->root;
            ke->vel_tick = now;
        }
        return;
    }

    ENT* e=&ents[*nent];
    memset(e,0,sizeof(*e));
    e->pawn=pawn; e->ps=ps;
    e->visible = 1;   /* no occlusion model: every marker draws in its faction colour */
    e->bone_slot = (int)((pawn >> 4) % 3);
    uint64_t root=0;
    /* One span-read instead of two: RootComponent@0x1C0 and Mesh@0x3A0 sit
       0x1E0 bytes apart on the pawn. Halves the IOCTL count for every new
       entity. Falls back to two reads if the span fails.
       РАЗМЕР ЗДЕСЬ — ЧАСТЬ КОНТРАКТА, а не украшение: буфер был объявлен под
       СТАРЫЙ офсет меша (0x390 -> (0x390-0x1C0)+8 = 0x1D8) и не вырос, когда
       меш переехал на 0x3A0. Тогда span[480..487] читались ЗА концом стекового
       массива — 16 байт чужого стека, и e->mesh получал мусор. Запись e->mesh
       делается ТОЛЬКО здесь, при создании записи; read_bones() выходит на
       !pl(e->mesh), меш больше не перечитывается, а запись живёт весь рейд —
       поэтому один испорченный меш убивал скелет до конца матча. Ломались не
       все: раскладка стека за массивом в обычной ветке и в ветке с посадкой
       разная, поэтому теряли меш именно те, кого впервые увидели в технике. */
    uint8_t span[OFF_PAWN_MESH - OFF_ACTOR_ROOT + 8];
    if(rd(pawn+OFF_ACTOR_ROOT, span, sizeof(span))){
        memcpy(&root, span, 8);
        memcpy(&e->mesh, span + (OFF_PAWN_MESH - OFF_ACTOR_ROOT), 8);
    } else {
        e->mesh=rd64(pawn+OFF_PAWN_MESH);
        root=rd64(pawn+OFF_ACTOR_ROOT);
    }
    if(!pl(root))return;
    if(!rd(root+OFF_SC_RELLOC,&e->root,sizeof(V3))||!isfinite(e->root.x))return;

    /* A player manning an emplacement (mortar, AA gun) has their mesh taken off
       the animation tick: the pawn's own transform stops updating and reads as
       (0,0,96), so the ordinary marker lands at the world origin and the ESP
       filter drops the entity entirely. The GUN is still a live, ticking actor
       with a real transform, and the pawn is welded to it - so for a seated
       player the vehicle's root replaces the pawn's. Seat detection has to run
       before this point for that reason, not with the rest of the meta. */
    {
        int vk=0; uint64_t va=0;
        read_vehicle_kind(pawn,&vk,&va);
        e->veh_kind=vk; e->veh_actor=va;
        if(vk && pl(va)){
            uint64_t vr = rd64(va + OFF_ACTOR_ROOT);
            V3 vp;
            if(pl(vr) && rd(vr+OFF_SC_RELLOC,&vp,sizeof(V3)) && isfinite(vp.x) && isfinite(vp.y))
                e->root = vp;
        }
    }
    e->proot = e->root; e->pos_tick = e->ppos_tick = GetTickCount();
    /* Same seated-player fallback as the update path: if the root or the vehicle
       transform did not give a world-space position, take the mesh's. */
    if (pl(e->mesh) && fabs(e->root.x) < 50000.0 && fabs(e->root.y) < 50000.0) {
        V3 mp;
        if (rd(e->mesh + OFF_SC_C2W + FT_TRANSLATION, &mp, sizeof(V3)) &&
            isfinite(mp.x) && fabs(mp.x) > 50000.0)
            e->root = mp;
    }
    {
        uint8_t dv = 0;
        if (rd(pawn + OFF_PAWN_VITSTATE + OFF_VS_BLEEDOUT, &dv, 1))
            e->dbno = dv;
    }

    int do_meta = 1; /* first sight: meta always */
    if(do_meta)
    read_fs(ps+OFF_PS_NAME,e->name,sizeof(e->name));


    if(do_meta){
    uint64_t vit=rd64(pawn+OFF_PAWN_VITALITY);
    if(pl(vit)){
        e->maxhp=rdf(vit+OFF_VIT_MAXHEALTH);
        /* live value: attribute object at +0x140, double at +0x30 */
        uint64_t attr=rd64(vit+OFF_VIT_HEALTHATTR);
        if(pl(attr)){
            double d=rdd(attr+OFF_ATTR_CURVALUE);
            if(isfinite(d)&&d>=0.0&&d<=1000000.0) e->hp=d;
        }
    }
    }
    if(!(e->maxhp>0)||!isfinite(e->maxhp)||e->maxhp>100000.0f) e->maxhp=100.0f;
    if(!isfinite(e->hp)||e->hp<0) e->hp=0;
    if(e->hp>e->maxhp) e->hp=e->maxhp;

    if(do_meta){
    uint64_t fc=rd64(ps+OFF_PS_FACTIONCOMP);
    if(pl(fc)) { e->faction=rd64(fc+OFF_FACTION_TAG); e->fac_cls=faction_class(e->faction); }
    read_weapon(e, pawn);
    {
        uint64_t sc0=rd64(ps+OFF_PS_SQUADCOMP);
        if(pl(sc0)){ e->squad_lo=rd64(sc0+OFF_SQUAD_ID); e->squad_hi=rd64(sc0+OFF_SQUAD_ID+8); }
        else { e->squad_lo=0; e->squad_hi=0; }
    }
    }

    if(pawn==mypawn){ e->is_self=1; e->fac_slot=0; }
    else if((g_mysq_lo||g_mysq_hi) && e->squad_lo==g_mysq_lo && e->squad_hi==g_mysq_hi){
        /* Same squad => teammate, drawn in its own faction colour and hidden
           when "Show teammates" is off. Matching on the squad rather than the
           faction is what makes this reliable: every player in the lobby can
           share a colour class, but only your squad shares the id. */
        e->fac_slot=0;
    }
    else if(e->faction && e->faction==g_myfac){
        e->fac_slot=0;   /* same colour class and no squad info: still friendly */
        if(e->fac_cls<0) e->fac_cls=faction_class(g_myfac);
    }
    else {
        int slot=-1;
        for(int k=0;k<g_fac_n;k++) if(g_fac_seen[k]==e->faction){slot=k+1;break;}
        if(slot<0&&g_fac_n<15){ g_fac_seen[g_fac_n]=e->faction; slot=++g_fac_n; }
        e->fac_slot = slot<0?1:slot;
    }

    if(pl(e->mesh)){
        {
            double bdx=e->root.x-g_cam.x, bdy=e->root.y-g_cam.y, bdz=e->root.z-g_cam.z;
            double bdist=sqrt(bdx*bdx+bdy*bdy+bdz*bdz);
            if(bdist < eff_skel_dist()){ read_bones(e); chams_capture(e, (int)(e - g_ents[g_write])); }
            else { for(int zb=0;zb<B__COUNT;zb++) e->bone_ok[zb]=0; g_chbn[g_write][(int)(e - g_ents[g_write])] = 0; }
        }
    }

    double dx=e->root.x-g_cam.x, dy=e->root.y-g_cam.y, dz=e->root.z-g_cam.z;
    e->dist=sqrt(dx*dx+dy*dy+dz*dz);
    if(!isfinite(e->dist)||e->dist>2e6)return;   /* >20 km is bogus */
    e->last_tick=g_scan_tick;
    (*nent)++;
    /* ---- one-shot debug dump (dj asked why DBNO/weapon do not show) ---- */
    {
        static int dbg_n = 0;
        /* continuous: sample every 40th entity write so the log stays small
           but keeps tracking through the whole raid */
        if ((dbg_n++ % 3) == 0) {
            uint64_t vit = rd64(pawn+OFF_PAWN_VITSTATE);
            uint64_t wbc = rd64(pawn+OFF_PAWN_WEAPONCOMP);
            uint32_t wk  = wbc ? rd32(wbc+OFF_WBC_HELDITEM) : 0;
            uint64_t itm = wbc ? weak_resolve(wbc+OFF_WBC_HELDITEM) : 0;
            uint32_t iid = itm ? rd32(itm+OFF_ITEM_ITEMID) : 0;
            const char* iid_s = iid ? fname_str(iid) : "";
            FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\dbg.txt","a");
            if (lf) {
                fprintf(lf, "pawn=%llX name=%s dbno=%u vitctrl=%llX wbc=%llX weakidx=0x%X item=%llX itemid=0x%X id=%s wep='%s' hp=%.0f\n",
                    (unsigned long long)pawn, e->name, (unsigned)e->dbno,
                    (unsigned long long)vit, (unsigned long long)wbc, wk,
                    (unsigned long long)itm, iid, iid_s, e->wep, e->hp);
                fclose(lf);
            }
        }
    }
}

static void proj_sight(uint64_t a, const char* cn);
static int scan(uint64_t world, int deep) {
    ENT* ents = g_ents[g_write];
    int* nent = &g_nents[g_write];
    uint64_t gi=rd64(world+OFF_UW_GAMEINSTANCE);
    uint64_t lpc=0, mypawn=0;
    g_myfac=0;
    if(pl(gi)){
        uint64_t lpa=rd64(gi+OFF_GI_LOCALPLAYERS);
        uint64_t lp0=pl(lpa)?rd64(lpa):0;
        lpc=pl(lp0)?rd64(lp0+OFF_LP_PLAYERCONTROLLER):0;
        if(pl(lpc)){
            mypawn=rd64(lpc+OFF_PC_PAWN);
            /* The shoot solution is re-read here, where the local pawn is already
               in hand: speed from the equipped weapon's stats asset, zeroing from
               the pawn. Cheap enough for every tick. */
            read_shoot_solution(mypawn);
            uint64_t ps=rd64(lpc+OFF_PC_PLAYERSTATE);
            if(pl(ps)){
                uint64_t fc=rd64(ps+OFF_PS_FACTIONCOMP);
                if(pl(fc)) g_myfac=rd64(fc+OFF_FACTION_TAG);
                /* Own squad id: a teammate is whoever shares the squad, NOT
                   whoever shares the faction. dj plays Charlie and his mates
                   draw blue/red - faction is a colour class of the whole lobby,
                   the squad is the four players he actually spawned with. */
                uint64_t sc=rd64(ps+OFF_PS_SQUADCOMP);
                if(pl(sc)){
                    g_mysq_lo=rd64(sc+OFF_SQUAD_ID);
                    g_mysq_hi=rd64(sc+OFF_SQUAD_ID+8);
                } else { g_mysq_lo=0; g_mysq_hi=0; }
            }
        }
    }
    /* Camera goes into LOCALS, never straight into the globals: publishing it
       mid-scan let the render thread pair a NEW camera with OLD entity
       positions, which is exactly the "marker trails behind the player"
       artefact. The globals are written once, in the publish block, under the
       same lock as the entity snapshot - one consistent frame per tick. */
    V3  c_cam = g_cam; ROT c_rot = g_camrot; double c_fov = g_fov; float c_asp = g_aspect;
    g_have_cam=0;
    if(pl(lpc)){
        uint64_t cm=rd64(lpc+OFF_PC_CAMMGR);
        if(pl(cm)){
            uint64_t pov=cm+OFF_CAM_CACHE+OFF_POV;
            if(!rd(pov+POV_LOCATION,&c_cam,sizeof(V3))) c_cam.x=NAN;
            /* FRotator in FMinimalViewInfo is 3x double; read as double and
               sanity-check, because a float-sized read silently halves it. */
            if(!rd(pov+POV_ROTATION,&c_rot,sizeof(ROT))){ c_rot.p=NAN; c_rot.y=NAN; }
            if(fabs(c_rot.p)>360.0||fabs(c_rot.y)>360.0){
                c_rot.p=(double)rdf(pov+POV_ROTATION+0);
                c_rot.y=(double)rdf(pov+POV_ROTATION+4);
                c_rot.r=(double)rdf(pov+POV_ROTATION+8);
            }
            c_fov=(double)rdf(pov+POV_FOV);
            float ar = rdf(pov+0x5C); /* FMinimalViewInfo::AspectRatio (0x5C in 11.09 SDK) */
            if(ar>1.0f&&ar<3.5f) c_asp = ar;
            /* Only replace the value when it is genuinely unreadable. Do NOT
               clamp to 90: this game runs a 120 degree FOV, so forcing 90
               descaled every projection and pushed distant players off screen. */
            if(!(c_fov>1.0&&c_fov<179.0)) c_fov=120.0;
            g_have_cam=isfinite(c_cam.x)&&isfinite(c_cam.y)&&isfinite(c_rot.y);
        }
    }

    /* Carry the previous snapshot into the buffer being filled. Without this
       the write buffer starts empty every other tick, every player is treated
       as brand new, and the whole meta path (weapon chain, faction, name,
       health - ~12 IOCTLs each) re-runs for all of them: measured 10.4k weapon
       reads/s where 71/s was expected. Only the position and the rare meta
       fields are refreshed for entities we already know. */
    {
        EnterCriticalSection(&g_cs);
        int prev = g_nents[g_front];
        if (prev > MAX_PLAYERS) prev = MAX_PLAYERS;
        memcpy(g_ents[g_write], g_ents[g_front], (size_t)prev * sizeof(ENT));
        *nent = prev;
        LeaveCriticalSection(&g_cs);
        /* Bone counts are NOT carried over with the entities: an entity that is
           no longer in bone range would otherwise keep the count from two frames
           ago and draw a ghost mesh at its previous pose. */
        memset(g_chbn[g_write], 0, sizeof(g_chbn[0]));
    }
    g_scan_tick++;

    /* primary: every PlayerState in GameState->PlayerArray */
    uint64_t gs=rd64(world+OFF_UW_GAMESTATE);
    if(pl(gs)){
        uint64_t parr=rd64(gs+OFF_GS_PLAYERARRAY);
        int32_t pcnt=(int32_t)rd32(gs+OFF_GS_PLAYERARRAY+8);
        if(pl(parr)&&pcnt>0&&pcnt<=512){
            g_c_parr += pcnt;
            for(int i=0;i<pcnt;i++){
                uint64_t ps=rd64(parr+(uint64_t)i*8);
                if(!pl(ps))continue;
                uint64_t pawn=rd64(ps+OFF_PS_PAWN);
                add_ent(pawn,ps,mypawn);
            }
        }
    }

    /* Drop entities not seen this tick (left the raid / died / out of range).
       Without this the inherited snapshot would keep stale markers forever. */
    {
        int keep = 0;
        for (int i = 0; i < *nent; i++) {
            if (ents[i].last_tick == g_scan_tick) {
                if (keep != i) ents[keep] = ents[i];
                keep++;
            }
        }
        *nent = keep;
    }

    /* fallback: sweep ULevel::Actors and keep objects whose class is a player
       character. Covers players missing from PlayerArray on a big map.
       Only on the slow tick - it is the costly part of the scan. */
    if (g_cfg_show_proj_trail && g_l81_n==0 && GetTickCount()>=g_l81_next) {
        g_l81_next=GetTickCount()+5000;
        l81_find();
    }
    if (g_l81_n>0) l81_update();
    /* Live shells: position update every tick, but ONLY for known flying
       rounds (<=16 slots x 2 reads, zero when none). Discovery rides the
       chunked deep sweep below for free. The old full-list sweep every tick
       (up to 40k IOCTL on a loaded map) is what stalled the read loop and
       made every marker lag with trails on. */
    if (g_cfg_show_proj_trail) {
        for (int i = 0; i < PROJ_MAX; i++)
            if (g_proj[i].used && g_proj[i].flying && pl(g_proj[i].act))
                proj_sight(g_proj[i].act, "tick");
    }
    /* Manual fire sweep (T): one full pass right after own shot. */
    if (g_proj_rescan_req) { g_proj_rescan_req = 0; proj_full_sweep(world); }
    if(!deep) goto publish;
    g_nveh[g_write]=0;
    for(int hi=0;hi<g_nvhold;hi++) g_vhold[hi].miss++;
    uint64_t lv=rd64(world+OFF_UW_LEVEL);
    if(pl(lv)){
        uint64_t arr=rd64(lv+0xA0);
        int32_t cnt=(int32_t)rd32(lv+0xA8);
        if(pl(arr)&&cnt>0&&cnt<200000){
            if(cnt>12000)cnt=12000;
            /* Chunked sweep: the old code burned 2-3 IOCTLs on EVERY one of
               12000 actors in a single tick - a 25k-transaction burst every
               deep tick that showed up as a periodic frame-time spike.
               400 actors per deep tick covers the full list in ~2.5 s at
               100-tick period; hold-table keeps discovered pawns alive. */
            int sweep_lo, sweep_hi;
            {
                static int s_cursor = 0;
                int CH = 400;
                sweep_lo = s_cursor;
                sweep_hi = sweep_lo + CH; if(sweep_hi > cnt) sweep_hi = cnt;
                s_cursor = (sweep_hi >= cnt) ? 0 : sweep_hi;
            }
            g_c_sweep += (sweep_hi - sweep_lo);
            for(int i=sweep_lo;i<sweep_hi;i++){
                uint64_t a=rd64(arr+(uint64_t)i*8);
                if(!pl(a))continue;
                char cn[128]; obj_class(a,cn,sizeof(cn));
                if(!cn[0])continue;
                /* Shell discovery rides this chunk for free (was: full list
                   every tick). Spent rounds persist, so slow coverage is fine;
                   fresh ones catch via T-sweep or within ~2.5s here. */
                if(g_cfg_show_proj_trail && strstr(cn,"120x800")){ proj_sight(a,cn); continue; }
                /* Vehicle markers. Drawn on the VEHICLE itself, empty or occupied:
                   the point is to see what armour stands around, not only who sits
                   in it. The name comes from the variant tag (stable content id,
                   survives patches that move struct offsets). */
                if(g_cfg_show_vehicles && is_vehicle_class(cn)){
                    /* Occupied only: count seats first, skip the wreck when
                       nobody sits inside. Kills the marker spam. */
                    int vocc=0, vseats=0;
                    {
                        uint64_t vsc0=rd64(a+OFF_VEH_SEATCOMP);
                        if(pl(vsc0)){
                            uint64_t occarr0=rd64(vsc0+OFF_SEAT_OCCUPANTS);
                            int32_t occn0=(int32_t)rd32(vsc0+OFF_SEAT_OCCUPANTS+8);
                            if(occn0>0&&occn0<32&&pl(occarr0)){
                                for(int s=0;s<occn0;s++){ uint64_t op=rd64(occarr0+(uint64_t)s*8); if(pl(op)) vocc++; }
                                vseats=occn0;
                            }
                        }
                    }
                    {
                        VENT* vs = g_veh[g_write]; int* vn = &g_nveh[g_write];
                        if(*vn<MAX_VEHICLES){
                            uint64_t vroot=rd64(a+OFF_ACTOR_ROOT);
                            V3 vp={0,0,0};
                            if(pl(vroot)&&rd(vroot+OFF_SC_RELLOC,&vp,sizeof(V3))&&isfinite(vp.x)){
                                VENT* v=&vs[(*vn)++];
                                v->act=a; v->pos=vp;
                                double vdx=vp.x-g_cam.x, vdy=vp.y-g_cam.y, vdz=vp.z-g_cam.z;
                                v->dist=sqrt(vdx*vdx+vdy*vdy+vdz*vdz);
                                v->occ=vocc; v->seats=vseats;
                                /* Name from the variant tag: a content identifier
                                   (VV_WHL_05_Humvee_Default), so the table keeps
                                   working across patches. Unknown variants fall back
                                   to the raw tag, so new content stays visible. */
                                {
                                    int varm=0, vkind=0;
                                    if(veh_variant_label(a, v->label, sizeof(v->label), &varm, &vkind)){
                                        v->armed=varm; v->kind=vkind;
                                        l81_objname(rd64(a+OFF_VEH_VARIANT), v->raw, sizeof(v->raw));
                                    } else {
                                        snprintf(v->label,sizeof(v->label),"%s",cn);
                                        v->armed=0; v->kind=2;
                                    }
                                }
                                /* merge into hold table */
                                {
                                    int hi=-1;
                                    for(int k=0;k<g_nvhold;k++) if(g_vhold[k].act==a){hi=k;break;}
                                    if(hi<0&&g_nvhold<MAX_VEHICLES){hi=g_nvhold++; g_vhold[hi].act=a;}
                                    if(hi>=0){ g_vhold[hi].v=*v; g_vhold[hi].miss=0; }
                                }
                            }
                        }
                    }
                    continue;
                }
                if(!strstr(cn,"MoverPlayerCharacter")&&!strstr(cn,"MoverCharacter"))continue;
                uint64_t ps=rd64(a+0x2D8);
                add_ent(a,pl(ps)?ps:a,mypawn);
            }
        }
    }
    /* Refresh held-vehicle positions EVERY tick. The deep sweep only revisits
       a given actor once per ~5 s (400-actor chunks), so a truck driving past
       kept its stale root and the marker stood still while the vehicle moved.
       RootComponent+RelativeLocation is one read per known vehicle - cheap. */
    for(int k=0;k<g_nvhold;k++){
        uint64_t vr = rd64(g_vhold[k].v.act + OFF_ACTOR_ROOT);
        V3 vp;
        if(pl(vr) && rd(vr+OFF_SC_RELLOC,&vp,sizeof(V3)) && isfinite(vp.x)){
            g_vhold[k].v.pos = vp;
            double dx=vp.x-c_cam.x, dy=vp.y-c_cam.y, dz=vp.z-c_cam.z;
            g_vhold[k].v.dist = sqrt(dx*dx+dy*dy+dz*dz);
        }
    }
    publish:
    EnterCriticalSection(&g_cs);
    g_cam = c_cam; g_camrot = c_rot; g_fov = c_fov; g_aspect = c_asp;
    g_front = g_write;
    g_write = 1 - g_front;
    {
        /* publish from the hold table: survives deep-tick gaps */
        int w=0;
        for(int k=0;k<g_nvhold;k++){
            /* A vehicle is only re-sighted when the sweep reaches its actor
               again, so a short cutoff made the box blink out between visits.
               Vehicles do not despawn without leaving a wreck. */
            if(g_vhold[k].miss>200) continue;
            if(w<MAX_VEHICLES) g_vehrender[w++]=g_vhold[k].v;
        }
        /* compact hold */
        {
            int d=0;
            for(int k=0;k<g_nvhold;k++) if(g_vhold[k].miss<=400){ if(d!=k) g_vhold[d]=g_vhold[k]; d++; }
            g_nvhold=d;
        }
        g_nvehrender=w;
    }
    LeaveCriticalSection(&g_cs);
    return 0;
}

/* Project the render-copy entities. Screen state is keyed by pawn: the
   snapshot order can change between scans, and index-keyed smoothing then
   glued one player's points onto another (teleporting lines). */
/* Interpolated projection: entity world positions arrive at the read tick
   (15 Hz) while the overlay presents at 60 Hz. The old code smoothed screen
   coordinates with a fixed factor k=0.28 per frame, which reads as "the box
   lags and then snaps". Instead interpolate the WORLD position between the two
   most recent reads by wall-clock alpha, so the marker moves at the game's
   speed and stays smooth at any read rate. */

/* Screen-space state lives here, not in the entity buffers: the renderer used
   to write projected points into g_ents[front], which becomes the back buffer
   on the next swap and is then overwritten by the read thread - wiping the
   skeleton right after it was drawn. */
static void project_all(ENT* arr,int n,int sw,int sh,const V3* cam,const ROT* crot,double fov,float aspect) {
    DWORD now = GetTickCount();
    for(int i=0;i<n;i++){
        ENT* e=&arr[i];
        SCR* sc=&g_scr[i];
        /* NO positional interpolation: drawing a between-reads position always
           trails the real player (dj saw the box "swim" behind them). Position
           accuracy beats smoothness here - the read loop now runs fast enough
           that the box tracks the body directly. */
        (void)now;
        if(sc->pawn!=e->pawn){ memset(sc,0,sizeof(*sc)); sc->pawn=e->pawn; }

        /* Interpolate the entity's world root between the last two samples. The
           box, the name row and the skeleton anchor all hang off this point, so
           one interpolation here makes every element of the marker move at the
           target's true speed instead of stepping at the read rate. */
        ipos_push(&sc->root_ip, &e->root, now);
        sc->root_ok = ipos_get(&sc->root_ip, now, &sc->root_smooth);
        for(int b=0;b<B__COUNT;b++){
            if(!e->bone_ok[b]){ sc->bok[b]=0; continue; }
            double x,y;
            if(!w2s_a(&e->bones[b],cam,crot,fov,aspect,sw,sh,&x,&y)){
                if(sc->bok[b] && sc->miss[b] < 45){ sc->miss[b]++; continue; }
                sc->bok[b]=0; continue;
            }
            sc->miss[b]=0;
            /* Screen-space smoothing with a high factor: at 60 fps present and
               30 Hz data the raw values step every other frame, which looks
               like stutter. k=0.72 reaches the new spot in ~1 frame - smooth
               to the eye, with no visible trailing (unlike the old k=0.28). */
            if(sc->bok[b] && sc->init){
                /* k is frame-rate dependent: the same time constant that k=0.72 gave at
                   30 Hz is k=0.45 at 60 Hz. Keeping 0.72 at the higher rate would
                   pass most of the raw per-frame jitter straight through. */
                /* NO screen-space smoothing. The 0.45 factor took ~3 frames to
                   reach a new position, which is a permanent 50 ms lag on top of
                   the read latency - the marker visibly trailed the body. World
                   positions now arrive fast enough that the raw projection reads
                   smooth, and any residual jitter is removed by the read-rate
                   itself rather than by trailing the target. */
                sc->sx[b] = x;
                sc->sy[b] = y;
            } else {
                sc->sx[b]=x; sc->sy[b]=y;
            }
            sc->bok[b]=1;
        }
        sc->init = 1;

        /* --- lead marker: interpolate between the last two world samples -----
           The aim point arrives at the read rate; the screen presents faster.
           Projecting the raw sample makes the circle step. Interpolating between
           the two most recent samples by wall-clock alpha makes it move at the
           target's true speed - and unlike a smoothing filter, this adds no lag,
           because both endpoints are real measurements, not a trailing average.
           A stale pair (older than 250 ms) is dropped rather than extrapolated:
           extrapolation overshoots badly the moment a target changes direction. */
        if (e->lead_ok) {
            DWORD now = GetTickCount();
            if (!sc->lead_have) {
                sc->lead_prev = e->lead_aim; sc->lead_cur = e->lead_aim;
                sc->lead_t_prev = sc->lead_t_cur = now;
                sc->lead_have = 1;
            } else if (e->lead_aim.x != sc->lead_cur.x || e->lead_aim.y != sc->lead_cur.y
                       || e->lead_aim.z != sc->lead_cur.z) {
                /* a genuinely new sample: shift the window */
                sc->lead_prev = sc->lead_cur; sc->lead_t_prev = sc->lead_t_cur;
                sc->lead_cur  = e->lead_aim;  sc->lead_t_cur  = now;
            }
            double span = (double)(sc->lead_t_cur - sc->lead_t_prev);
            double alpha = 1.0;
            if (span > 1.0) {
                alpha = (double)(now - sc->lead_t_prev) / span;
                if (alpha > 1.0) alpha = 1.0;
                if (alpha < 0.0) alpha = 0.0;
            }
            V3 ip;
            ip.x = sc->lead_prev.x + (sc->lead_cur.x - sc->lead_prev.x) * alpha;
            ip.y = sc->lead_prev.y + (sc->lead_cur.y - sc->lead_prev.y) * alpha;
            ip.z = sc->lead_prev.z + (sc->lead_cur.z - sc->lead_prev.z) * alpha;

            double lx, ly;
            if (w2s_a(&ip, cam, crot, fov, aspect, sw, sh, &lx, &ly)) {
                sc->lead_sx = lx; sc->lead_sy = ly; sc->lead_sok = 1;
            } else {
                sc->lead_sok = 0;
            }
        } else {
            sc->lead_have = 0;
            sc->lead_sok = 0;
        }
    }
}


/* Effective skeleton distance: explicit setting wins, 0 = follow draw. */
static double eff_skel_dist(void){
    if(g_cfg_skel_distance>0) return (double)g_cfg_skel_distance;
    if(g_cfg_max_distance>0) return (double)g_cfg_max_distance;
    return 80000.0;
}   /* max_distance 0 = unlimited */

/* ============================================================================
   GDI shim -> GPU backend.

   The marker code below was written against a device context: MoveToEx/LineTo,
   Rectangle/Ellipse, FillRect with a brush, CreatePen/CreateSolidBrush. Every
   one of those calls is remapped here onto the D3D11/D2D backend, so the
   drawing logic keeps its shape while the pixels come from the GPU. Handles are
   encoded integers (never real GDI objects); the hdc argument is ignored and
   callers pass NULL.

   Why not rewrite the call sites: 400 lines of marker code with 30 GDI calls is
   exactly the kind of edit that introduces one wrong argument and ships a
   broken overlay. The shim keeps the diff small and reviewable.
   ========================================================================== */

#define GTAG_PEN 0x50454E00u    /* 'PEN' */
#define GTAG_BR  0x42520000u    /* 'BR ' */

static COLORREF g_cur_pen_c = RGB(255, 255, 255);
static float    g_cur_pen_w = 1.0f;
static COLORREF g_cur_br_c  = RGB(0, 0, 0);
static int      g_cur_br_on = 0;
static float    g_cur_x = 0.0f, g_cur_y = 0.0f;

static HPEN gdi_pen(COLORREF c, int w) {
    uintptr_t v = ((uintptr_t)GTAG_PEN << 32) |
                  ((uintptr_t)(w & 0xFF) << 24) |
                  ((uintptr_t)GetRValue(c) << 16) |
                  ((uintptr_t)GetGValue(c) << 8) |
                  (uintptr_t)GetBValue(c);
    return (HPEN)v;
}
static HBRUSH gdi_brush(COLORREF c) {
    uintptr_t v = ((uintptr_t)GTAG_BR << 32) |
                  ((uintptr_t)GetRValue(c) << 16) |
                  ((uintptr_t)GetGValue(c) << 8) |
                  (uintptr_t)GetBValue(c);
    return (HBRUSH)v;
}
static int gdi_tag(HGDIOBJ o) { return (int)(((uintptr_t)o >> 32) & 0xFFFFFF00u); }
static COLORREF gdi_col(HGDIOBJ o) {
    uintptr_t v = (uintptr_t)o;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

/* Selecting a pen or a brush updates the current draw state; the previous
   handle is returned because the call sites restore it afterwards. A real GDI
   stock object (NULL_BRUSH, WHITE_BRUSH) turns the fill off or on instead. */
static HGDIOBJ gdi_select(HGDIOBJ o) {
    static HPEN   last_pen = NULL;
    static HBRUSH last_br  = NULL;
    if (!o) return NULL;
    if (gdi_tag(o) == GTAG_PEN) {
        HPEN prev = last_pen;
        g_cur_pen_c = gdi_col(o);
        g_cur_pen_w = (float)(((uintptr_t)o >> 24) & 0xFF);
        if (g_cur_pen_w < 1.0f) g_cur_pen_w = 1.0f;
        last_pen = (HPEN)o;
        return prev;
    }
    if (gdi_tag(o) == GTAG_BR) {
        HBRUSH prev = last_br;
        g_cur_br_c = gdi_col(o);
        g_cur_br_on = 1;
        last_br = (HBRUSH)o;
        return prev;
    }
    if (o == (HGDIOBJ)GetStockObject(NULL_BRUSH)) {
        HBRUSH prev = last_br;
        g_cur_br_on = 0;
        last_br = NULL;
        return prev;
    }
    if (o == (HGDIOBJ)GetStockObject(BLACK_BRUSH)) {
        HBRUSH prev = last_br;
        g_cur_br_c = RGB(0, 0, 0);
        g_cur_br_on = 1;
        return prev;
    }
    if (o == (HGDIOBJ)GetStockObject(WHITE_BRUSH)) {
        HBRUSH prev = last_br;
        g_cur_br_c = RGB(255, 255, 255);
        g_cur_br_on = 1;
        return prev;
    }
    return o;   /* fonts and anything else: ignored by the GPU path */
}

static void gdi_moveto(float x, float y) { g_cur_x = x; g_cur_y = y; }
static void gdi_lineto(float x, float y) {
    r_line_c(g_cur_x, g_cur_y, x, y, RCOLORREF(g_cur_pen_c), g_cur_pen_w);
    g_cur_x = x; g_cur_y = y;
}
static void gdi_rect(float l, float t, float r, float b) {
    if (g_cur_br_on) r_fill(l, t, r - l, b - t, RCOLORREF(g_cur_br_c));
    r_rect(l, t, r - l, b - t, RCOLORREF(g_cur_pen_c), g_cur_pen_w);
}
static void gdi_ellipse(float l, float t, float r, float b) {
    float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
    float rx = (r - l) * 0.5f, ry = (b - t) * 0.5f;
    if (g_cur_br_on) r_fill_ellipse(cx, cy, rx, ry, RCOLORREF(g_cur_br_c));
    r_ellipse(cx, cy, rx, ry, RCOLORREF(g_cur_pen_c), g_cur_pen_w);
}
static void gdi_fillrect(const RECT* pr, HGDIOBJ br) {
    if (!pr) return;
    COLORREF c;
    if (gdi_tag(br) == GTAG_BR) c = gdi_col(br);
    else if (br == (HGDIOBJ)GetStockObject(BLACK_BRUSH)) c = RGB(0, 0, 0);
    else if (br == (HGDIOBJ)GetStockObject(WHITE_BRUSH)) c = RGB(255, 255, 255);
    else return;
    r_fill((float)pr->left, (float)pr->top,
           (float)(pr->right - pr->left), (float)(pr->bottom - pr->top),
           RCOLORREF(c));
}

#define MoveToEx(h,x,y,o)     gdi_moveto((float)(x),(float)(y))
#define LineTo(h,x,y)         gdi_lineto((float)(x),(float)(y))
#define Rectangle(h,l,t,r,b)  gdi_rect((float)(l),(float)(t),(float)(r),(float)(b))
#define Ellipse(h,l,t,r,b)    gdi_ellipse((float)(l),(float)(t),(float)(r),(float)(b))
#define FillRect(h,pr,br)     gdi_fillrect((pr),(HGDIOBJ)(br))
#define SelectObject(h,o)     gdi_select((HGDIOBJ)(o))
#define CreatePen(s,w,c)      gdi_pen((c),(w))
#define CreateSolidBrush(c)   gdi_brush((c))

/* ---- marker / chams look knobs ------------------------------------------ */
/* Chams border: the silhouette grown by N px in the pixel shader, drawn under
   the fill. 2 px reads as an outline at 1440p; 0 turns it off. */
static int      g_cham_outline     = 2;
static COLORREF g_cham_outline_col = RGB(255, 255, 255);

/* Frame rate of the overlay itself. With the GPU path the interesting number is
   no longer "how much CPU did the draw cost" but whether the composition keeps
   up with the monitor, so this is what the readout shows. */
static float g_fps = 0.0f;
static int   g_show_fps = 1;   /* hertz readout, bottom right */
static void DrawFps(int vw, int vh) {
    static DWORD t0 = 0, cnt = 0;
    DWORD now = GetTickCount();
    if (!t0) t0 = now;
    cnt++;
    if (now - t0 >= 500) {
        g_fps = (float)cnt * 1000.0f / (float)(now - t0);
        cnt = 0; t0 = now;
    }
    if (!g_show_fps) return;
    char s[48];
    snprintf(s, sizeof(s), "%.0f fps", g_fps);
    gpu_font(14.0f);
    r_text((float)(vw - 96), (float)(vh - 30), s, 0xFF00FFC0u, RT_LEFT | RT_OUTLINE);
}

static void DrawTextC(HDC hdc,int cx,int y,const char* t,COLORREF col);
static HBRUSH esp_pen_brush(COLORREF c);

/* Offline marker check: set WDE_SELFTEST=1 and the overlay draws one marker
   through the exact same primitives the real marker path uses (the GDI shim),
   at fixed screen coordinates. Lets the drawing be verified without the game -
   a screenshot shows whether lines, fills and text land where they should. */
static void DrawSelfTest(void) {
    static int on = -1;
    if (on < 0) { const char* v = getenv("WDE_SELFTEST"); on = (v && v[0] == '1') ? 1 : 0; }
    if (!on) return;

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 64, 64));
    HPEN old = (HPEN)SelectObject(NULL, pen);
    MoveToEx(NULL, 200, 200, NULL);
    LineTo(NULL, 340, 200); LineTo(NULL, 340, 520); LineTo(NULL, 200, 520); LineTo(NULL, 200, 200);
    MoveToEx(NULL, 270, 200, NULL); LineTo(NULL, 270, 280);
    LineTo(NULL, 270, 420); LineTo(NULL, 220, 500); LineTo(NULL, 270, 420); LineTo(NULL, 320, 500);
    SelectObject(NULL, old);

    RECT bar = { 200, 560, 340, 568 };
    FillRect(NULL, &bar, esp_pen_brush(RGB(64, 255, 64)));
    DrawTextC(NULL, 270, 160, "SELFTEST 87m", RGB(255, 255, 255));
}

/* Colour logic, like the reference screenshot:
     own faction = cyan, visible enemy = neon green, hidden enemy = red. */
static COLORREF esp_color(const ENT* e);
static int esp_line_w(int w){ int lw=g_cfg_line_w; if(lw<1)lw=1; if(lw>4)lw=4; return w>1?lw:1; }
static HPEN esp_pen_for(COLORREF c, int width);
static void palette_cache_reset(void);

static HPEN esp_pen(const ENT* e) {
    /* Downed highlight removed for good: the bleedout byte only flips when the
       player presses the give-up key, so it cannot identify a fresh knock-down;
       stance (0x1D) lit up crouched/prone players instead. */
    /* Teammates are colored by THEIR (own) faction color, per dj - cyan team
       pen is gone. Enemies keep the stable per-faction class color.
       Visibility = brightness: lit = visible, dim = hidden. */
    /* Pens are created on demand and keyed by colour, because the palette is
       editable at runtime: the old fixed g_pen_* objects kept drawing the
       compile-time colours no matter what the Colors page said. */
    return esp_pen_for(esp_color(e), esp_line_w(2));
}

/* Pen for an occupied mortar. Deliberately ignores the faction colour: the
   whole point is that a gunner looks different from a rifleman, and picking
   them out must not depend on reading a nickname. Colour comes from the
   dedicated "L81 Users" palette slot so it stays user-tunable. */
static HPEN esp_pen_for_l81(void) {
    return esp_pen_for(RGB(g_pal_v.l81[0], g_pal_v.l81[1], g_pal_v.l81[2]), esp_line_w(2));
}

/* Solid brushes matching each pen colour, cached like the pens: FillRect needs
   a brush, and creating one per joint per frame would churn GDI objects. */
/* Runtime palette caches. Colour is the cache key, so retuning a channel in
   the menu simply yields a new object; nothing needs explicit invalidation of
   the old handles. */
static HPEN    g_pen_cache[96]; static COLORREF g_pen_col[96]; static int g_pen_w[96]; static int g_npen = 0;
static HBRUSH  g_br_cache[64];  static COLORREF g_br_col[64]; static int g_nbr = 0;

static void palette_cache_reset(void) { g_npen = 0; g_nbr = 0; }

static HBRUSH esp_pen_brush(COLORREF c) {
    for (int i = 0; i < g_nbr; i++) if (g_br_col[i] == c) return g_br_cache[i];
    if (g_nbr >= 64) return (HBRUSH)GetStockObject(WHITE_BRUSH);
    g_br_col[g_nbr] = c;
    g_br_cache[g_nbr] = CreateSolidBrush(c);
    return g_br_cache[g_nbr++];
}

static HPEN esp_pen_for(COLORREF c, int width) {
    for (int i = 0; i < g_npen; i++) if (g_pen_col[i] == c && g_pen_w[i] == width) return g_pen_cache[i];
    /* Кэш переполнен (палитра 14 цветов × толщины 1..4): сбрасываем и строим
       заново. Прежний код возвращал WHITE_PEN — «бокс внезапно белый» при
       смене толщины после долгой сессии. Старые HPEN не утекают: DeleteObject. */
    if (g_npen >= 96) {
        for (int i = 0; i < g_npen; i++) if (g_pen_cache[i]) DeleteObject(g_pen_cache[i]);
        g_npen = 0;
    }
    g_pen_col[g_npen] = c; g_pen_w[g_npen] = width;
    g_pen_cache[g_npen] = CreatePen(PS_SOLID, width, c);
    return g_pen_cache[g_npen++];
}

/* Colour row index -> the RGB triple it edits (NULL for non-colour rows).
 * Индексы совпадают с BR_ColorSlot в menu_bridge.h и со списком k_names
 * во вкладке Colors — одна таблица на две стороны, без расхождений. */
static unsigned char* pal_row(int i) {
    /* Index == row on the Colors page (R3). Mode rows (name from faction,
       distance from faction, head from faction, head on/off) return NULL and are
       handled by the caller, so the mapping is one-to-one and readable. */
    switch (i) {
    case  0: return g_pal_v.blue;      case  1: return g_pal_v.red;
    case  2: return g_pal_v.green;     case  3: return g_pal_v.own;
    case  4: return g_pal_v.unknown;   case  5: return g_pal_v.name;
    case  7: return g_pal_v.weapon;    case  8: return g_pal_v.distance;
    case 10: return g_pal_v.armor;     case 11: return g_pal_v.head;
    case 14: return g_pal_v.hp_hi;     case 15: return g_pal_v.hp_mid;
    case 16: return g_pal_v.hp_lo;    case 17: return g_pal_v.downed;
    default: return NULL;
    }
}

static COLORREF esp_color(const ENT* e) {
    /* A knocked-down player is drawn in the dedicated grey so the difference
       between "lying down fighting" and "waiting to be revived" is visible at a
       glance. Dead players never reach the draw loop (hp filter), so a marker
       that disappears is the "dead" case. */
    if (e->downed) return RGB(g_pal_v.downed[0], g_pal_v.downed[1], g_pal_v.downed[2]); /* dark grey, whole marker */
    /* Colours come from the editable palette (Colors page). */
    const unsigned char* p = NULL;
    if      (e->fac_cls == 0) p = g_pal_v.blue;
    else if (e->fac_cls == 1) p = g_pal_v.red;
    else if (e->fac_cls == 2) p = g_pal_v.green;
    else if (e->fac_slot == 0) p = g_pal_v.own;
    else                       p = g_pal_v.unknown;
    return RGB(p[0], p[1], p[2]);
}

/* One colour for the whole marker. There is no occlusion model any more: the
   per-mesh render stamp flickered (a visible player turned grey and back), and
   per-bone needs the map's collision geometry, which this build does not
   carry. Everything draws in its faction colour. */
static COLORREF esp_bone_color(const ENT* e, int b) {
    (void)b;
    return esp_color(e);
}
static COLORREF esp_edge_color(const ENT* e, int a, int b) {
    (void)a; (void)b;
    return esp_color(e);
}
static COLORREF esp_box_color(const ENT* e) {
    return esp_color(e);
}

/* ============================ overlay ================================== */
static int g_use_ulw = 0;
static HWND g_hOverlay=NULL, g_hMenu=NULL, g_hRange=NULL;


enum { ID_ENABLED=100, ID_BOX, ID_SKEL, ID_NAME, ID_HP, ID_DIST,
       ID_TEAM, ID_VIS, ID_RANGE, ID_RANGEVAL, ID_STATUS, ID_ONEC };

static void DrawCornerBox(HDC hdc,double minx,double miny,double maxx,double maxy,HPEN pen) {
    /* Corner brackets, like the reference screenshot: no full rectangle. */
    double w=maxx-minx, h=maxy-miny;
    double L = w*0.28; if(L>h*0.28) L=h*0.28; if(L<4.0) L=4.0;
    HPEN old=(HPEN)SelectObject(hdc,pen);
    MoveToEx(hdc,(int)minx,(int)(miny+L),NULL); LineTo(hdc,(int)minx,(int)miny); LineTo(hdc,(int)(minx+L),(int)miny);
    MoveToEx(hdc,(int)(maxx-L),(int)miny,NULL); LineTo(hdc,(int)maxx,(int)miny); LineTo(hdc,(int)maxx,(int)(miny+L));
    MoveToEx(hdc,(int)minx,(int)(maxy-L),NULL); LineTo(hdc,(int)minx,(int)maxy); LineTo(hdc,(int)(minx+L),(int)maxy);
    MoveToEx(hdc,(int)(maxx-L),(int)maxy,NULL); LineTo(hdc,(int)maxx,(int)maxy); LineTo(hdc,(int)maxx,(int)(maxy-L));
    SelectObject(hdc,old);
}

/* Centered label with a 1px dark outline so it stays readable on any map.
   The text arrives as UTF-8 (nicknames are Cyrillic / accented / emoji), so it
   is converted to UTF-16 and drawn with TextOutW. TextOutA treated the UTF-8
   bytes as ANSI and rendered "Илюша Шуруп" as mojibake - the reader was fine,
   only the painting was wrong. */

static void DrawTextC(HDC hdc,int cx,int y,const char* t,COLORREF col) {
    (void)hdc;
    if(!t || !t[0]) return;
    /* DirectWrite, centred, with the halo built into the backend: one layout
       per label instead of nine TextOutW passes. UTF-8 goes in, the backend
       converts; nicknames are Cyrillic and used to need a separate path. */
    gpu_font(13.0f);
    r_text((float)cx, (float)y, t, RCOLORREF(col), RT_CENTER | RT_OUTLINE);
}

/* ============================================================================
   AIM ASSIST — доводка прицела по удержанию клавиши.

   Как это работает. WarDogs не даёт писать углы обзора: PlayerController
   держит ввод (Enhanced Input) и каждый тик перезаписывает ControlRotation из
   накопленной мыши, так что запись в поворот контроллера или камеры игнорируется
   или откатывается. Единственный внешний канал, который игра принимает как
   собственный ввод, — настоящие сообщения мыши. Поэтому ассист двигает КУРСОР
   (SendInput, MOUSEEVENTF_MOVE) на величину ошибки в пикселях, а игра сама
   доворачивает прицел своим обычным сенсором. Ничего в память не пишется.

   Цель. Берём УЖЕ посчитанную мировую позицию кости головы (ENT.bones[B_HEAD])
   — она приходит из того же чтения, что рисует маркеры, поэтому ассист не
   добавляет ни одного IOCTL в горячий путь. Круг FOV задан в пикселях и
   одновременно является радиусом захвата: что видно в круге, то и берётся.

   Калибровка. Пиксели-на-градус — единственная величина, которая зависит от
   разрешения, FOV, ADS-множителя и сенсы. Вместо того чтобы требовать её от
   пользователя, ассист её ИЗМЕРЯЕТ: сравнивает фактический доворот камеры с
   числом отправленных пикселей (скользящее среднее с утечкой) и подставляет
   результат в расчёт. Первый промах в 2–3 раза сам сходится к 1.0 за десять
   кадров, а смена оружия/прицела перестаёт ломать наводку.
   ============================================================================ */

typedef struct {
    int      on;             /* 1 = цель захвачена в этом кадре            */
    uint64_t pawn;           /* пешка цели (чтобы не переключаться зря)    */
    int      key_down;       /* клавиша удержания была нажата в прошлом кадре */
    int      moved;          /* в прошлом кадре отправлено движение мыши   */
    unsigned long t_prev;    /* время прошлого кадра с движением           */
    ROT      rot_prev;       /* поворот камеры в тот момент                */
    double   pend_x, pend_y; /* остаток пикселей, который ещё не отправили */
    double   px_sent, py_sent; /* сколько пикселей ушло в прошлом кадре    */
    double   gain_x, gain_y; /* измеренные пиксели на градус               */
    double   gain_n;         /* объём выборки измерения                    */
    int      warm;           /* 1 = калибровка снята, движок работает по ней */
    int      logical_zoom;   /* 1 = надета оптика (видно по измеренному масштабу) */
    DWORD    zoom_hold;      /* пауза после смены масштаба (не учим опору)  */
    double   err_prev;       /* ошибка прошлого кадра, для гейта калибровки */
    double   err_last;       /* остаточная ошибка, градусы                 */
} AA_STATE;

#define AA_GAIN_X_DEFAULT   20.0    /* 1920x1080, FOV 120 -> ~20 px/deg         */
#define AA_GAIN_Y_DEFAULT   20.0
#define AA_GAIN_MIN          1.0
#define AA_GAIN_MAX       1200.0
#define AA_TOLERANCE_DEG     0.30   /* не используется для допуска; см. ниже  */
#define AA_TOLERANCE_PX      3.0    /* допуск прицела в ПИКСЕЛЯХ: одинаков на
                                       любом зуме. В градусах он плавал от 3 px
                                       на хилфаере до 57 px на 12x, и прицел
                                       «наводился» далеко от головы.          */
#define AA_SEND_PERIOD_MS    15ul   /* одна порция пикселей на 15 мс: курсор
                                       доезжает до прицела кадром ИГРЫ, а не
                                       оверлея, поэтому чаще слать бессмысленно,
                                       а две команды в полёте ломают замер
                                       калибровки (делим на одну из двух).     */
#define AA_ZONE_RATIO_MAX    40.0   /* предел роста зоны под оптикой (40x)     */
#define AA_REF_ALPHA         0.02   /* скорость обучения опорного масштаба     */
#define AA_REF_MIN_SAMPLES   40.0   /* замеров до обновления опоры             */
#define AA_ZOOM_SETTLE_MS   120ul   /* пауза после смены масштаба, чтобы опора
                                       не впитала зум (смена оружия)          */
#define AA_CAL_ALPHA         0.25   /* вес нового замера в среднем              */
#define AA_CAL_ALPHA_WARM    0.05   /* тот же вес после калибровки: смена прицела
                                       переучивается, но шум контура уже не
                                       размывает оценку                         */
#define AA_CAL_SAMPLES       8.0    /* замеров до перехода на измеренный gain   */
#define AA_MAX_STEP_DEG      2.5    /* предел доворота за кадр (анти-рывок)      */
#define AA_DEADBAND_PX       1.0    /* меньше пикселя — не дёргаем курсор       */
#define AA_K_SCALE           0.45   /* общий множитель усиления контура         */
/* Усиление ДО калибровки. Пока пиксели-на-градус не измерены, полагаться на
   дефолтные 20 нельзя: на снайперском прицеле их пять, и контур с усилением
   больше единицы расходится за десять кадров — прицел уходит в разнос вместо
   наводки. Поэтому холодный старт идёт «в четверть силы» и просто медленно
   подтягивает цель, пока не наберётся восемь замеров. */
#define AA_K_COLD            0.15

static AA_STATE g_aa;
static volatile int g_aa_active = 0;   /* читает отрисовка: подсветить круг */
static double g_gain_min_x = 0, g_gain_min_y = 0;   /* опора: масштаб широкого вида */
static double g_zone_prev = 1.0;                    /* прошлое отношение зума       */

/* Опорный масштаб (самый широкий вид). Ползунок задаёт зону для широкого вида,
   поэтому опора — это МИНИМУМ измеренного масштаба, а не «текущее без оптики»:
   игра не сообщает, надета ли оптика (поле FOV на этом билде врёт), а измерить
   «состояние без оптики» неоткуда. Минимум даёт то, что нужно: зона постоянна на
   широком виде и растёт ровно на столько, на сколько сузился вид. Ассист сам
   ходит по разным видам (смена оружия, прицеливание), поэтому минимум набирается
   без участия игрока. */
static void aa_ref_track(double gx) {
    if (!(gx > AA_GAIN_MIN)) return;
    if (g_gain_min_x <= 0.0) { g_gain_min_x = gx; g_gain_min_y = g_aa.gain_y; }
    double ratio = gx / g_gain_min_x;
    if (ratio < 1.0) ratio = 1.0;
    /* Резкая смена масштаба — это смена оружия или прицела: даём контуру
       успокоиться, иначе опора впитает новый вид. */
    if (fabs(ratio - g_zone_prev) / ratio > 0.05) {
        g_zone_prev = ratio;
        g_aa.zoom_hold = GetTickCount();
        return;
    }
    g_zone_prev = ratio;
    if ((GetTickCount() - g_aa.zoom_hold) < AA_ZOOM_SETTLE_MS) return;
    if (gx < g_gain_min_x) { g_gain_min_x = gx; g_gain_min_y = g_aa.gain_y; }

    /* Оптика не сообщается ничем, кроме самого измерения, поэтому «надета ли
       оптика» — это «масштаб заметно выше самого широкого вида». Порог ×1.5
       отличает настоящий зум от подросшего за счёт подстройки gain и попадает
       между 1.2x и 1.5x. */
    g_aa.logical_zoom = (gx > g_gain_min_x * 1.5) ? 1 : 0;

    /* Опорный уровень подтягиваем МЕДЛЕННО и только когда оптика отпущена:
       иначе опора впитает текущий зум и зона снова сожмётся. */
    if (!g_aa.logical_zoom && g_aa.gain_n >= AA_REF_MIN_SAMPLES) {
        g_gain_min_x += AA_REF_ALPHA * (gx - g_gain_min_x);
        g_gain_min_y += AA_REF_ALPHA * (g_aa.gain_y - g_gain_min_y);
    }
    br_get()->aa_gain_ref_x100 = (int)(g_gain_min_x * 100.0 + 0.5);
}

/* Накопитель пикселей. SendInput с целыми dx/dy иначе теряет доли на каждом
   кадре, а ошибка в четверть пикселя на 60 Гц — это 15 px/с промаха. */
static void aa_move_mouse(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

/* Углы от камеры до мировой точки.
   yaw   = atan2(right, forward)
   pitch = atan2(up, hypot(forward, right))
   Считается в системе САМОЙ камеры, через те же строки матрицы поворота, что
   использует w2s_a: forward — строка 0, right — строка 1, up — строка 2.
   Прошлый вариант брал yaw от МИРОВОГО горизонта и завышал боковой угол вне
   центра обзора (35.3° вместо 45° на камеру, развёрнутую от оси), из-за чего
   доводка по диагонали уходила не туда. */
static int aa_angles_to(const V3* target, double* d_yaw, double* d_pitch) {
    V3 d = { target->x - g_rcam.x, target->y - g_rcam.y, target->z - g_rcam.z };
    double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(len > 0.5)) return 0;
    double m[3][3];
    rot_matrix(&g_rcamrot, m);
    double fwd = d.x * m[0][0] + d.y * m[0][1] + d.z * m[0][2];
    double rgt = d.x * m[1][0] + d.y * m[1][1] + d.z * m[1][2];
    double up  = d.x * m[2][0] + d.y * m[2][1] + d.z * m[2][2];
    if (fwd <= 0.0) return 0;                  /* сзади камеры — не цель */
    *d_yaw   = atan2(rgt, fwd) * 180.0 / PI_;
    *d_pitch = atan2(up, sqrt(fwd * fwd + rgt * rgt)) * 180.0 / PI_;
    return 1;
}

/* Коллинеарные углы: g_rcamrot.y приходит из FMinimalViewInfo и может быть
   любым числом витков, поэтому разница берётся по модулю 360. */
static double aa_wrap180(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

/* Выбор цели. Не «первый в цикле», а сначала лучшее совпадение С ПАМЯТЬЮ:
   пока прежняя цель жива и не ушла дальше 1.6 радиуса круга, держимся неё —
   иначе ассист дёргался бы между двумя соседями на границе зоны. */
/* Радиус зоны захвата в пикселях.
   Экранный режим: ползунок как есть. Мировой: радиус считается из того же
   конуса, который покрывает круг при базовом FOV (без оптики), и растёт
   пропорционально зуму. Без этого зона физически сжимается вместе с прицелом:
   150 px при FOV 120 покрывают конус 15.5°, а при FOV 10 — уже 0.78°,
   то есть цель, стоящая в круге на 1.5° в стороне, перестаёт захватываться на
   снайперском прицеле, хотя круг её накрывает. Именно это выглядит как
   «ассист нарочно не доводит». */
/* Радиус зоны захвата в пикселях — ТОЛЬКО из измеренного масштаба.
   Ползунок задаёт зону для широкого вида (без оптики). Под оптикой зона растёт
   во столько раз, во сколько выросла угловая шкала экрана, а её ассист и так
   измеряет каждый кадр (gain в пикс/градус). Отношение gain/gain_ref — это и
   есть зум, честный для любого способа, которым игра его делает: смена поля FOV,
   сужение вида, отдельная камера прицела. Никакой зависимости от поля FOV игры
   здесь больше нет — именно оно врало на этом билде (оптика не двигала его, и
   зона не росла, поэтому цель «проскальзывала за FOV»). */
static double aa_zone_ratio(void) {
    if (g_cfg_aa_zoom_mode == 0) return 1.0;          /* жёсткий экранный круг */
    if (!g_aa.warm) return 1.0;
    double ref = (double)g_cfg_aa_gain_ref_x100 / 100.0;
    if (ref < AA_GAIN_MIN) ref = AA_GAIN_X_DEFAULT;
    double r = g_aa.gain_x / ref;
    if (!(r > 0.0)) r = 1.0;
    if (r < 1.0) r = 1.0;
    if (r > AA_ZONE_RATIO_MAX) r = AA_ZONE_RATIO_MAX;
    return r;
}

static double aa_fov_px(void) {
    double px = (double)g_cfg_aa_fov_px;
    if (px < (double)AA_FOV_PX_MIN) px = (double)AA_FOV_PX_MIN;
    double out = px * aa_zone_ratio();
    double lim = (double)g_sw - 10.0;            /* больше экрана смысла нет */
    if (out > lim) out = lim;
    return out;
}

/* Выбор цели.
   Захват считается по УГЛОВОЙ ошибке до кости, переведённой в пиксели
   ИЗМЕРЕННЫМ масштабом — а не по экранной позиции bone, спроецированной
   камерой. Так захват не зависит ни от поля FOV игры (оно на оптике врёт), ни
   от FOV, которым оверлей считает проекцию, ни от того, попадает ли кость в
   кадр по мнению проектора. Мировые позиции и поворот камеры — это всё, что
   нужно, и то и другое у нас точное.
   Память цели: пока прежняя цель жива и не ушла дальше 1.6 радиуса, держимся
   неё — иначе ассист дёргался бы между двумя соседями на границе зоны. */
static ENT* aa_pick(void) {
    ENT* best_keep = NULL;
    ENT* best_new  = NULL;  double best_new_d  = 1e18;
    double r = aa_fov_px();
    double r_keep = r * 1.6;
    double gx = g_aa.warm ? g_aa.gain_x : AA_GAIN_X_DEFAULT;
    double gy = g_aa.warm ? g_aa.gain_y : AA_GAIN_Y_DEFAULT;
    double max_cm = (double)g_cfg_aa_dist * 100.0;
    if (g_cfg_aa_dist > 0 && max_cm < 1000.0) max_cm = 1000.0;   /* не меньше 10 м */

    /* Пиксельное расстояние до кости при текущем масштабе, 0 = не берём. */
    #define AA_TARGET_PX(e_, out_)                                              \
        do {                                                                    \
            int b_ = g_cfg_aa_head ? B_HEAD : B_NECK;                            \
            V3 t_;                                                              \
            if (!pl((e_)->pawn)) { out_ = -1.0; break; }                         \
            t_ = (e_)->bones[b_];                                               \
            if (!(e_)->bone_ok[b_]) t_.z -= 165.0;   /* нет кости — берём корень */ \
            double dy_, dp_;                                                    \
            if (!aa_angles_to(&t_, &dy_, &dp_)) { out_ = -1.0; break; }          \
            {   double ex = dy_ * gx, ey = dp_ * gy;                            \
                out_ = sqrt(ex * ex + ey * ey); }                                \
        } while (0)

    for (int i = 0; i < g_nrender; i++) {
        ENT* e = &g_render[i];
        if (e->is_self) continue;
        if (e->pawn == g_aa.pawn) continue;              /* прежняя — отдельно */
        if (e->fac_slot == 0 && !g_cfg_aa_team) continue; /* свои — по тумблеру */
        if (g_cfg_aa_visible && !e->visible) continue;
        if (g_cfg_aa_dist > 0 && e->dist > max_cm) continue;
        if (e->dist < 250.0) continue;                    /* 2.5 м — это свой же силуэт */
        double d;
        AA_TARGET_PX(e, d);
        if (d < 0.0) continue;                            /* сзади камеры */
        if (d <= r && d < best_new_d) { best_new_d = d; best_new = e; }
    }

    /* Прежняя цель: тот же паун должен остаться в расширенной зоне. */
    if (pl(g_aa.pawn)) {
        for (int i = 0; i < g_nrender; i++) {
            ENT* e = &g_render[i];
            if (e->pawn != g_aa.pawn) continue;
            if (e->fac_slot == 0 && !g_cfg_aa_team) break;
            if (g_cfg_aa_visible && !e->visible) break;
            if (g_cfg_aa_dist > 0 && e->dist > max_cm) break;
            double d;
            AA_TARGET_PX(e, d);
            if (d >= 0.0 && d <= r_keep) best_keep = e;
            break;
        }
    }
    #undef AA_TARGET_PX
    if (best_keep) return best_keep;
    return best_new;
}

/* Один кадр ассиста. Вызывается из отрисовки сразу после project_all, когда
   экранные координаты костей текущего кадра уже посчитаны. */
static void aa_tick(void) {
    /* --- калибровка по ПРЕДЫДУЩЕМУ кадру -------------------------------
       Ссылка для замера — состояние камеры, снятое ПОСЛЕ прошлого кадра с
       движением. Раньше она записывалась сразу после обновления g_rcamrot,
       то есть уже внутри кадра с этим движением, и разница выходила нулевой:
       gain никогда не менялся и доводка считалась по дефолту.
       Ждём 16 мс, чтобы движение успело примениться: игра обрабатывает
       сообщения мыши в своём следующем кадре. */
    if (g_aa.moved) {
        DWORD now = GetTickCount();
        /* Замер снимаем не раньше, чем через 14 мс после отправки: за это время
           в полёте была РОВНО одна порция пикселей. Отправки реже 15 мс (см.
           ниже) — именно для этого: если в полёте окажется две команды,
           фактический доворот камеры будет их суммой, а делим мы на одну, и
           оценка пикселей-на-градус уезжает. */
        if ((now - g_aa.t_prev) >= 14ul) {
            double dyaw = aa_wrap180(g_rcamrot.y - g_aa.rot_prev.y);
            double dpit = g_rcamrot.p - g_aa.rot_prev.p;
            /* Если камера ушла больше 12 градусов, это уже движение игрока, а
               не наш доворот — такой замер выбрасывается. */
            if (fabs(dyaw) < 12.0 && fabs(dpit) < 12.0) {
                /* Калибруемся только на СХОДЯЩЕМСЯ контуре. Пока ошибка растёт,
                   это автоколебание: пиксели, которые ещё летят к цели,
                   прибавляются к фактическому довороту и занижают оценку —
                   именно так дрожь прицела портила калибровку и контур
                   самоподдерживался. */
                double alpha = g_aa.warm ? AA_CAL_ALPHA_WARM : AA_CAL_ALPHA;
                int converging = !(g_aa.err_last > g_aa.err_prev + 0.05);
                if (converging) {
                    if (fabs(g_aa.px_sent) > 0.4 && fabs(dyaw) > 0.004) {
                        double gx = fabs(g_aa.px_sent) / fabs(dyaw);
                        if (gx > AA_GAIN_MIN && gx < AA_GAIN_MAX) {
                            g_aa.gain_x += alpha * (gx - g_aa.gain_x);
                            g_aa.gain_n += 1.0;
                        }
                    }
                    if (fabs(g_aa.py_sent) > 0.4 && fabs(dpit) > 0.004) {
                        double gy = fabs(g_aa.py_sent) / fabs(dpit);
                        if (gy > AA_GAIN_MIN && gy < AA_GAIN_MAX) {
                            g_aa.gain_y += alpha * (gy - g_aa.gain_y);
                            g_aa.gain_n += 1.0;
                        }
                    }
                }
                if (g_aa.gain_n > AA_CAL_SAMPLES) g_aa.warm = 1;
            }
            /* Измеренную калибровку держим в конфиге: она не зависит от того,
               куда смотрит игрок, и следующая сессия стартует уже с ней. */
            if (g_aa.warm) {
                br_get()->aa_gain_x100 = (int)(g_aa.gain_x * 100.0);
                br_get()->aa_gain_y100 = (int)(g_aa.gain_y * 100.0);
            }
            /* Опорный масштаб и признак оптики — по измеренному gain. */
            aa_ref_track(g_aa.gain_x);
            g_aa.moved = 0;
        }
    }
    if (g_aa.gain_x < AA_GAIN_MIN) g_aa.gain_x = AA_GAIN_MIN;
    if (g_aa.gain_y < AA_GAIN_MIN) g_aa.gain_y = AA_GAIN_MIN;

    int down = (GetAsyncKeyState(g_cfg_aa_key) & 0x8000) != 0;
    int want = g_cfg_aa_on && down;
    if (!want) {
        /* Отпустили: гасим цель и остаток. Синтетических движений больше нет. */
        g_aa.pawn = 0; g_aa.key_down = 0; g_aa.pend_x = g_aa.pend_y = 0;
        g_aa_active = 0;
        br_get()->telem_aa_tgt = 0;
        return;
    }
    g_aa.key_down = 1;

    ENT* e = aa_pick();
    if (!e) {
        g_aa.pawn = 0; g_aa.pend_x = g_aa.pend_y = 0;
        g_aa_active = 0;
        br_get()->telem_aa_tgt = 0;
        return;
    }
    /* Смена цели: остаток и ссылку калибровки сбрасываем — замер «доворот
       камеры против отправленных пикселей» иначе считался бы по движению,
       которого ассист не делал. */
    if (e->pawn != g_aa.pawn) {
        g_aa.pawn = e->pawn; g_aa.pend_x = g_aa.pend_y = 0;
        g_aa.moved = 0;
    }
    g_aa_active = 1;
    br_get()->telem_aa_tgt = 1;
    br_get()->telem_aa_fov = (int)(aa_fov_px() + 0.5);
    br_get()->telem_aa_zoom_x100 = (int)(aa_zone_ratio() * 100.0 + 0.5);

    /* Экранная проекция кости больше НЕ проверяется: захват и доворот считаются
       от мировых координат и поворота камеры, поэтому решение не зависит от
       того, каким FOV оверлей рисует проекцию. Из-за этого же не нужен и
       проектор в пути принятия решения. */
    double d_yaw, d_pitch;
    {
        int bb = g_cfg_aa_head ? B_HEAD : B_NECK;
        V3 aim_pt = e->bones[bb];
        if (!e->bone_ok[bb]) aim_pt.z -= 165.0;   /* кости нет — ведём в уровень головы */
        if (!aa_angles_to(&aim_pt, &d_yaw, &d_pitch)) {
            /* Цель за спиной: поворот на 180 доводкой не делается, отпускаем. */
            g_aa_active = 0;
            br_get()->telem_aa_tgt = 0;
            return;
        }
    }
    g_aa.err_last = sqrt(d_yaw * d_yaw + d_pitch * d_pitch);
    br_get()->telem_aa_err = (float)g_aa.err_last;

    /* Куда сейчас смотрит прицел относительно кости, в пикселях. Допуск задан
       экранной величиной, а не угловой: 0.30° — это 3 px на хилфаере, но 57 px
       на двенадцатикратном прицеле, то есть прицел «наводился» в полуметре от
       головы. Пиксели одинаковы на любом зуме, поэтому и допуск в них. */
    {
        double gxp = g_aa.warm ? g_aa.gain_x : AA_GAIN_X_DEFAULT;
        double gyp = g_aa.warm ? g_aa.gain_y : AA_GAIN_Y_DEFAULT;
        double ex = d_yaw * gxp, ey = d_pitch * gyp;
        if (sqrt(ex * ex + ey * ey) < AA_TOLERANCE_PX) {
            /* Наведено. Замер из прошлого кадра ОБЯЗАН быть выброшен: мы больше
               не двигаем курсор, а камера ещё доедет на уже отправленных
               пикселях. Если оставить замер, следующий кадр сравнит это
               «доезжание» с числом пикселей, которых контур не отправлял, и
               оценка gain поедет — а с ней и цель: ровно та дрожь вокруг
               головы, которую видно при стрельбе. */
            g_aa.moved = 0;
            g_aa.err_prev = g_aa.err_last;
            return;
        }
    }

    double gx = g_aa.warm ? g_aa.gain_x : AA_GAIN_X_DEFAULT;
    double gy = g_aa.warm ? g_aa.gain_y : AA_GAIN_Y_DEFAULT;

    /* --- частота воздействия -------------------------------------------
       Курсор доезжает до прицела кадром ИГРЫ (60–144 Гц), а не кадром оверлея.
       Отправлять пиксели на каждые 8 мс бессмысленно: команды выстраиваются в
       очередь, и замер калибровки перестаёт соответствовать одной отправке.
       Одна порция на 15 мс — это 66 Гц, с запасом покрывает любой игровой
       кадр и делает контур устойчивым. */
    {
        DWORD now = GetTickCount();
        if (now - g_aa.t_prev < AA_SEND_PERIOD_MS) return;
    }

    /* --- закон управления ------------------------------------------------
       Шаг = ошибка × пиксели-на-градус × мягкость. Мягкость (ползунок 1..10)
       это и есть коэффициент усиления контура: 1 — почти весь промах за кадр,
       10 — сотая доля. Прямое «весь промах за кадр» здесь нельзя: курсор
       уходит в игру с задержкой в 1–2 кадра, поэтому контур с усилением больше
       единицы превращается в автоколебание вокруг цели с размахом, равным
       шагу. Отсюда же и предел доворота за кадр (AA_MAX_STEP_DEG) — он держит
       бросок при захвате новой цели и не даёт «щелчка» в бок. */
    int s = g_cfg_aa_smooth;
    if (s < 1) s = 1; if (s > 10) s = 10;
    /* k = 1 (почти «весь промах за кадр») при s=1, k = 0.18 при s=10.
       Множитель AA_K_SCALE держит контур в устойчивости: курсор уходит в игру
       с задержкой в 1–2 кадра, поэтому усиление больше ~0.5 превращает наводку
       в автоколебание вокруг цели — тот самый «прицел кружится при стрельбе». */
    double k = (11.0 - s) / 10.0 * 0.90 + 0.09;
    k *= AA_K_SCALE;
    if (!g_aa.warm) k = AA_K_COLD;            /* до калибровки — четверть силы */
    double px = d_yaw * gx * k;
    double py = -d_pitch * gy * k;            /* экранный Y вниз, pitch вверх */
    /* Предел доворота: базовые AA_MAX_STEP_DEG, но не меньше прицельного
       допуска — иначе на большом зуме допуск и шаг задают друг друга и контур
       либо не доходит, либо дёргается. */
    double cap = AA_MAX_STEP_DEG * gx;
    {
        double floor_px = AA_TOLERANCE_PX * 1.5;
        if (cap < floor_px) cap = floor_px;
    }
    if (px >  cap) px =  cap; if (px < -cap) px = -cap;
    if (py >  cap) py =  cap; if (py < -cap) py = -cap;

    /* Плюс дробный остаток прошлого кадра: на 120 Гц пиксель за кадр — это
       уже предел, и без остатка мелкая ошибка не доводилась бы никогда.
       Накопленный промах НЕ переносится: он всё равно пересчитывается заново
       от свежих углов, и перенос дал бы перелёт. */
    px += g_aa.pend_x; py += g_aa.pend_y;

    /* Мёртвая зона: меньше 0.6 пикселя курсор не трогаем вовсе. Без неё контур
       «дышит» вокруг нуля и цель дрожит в прицеле вместо того, чтобы стоять. */
    if (fabs(px) < AA_DEADBAND_PX && fabs(py) < AA_DEADBAND_PX) {
        g_aa.pend_x = px; g_aa.pend_y = py;
        return;
    }

    int imx = (int)(px > 0 ? px + 0.5 : px - 0.5);
    int imy = (int)(py > 0 ? py + 0.5 : py - 0.5);
    if (imx || imy) {
        /* Ссылку для калибровки снимаем ДО отправки: это состояние камеры, в
           котором не было нашего движения. Если снять после, при удачном
           попадании в кадр чтения разница выйдет нулевой и калибровка не
           сойдётся никогда. */
        g_aa.rot_prev = g_rcamrot;
        g_aa.px_sent = (double)imx;
        g_aa.py_sent = (double)imy;
        aa_move_mouse(imx, imy);
        g_aa.pend_x = px - imx; g_aa.pend_y = py - imy;   /* только дробь */
        g_aa.t_prev = GetTickCount();
        g_aa.moved = 1;
    } else {
        g_aa.pend_x = px; g_aa.pend_y = py;
    }
}

/* Круг FOV: центр экрана, радиус = зона захвата. Пунктиром и в два цвета,
   чтобы не путаться с маркерами: внешнее кольцо цветом слота 14, внутренняя
   точка — центр прицела. */
static void aa_draw_fov(HDC hdc) {
    if (!g_cfg_aa_fov) return;
    /* Рисуем ровно тот радиус, которым пользуется захват (aa_fov_px), а не
       значение ползунка: в мировом режиме при включённой оптике они разные, и
       нарисованный круг перестал бы совпадать с зоной, куда ассист реально
       берёт цель. */
    double rd = aa_fov_px();
    int r = (int)(rd + 0.5);
    if (r < AA_FOV_PX_MIN) r = AA_FOV_PX_MIN;
    int cx = g_sw / 2, cy = g_sh / 2;
    COLORREF c = RGB(g_pal_v.aa_fov[0], g_pal_v.aa_fov[1], g_pal_v.aa_fov[2]);
    if (g_aa_active) c = RGB(255, 60, 60);      /* захват — красный */
    HPEN p = esp_pen_for(c, g_aa_active ? 2 : 1);
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    /* Штриховка вручную: GDI Ellipse не рисует пунктир по дуге, а сплошной
       круг перекрывает цель в момент захвата. */
    for (int a = 0; a < 360; a += 8) {
        double a0 = a * PI_ / 180.0, a1 = (a + 4) * PI_ / 180.0;
        MoveToEx(hdc, cx + (int)(cos(a0) * r), cy + (int)(sin(a0) * r), NULL);
        LineTo  (hdc, cx + (int)(cos(a1) * r), cy + (int)(sin(a1) * r));
    }
    MoveToEx(hdc, cx - 3, cy, NULL); LineTo(hdc, cx + 3, cy);
    MoveToEx(hdc, cx, cy - 3, NULL); LineTo(hdc, cx, cy + 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
}

static const char* MENU_ROWS[] = {
    "ESP", "Corner box", "Skeleton", "Name", "Health", "Distance",
    "Show teammates", "Visible only", "Draw distance", "Skeleton distance",
    "Weapon", "Dump bones ->"
};
#define MENU_N (sizeof(MENU_ROWS)/sizeof(MENU_ROWS[0]))

static int* menu_flag(int i) {
    switch(i){
        case 0: return &g_cfg_enabled;
        case 1: return &g_cfg_show_box;
        case 2: return &g_cfg_show_skeleton;
        case 3: return &g_cfg_show_name;
        case 4: return &g_cfg_show_health;
        case 5: return &g_cfg_show_distance;
        case 6: return &g_cfg_show_team;
        case 7: return &g_cfg_only_visible;
        case 10: return &g_cfg_show_weapon;
        default: return NULL;
    }
}

/* In-game menu, ABI style: one overlay window, toggled by HOME. Up/Down to
   move, Left/Right to change, HOME to close. No second Win32 window. */
/* Menu action: dump the bone list of every distinct skeleton currently known.
   Writes to bones.txt next to the ESP. */
/* Raw probe: for the first live skeleton, write both candidate bone arrays
   plus the mesh ComponentToWorld. Comparing them shows which array is already
   in world space (and therefore must NOT be rotated again). */
/* Motion probe: is the bone array actually LIVE for distant players?
   Samples the same entities twice, ~1.2 s apart, and logs a checksum of the
   raw bone translations. A moving player whose checksum never changes means
   the array we read is a cache the engine does not refresh at range - which
   looks exactly like a skeleton frozen in place. */
/* Automatic bone-array liveness probe. Runs from the read thread every ~2 s and
   compares consecutive checksums per entity for BOTH candidate arrays, so the
   frozen-skeleton question is answered without anyone pressing a key: play for a
   minute and motion.txt fills itself. */
void motion_probe_tick(void) {
    static DWORD last = 0;
    static uint64_t am[256][2];
    static double   asum[256][2];
    static char     an[256][48];
    static uint64_t apawn[256];
    static uint64_t aptr[256], amesh[256];
    static int      acnt[256];
    static int      have = 0;
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;

    int n = g_nrender; if (n > 256) n = 256;
    FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\motion.txt","a");
    if (!lf) return;
    const uint64_t ofs[2] = { OFF_MESH_BONES_SDK, OFF_MESH_BONES };
    if (!have) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(lf, "\n=== run %02d:%02d:%02d (pass 1 of 2) ===\n", st.wHour, st.wMinute, st.wSecond);
    }
    for (int i = 0; i < n; i++) {
        ENT* e = &g_render[i];
        if (!pl(e->mesh)) continue;
        double   cur[2] = { 1e30, 1e30 };
        uint64_t ptr[2] = { 0, 0 };
        for (int w = 0; w < 2; w++) {
            uint64_t arr = rd64(e->mesh + ofs[w]);
            int32_t  cnt = (int32_t)rd32(e->mesh + ofs[w] + 8);
            if (!pl(arr) || cnt <= 0 || cnt > 2048) continue;
            uint8_t ft[0x60]; double sum = 0; int got = 0;
            for (int b = 1; b < 20 && b < cnt; b++) {
                if (!rd(arr + (uint64_t)b*0x60, ft, sizeof(ft))) break;
                double tx,ty,tz;
                memcpy(&tx,ft+0x20,8); memcpy(&ty,ft+0x28,8); memcpy(&tz,ft+0x30,8);
                sum += tx + ty*2.0 + tz*3.0; got++;
            }
            if (got) { cur[w] = sum; ptr[w] = arr; }
        }
        /* key on the pawn, not the slot: entity order shuffles between scans */
        int slot = -1;
        for (int k = 0; k < 256; k++) if (apawn[k] == e->pawn && apawn[k]) { slot = k; break; }
        if (!have) {
            for (int k = 0; k < 256; k++) if (!apawn[k]) {
                apawn[k] = e->pawn;
                snprintf(an[k], sizeof(an[k]), "%s", e->name);
                for (int w=0; w<2; w++) { am[k][w]=ptr[w]; asum[k][w]=cur[w]; }
                aptr[k]=ptr[1]; amesh[k]=e->mesh; acnt[k]=(int)rd32(e->mesh + OFF_MESH_BONES_N);
                break;
            }
        } else if (slot >= 0 && !strcmp(an[slot], e->name)) {
            char v0[8]="n/a", v1[8]="n/a";
            if (am[slot][0] && ptr[0] && am[slot][0]==ptr[0])
                snprintf(v0,sizeof(v0),"%s",(fabs(cur[0]-asum[slot][0])<1e-6)?"FROZEN":"live");
            if (am[slot][1] && ptr[1] && am[slot][1]==ptr[1])
                snprintf(v1,sizeof(v1),"%s",(fabs(cur[1]-asum[slot][1])<1e-6)?"FROZEN":"live");
            fprintf(lf, "%-18s dist=%7.0fcm  0x620=%-6s  ptr=%llX  cnt=%d  mesh=%llX\n",
                    an[slot], e->dist, v1,
                    (unsigned long long)ptr[1],
                    (int)rd32(e->mesh + OFF_MESH_BONES_N),
                    (unsigned long long)e->mesh);
            for (int w=0; w<2; w++) { am[slot][w]=ptr[w]; asum[slot][w]=cur[w]; }
        }
    }
    fclose(lf);
    have = !have;
}

void dump_bone_probe(void) {
    FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\boneprobe.txt","w");
    if (!lf) return;
    for (int i = 0; i < g_nrender; i++) {
        uint64_t m = g_render[i].mesh;
        if (!pl(m)) continue;
        uint64_t a = rd64(m + OFF_MESH_SKINNEDASSET);
        int32_t n  = (int32_t)rd32(m + OFF_MESH_BONES_SDK + 8);
        int32_t n2 = (int32_t)rd32(m + OFF_MESH_BONES_N);
        uint64_t a0 = rd64(m + OFF_MESH_BONES_SDK);
        uint64_t a2 = rd64(m + OFF_MESH_BONES);
        fprintf(lf, "\n### ent %d name=%s mesh=%llX asset=%llX\n",
                i, g_render[i].name, (unsigned long long)m, (unsigned long long)a);
        fprintf(lf, "  0xA08: arr=%llX cnt=%d   0x620: arr=%llX cnt=%d\n",
                (unsigned long long)a0, n, (unsigned long long)a2, n2);
        /* mesh transform */
        Q4 rot={0,0,0,0}; V3 pos={0,0,0}, scl={0,0,0};
        rd(m+OFF_SC_C2W+FT_ROTATION,&rot,sizeof(rot));
        rd(m+OFF_SC_C2W+FT_TRANSLATION,&pos,sizeof(pos));
        rd(m+OFF_SC_C2W+FT_SCALE3D,&scl,sizeof(scl));
        fprintf(lf, " c2w pos=%.2f %.2f %.2f  rot=%.3f %.3f %.3f %.3f  scl=%.3f %.3f %.3f\n",
                pos.x,pos.y,pos.z, rot.x,rot.y,rot.z,rot.w, scl.x,scl.y,scl.z);
        /* root component too */
        uint64_t rcmp = rd64(m + 0x1E0);
        if (pl(rcmp)) {
            V3 rp={0,0,0}; Q4 rr={0,0,0,0};
            rd(rcmp+OFF_SC_RELLOC,&rp,sizeof(rp));
            rd(rcmp+0x210+FT_ROTATION,&rr,sizeof(rr));
            fprintf(lf, " root pos=%.2f %.2f %.2f rot=%.3f %.3f %.3f %.3f\n",
                    rp.x,rp.y,rp.z, rr.x,rr.y,rr.z,rr.w);
        }
        /* first 6 bones of each candidate array */
        struct { const char* tag; uint64_t arr; } cand[2] = {
            {"Bones_SDK(0xA08)", a0}, {"Bones(0x620)", a2} };
        for (int which = 0; which < 2; which++) {
            uint64_t arr = cand[which].arr;
            fprintf(lf, "  --- %s arr=%llX\n", cand[which].tag, (unsigned long long)arr);
            if (!pl(arr)) continue;
            for (int b = 0; b < 8; b++) {
                uint8_t ft[0x60];
                if (!rd(arr + (uint64_t)b*0x60, ft, sizeof(ft))) break;
                double tx,ty,tz,sx,sy,sz;
                memcpy(&tx,ft+0x20,8); memcpy(&ty,ft+0x28,8); memcpy(&tz,ft+0x30,8);
                memcpy(&sx,ft+0x38,8); memcpy(&sy,ft+0x40,8); memcpy(&sz,ft+0x48,8);
                fprintf(lf, "     [%d] t=%.2f %.2f %.2f  s=%.3f %.3f %.3f\n",
                        b, tx,ty,tz, sx,sy,sz);
            }
        }
    }
    fclose(lf);
}

void dump_motion_probe(void);
void dump_all_bones(void) {
    FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\bones.txt","a");
    if (lf) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(lf, "\n########## dump %02d:%02d:%02d  entities=%d ##########\n",
                st.wHour, st.wMinute, st.wSecond, g_nrender);
        fclose(lf);
    }
    int seen = 0;
    uint64_t done[256];
    for (int i = 0; i < g_nrender && seen < 256; i++) {
        uint64_t m = g_render[i].mesh;
        if (!pl(m)) continue;
        uint64_t a = rd64(m + OFF_MESH_SKINNEDASSET);
        if (!pl(a)) continue;
        int dup = 0;
        for (int k = 0; k < seen; k++) if (done[k] == a) { dup = 1; break; }
        if (dup) continue;
        done[seen++] = a;
        dump_skeleton(a);
    }
}

/* ---------------------------------------------------------------------
   CoreWD menu
   Original look: dark panel, cyan accent, plain row list with ON/OFF values.
   Added: page tabs, draggable title bar, saved position, Save/Reset.
   --------------------------------------------------------------------- */
#define C_BG       RGB(10,14,18)
#define C_BORDER   RGB(90,220,255)
#define C_SEL      RGB(24,60,66)
#define C_TEXT     RGB(200,210,220)
#define C_TEXT_HI  RGB(255,255,255)
#define C_DIM      RGB(110,120,130)
#define C_ACCENT   RGB(90,220,255)

static int g_menu_h = 200;
int g_probe_disable_anchor = 0;
int g_dump_vit = 1;   /* dump vitality grid so the real HP slot is identified from live data */   /* 1 = dump the vitality float grid every 3 s */
static int g_color_ch = 0;
/* Transient feedback line. An action that fires silently is indistinguishable
   from one that does nothing - Save/Load felt broken for exactly that reason. */
static char  g_toast[48] = {0};
static DWORD g_toast_until = 0;
static void toast(const char* msg) {
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until = GetTickCount() + 2200;
}


/* ============================================================================
   L81 MAP — вид сверху: миномёт, линия наведения, точка падения, враги.
   Отвечает на вопрос «куда стрелять» визуально, а не цифрами.

   Координаты мира: UE5 cm, X/Y горизонталь, Z вверх.
   Экран: map_x = cx + (wx-ox)*s,  map_y = cy - (wy-oy)*s   (север вверх).
   Масштаб авто: точка падения всегда в кадре, минимум 150 м обзора.
   ============================================================================ */
/* ============================================================================
   L81 MAP — вид сверху: миномёт, линия наведения, точка падения, враги.
   Отвечает на вопрос «куда стрелять» визуально, а не цифрами.

   Координаты мира: UE5 cm, X/Y горизонталь, Z вверх.
   Экран: map_x = cx + (wx-ox)*s,  map_y = cy - (wy-oy)*s   (север вверх).
   Масштаб авто: точка падения всегда в кадре, минимум 150 м обзора.
   ============================================================================ */
static void L81_Draw(HDC hdc, int vw, int vh) {
    if (!g_l81_on) return;

    DWORD now = GetTickCount();
    static DWORD t_upd = 0;
    if (now - t_upd >= 50) { t_upd = now; l81_update(); }
    if (now >= g_l81_next) { g_l81_next = now + 2000; if (g_l81_n == 0) l81_find(); }

    int S = 320;
    if (S > vw/2) S = vw/2;
    if (S > vh/2) S = vh/2;
    int px = vw - S - 14, py = vh - S - 14;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    int cx = px + S/2, cy = py + S/2;

    /* фон */
    HBRUSH bg = CreateSolidBrush(RGB(12, 15, 19));
    RECT r; SetRect(&r, px, py, px + S, py + S);
    FillRect(hdc, &r, bg); DeleteObject(bg);
    /* рамка */
    HPEN bd = CreatePen(PS_SOLID, 1, RGB(70, 130, 190));
    HGDIOBJ op = SelectObject(hdc, bd);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, px, py, px + S, py + S);
    SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(bd);
    /* сетка */
    HPEN gp = CreatePen(PS_SOLID, 1, RGB(28, 34, 42));
    HGDIOBJ og = SelectObject(hdc, gp);
    { int k; for (k = 1; k < 6; k++) {
        int o = k * S / 6;
        MoveToEx(hdc, px + o, py + 1, NULL); LineTo(hdc, px + o, py + S - 1);
        MoveToEx(hdc, px + 1, py + o, NULL); LineTo(hdc, px + S - 1, py + o);
    } }
    SelectObject(hdc, og); DeleteObject(gp);

    SetBkMode(hdc, TRANSPARENT);

    /* миномёт: берём ПЕРВОЕ валидное орудие */
    const L81_GUN* gg = NULL;
    { int i; for (i = 0; i < g_l81_n; i++) if (g_l81[i].valid) { gg = &g_l81[i]; break; } }

    /* Опорная точка карты. В миномёте камера стоит у орудия, поэтому центрируем
       на стволе: иначе «я» уезжает вместе с камерой и карта прыгает. */
    int have_me = g_have_cam && isfinite(g_cam.x) && isfinite(g_cam.y);
    double ox, oy;
    if (gg)           { ox = gg->ipos[0]; oy = gg->ipos[1]; }
    else if (have_me) { ox = g_cam.x;     oy = g_cam.y; }
    else {
        SetTextColor(hdc, RGB(150,150,150));
        TextOutA(hdc, px+8, py+8, "no position / no L81", 21);
        return;
    }

    /* Дальность: берём с HUD-ретикла (игра уже посчитала). Физика — запасной
       путь. Rm — в МЕТРАХ; в мировые сантиметры переводится ровно один раз,
       при вычислении точки падения. */
    double Rm = 0.0;
    if (gg) Rm = l81_range_m_gun(gg);
    double span = 150.0 * 100.0;
    if (Rm * 100.0 * 1.3 > span) span = Rm * 100.0 * 1.3;
    double scale = (S/2.0 - 16.0) / span;

    /* --- игроки из снапшота --- */
    EnterCriticalSection(&g_cs);
    int n = g_nents[g_front];
    if (n > MAX_PLAYERS) n = MAX_PLAYERS;
    int i;
    for (i = 0; i < n; i++) {
        const ENT* e = &g_ents[g_front][i];
        if (!pl(e->pawn)) continue;
        int ex = cx + (int)((e->root.x - ox) * scale);
        int ey = cy - (int)((e->root.y - oy) * scale);
        if (ex < px+1 || ex > px+S-1 || ey < py+1 || ey > py+S-1) continue;

        int is_team = (g_myfac != 0 && e->faction == g_myfac);
        COLORREF col = is_team ? RGB(80,150,255) : RGB(255,80,80);

        /* дистанция ОТ ТОЧКИ ПАДЕНИЯ — «кого накроет» */
        double dd = -1.0;
        if (gg && Rm > 1.0) {
            double yy = gg->yaw * L81_RAD;
            double ix2 = gg->ipos[0] + Rm * 100.0 * cos(yy);
            double iy2 = gg->ipos[1] + Rm * 100.0 * sin(yy);
            double ax = e->root.x - ix2, ay = e->root.y - iy2;
            dd = sqrt(ax*ax + ay*ay) / 100.0;
        }

        HBRUSH eb = CreateSolidBrush(col);
        HGDIOBJ oeb = SelectObject(hdc, eb);
        HGDIOBJ oepn = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, ex-3, ey-3, ex+3, ey+3);
        SelectObject(hdc, oepn); SelectObject(hdc, oeb); DeleteObject(eb);

        if (dd >= 0.0 && dd < 2000.0) {
            char tb[24]; _snprintf(tb, sizeof(tb), "%.0f", dd);
            SetTextColor(hdc, dd < 30.0 ? RGB(255,230,120) : RGB(200,200,200));
            TextOutA(hdc, ex+5, ey-6, tb, (int)strlen(tb));
        }
    }
    LeaveCriticalSection(&g_cs);

    /* --- миномёт: сектор наведения, дуга, точка падения --- */
    if (gg) {
        int gx = cx + (int)((gg->ipos[0] - ox) * scale);
        int gy = cy - (int)((gg->ipos[1] - oy) * scale);

        double yr = gg->yaw * L81_RAD;
        double imp_x = gg->ipos[0] + Rm*100.0*cos(yr);
        double imp_y = gg->ipos[1] + Rm*100.0*sin(yr);
        int ix = cx + (int)((imp_x - ox) * scale);
        int iy = cy - (int)((imp_y - oy) * scale);

        /* метка орудия */
        HBRUSH gb = CreateSolidBrush(RGB(120,190,255));
        HGDIOBJ ogb = SelectObject(hdc, gb);
        HGDIOBJ ogp = SelectObject(hdc, GetStockObject(NULL_PEN));
        Rectangle(hdc, gx-4, gy-4, gx+4, gy+4);
        SelectObject(hdc, ogp); SelectObject(hdc, ogb); DeleteObject(gb);

        /* сектор наведения +/- 15 град */
        int wlen = (int)(60.0 * 100.0 * scale);
        if (wlen > S/2) wlen = S/2;
        HPEN wp = CreatePen(PS_SOLID, 1, RGB(90,140,190));
        HGDIOBJ owp = SelectObject(hdc, wp);
        { int sgn; for (sgn = -1; sgn <= 1; sgn += 2) {
            double a = yr + sgn * 0.26;
            MoveToEx(hdc, gx, gy, NULL);
            LineTo(hdc, gx + (int)(wlen*cos(a)), gy - (int)(wlen*sin(a)));
        } }
        SelectObject(hdc, owp); DeleteObject(wp);

        /* линия полёта */
        HPEN fp = CreatePen(PS_DASH, 1, RGB(255,180,60));
        HGDIOBJ ofp = SelectObject(hdc, fp);
        MoveToEx(hdc, gx, gy, NULL); LineTo(hdc, ix, iy);
        SelectObject(hdc, ofp); DeleteObject(fp);

        /* кольца рассеивания 25/50 м */
        HPEN cpen = CreatePen(PS_SOLID, 1, RGB(150,85,45));
        HGDIOBJ ocpen = SelectObject(hdc, cpen);
        { double rr; for (rr = 25.0; rr <= 50.0; rr += 25.0) {
            int rp = (int)(rr * 100.0 * scale);
            if (rp > 2) Ellipse(hdc, ix-rp, iy-rp, ix+rp, iy+rp);
        } }
        SelectObject(hdc, ocpen); DeleteObject(cpen);

        /* крест точки падения */
        HPEN xpen = CreatePen(PS_SOLID, 2, RGB(255,90,60));
        HGDIOBJ oxpen = SelectObject(hdc, xpen);
        MoveToEx(hdc, ix-8, iy, NULL); LineTo(hdc, ix+8, iy);
        MoveToEx(hdc, ix, iy-8, NULL); LineTo(hdc, ix, iy+8);
        SelectObject(hdc, oxpen); DeleteObject(xpen);

        { char b[64];
          _snprintf(b, sizeof(b), "%.0f m", Rm);
          SetTextColor(hdc, RGB(255,200,120));
          TextOutA(hdc, ix+11, iy+2, b, (int)strlen(b)); }
    }

    /* --- шапка --- */
    SetTextColor(hdc, RGB(120,190,255));
    TextOutA(hdc, px+7, py+6, "L81 MAP", 7);
    { char hb[80];
      _snprintf(hb, sizeof(hb), "%.0f m span", span/100.0);
      SetTextColor(hdc, RGB(110,120,130));
      TextOutA(hdc, px+S-92, py+6, hb, (int)strlen(hb));
      if (!gg) { SetTextColor(hdc, RGB(255,140,90)); TextOutA(hdc, px+72, py+6, "no mortar", 9); } }

    if (gg) {
        char sb[96];
        _snprintf(sb, sizeof(sb), "RNG %.0fm  ELEV %.2f  AZ %.2f%s",
                  gg->rng_m, gg->elev_deg, gg->yaw,
                  gg->rng_m > 1.0 ? "" : " (no HUD)");
        SetTextColor(hdc, RGB(150,160,175));
        TextOutA(hdc, px+7, py+S-16, sb, (int)strlen(sb));
    }

    if (g_l81_msg[0] && now - g_l81_msg_t < 4000) {
        SetTextColor(hdc, RGB(255,230,120));
        TextOutA(hdc, px+7, py+S-30, g_l81_msg, (int)strlen(g_l81_msg));
    }
}

   /* 0=R 1=G 2=B, which channel the arrows edit */   /* 1 = A/B test: draw the raw frozen pose */

static void DrawMenu(HDC hdc,int sw,int sh) {
    static const char* PAGES[] = { "ESP", "Visuals", "Colors", "System" };
    static const int tabw[4] = { 46, 66, 60, 62 };

    /* row text per page */
    static const char* R0[] = { "ESP","Corner box","Skeleton","Name","Health","Armor bar","Distance","Weapon" };
    static const char* R1[] = { "Show teammates","Draw distance","Skeleton distance" };
    static const char* R2[] = { "Save settings","Load settings","Reset defaults","Dump bones",
                                "L81 Detector","L81 rescan","L81 calib (use RNG)","L81 model v0" };
    static const char* R3[] = {
        "Marker: Blue","Marker: Red","Marker: Green","Marker: Own","Marker: Unknown",
        "Name colour","Name from faction","Weapon colour","Distance colour","Distance from faction",
        "Armor bar colour","Head fill colour","Head from faction","Skeleton head colour",
        "HP high","HP mid","HP low","Downed colour"
    };
    /* Page -> row table. R3 (the colour rows) MUST be selected here: with the
       old two-way choice every non-ESP/Visuals page fell through to R2, so the
       Colors page drew six rows out of a three-entry array and read past the
       end - the garbage pointer that crashed strlen with 0xc0000005. */
    const char** rows = (g_menu_page==0)?R0 : (g_menu_page==1)?R1 : (g_menu_page==2)?R3 : R2;
    int nrows = (g_menu_page==0)?8 : (g_menu_page==1)?3 : (g_menu_page==2)?18 : 8;

    int W = MENU_W;
    int H = 66 + nrows*26 + 24 + (g_cfg_enabled ? 0 : 16);
    g_menu_h = H;

    int x,y;
    if (g_menu_x < 0 || g_menu_y < 0) { x=(sw-W)/2; y=(sh-H)/2; g_menu_x=x; g_menu_y=y; }
    else { x=g_menu_x; y=g_menu_y; }
    if(x<0)x=0; if(y<0)y=0; if(x>sw-W)x=sw-W; if(y>sh-H)y=sh-H;
    g_menu_x=x; g_menu_y=y;

    /* open animation: short slide, as before */
    DWORD now=GetTickCount(); float t01=1.0f;
    if(g_menu_anim){ DWORD dt=now-g_menu_anim; t01=(dt>=150)?1.0f:(float)dt/150.0f; }
    y += (int)((1.0f-t01)*12.0f);

    /* panel + border */
    RECT rc={x,y,x+W,y+H};
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(hdc,&rc,bg);
    DeleteObject(bg);
    HPEN op=(HPEN)SelectObject(hdc,g_pen_border);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
    Rectangle(hdc,x,y,x+W,y+H);
    SelectObject(hdc,ob); SelectObject(hdc,op);

    SetBkMode(hdc,TRANSPARENT);

    /* title: CoreWD + tabs + F1 hint */
    SetTextColor(hdc,C_ACCENT);
    TextOutA(hdc,x+14,y+10,"CoreWD",8);
    int tx = x+96;
    /* Four tabs, four iterations: this loop ran to 3 and simply never drew the
       System tab, which is why it was reachable with Q/E but invisible. */
    for(int p=0;p<4;p++){
        RECT rr={tx,y+7,tx+tabw[p],y+27};
        if(p==g_menu_page){
            HBRUSH hb=CreateSolidBrush(C_SEL); FillRect(hdc,&rr,hb); DeleteObject(hb);
        }
        SetTextColor(hdc, p==g_menu_page?C_ACCENT:C_DIM);
        DrawTextA(hdc,PAGES[p],-1,&rr,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
        tx += tabw[p]+4;
    }
    SetTextColor(hdc,RGB(90,100,110));
    TextOutA(hdc,x+W-30,y+10,"[F1]",4);

    /* separator under the title */
    RECT sl={x+1,y+34,x+W-1,y+35};
    HBRUSH sb=CreateSolidBrush(RGB(30,44,52)); FillRect(hdc,&sl,sb); DeleteObject(sb);

    /* rows */
    int ry = y + 42;
    for(int i=0;i<nrows;i++){
        if(i==g_menu_sel||i==g_menu_hover){
            RECT s2={x+8,ry-1,x+W-8,ry+20};
            HBRUSH hb=CreateSolidBrush(i==g_menu_sel?C_SEL:RGB(20,30,36));
            FillRect(hdc,&s2,hb); DeleteObject(hb);
        }
        SetTextColor(hdc, i==g_menu_sel?C_TEXT_HI:C_TEXT);
        /* Defensive: a bad row index here is what fed strlen() a garbage
           pointer and killed the process. */
        if(i>=0 && i<(int)(sizeof(void*)*64) && rows[i]) {
            TextOutA(hdc,x+16,ry,rows[i],(int)strlen(rows[i]));
        }

        char val[24]; val[0]=0;
        if(g_menu_page==0){
            int* f=NULL;
            switch(i){case 0:f=&g_cfg_enabled;break;case 1:f=&g_cfg_show_box;break;
                case 2:f=&g_cfg_show_skeleton;break;case 3:f=&g_cfg_show_name;break;
                case 4:f=&g_cfg_show_health;break;case 5:f=&g_cfg_show_armor;break;
                case 6:f=&g_cfg_show_distance;break;case 7:f=&g_cfg_show_weapon;break;}
            if(f) snprintf(val,sizeof(val),"%s",*f?"ON":"OFF");
        } else if(g_menu_page==2){
            /* Colour rows: a live swatch plus the RGB triple, so the value on
               screen is exactly what the marker will draw. */
            unsigned char* c = pal_row(i);
            if(i==6)      snprintf(val,sizeof(val),"%s", (*g_name_fac)?"FACTION":"CUSTOM");
            else if(i==9) snprintf(val,sizeof(val),"%s", (*g_dist_fac)?"FACTION":"CUSTOM");
            else if(i==12) snprintf(val,sizeof(val),"%s",
                    (*g_head_fac)==2?"NONE":((*g_head_fac)==1?"FACTION":"CUSTOM"));
            else if(i==13) snprintf(val,sizeof(val),"%s", g_cfg_show_head?"ON":"OFF");
            else if(c){
                RECT sw = {x+W-140, ry+3, x+W-116, ry+17};
                HBRUSH hb = CreateSolidBrush(RGB(c[0],c[1],c[2]));
                FillRect(hdc,&sw,hb); DeleteObject(hb);
                snprintf(val,sizeof(val),"[%d] %d %d",
                         g_color_ch==0?c[0]:(g_color_ch==1?c[1]:c[2]),
                         g_color_ch==0?c[1]:c[0],
                         g_color_ch==2?c[1]:c[2]);
            }
        } else if(g_menu_page==1){
            if(i==0) snprintf(val,sizeof(val),"%s",g_cfg_show_team?"ON":"OFF");
            else if(i==1){ if(g_cfg_max_distance<=0) snprintf(val,sizeof(val),"ALL"); else snprintf(val,sizeof(val),"%dm",g_cfg_max_distance/100); }
            else { if(g_cfg_skel_distance<=0) snprintf(val,sizeof(val),"FOLLOW"); else snprintf(val,sizeof(val),"%dm",g_cfg_skel_distance/100); }
        } else if(g_menu_page==3){
            /* Per-row verbs: "RUN" on every action row was meaningless - each
               action names itself now. */
            if(i==0) snprintf(val,sizeof(val),"%s","WRITE");
            else if(i==1) snprintf(val,sizeof(val),"%s","READ");
            else if(i==2) snprintf(val,sizeof(val),"%s","RESET");
            else if(i==3) snprintf(val,sizeof(val),"%s","RUN");
            else if(i==4) snprintf(val,sizeof(val),"%s",g_l81_on?"ON":"OFF");
            else if(i==5) snprintf(val,sizeof(val),"%s","RUN");
            else if(i==6) snprintf(val,sizeof(val),"%.0fm",g_l81_lastrng);
            else snprintf(val,sizeof(val),"%s", g_l81_k>0.0 ? "CALIB" : "PHYS");
        }
        if(val[0]){
            SetTextColor(hdc, i==g_menu_sel?C_ACCENT:C_DIM);
            TextOutA(hdc,x+W-16-(int)strlen(val)*6,ry,val,(int)strlen(val));
        }
        ry += 26;
    }

    if(!g_cfg_enabled){
        SetTextColor(hdc,RGB(255,190,60));
        TextOutA(hdc,x+16,y+H-34,"ESP IS OFF - press Right on 'ESP' to enable",43);
    }
    /* feedback first: while a toast is fresh it replaces the hint line, so a
       key press visibly does something */
    if (g_toast[0] && (LONG)(GetTickCount() - g_toast_until) < 0) {
        SetTextColor(hdc, RGB(90,230,140));
        TextOutA(hdc,x+16,y+H-20,g_toast,(int)strlen(g_toast));
    } else {
        SetTextColor(hdc,C_DIM);
        TextOutA(hdc,x+16,y+H-20,
                 g_menu_page==2?"R/G/B channel   Left/Right adjust   Q/E page   F1":"Up/Down move   Left/Right change   Q/E page   F1 close",
                 g_menu_page==2?42:49);
    }
}

/* Bounding boxes of everything drawn last frame. The worker asks the window to
   repaint only these bands - with LWA_COLORKEY, DWM re-composites every pixel
   it is told is dirty, so repainting the full 1440p canvas for eight markers is
   pure waste and lands on the same composition path as the mouse cursor. */
static RECT g_dirty[512];
static volatile LONG g_ndirty = 0;
/* Markers from the PREVIOUS frame must be repainted too: a box that moved left
   its old outline behind, and repainting only the new rectangles leaves those
   ghosts on screen (the trail/accordion effect). The window keeps a copy of the
   last frame's rects and unions the two sets. */
static RECT g_dirty_prev[512];
static volatile LONG g_ndirty_prev = 0;
static int g_canvas_dirty = 1;

void esp_dirty_rects(RECT* out, int max, int* count) {
    int n = (int)InterlockedCompareExchange(&g_ndirty, 0, 0);
    if (n > 512) n = 512;
    int k = 0;
    for (int i = 0; i < n && k < max; i++) out[k++] = g_dirty[i];
    /* union in last frame's rects so moved markers get erased */
    LONG pn = InterlockedCompareExchange(&g_ndirty_prev, 0, 0);
    if (pn > 512) pn = 512;
    for (int i = 0; i < (int)pn && k < max; i++) out[k++] = g_dirty_prev[i];
    /* remember this frame for the next one */
    memcpy(g_dirty_prev, g_dirty, (size_t)n * sizeof(RECT));
    InterlockedExchange(&g_ndirty_prev, n);
    *count = k;
}

/* Solve the arc from (p0, v0): integrate a = -v/tau - g with dt=0.25 s,
   record PROJ_ARC_PTS samples and stop when z returns to the start height. */
static void proj_solve(PROJ* pr) {
    /* No fitted drag. The game owns the range table. The projectile supplies
       the actual source direction; the table supplies the exact ground range. */
    if (pr->n_hist < 3 || g_l81_n <= 0) {
        FILE* f=fopen("l81_runtime.log","a"); if(f){fprintf(f,"exit guard hist=%d guns=%d\n",pr->n_hist,g_l81_n);fclose(f);} return;
    }
    int n=pr->n_hist;
    double x0=pr->hist[0][1], y0=pr->hist[0][2], z0=pr->hist[0][3];
    int m=n<6?n:6;
    double dx=0,dy=0;
    for(int i=1;i<m;i++){ dx+=pr->hist[i][1]-pr->hist[i-1][1]; dy+=pr->hist[i][2]-pr->hist[i-1][2]; }
    double h=sqrt(dx*dx+dy*dy); if(h<30.0) { FILE* f=fopen("l81_runtime.log","a"); if(f){fprintf(f,"exit h=%.1f\n",h);fclose(f);} return; }
    dx/=h; dy/=h;
    L81_GUN* gg=&g_l81[0];
    double range_m=pr->range_locked ? pr->locked_range_m : gg->table_m;
    if(range_m<=1.0 || !isfinite(range_m)) { FILE* f=fopen("l81_runtime.log","a"); if(f){fprintf(f,"exit range=%.2f pitch=%.3f rows=%d\n",range_m,gg->pitch,gg->range_count);fclose(f);} return; }
    if (!pr->range_locked) { pr->locked_range_m=range_m; pr->range_locked=1; }
    /* Range is measured from the weapon, not from the first sampled shell. */
    double ox=gg->origin[0], oy=gg->origin[1];
    if (!isfinite(ox) || !isfinite(oy) || fabs(ox)<1.0 || fabs(oy)<1.0) { FILE* f=fopen("l81_runtime.log","a"); if(f){fprintf(f,"exit origin=%.1f,%.1f\n",ox,oy);fclose(f);} return; }
    double ix=ox+dx*range_m*100.0;
    double iy=oy+dy*range_m*100.0;
    double iz=z0;
    pr->impact.x=(float)ix; pr->impact.y=(float)iy; pr->impact.z=(float)iz;
    pr->impact_ok=1; pr->range_m=range_m;
    pr->arc_n=0;
    /* observed projectile samples */
    int step=n>24?n/24:1;
    for(int i=0;i<n && pr->arc_n<PROJ_ARC_PTS-8;i+=step){
        pr->arc[pr->arc_n].x=(float)pr->hist[i][1];
        pr->arc[pr->arc_n].y=(float)pr->hist[i][2];
        pr->arc[pr->arc_n].z=(float)pr->hist[i][3]; pr->arc_n++;
    }
    /* Continue the observed path to the table-derived impact. Cubic Hermite
       joins the last measured tangent and ends on the ground while descending. */
    double xl=pr->hist[n-1][1], yl=pr->hist[n-1][2], zl=pr->hist[n-1][3];
    int prev=n-4; if(prev<0)prev=0;
    double ds=sqrt((xl-pr->hist[prev][1])*(xl-pr->hist[prev][1])+
                   (yl-pr->hist[prev][2])*(yl-pr->hist[prev][2]));
    double rem=sqrt((ix-xl)*(ix-xl)+(iy-yl)*(iy-yl));
    double m0=ds>1.0?(zl-pr->hist[prev][3])*rem/ds:0.0;
    if(m0>rem*3.0)m0=rem*3.0;
    if(m0< -rem*3.0)m0= -rem*3.0;
    double m1= -rem;
    for(int k=1;k<=8 && pr->arc_n<PROJ_ARC_PTS-1;k++){
        double u=(double)k/8.0, u2=u*u, u3=u2*u;
        pr->arc[pr->arc_n].x=(float)(xl+(ix-xl)*u);
        pr->arc[pr->arc_n].y=(float)(yl+(iy-yl)*u);
        pr->arc[pr->arc_n].z=(float)((2*u3-3*u2+1)*zl + (u3-2*u2+u)*m0 +
                                   (-2*u3+3*u2)*iz + (u3-u2)*m1);
        pr->arc_n++;
    }
    pr->arc[pr->arc_n].x=(float)ix; pr->arc[pr->arc_n].y=(float)iy; pr->arc[pr->arc_n].z=(float)iz; pr->arc_n++;
    pr->vx=dx; pr->vy=dy; pr->vz=0; (void)rem;
}

static void proj_sight(uint64_t a, const char* cn) {
    (void)cn;
    uint64_t rc = rd64(a + 0x1C0);
    if (!pl(rc)) {
        for (int i = 0; i < PROJ_MAX; i++)
            if (g_proj[i].act == a) { memset(&g_proj[i], 0, sizeof(g_proj[i])); break; }
        return;
    }
    uint8_t b[24];
    if (!rd(rc + 0x168, b, 24)) return;
    double x, y, z;
    memcpy(&x, b, 8); memcpy(&y, b + 8, 8); memcpy(&z, b + 16, 8);
    if (x == 0 && y == 0 && z == 0) return;
    double now = GetTickCount64() / 1000.0;
    PROJ* slot = NULL;
    for (int i = 0; i < PROJ_MAX; i++) {
        if (g_proj[i].act == a) { slot = &g_proj[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < PROJ_MAX; i++)
            if (g_proj[i].act == 0) { slot = &g_proj[i]; break; }
    }
    if (!slot) {
        slot = &g_proj[0];
        for (int i = 1; i < PROJ_MAX; i++)
            if (g_proj[i].born_ms < slot->born_ms) slot = &g_proj[i];
    }
    if (slot->act != a) {
        memset(slot, 0, sizeof(*slot));
        slot->act = a;
        slot->t0 = (unsigned long)(now * 1000.0);
        slot->used = 1;
        slot->born_ms = (unsigned long)(now * 1000.0);
        slot->flying = 1;
    }
    /* Landing is a sustained stop after the initial climb; no position-buffer
       dependency, since a round can remain as an actor for minutes. */
    if (slot->flying && slot->n_hist >= 2 && slot->impact_ok) {
        double dx = x - slot->last_pos.x;
        double dy = y - slot->last_pos.y;
        double dz = z - slot->last_pos.z;
        double elapsed = now - slot->hist[0][0];
        if (elapsed > 2.0 && dx*dx + dy*dy + dz*dz < 2500.0) {
            if (!slot->still_since_ms) slot->still_since_ms = GetTickCount();
            if ((unsigned long)(GetTickCount() - slot->still_since_ms) >= 300) {
                slot->flying = 0;
                slot->land_ms = GetTickCount();
            }
        } else slot->still_since_ms = 0;
    }
    /* The actor can persist after impact: never allocate a new shot for the
       same pointer once its trail has expired. */
    if (!slot->flying && slot->land_ms &&
        (unsigned long)(GetTickCount() - slot->land_ms) > PROJ_POST_IMPACT_MS)
        return;
    /* Continue tracking after the 96-sample arc buffer fills. */
    if (slot->n_hist >= 96 && slot->flying) {
        /* Keep the muzzle sample and the most recent 80 samples. */
        memmove(slot->hist + 1, slot->hist + 16, 80 * sizeof(slot->hist[0]));
        slot->n_hist = 81;
    }
    slot->last_pos.x = (float)x; slot->last_pos.y = (float)y; slot->last_pos.z = (float)z;
    if (slot->n_hist == 0 || now - slot->hist[slot->n_hist-1][0] >= 0.08) {
        if (slot->n_hist < 96 && slot->flying) {
            int li = slot->n_hist;
            slot->hist[li][0] = now;
            slot->hist[li][1] = x; slot->hist[li][2] = y; slot->hist[li][3] = z;
            slot->n_hist++;
        }
        if (slot->n_hist == 3) {
            FILE* f=fopen("l81_runtime.log","a");
            if(f){fprintf(f,"shell=%llX n=%d guns=%d table=%.2f origin=%.0f,%.0f,%.0f\n",
                (unsigned long long)a,slot->n_hist,g_l81_n,
                g_l81_n?g_l81[0].table_m:0.0,
                g_l81_n?g_l81[0].origin[0]:0.0,
                g_l81_n?g_l81[0].origin[1]:0.0,
                g_l81_n?g_l81[0].origin[2]:0.0);fclose(f);}
        }
        if (slot->n_hist >= 3 && slot->flying) {
            proj_solve(slot);
            slot->solved = (slot->impact_ok != 0);
            if (slot->n_hist == 3) {
                FILE* f=fopen("l81_runtime.log","a");
                if(f){fprintf(f,"solve=%d arc=%d range=%.1f\n",slot->solved,slot->arc_n,slot->range_m);fclose(f);}
            }
        }
    }
}

/* Render: world-space arc + live dot + impact cross. Called from DrawOverlay
   with the render camera. */
static void Blast_Draw(HDC hdc, const V3* c, double z);
static void Proj_Draw(HDC hdc) {
    if (!g_cfg_show_proj_trail) return;
    DWORD now = GetTickCount();
    HPEN arc = NULL, hot = NULL;
    HPEN old = NULL;
    int started = 0;
    for (int i = 0; i < PROJ_MAX; i++) {
        PROJ* pr = &g_proj[i];
        if (!pr->used || !pr->solved) continue;
        if (pr->land_ms &&
            (unsigned long)(now - pr->land_ms) > PROJ_POST_IMPACT_MS) continue;
        if (!arc) {
            arc = esp_pen_for(RGB(80, 220, 255), 2);
            hot  = esp_pen_for(RGB(255, 70, 90), 2);
            old = (HPEN)SelectObject(hdc, arc);
        }
        /* Cyan base line plus a short white segment travelling along it. */
        HPEN shine = esp_pen_for(RGB(188, 248, 255), 3);
        SelectObject(hdc, arc);
        started = 0;
        for (int k = 0; k < pr->arc_n; k++) {
            double sx, sy;
            if (w2s_a(&pr->arc[k], &g_rcam, &g_rcamrot, g_rfov, g_raspect, g_sw, g_sh, &sx, &sy)) {
                if (!started) { MoveToEx(hdc, (int)sx, (int)sy, NULL); started = 1; }
                else LineTo(hdc, (int)sx, (int)sy);
            } else started = 0;
        }
        if (!pr->land_ms && pr->arc_n > 2) {
            int phase = (int)((now % 1500) * (unsigned long)(pr->arc_n - 1) / 1500);
            int end = phase + 4; if (end >= pr->arc_n) end = pr->arc_n - 1;
            SelectObject(hdc, shine);
            started = 0;
            for (int k = phase; k <= end; k++) {
                double sx, sy;
                if (w2s_a(&pr->arc[k], &g_rcam, &g_rcamrot, g_rfov, g_raspect, g_sw, g_sh, &sx, &sy)) {
                    if (!started) { MoveToEx(hdc, (int)sx, (int)sy, NULL); started = 1; }
                    else LineTo(hdc, (int)sx, (int)sy);
                } else started = 0;
            }
        }
        /* moving projectile: magenta box (hitbox) + dot */
        if (pr->flying) {
            double sx, sy;
            if (w2s_a(&pr->last_pos, &g_rcam, &g_rcamrot, g_rfov, g_raspect, g_sw, g_sh, &sx, &sy)) {
                double dx = pr->last_pos.x-g_rcam.x, dy = pr->last_pos.y-g_rcam.y, dz = pr->last_pos.z-g_rcam.z;
                double dist = sqrt(dx*dx+dy*dy+dz*dz);
                int hs = (dist > 1.0) ? (int)(120000.0/dist) : 10;
                if (hs < 6) hs = 6; if (hs > 26) hs = 26;
                HPEN prev_pen = (HPEN)SelectObject(hdc, hot);
                MoveToEx(hdc,(int)sx-hs,(int)sy-hs,NULL); LineTo(hdc,(int)sx+hs,(int)sy-hs);
                LineTo(hdc,(int)sx+hs,(int)sy+hs); LineTo(hdc,(int)sx-hs,(int)sy+hs);
                LineTo(hdc,(int)sx-hs,(int)sy-hs);
                HBRUSH dot = CreateSolidBrush(RGB(218, 86, 190));
                HBRUSH prev_brush = (HBRUSH)SelectObject(hdc, dot);
                Ellipse(hdc, (int)sx - 4, (int)sy - 4, (int)sx + 4, (int)sy + 4);
                SelectObject(hdc, prev_brush);
                DeleteObject(dot);
                SelectObject(hdc, prev_pen);
            }
        }
        /* impact marker: crosshair at the solved impact point */
        if (pr->impact_ok) {
            double sx, sy;
            if (w2s_a(&pr->impact, &g_rcam, &g_rcamrot, g_rfov, g_raspect, g_sw, g_sh, &sx, &sy)) {
                SelectObject(hdc, hot);
                MoveToEx(hdc, (int)sx - 7, (int)sy, NULL); LineTo(hdc, (int)sx + 7, (int)sy);
                MoveToEx(hdc, (int)sx, (int)sy - 7, NULL); LineTo(hdc, (int)sx, (int)sy + 7);
                char b[48];
                snprintf(b, sizeof(b), "%.0fm", pr->range_m);
                DrawTextC(hdc, (int)sx + 10, (int)sy - 7, b, RGB(80, 220, 255));
            }
            Blast_Draw(hdc, &pr->impact, pr->impact.z);
        }
    }
    if (old) SelectObject(hdc, old);
}

/* Круг радиуса поражения на земле (проецированный многоугольник). */
static void Blast_Draw(HDC hdc, const V3* c, double z) {
    int rm = 15;
    if (rm < 5) rm = 5; if (rm > 60) rm = 60;
    double R = (double)rm * 100.0;
    HPEN bc = esp_pen_for(RGB(120, 255, 120), 1);
    HPEN prev = (HPEN)SelectObject(hdc, bc);
    int started = 0;
    for (int k = 0; k <= 24; k++) {
        double a = (double)k / 24.0 * 2.0 * PI_;
        V3 p; p.x = c->x + cos(a)*R; p.y = c->y + sin(a)*R; p.z = z;
        double sx, sy;
        if (w2s_a(&p, &g_rcam, &g_rcamrot, g_rfov, g_raspect, g_sw, g_sh, &sx, &sy)) {
            if (!started) { MoveToEx(hdc,(int)sx,(int)sy,NULL); started = 1; }
            else LineTo(hdc,(int)sx,(int)sy);
        } else started = 0;
    }
    SelectObject(hdc, prev);
}

static void DrawOverlay(HDC hdc, int vw, int vh) {
    /* GPU frame: WM_PAINT already cleared the surface and drew the chams layer,
       so this function only emits markers. `hdc` is kept for signature
       compatibility and is ignored by every primitive (see the GDI shim). */
    (void)hdc;
    (void)vw; (void)vh;
    InterlockedExchange(&g_ndirty, 0);
    /* ESP off must NOT hide the menu: the toggle that turns it back on lives in
       the menu, so returning here left the user with no way to re-enable it
       (dj hit this by clicking "ESP" by accident). Draw the menu, skip only the
       markers. */
    if(!g_cfg_enabled && !g_cfg_show_l81_users && !g_cfg_aa_on){
        /* Both switches off means nothing to draw. With the mortar panel left
           on, the masterswitch only silences the ordinary markers - that is
           the whole point: turn everything else off and keep only the gunners.
           Ассист тоже держит этот путь живым: с выключенными маркерами ему
           всё равно нужны экранные координаты костей из project_all. */
        return;   /* меню рисует ImGui в своём окне; GDI-путь только маркеры */
    }
    project_all(g_render,g_nrender,g_sw,g_sh,&g_rcam,&g_rcamrot,g_rfov,g_raspect);

    /* Ассист работает на тех же экранных координатах, что только что посчитаны:
       цель выбирается по позиции кости в кадре, доворот считается от текущего
       поворота камеры. Отдельного чтения памяти здесь нет. */
    aa_tick();
    aa_draw_fov(hdc);

#if BRIDGE_ON
    /* Publish world space for the remote renderer. Sent from here because this
       is the one place where the full render snapshot (positions, health,
       faction, names, bones) is assembled and stable - the read thread has
       already finished with it. Costs no reads: everything below is in RAM. */
    {
        static unsigned long last_pub = 0;
        unsigned long nowms = GetTickCount();
        if (nowms - last_pub >= 66) {          /* ~15 Hz, matches the read rate */
            last_pub = nowms;
            wd_state_begin();
            /* The sender's own camera, for reference only. The remote side does
               NOT use it - it has its own - but it is useful to display. */
            wd_state_self((float)g_rcam.x, (float)g_rcam.y, (float)g_rcam.z,
                          (float)(g_rcamrot.y * PI_ / 180.0),
                          (float)(g_rcamrot.p * PI_ / 180.0),
                          (float)g_rfov);
            for (int bi = 0; bi < g_nrender && bi < WDS_MAX_PLAYERS; bi++) {
                ENT* be = &g_render[bi];
                int tm = 0;
                if (!be->is_self && (g_mysq_lo || g_mysq_hi) &&
                    be->squad_lo == g_mysq_lo && be->squad_hi == g_mysq_hi) tm = 1;
                wd_state_player((float)be->root.x, (float)be->root.y, (float)be->root.z,
                                (float)be->dist,
                                be->hp, be->hp_valid,
                                be->downed == 2, be->armor,
                                be->name, be->fac_cls,
                                be->is_self, be->visible, tm, be->veh_kind);
            }
            wd_state_end();

            /* Skeletons. Only players whose bones are actually valid are sent;
               sending a stale pose would tear the model apart on the far side.
               Local labels, remote geometry: same 17 edges the local draw uses. */
            wd_state_bones_begin();
            for (int bi = 0; bi < g_nrender && bi < WDS_MAX_PLAYERS; bi++) {
                ENT* be = &g_render[bi];
                if (be->is_self) continue;
                float xyz[B__COUNT * 3];
                int ok[B__COUNT];
                int nb = 0, any = 0;
                for (int b = 0; b < B__COUNT; b++) {
                    ok[b] = be->bone_ok[b] ? 1 : 0;
                    xyz[b * 3 + 0] = (float)be->bones[b].x;
                    xyz[b * 3 + 1] = (float)be->bones[b].y;
                    xyz[b * 3 + 2] = (float)be->bones[b].z;
                    if (ok[b]) any = 1;
                    nb = b + 1;
                }
                if (!any) continue;
                wd_state_bones_add(bi, (float)be->root.x, (float)be->root.y,
                                   (float)be->root.z, xyz, ok, nb);
            }
            wd_state_bones_end();
        }
    }
#endif

    char lbl[96];
    for(int i=0;i<g_nrender;i++){
        ENT* e=&g_render[i];
        if(e->is_self) continue;
        /* Second guard: a player standing exactly where the camera is can only
           be the local one. LocalPlayer resolution reads a four-level pointer
           chain (GameInstance -> LocalPlayers -> Controller -> Pawn) and any
           level being unpopulated at scan time zeroes mypawn, which used to
           let the local skeleton draw itself. The camera distance cannot go
           stale the same way. */
        /* Own-player guard. The camera sits in the head while the model root is
           at the feet (~180 cm lower), so a standing player's root is always
           well under 2 m from the camera and anyone turning on the spot ends up
           with their own marker in their face. 60 cm was far too tight; 250 cm
           covers the local skeleton at every pose while still being far inside
           the distance any real opponent keeps. */
        /* Sub-2.5 m guard against drawing your own skeleton. A seated player is
           exempt: their position now comes from the GUN, and standing beside a
           mortar you are not manning is a legitimate, useful marker. */
        if(!e->veh_kind && e->dist > 0.0 && e->dist < 250.0) continue;
        double metres = e->dist / 100.0;
        if(g_cfg_max_distance>0 && e->dist>(double)g_cfg_max_distance && e->veh_kind != 1) continue;
        /* Beyond ~1.5 km the projected size collapses to a few pixels and the
           markers pile up in one spot behind the play area. */
        if(metres > 1500.0 && g_cfg_max_distance==0 && e->veh_kind != 1) continue;
        /* Teammate filter. A mortar gunner is exempt while the L81 switch is
           on: knowing a teammate is holding a mortar position is exactly the
           information this feature exists to give, and "hide teammates" is a
           separate, deliberate choice about the ordinary markers. */
        if(e->fac_slot==0 && !g_cfg_show_team && !(e->veh_kind==1 && g_cfg_show_l81_users)) continue;
        /* Occupied emplacements (mortars) get their own switch: a gunner is
           worth knowing about even with the ordinary ESP off, and with the
           ordinary ESP on they would otherwise be drawn twice. */
        if(e->veh_kind == 1 && !g_cfg_show_l81_users) continue;
        if(e->veh_kind && e->veh_kind != 1 && !g_cfg_show_box && !g_cfg_show_skeleton) continue;
        /* Corpse filter, deliberately weak. Remote health is NOT replicated
           (only the local pawn has a live value), so a remote player reading
           0.0 hp is missing data, not dead - treating it as death made a
           knocked player's marker disappear instead of turning grey (dj:
           "в ноке у противников маркер пропал ник Votblin"). Death on this
           build is departure from the player array, which the scan already
           handles by pruning absent entities. Keep this guard only for the
           local pawn, where hp is real. */
        if(e->is_self && e->hp_valid && e->maxhp>0 && e->hp<=0.0 && !e->downed) continue;

        SCR* sc = &g_scr[i];
        double minx=1e18,miny=1e18,maxx=-1e18,maxy=-1e18;
        int any=0;
        for(int b=0;b<B__COUNT;b++){
            if(!sc->bok[b])continue;
            if(sc->sx[b]<minx)minx=sc->sx[b];
            if(sc->sx[b]>maxx)maxx=sc->sx[b];
            if(sc->sy[b]<miny)miny=sc->sy[b];
            if(sc->sy[b]>maxy)maxy=sc->sy[b];
            any=1;
        }
        int has_skel = any;
        if(!any){
            /* No bones on screen. LOD drops the mesh for players past the
               detail distance, so a loaded player would vanish entirely.
               Fall back to the root position and project a capsule-sized box
               (~180 cm) so the player stays visible without a skeleton. */
            /* Use the interpolated root: the raw one steps at the read rate,
               which is what made the box trail the body. */
            SCR* fsc = &g_scr[i];
            V3 base = (fsc->root_ok) ? fsc->root_smooth : e->root;
            V3 feet = base;
            V3 head = base; head.z += 180.0;
            double fx, fy, hx2, hy2;
            int fok = w2s(&feet,&g_rcam,&g_rcamrot,g_rfov,g_sw,g_sh,&fx,&fy);
            int hok = w2s(&head,&g_rcam,&g_rcamrot,g_rfov,g_sw,g_sh,&hx2,&hy2);
            if(!fok && !hok) continue;
            if(!fok){ fx = hx2; fy = hy2 + 60.0; }
            if(!hok){ hx2 = fx; hy2 = fy - 60.0; }
            double bw = fabs(hx2 - fx) * 0.9;
            if(bw < 6.0) bw = (fabs(fy - hy2) > 1.0) ? fabs(fy - hy2) * 0.5 : 20.0;
            if(bw < 6.0) bw = 20.0;
            minx = ((fx + hx2) * 0.5) - bw * 0.5;
            maxx = ((fx + hx2) * 0.5) + bw * 0.5;
            miny = (hy2 < fy) ? hy2 : fy;
            maxy = (hy2 > fy) ? hy2 : fy;
            any = 1;
        }
        /* Downed probe on every drawn entity. `+0x1C` is the knockout byte
           (00 -> 02 -> 00 confirmed on named knockouts), but it is NOT the
           only signal: VotBlin stayed at 00 through an entire knockout while
           his 0x10-0x12 position block kept ticking. So treat 0x1C as one
           source and keep the entity on screen either way - the marker
           disappearing is the failure mode, not a missed grey. */
        {
            uint8_t dv2 = 0;
            if (rd(e->pawn + OFF_PAWN_VITSTATE + OFF_VS_BLEEDOUT, &dv2, 1))
                e->dbno = dv2;
        }
        COLORREF col=esp_color(e);
        /* Box colour follows the body: faction hue while any joint is exposed,
           grey once the whole body is behind geometry. */
        COLORREF bcol = esp_box_color(e);
        double w=maxx-minx, h=maxy-miny;
        if(h<8){h=8;maxy=miny+8;}
        if(w<4){w=4;maxx=minx+4;}
        {
            LONG nd = InterlockedIncrement(&g_ndirty) - 1;
            if (nd < 512) {
                /* Padding must cover everything actually painted: the label
                   row sits above the box, and the skeleton's feet plus the
                   distance text hang BELOW the bone bounding box. Too tight a
                   pad leaves a ghost at the feet when the player moves - the
                   residual trail dj still sees. 40 px below covers the foot
                   bones at close range where the player is tallest on screen. */
                g_dirty[nd].left   = (LONG)minx - 12;
                g_dirty[nd].top    = (LONG)miny - 30;
                g_dirty[nd].right  = (LONG)maxx + 12;
                g_dirty[nd].bottom = (LONG)maxy + 42;
            }
        }
        HPEN pen = esp_pen(e);
        HPEN bpen = (bcol == col) ? pen : esp_pen_for(bcol, esp_line_w(2));
        int cx=(int)((minx+maxx)*0.5);

        /* Distance gate belongs on DRAW, not only on read: bones are cached in
           the entity and stay valid long after the player walked out of range,
           so a skeleton kept drawing at any distance the player had once been
           inside (and after "Skeleton distance" was lowered). */
        /* is_l81: this entity is manning a mortar. Every ordinary element below
           is gated by its own menu switch, but a gunner ignores those switches
           and draws a fixed, minimal marker (box + name + distance). Otherwise
           turning Box/Skeleton/Name off - the normal way to get a clean screen -
           silently took the mortar crew with it. */
        /* Захват круга ассиста: кость головы на экране, цель в зоне. Ищем и
           среди тех, кого обычный отбор маркеров может пропустить (сидящий в
           технике не должен выпадать из наводки), и подсвечиваем цель, чтобы
           было видно, в кого именно ведёт. Порог дистанции тот же. */
        if (g_cfg_aa_on && !e->veh_kind && e->dist <= eff_skel_dist() && !e->bone_ok[B_HEAD])
            continue;
        int is_l81 = (e->veh_kind == 1);
        if(g_cfg_show_skeleton && has_skel && e->dist <= eff_skel_dist() && (!is_l81 || e->bone_ok[B_HEAD])){
            HPEN old=(HPEN)SelectObject(hdc,pen);
            HBRUSH ob=(HBRUSH)SelectObject(hdc,GetStockObject(NULL_BRUSH));
            /* Every edge picks its own pen: an edge is drawn in the faction
               colour only when BOTH of its joints are in the open. That is what
               makes a head over a wall read as a coloured head on a grey body
               instead of one uniform silhouette. Pen handles are cached by
               colour+width, so switching per edge costs a linear lookup, not a
               GDI object per frame. */
            for(int k=0;k<(int)N_EDGES;k++){
                int a=BONE_EDGES[k][0], b=BONE_EDGES[k][1];
                if(!sc->bok[a]||!sc->bok[b])continue;
                SelectObject(hdc, esp_pen_for(esp_edge_color(e,a,b), esp_line_w(2)));
                MoveToEx(hdc,(int)sc->sx[a],(int)sc->sy[a],NULL);
                LineTo(hdc,(int)sc->sx[b],(int)sc->sy[b]);
            }
            /* Голова цели ассиста: рисуем ДАЖЕ если тумблер «точка головы»
               выключен — иначе видно, что ассист ведёт, но не видно, куда
               именно, и снайпер не может проверить наводку глазами. */
            int aa_tag = (g_cfg_aa_on && aa_pick() == e);
            if((g_cfg_show_head || aa_tag) && has_skel && sc->bok[B_HEAD]){
                double dx=sc->sx[B_HEAD]-sc->sx[B_NECK];
                double dy=sc->sy[B_HEAD]-sc->sy[B_NECK];
                double r=sqrt(dx*dx+dy*dy)*0.62;
                if(r<2.5) r=2.5;                     /* never vanish at long range */
                if(r>40.0) r=40.0;                   /* clamp at point-blank range */
                /* Head marker: filled disc in its own colour, so the head reads
                   as a solid blob instead of a faint outline. Face colour keeps
                   the faction hue; the fill uses the dedicated Head colour when
                   the user set one. */
                COLORREF hc = (e->downed || (*g_head_fac) == 1)
                              ? col
                              : RGB(g_pal_v.head[0], g_pal_v.head[1], g_pal_v.head[2]);
                /* mode 2 = NONE: outline only, no fill. Downed always fills
                   so the whole marker reads as one dark-grey silhouette. */
                HBRUSH hb = (!e->downed && (*g_head_fac) == 2)
                            ? (HBRUSH)GetStockObject(NULL_BRUSH)
                            : esp_pen_brush(hc);
                HBRUSH rb = (HBRUSH)SelectObject(hdc, hb);
                HPEN   hp = (HPEN)SelectObject(hdc, esp_pen_for(hc, esp_line_w(2)));
                Ellipse(hdc,(int)(sc->sx[B_HEAD]-r),(int)(sc->sy[B_HEAD]-r),
                            (int)(sc->sx[B_HEAD]+r),(int)(sc->sy[B_HEAD]+r));
                SelectObject(hdc,rb);
                SelectObject(hdc,hp);
            }
            /* Joint dots: small filled squares at the main articulations, the
               way the reference screenshot draws them. */
            for(int b=0;b<B__COUNT;b++){
                if(!sc->bok[b]) continue;
                int jx=(int)sc->sx[b], jy=(int)sc->sy[b];
                RECT jr={jx-1,jy-1,jx+2,jy+2};
                FillRect(hdc,&jr,(HBRUSH)(size_t)esp_pen_brush(esp_bone_color(e,b)));
            }
            SelectObject(hdc,ob);
            SelectObject(hdc,old);
        }
        /* A mortar crewman is drawn deliberately unlike everyone else: the box
           switches to the dedicated L81 colour so "someone is on a mortar" is
           readable at a glance without reading the nickname. Health/armour/
           name still use the faction colour, so team identity is not lost. */
        if(g_cfg_show_box || is_l81)
            DrawCornerBox(hdc,minx,miny,maxx,maxy, is_l81 ? esp_pen_for_l81() : bpen);
        /* Name above, distance below - like the reference screenshot. */
        /* Name and weapon are INDEPENDENT rows in the menu: the weapon draw used
           to live inside `if(show_name)`, so turning the nickname off silently
           took the weapon with it. Each element now has its own anchor line, so
           the stack closes up cleanly when one of them is off. */
        int labelY = (int)miny - 38;
        int distY  = (int)maxy + 6;
        if(g_cfg_show_name || is_l81){
            COLORREF ncol = (e->downed || (*g_name_fac)) ? col : RGB(g_pal_v.name[0],g_pal_v.name[1],g_pal_v.name[2]);
            char nb[120];
            if (is_l81) {
                /* tag the nickname so the marker survives screenshots and
                   recordings where the box colour alone is ambiguous */
                snprintf(nb, sizeof(nb), "[L81] %s", e->name[0] ? e->name : "?");
                ncol = RGB(g_pal_v.l81[0], g_pal_v.l81[1], g_pal_v.l81[2]);
            } else {
                snprintf(nb, sizeof(nb), "%s", e->name[0] ? e->name : "?");
            }
            DrawTextC(hdc,cx,labelY,nb,ncol);
            labelY += 17;
        } else if (e->veh_kind == 1) {
            /* name row is off but this is a mortar - still show what it is */
            COLORREF lc = RGB(g_pal_v.l81[0], g_pal_v.l81[1], g_pal_v.l81[2]);
            DrawTextC(hdc,cx,labelY,"[L81]",lc);
            labelY += 17;
        }
        if(g_cfg_show_weapon && e->wep[0]){
            COLORREF wcol = e->downed ? col : RGB(g_pal_v.weapon[0],g_pal_v.weapon[1],g_pal_v.weapon[2]);
            DrawTextC(hdc,cx,labelY,e->wep,wcol);
        }
        if(g_cfg_show_distance || is_l81){
            if (is_l81) snprintf(lbl,sizeof(lbl),"%dm L81",(int)metres);
            else        snprintf(lbl,sizeof(lbl),"%dm",(int)metres);
            COLORREF dcol = (e->downed || (*g_dist_fac)) ? col : RGB(g_pal_v.distance[0],g_pal_v.distance[1],g_pal_v.distance[2]);
            DrawTextC(hdc,cx,(int)maxy+6,lbl,dcol);
        }
        /* Horizontal health bar above the name, with an armour bar stacked
           over it. The old vertical strip beside the box was easy to miss and
           had no label; two thin bars over the nickname read like the game's
           own HUD and leave the marker outline clean. */
        /* Вертикальные шкалы СБОКУ бокса, во весь рост (как в референсе):
           HP вплотную к боксу, броня левее. Длина = высоте бокса, заполнение
           снизу вверх. Верхние горизонтальные полоски мешались с ником. */
        {
            int boxH = (int)(maxy - miny);
            if (boxH < 20) boxH = 20;
            int hpX  = (int)minx - 7;            /* HP-полоса у самого бокса */
            int arX  = (int)minx - 14;           /* броня левее */

            if(g_cfg_show_armor && e->armor_max > 0.0){
                double afr = e->armor / e->armor_max;
                if(afr < 0.0) afr = 0.0; if(afr > 1.0) afr = 1.0;
                int ah = (int)(boxH * afr);
                RECT ab = {arX, (int)miny, arX+4, (int)maxy};
                FillRect(hdc, &ab, g_br_hpbg);
                RECT af = {arX, (int)maxy - ah, arX+4, (int)maxy};
                FillRect(hdc, &af, esp_pen_brush(e->downed ? col : RGB(g_pal_v.armor[0],g_pal_v.armor[1],g_pal_v.armor[2])));
            }
            /* Draw the health bar only where the number is real - a bar that
               always sits at full because the data never arrives is worse than
               no bar. */
            if((g_cfg_show_health || is_l81) && e->hp_valid && e->maxhp > 0){
                double fr=e->hp/e->maxhp;
                if(fr<0)fr=0; if(fr>1)fr=1;
                int hh = (int)(boxH * fr);
                RECT hb = {hpX, (int)miny, hpX+4, (int)maxy};
                FillRect(hdc, &hb, g_br_hpbg);
                RECT hf = {hpX, (int)maxy - hh, hpX+4, (int)maxy};
                COLORREF hc = e->downed ? col
                            : (fr>0.5) ? RGB(g_pal_v.hp_hi[0],g_pal_v.hp_hi[1],g_pal_v.hp_hi[2])
                            : (fr>0.25)? RGB(g_pal_v.hp_mid[0],g_pal_v.hp_mid[1],g_pal_v.hp_mid[2])
                                       : RGB(g_pal_v.hp_lo[0],g_pal_v.hp_lo[1],g_pal_v.hp_lo[2]);
                FillRect(hdc, &hf, esp_pen_brush(hc));
            }
        }
    }

    /* Vehicles: the same box the players get. The chassis is drawn from two
       world points (ground + roof) and the screen box falls out of the
       projection, exactly like the player fallback path. Only the name and the
       distance are drawn - hull and crew readouts were noise on a screen that
       already carries one marker per player.
       Armed variants (turret / minigun / rockets / tank / howitzer) keep an
       orange box and an [ARM] tag: those are the ones that kill you. */
    if(g_cfg_show_vehicles && g_nvehrender>0){
        for(int i=0;i<g_nvehrender;i++){
            VENT* v=&g_vehrender[i];
            if(g_cfg_veh_armed_only && !v->armed) continue;
            if(g_cfg_max_distance>0 && v->dist>(double)g_cfg_max_distance) continue;

            /* Chassis roof height per family, so the box hugs the real vehicle:
               a Humvee roofline is ~2.2 m, a Ural ~2.6 m, a helicopter body
               ~2.5 m above the skids, tracked armour ~3.0 m. */
            double h = 220.0;
            if      (v->kind == 3) h = 250.0;
            else if (v->kind == 4) h = 300.0;
            else if (v->kind == 2) h = 260.0;

            const V3* vp = veh_smooth_pos(v);
            V3 feet = *vp;
            V3 roof = *vp; roof.z += h;
            double fx, fy, rx, ry;
            int fok = w2s_a(&feet,&g_rcam,&g_rcamrot,g_rfov,g_raspect,g_sw,g_sh,&fx,&fy);
            int rok = w2s_a(&roof,&g_rcam,&g_rcamrot,g_rfov,g_raspect,g_sw,g_sh,&rx,&ry);
            if(!fok && !rok) continue;
            if(!fok){ fx = rx; fy = ry + 60.0; }
            if(!rok){ rx = fx; ry = fy - 60.0; }

            double minx, miny, maxx, maxy;
            minx = (fx < rx) ? fx : rx;
            maxx = (fx > rx) ? fx : rx;
            miny = (fy < ry) ? fy : ry;
            maxy = (fy > ry) ? fy : ry;
            double bh = maxy - miny;
            double bw = bh * 1.55;
            if(bw < 10.0) bw = 10.0;
            if(bw > 420.0) bw = 420.0;
            double mxx = (minx + maxx) * 0.5;
            minx = mxx - bw * 0.5;
            maxx = mxx + bw * 0.5;

            if(maxx < -200 || minx > g_sw + 200 || maxy < -200 || miny > g_sh + 200) continue;

            COLORREF vcol = v->armed ? RGB(255,140,0) : RGB(255,200,60);
            HPEN vpen = esp_pen_for(vcol, 1);
            HPEN vold = (HPEN)SelectObject(hdc, vpen);
            HBRUSH obr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, (int)minx, (int)miny, (int)maxx, (int)maxy);
            SelectObject(hdc, obr);
            SelectObject(hdc, vold);

            int cx = (int)((minx + maxx) * 0.5);
            int labelY = (int)miny - 38;

            char nb[96];
            snprintf(nb, sizeof(nb), "%s%s", v->armed ? "[ARM] " : "",
                     v->label[0] ? v->label : "Vehicle");
            DrawTextC(hdc, cx, labelY, nb, vcol);
            labelY += 17;

            /* Crew count only when somebody is inside: an empty vehicle needs
               no "0/4". */
            char ib[64];
            if(v->occ > 0 && v->seats > 0)
                snprintf(ib, sizeof(ib), "%d/%d  %.0fm", v->occ, v->seats, v->dist/100.0);
            else
                snprintf(ib, sizeof(ib), "%.0fm", v->dist/100.0);
            DrawTextC(hdc, cx, (int)maxy + 6, ib,
                      RGB(g_pal_v.distance[0], g_pal_v.distance[1], g_pal_v.distance[2]));
        }
    }

    /* ---- Lead marker: the WarThunder-style circle --------------------------
       A small open circle with a centre dot and four ticks, drawn at the world
       point the shot must actually pass through: ahead of the target by its own
       velocity over the flight time, and above it by the drop the dialled-in
       zeroing has not already compensated. Because it projects a real world
       point it slides naturally as the target moves and as the player turns -
       nothing here is a screen-space offset.
       Only drawn where a velocity has been established: on a stationary target
       the circle would sit exactly on the body, which is noise, not information. */
    /* lead marker removed by dj request 02.10 */

    Proj_Draw(hdc);
    L81_Draw(hdc,vw,vh);
}


static int KeyPressed(int vk, int* was_down) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = down && !(*was_down);
    *was_down = down;
    return hit;
}

/* Hotkeys are polled, not registered: the overlay runs elevated while the game
   does not, and UIPI blocks RegisterHotKey from ever firing in that split.
   F1 toggles ESP. When the menu is open, Up/Down/Left/Right navigate it and
   HOME closes it; F-keys stay live so dj can flip features without closing. */
/* The game hides and clips the cursor during a raid (mouse capture). While the
   menu is open we take it back: ShowCursor counter-pairs every hide the game
   issued, and ClipCursor(NULL) releases the confining rectangle - without that
   the pointer cannot even reach the panel. */
/* While the menu is open the overlay must RECEIVE the mouse, not pass it
   through to the game. WS_EX_TRANSPARENT is what made clicks fall into the game
   and left the menu keyboard-only. Swapping the ex-style also lets us release
   the game's cursor capture: without ClipCursor(NULL) the pointer is confined
   to the game's window rect and cannot reach the panel at all. */
static void menu_cursor(int show) {
    static int shown = 0;
    if (!g_hOverlay) return;
    LONG ex = GetWindowLongA(g_hOverlay, GWL_EXSTYLE);
    if (show && !shown) {
        SetWindowLongA(g_hOverlay, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        while (ShowCursor(TRUE) < 0) { }
        ClipCursor(NULL);
        SetCursor(LoadCursorA(NULL, IDC_ARROW));
        shown = 1;
    } else if (!show && shown) {
        SetWindowLongA(g_hOverlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        ShowCursor(FALSE);
        shown = 0;
    }
}

static void palette_cache_reset(void);
static void menu_cursor(int show);

static void PollHotkeys(void) {
    static int was[20] = {0};

    /* --- запросы ImGui-меню: Save/Load/Reset ---
     * Меню поднимает флаг, здесь он виден и сбрасывается. Делаем это в
     * PollHotkeys, а не в ReadThread: файл трогает тот же поток, что и
     * остальная работа с конфигом, — без гонки за CoreWD.cfg. */
    {
        BR_Config* b = br_get();
        if (b->req_save)      { b->req_save = 0;     cfg_save(); }
        if (b->req_load)      { b->req_load = 0;     cfg_load(); }
        if (b->req_defaults)  { b->req_defaults = 0; cfg_defaults(); cfg_save(); }
    }

    /* --- global --- */
    /* F1/HOME теперь дёргают ImGui-меню (br_request_toggle). Родное GDI-меню
       осталось как аварийный путь на INSERT — оно не мешает маркерам и
       работает даже если DX11-поток не поднялся. */
    if (KeyPressed(VK_F1, &was[0]) || KeyPressed(VK_HOME, &was[9])) {
        br_request_toggle();          /* единственное меню — WebView2 */
    }

    {
        if (KeyPressed(VK_F2, &was[1]))  g_cfg_show_box      ^= 1;
        if (KeyPressed(VK_F3, &was[2]))  g_cfg_show_skeleton ^= 1;
        if (KeyPressed(VK_F4, &was[3]))  g_cfg_show_name     ^= 1;
        if (KeyPressed(VK_F5, &was[4]))  g_cfg_show_health   ^= 1;
        if (KeyPressed(VK_F7, &was[6]))  g_cfg_show_team     ^= 1;
        /* L81: F8 - тумблер детектора, F9 - пересканировать мир */
        { static int w8=0,w9=0;
          if (KeyPressed(VK_F8, &w8)) l81_toggle();
          if (KeyPressed(VK_F9, &w9)) { l81_find(); toast(g_l81_n?"L81 FOUND":"L81 NOT FOUND"); } }
        /* Chams: F10 — свечение противников (OverlayMaterial mesh+0x560) */
        { static int w10=0;
          if (KeyPressed(VK_F10, &w10)) {
              g_cfg_cham_on ^= 1;
              toast(g_cfg_cham_on ? "CHAMS ON" : "CHAMS OFF");
          } }
        return;
    }

    /* --- menu open: mouse --- */
    /* The game re-hides the cursor every frame while it has capture, so the
       "show" has to be re-asserted, not just set once on open. */
    { static DWORD last=0; if (GetTickCount()-last > 200) { last=GetTickCount();
        while (ShowCursor(TRUE) < 0) { } ClipCursor(NULL);
        SetCursor(LoadCursorA(NULL, IDC_ARROW)); } }
    POINT mp; GetCursorPos(&mp);
    int mx = mp.x - g_menu_x, my = mp.y - g_menu_y;   /* window is at 0,0 */
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    static int wasdown = 0;
    int clicked = down && !wasdown;
    wasdown = down;

    int W = MENU_W;
    /* Row counts MUST mirror the draw path. They drifted apart when the pages
       grew (ESP gained "Armor bar", Colors was added) and the input code kept
       the old numbers, so the cursor could walk past the end of the row arrays
       and strlen() read a garbage pointer - the 0xc0000005 crash at
       strlen+0x31 in the map. */
    int nrows = (g_menu_page==0)?8 : (g_menu_page==1)?3 : (g_menu_page==2)?18 : 8;
    int body_top = 42;

    /* Title bar: clicking a TAB switches page, clicking empty space starts a
       drag. Previously the whole bar was a drag handle, so the tabs were
       unreachable with the mouse and only Tab worked. */
    /* Tabs live in the title bar: clicking one switches page, clicking the
       empty part of the bar drags the window. */
    int tab_hit = -1;
    {
        int tx = 96;
        for (int p=0;p<4;p++){
            int tw = (p==0)?46:(p==1)?66:(p==2)?60:62;
            if (mx >= tx && mx < tx+tw && my >= 6 && my <= 28) { tab_hit = p; break; }
            tx += tw+4;
        }
    }
    if (clicked && tab_hit >= 0) {
        g_menu_page = tab_hit; g_menu_sel = 0;
    } else if (clicked && my >= 0 && my < 34 && mx >= 0 && mx < W) {
        g_menu_drag = 1; g_menu_dx = mx; g_menu_dy = my;
    }
    if (!down) g_menu_drag = 0;
    if (g_menu_drag && down) {
        g_menu_x = mp.x - g_menu_dx;
        g_menu_y = mp.y - g_menu_dy;
        /* keep it on screen */
        int sw2 = GetSystemMetrics(SM_CXSCREEN), sh2 = GetSystemMetrics(SM_CYSCREEN);
        if (g_menu_x < 0) g_menu_x = 0;
        if (g_menu_y < 0) g_menu_y = 0;
        if (g_menu_x > sw2-W) g_menu_x = sw2-W;
        if (g_menu_y > sh2-g_menu_h) g_menu_y = sh2-g_menu_h;
    }

    /* Hover + click on rows */
    g_menu_hover = -1;
    /* Row hit-test: rows are 34 px, plus 14 px for each slider row above this
       one (sliders take a second line under their label). */
    if (mx >= 0 && mx < W && my >= body_top && my < body_top + nrows*26) {
        int row = (my - body_top) / 26;
        if (row >= 0 && row < nrows) {
            g_menu_hover = row;
            if (clicked && !g_menu_drag) {
                g_menu_sel = row;
                if (g_menu_page==0) {
                    int* f=NULL;
                    switch(row){case 0:f=&g_cfg_enabled;break;case 1:f=&g_cfg_show_box;break;
                        case 2:f=&g_cfg_show_skeleton;break;case 3:f=&g_cfg_show_name;break;
                        case 4:f=&g_cfg_show_health;break;case 5:f=&g_cfg_show_distance;break;
                        case 6:f=&g_cfg_show_weapon;break;}
                    if(f) *f ^= 1;
                } else if (g_menu_page==1) {
                    if(row==0) g_cfg_show_team ^= 1;
                    /* sliders: clicking the right half increases, left half decreases */
                    else if(row==1 || row==2){
                        int* v = (row==1)?&g_cfg_max_distance:&g_cfg_skel_distance;
                        int pos = *v/1000; int step = (mx > W/2)?1:-1;
                        pos += step; if(pos<0)pos=0; if(pos>20)pos=20;
                        *v = pos*1000;
                    }
                } else {
                    if(row==0) cfg_save();
                    else if(row==1) cfg_defaults();
                    else if(row==2){ dump_all_bones(); dump_bone_probe(); }
                }
            }
        }
    }
    /* Slider drag: hold on a slider row to sweep its value. */
    if (down && !g_menu_drag && g_menu_page==1 && g_menu_hover>=1) {
        static DWORD last = 0;
        if (GetTickCount() - last > 100) {
            last = GetTickCount();
            int* v = (g_menu_hover==1)?&g_cfg_max_distance:&g_cfg_skel_distance;
            int pos = *v/1000; int step = (mx > W/2)?1:-1;
            pos += step; if(pos<0)pos=0; if(pos>20)pos=20;
            *v = pos*1000;
        }
    }

    /* --- menu open: keyboard still works --- */
    /* Clamp before use: a stale index from a previous page used to survive the
       page switch and index past the end of the new row array. */
    if (g_menu_sel < 0) g_menu_sel = 0;
    if (g_menu_sel >= nrows) g_menu_sel = nrows - 1;
    if (KeyPressed(VK_UP, &was[10])){ g_menu_sel--; if(g_menu_sel<0) g_menu_sel=nrows-1; }
    if (KeyPressed(VK_DOWN, &was[11])){ g_menu_sel++; if(g_menu_sel>=nrows) g_menu_sel=0; }
    /* Page switching on Q/E: TAB opens the in-game inventory, so it cannot be
       the menu key. Q and E are free while the menu is open and next to each
       other on the keyboard. */
    if (KeyPressed('Q', &was[15])){ g_menu_page--; if(g_menu_page<0) g_menu_page=3; g_menu_sel=0; g_menu_hover=-1; }
    if (KeyPressed('E', &was[14])){ g_menu_page++; if(g_menu_page>3) g_menu_page=0; g_menu_sel=0; g_menu_hover=-1; }
    /* Both keys must be POLLED every frame: calling them inside one `||` lets the
   short-circuit skip the right-hand poll while LEFT is held, so RIGHT's
   edge-state goes stale and fires a phantom press on release - which is what
   made toggles slip ON->OFF when pressing right, but not left. */
    int kleft  = KeyPressed(VK_LEFT,  &was[12]);
    int kright = KeyPressed(VK_RIGHT, &was[13]);
    /* Colour page: R/G/B pick which channel Left/Right edits, so a full colour
       is reachable without a second menu level. */
    if (g_menu_page==2) {
        if (KeyPressed('R', &was[17])) g_color_ch = 0;
        if (KeyPressed('G', &was[18])) g_color_ch = 1;
        if (KeyPressed('B', &was[19])) g_color_ch = 2;
    }
    if (kleft || kright) {
        if (g_menu_page==0) {
            int* f=NULL;
            switch(g_menu_sel){case 0:f=&g_cfg_enabled;break;case 1:f=&g_cfg_show_box;break;
                case 2:f=&g_cfg_show_skeleton;break;case 3:f=&g_cfg_show_name;break;
                case 4:f=&g_cfg_show_health;break;case 5:f=&g_cfg_show_armor;break;
                case 6:f=&g_cfg_show_distance;break;case 7:f=&g_cfg_show_weapon;break;}
            if(f) *f ^= 1;
        } else if (g_menu_page==2) {
            unsigned char* c = pal_row(g_menu_sel);
            int d = kright ? 8 : -8;
            if(g_menu_sel==6){ (*g_name_fac) ^= 1; }
            else if(g_menu_sel==9){ (*g_dist_fac) ^= 1; }
            /* Head mode cycles CUSTOM -> FACTION -> NONE -> CUSTOM. NONE is a
               real option, not just "off": it draws the outline only, which is
               what you want when the solid disc hides the target. */
            else if(g_menu_sel==12){ (*g_head_fac) = ((*g_head_fac) + 1) % 3; }
            else if(g_menu_sel==13){ g_cfg_show_head = g_cfg_show_head ? 0 : 1; }
            else if(c){ int v = (int)c[g_color_ch] + d; if(v<0)v=0; if(v>255)v=255; c[g_color_ch]=(unsigned char)v; }
            palette_cache_reset();
            /* keep the pen cache in sync - it keys on colour */
        } else if (g_menu_page==3) {
            /* Action rows fire on Left/Right too: pressing an arrow on "Save
               settings" is the natural move, and requiring Enter there was what
               made the feature look broken. */
            if(g_menu_sel==0){ cfg_save(); toast("SETTINGS SAVED"); }
            else if(g_menu_sel==1){ cfg_load(); toast("SETTINGS LOADED"); }
            else if(g_menu_sel==2){ cfg_defaults(); cfg_save(); toast("RESET TO DEFAULTS"); }
            else if(g_menu_sel==3){ dump_all_bones(); dump_bone_probe(); toast("BONES DUMPED"); }
            else if(g_menu_sel==4){ l81_toggle(); toast(g_l81_on?"L81 DETECTOR ON":"L81 DETECTOR OFF"); }
            else if(g_menu_sel==5){ l81_find(); toast(g_l81_n?"L81 FOUND":"L81 NOT FOUND"); }
            else if(g_menu_sel==6){
                /* калибровка: текущий угол -> введённая с экрана дальность */
                g_l81_lastrng = (g_l81_lastrng < 100.0) ? 500.0 : g_l81_lastrng + 50.0;
                if (g_l81_lastrng > 2000.0) g_l81_lastrng = 100.0;
                l81_calibrate(g_l81_lastrng);
                toast(g_l81_msg);
            }
            else if(g_menu_sel==7){
                /* источник дальности: PHYS (v0=250) <-> CALIB (k) */
                if (g_l81_k > 0.0) { g_l81_k = 0.0; toast("MODEL v0=250"); }
                else               { g_l81_k = 1.0; g_l81_b = 0.0; toast("CALIB k=1 (adjust)"); }
            }
        } else if (g_menu_page==1) {
            if(g_menu_sel==0) g_cfg_show_team ^= 1;
            else {
                int* v=(g_menu_sel==1)?&g_cfg_max_distance:&g_cfg_skel_distance;
                int pos=*v/1000; int step=kleft?-1:1;
                pos+=step; if(pos<0)pos=0; if(pos>250)pos=250; *v=pos*1000;
            }
        }
    }
    if (KeyPressed(VK_RETURN, &was[16])) {
        if (g_menu_page==3) {
            if(g_menu_sel==0) cfg_save();
            else if(g_menu_sel==1) cfg_load();
            else if(g_menu_sel==2) cfg_defaults();
            else if(g_menu_sel==3){ dump_all_bones(); dump_bone_probe(); }
            else if(g_menu_sel==4){ l81_toggle(); }
            else if(g_menu_sel==5){ l81_find(); }
        } else if (g_menu_page==0) {
            int* f=NULL;
            switch(g_menu_sel){case 0:f=&g_cfg_enabled;break;case 1:f=&g_cfg_show_box;break;
                case 2:f=&g_cfg_show_skeleton;break;case 3:f=&g_cfg_show_name;break;
                case 4:f=&g_cfg_show_health;break;case 5:f=&g_cfg_show_armor;break;
                case 6:f=&g_cfg_show_distance;break;case 7:f=&g_cfg_show_weapon;break;}
            if(f) *f ^= 1;
        }
    }

    if (KeyPressed(VK_F2, &was[1]))  g_cfg_show_box      ^= 1;
    if (KeyPressed(VK_F3, &was[2]))  g_cfg_show_skeleton ^= 1;
    if (KeyPressed(VK_F4, &was[3]))  g_cfg_show_name     ^= 1;
    if (KeyPressed(VK_F5, &was[4]))  g_cfg_show_health   ^= 1;
    if (KeyPressed(VK_F7, &was[6]))  g_cfg_show_team     ^= 1;

    /* Write the config whenever anything changed. Relying on the close-menu
       hook alone lost everything if the process was killed or crashed, and the
       load side was never wired up at all - both are why settings appeared not
       to persist. Debounced to 1 s so holding an arrow does not hammer the
       disk. */
    {
        static DWORD lastwrite = 0;
        static char  snapshot[256] = {0};
        char now[256];
        snprintf(now, sizeof(now), "%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d",
                 g_cfg_enabled, g_cfg_show_box, g_cfg_show_skeleton, g_cfg_show_name,
                 g_cfg_show_health, g_cfg_show_distance, g_cfg_show_team, g_cfg_only_visible,
                 g_cfg_show_weapon, g_cfg_show_armor, g_cfg_max_distance, g_cfg_skel_distance * 10,
                 (*g_dim_pct), (*g_name_fac), (*g_dist_fac),
                 g_pal_v.weapon[0], g_pal_v.name[0], g_pal_v.armor[0], g_color_ch);
        if (strcmp(now, snapshot) != 0) {
            DWORD nd = GetTickCount();
            if (nd - lastwrite > 1000) {
                lastwrite = nd;
                snprintf(snapshot, sizeof(snapshot), "%s", now);
                cfg_save();
            }
        }
    }
}

/* --- шрифт надписей: TT Norms Std Trl Cnd (приватно, из папки exe) --------- */
static HFONT g_fnt_esp = NULL;
static void InitEspFont(void) {
    char exe[MAX_PATH]; exe[0]=0;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* slash = strrchr(exe, (char)92); if (slash) *(slash+1) = 0;  /* backslash = 0x5C */
    char fpath[MAX_PATH];
    snprintf(fpath, sizeof(fpath), "%sTTNormsTrlCnd-Medium.otf", exe);
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, fpath, -1, wpath, MAX_PATH);
    AddFontResourceExW(wpath, FR_PRIVATE, 0);
    g_fnt_esp = CreateFontW(
        -13, 0, 0, 0,
        FW_MEDIUM,                 /* вес: Medium начертание */
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"TT Norms Std Trl Cnd");
    if (!g_fnt_esp) g_fnt_esp = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void InitGdiCache(void) {
    InitEspFont();
    g_pen_team   = CreatePen(PS_SOLID,2,RGB(90,220,255));
    g_pen_blue   = CreatePen(PS_SOLID,2,RGB(90,170,255));
    g_pen_red    = CreatePen(PS_SOLID,2,RGB(255,70,70));
    g_pen_green  = CreatePen(PS_SOLID,2,RGB(80,255,120));
    g_pen_grey   = CreatePen(PS_SOLID,2,RGB(200,200,200));
    g_pen_blue_dim  = CreatePen(PS_SOLID,2,RGB(45,85,128));
    g_pen_red_dim   = CreatePen(PS_SOLID,2,RGB(128,35,35));
    g_pen_green_dim = CreatePen(PS_SOLID,2,RGB(40,128,60));
    g_pen_dbno      = CreatePen(PS_SOLID,2,RGB(255,210,0));
    g_pen_dbno_dim  = CreatePen(PS_SOLID,2,RGB(140,110,0));
    g_pen_vis    = CreatePen(PS_SOLID,2,RGB(80,255,120));
    g_pen_hid    = CreatePen(PS_SOLID,2,RGB(255,70,70));
    g_pen_border = CreatePen(PS_SOLID,1,RGB(90,220,255));
    g_br_panel = CreateSolidBrush(RGB(10,14,18));
    g_br_sel   = CreateSolidBrush(RGB(24,60,66));
    g_br_hpbg  = CreateSolidBrush(RGB(25,25,25));
    g_br_hpgr  = CreateSolidBrush(RGB(60,230,90));
    g_br_hpyl  = CreateSolidBrush(RGB(240,200,50));
    g_br_hprd  = CreateSolidBrush(RGB(240,60,60));
    g_br_armor = CreateSolidBrush(RGB(120,170,255));   /* armour: cool blue-grey */
}

static LRESULT CALLBACK OverlayProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    /* Belt and braces with WS_EX_NOACTIVATE: even if a click somehow reaches
       this window, it must not take the foreground from the game. */
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_APP + 1: {
        /* Whole-window repaint. Correct beats clever here: partial repaints
           under a colour-keyed layered window both froze stale pixels and
           clipped markers, and cost nothing measurable to avoid. */
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc=BeginPaint(h,&ps);
        DWORD t0 = GetTickCount();
        {
            RECT rc; GetClientRect(h,&rc);
            int w=rc.right-rc.left, hgt=rc.bottom-rc.top;
            if(w>0&&hgt>0){
                /* GPU surface. DirectComposition composites it, so there is no
                   DIB, no per-pixel alpha fix-up pass and no 14 MB copy to DWM:
                   a transparent pixel is simply alpha 0 in the swap chain. */
                if(!gpu_ready() && !gpu_init(h, w, hgt)) {
                    MessageBoxA(NULL, "GPU backend unavailable (D3D11/DComp)", "ESP", 0);
                    EndPaint(h,&ps);
                    return 0;
                }
                gpu_size(w, hgt);
                EnterCriticalSection(&g_cs);
                g_nrender = g_nents[g_front];
                if (g_nrender > MAX_PLAYERS) g_nrender = MAX_PLAYERS;
                memcpy(g_render, g_ents[g_front], (size_t)g_nrender*sizeof(ENT));
                g_render_gen = g_front;
                g_rcam = g_cam; g_rcamrot = g_camrot; g_rfov = g_fov; g_raspect = g_aspect;
                LeaveCriticalSection(&g_cs);
                /* bones snapshot is small; take it outside the entity lock */
                EnterCriticalSection(&g_cs);
                for (int i=0;i<g_nrender;i++) {
                    int n=g_chbn[g_front][i];
                    if (n<0 || n>CHAMS_MAX_BONES) n=0;
                    g_chrn[i]=n;
                    if (n) {
                        memcpy(g_chr[i], g_chb[g_front][i], (size_t)n*sizeof(CH_XF));
                        memcpy(g_chrmap[i], g_chmap[g_front][i], (size_t)ch_bone_count()*sizeof(int));
                    }
                }
                LeaveCriticalSection(&g_cs);

                /* ---- frame ----
                   Order matters: clear, chams (raw D3D, under the D2D batch),
                   then the 2D markers, then present. */
                gpu_begin();
                {
                    /* Upload only the rectangle chams touched since the last
                       frame: a full 1440p upload is 14 MB, a marker-sized box
                       is a few KB. */
                    int bx0=0, by0=0, bx1=0, by1=0;
                    ch_bbox(&bx0, &by0, &bx1, &by1);
                    if (g_chams_bb && bx1 > bx0 && by1 > by0)
                        gpu_chams_upload(g_chams_bb, g_chams_bb_w, g_chams_bb_h,
                                         g_chams_bb_w * 4, bx0, by0, bx1, by1);
                    /* The chams border: the silhouette grown by N px in the
                       pixel shader. 2 px reads as an outline at 1440p without
                       turning into a glow. */
                    if (g_cfg_cham_on)
                        gpu_chams_draw(g_cham_outline, RCOLORREF(g_cham_outline_col), 1.0f);
                }
                DrawOverlay(NULL, w, hgt);
                DrawSelfTest();
                DrawFps(w, hgt);
                gpu_present();
#if MIRROR_ON
                /* Mirror path still wants the finished surface; it reads the
                   chams buffer, which is the only CPU-side copy left. */
                if (g_chams_bb) ws_send_frame((const uint32_t*)g_chams_bb, w, hgt);
#endif
            }
        }
        EndPaint(h,&ps);
        {
            static DWORD acc=0, cnt=0, t_win=0;
            DWORD dt = GetTickCount() - t0;
            acc += dt; cnt++;
            if (t_win == 0) t_win = GetTickCount();
            if (GetTickCount() - t_win >= 5000) {
                FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\paint.txt","a");
                if (lf) { fprintf(lf, "paints=%lu avg_ms=%lu\n", cnt, cnt?acc/cnt:0); fclose(lf); }
                acc=0; cnt=0; t_win=GetTickCount();
            }
        }
        return 0;
    }
    }
    return DefWindowProc(h,m,w,l);
}

void motion_probe_tick(void);
static DWORD WINAPI ReadThread(LPVOID lp){ (void)lp;
    /* 1 ms timer resolution for THIS thread as well. The render thread already
       raised it, but Sleep() resolution is per-process and the read loop is the
       one whose cadence the markers actually follow - leaving it at the default
       15.6 ms quantum was what made the boxes lag behind the players. */
    timeBeginPeriod(1);
    /* The roster scan is cheap; the 4600-entry actor sweep is not, so it runs
       once every ~2 s. World lookup is cached for 5 s: the old code called
       find_world() (up to millions of probes) every 8 ms. */
    uint64_t world = 0;
    int tick = 0, wcheck = 0;
    for(;;){
        if(!world || (wcheck++ % 150)==0){
            uint64_t w = find_world();
            if(w) world = w;
        }
        if(world && !probe_world(world)) world = 0;
        if(world) scan(world, (tick % 40) == 0);
        tick++;
        motion_probe_tick();   /* automatic frozen-skeleton check */
        dump_vitality_state(); /* raw FWDCharacterVitalityState grid */
        dump_local_health_block(); /* local-pawn vitality bytes 0x110..0x1A0 */

        /* Телеметрия для ImGui-меню. Пишем её здесь, а не в отрисовке:
         * ReadThread крутится постоянно, а RenderThread спит, когда в кадре
         * нет маркеров, — счётчики в меню не должны замирать вместе с ним. */
        {
            BR_Config* b = br_get();
            b->telem_targets  = g_nents[g_front];
            b->telem_game_pid = (int)g_pid;
        }

        if ((tick % 150) == 0) {
            long c = g_ioctl_count, b = g_ioctl_bytes;
            g_ioctl_count = 0; g_ioctl_bytes = 0;
            FILE* lf = fopen("E:\\Project\\wardogs\\WarDogsESP\\perf.txt","a");
            if (lf) {
                fprintf(lf, "ioctl/s=%ld bytes/s=%ld ents=%d | span=%ld relloc=%ld bones=%ld wep=%ld sweep=%ld objcls=%ld fname=%ld parr=%ld findw=%ld | calls/s: span=%ld relloc=%ld bones=%ld wep=%ld findw=%ld\n",
                    c / 5, b / 5, g_nents[g_front],
                    g_c_span, g_c_relloc, g_c_bones, g_c_wep, g_c_sweep, g_c_objclass, g_c_fname, g_c_parr, g_c_scan,
                    g_c_span/5, g_c_relloc/5, g_c_bones/5, g_c_wep/5, g_c_scan/5);
                fclose(lf);
            }
        }
    if (g_nproj > 0 || g_cfg_show_proj_trail) {
        EnterCriticalSection(&g_cs);
        int np = 0;
        for (int i = 0; i < PROJ_MAX; i++)
            if (g_proj[i].used && g_proj[i].solved &&
                (g_proj[i].flying || (g_proj[i].land_ms &&
                 (unsigned long)(GetTickCount() - g_proj[i].land_ms) <= PROJ_POST_IMPACT_MS))) np++;
        g_nproj = np;
        LeaveCriticalSection(&g_cs);
    }
    /* Read as fast as the driver allows. timeBeginPeriod(1) is raised at the
           top of this thread: without it Sleep(8) really sleeps 20-25 ms, so the
           "fast" read loop was running at ~40 Hz and every marker trailed the
           body by a full read period. The snapshot hand-off is a memcpy under a
           lock, so the higher rate costs one extra memcpy per tick, not more
           IOCTLs. */
        Sleep(8);
    }
    return 0;
}
static DWORD WINAPI RenderThread(LPVOID lp){ (void)lp;
    /* Frame pacing. The old loop called UpdateWindow (a *synchronous* paint)
       from this worker thread, which races the DWM composition path and shows
       up as mouse lag; Sleep(16) without timeBeginPeriod actually sleeps
       20-25ms, so "60 Hz" was really ~40 Hz with jitter. Now: raise the timer
       resolution once, snapshot under the lock, and let the window's own
       message loop invalidate the regions that actually contain markers.
       Painting only dirty rects instead of the full 1440p canvas is the single
       biggest win against DWM color-key compositing cost. */
    timeBeginPeriod(1);
    for(;;){
        PollHotkeys();
        /* WM_PAINT owns the render snapshot. Do not mutate its entities or
           camera while the GUI thread is rasterising a frame. */
        /* Advance the vehicle interpolators once per presented frame, against
           the freshly copied snapshot. Doing it here (not on the read thread)
           is what lets the boxes glide between reads instead of stepping. */
        veh_interp_tick();
        /* Rasterise chams on THIS thread, off the paint path: the raster is
           15-25 ms on a full server and was serialising the marker draw behind
           it inside WM_PAINT - that serialisation is the marker lag dj sees
           with chams enabled. The DIB is owned by the paint loop; a paint can
           run concurrently, but each writes disjoint regions this frame and
           the next paint re-erases via ch_undo(), so a rare overlap costs one
           frame of a wireframe, not a crash. */
        if (g_hOverlay && gpu_ready()) {
            RECT rc; if (GetClientRect(g_hOverlay, &rc) && rc.right > rc.left) {
                chams_draw_all(rc.right - rc.left, rc.bottom - rc.top);
            }
        }
        if(g_hOverlay){
            /* Ask the GUI thread to repaint. PostMessage (not SendMessage)
               keeps this thread off the paint path entirely. */
            PostMessage(g_hOverlay, WM_APP + 1, 0, 0);
        }
        /* Markers at 60 Hz. Keep the mortar animation at 60 Hz even when
           there are no player markers in view. */
        Sleep((g_nrender > 0 || g_nproj > 0) ? 8 : 90);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE hPrev,LPSTR lpCmd,int nShow){
    (void)hPrev;(void)nShow;
    /* Режим предпросмотра меню: «-menu» -> только окно WebView2-меню, без
     * проверки игры/драйвера. Удобно для доводки интерфейса, пока игра
     * недоступна (обновление/патч). Телеметрия остаётся нулевой. */
    int menu_only = (lpCmd && strstr(lpCmd,"-menu"));
    if(!menu_only){
      { HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
      if(s!=INVALID_HANDLE_VALUE){ PE32A e; e.dwSize=sizeof(e);
        typedef BOOL(WINAPI*F)(HANDLE,PE32A*);
        F pf=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32First");
        F pn=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32Next");
        if(pf&&pf(s,&e)){ do{ if(!_stricmp(e.szExeFile,"WardogsClient-Win64-Shipping.exe")){g_pid=e.th32ProcessID;break;} }while(pn&&pn(s,&e)); }
        CloseHandle(s);} }   /* HANDLE snapshot closed explicitly */
      if(!g_pid){ MessageBoxA(NULL,"Wardogs is not running","ESP",0); return 1; }
      if(!drv_open()){ MessageBoxA(NULL,"Kernel backend unavailable - map the driver","ESP",0); return 2; }
      /* Чамсы: геометрия грузится один раз, из памяти читаются только кости. */
      if(!chams_init()) MessageBoxA(NULL,"chams: mesh.bin not loaded (chams off)","ESP",0);

      /* The session key changes every time the driver is mapped, so read it from
         the registry instead of compiling it in. A stale key makes every read
         return STATUS_ACCESS_DENIED and the ESP silently shows nothing. */
      {
        HKEY hk = NULL;
        LONG rr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\AbiComm", 0, KEY_READ, &hk);
        if (rr == ERROR_SUCCESS && hk) {
            DWORD v = 0; DWORD sz = sizeof(v); DWORD ty = 0;
            if (RegQueryValueExA(hk, "SessionKey", NULL, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && v)
                g_drvkey = v;
            RegCloseKey(hk);
        }
      }
      g_base=drv_base();
      if(!g_base){ MessageBoxA(NULL,"Cannot read the game image base","ESP",0); return 3; }
    }

    g_sw=GetSystemMetrics(SM_CXSCREEN);
    g_sh=GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    InitGdiCache();
    pal_bind();                     /* слоты моста -> указатели палитры (ДО cfg_load) */
    cfg_load();                     /* restore menu position, toggles and palette */
    /* Калибровка ассиста приезжает из конфига: она не зависит от того, куда
       смотрит игрок, и следующая сессия стартует уже с измеренным значением,
       а не с дефолтных 20 пикс/градус. Неправдоподобное значение игнорируем. */
    {
        int gx = br_get()->aa_gain_x100, gy = br_get()->aa_gain_y100;
        if (gx >= 100 && gx <= 120000 && gy >= 100 && gy <= 120000) {
            g_aa.gain_x = gx / 100.0; g_aa.gain_y = gy / 100.0; g_aa.warm = 1;
        }
        /* Опорный масштаб (широкий вид) — отдельно от текущего: по нему
           считается рост зоны под оптикой. Без него первая же сессия с
           оптикой приняла бы зум за опору и снова сжала зону. */
        int rf = br_get()->aa_gain_ref_x100;
        if (rf >= 100 && rf <= 120000) { g_gain_min_x = rf / 100.0; g_zone_prev = 1.0; }
    }
    if (g_menu_page < 0 || g_menu_page > 3) g_menu_page = 0;   /* config sanity */

    /* ImGui-меню живёт в своём потоке и правит ТУ ЖЕ BR_Config, что читает
     * этот код, — поэтому тумблеры и цвета доезжают до маркеров без обмена
     * сообщениями. Родное GDI-меню осталось как fallback (клавиша INSERT). */
    br_start_menu();

    WNDCLASSA wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=OverlayProc; wc.hInstance=hInst; wc.lpszClassName="WdOverlay";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClassA(&wc);
    g_hOverlay=CreateWindowExA(
        /* NOREDIRECTIONBITMAP: no window surface at all - the window is just an
           anchor for the DirectComposition visual. LAYERED is gone with the
           UpdateLayeredWindow path.
           NOACTIVATE: an overlay must never take the foreground. Without it a
           topmost full-screen window becomes a candidate whenever the current
           foreground goes away, and every keystroke then lands in a window that
           ignores them - the game looks dead while it is still running. */
        WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|
        WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
        "WdOverlay","",WS_POPUP|WS_VISIBLE,0,0,g_sw,g_sh,NULL,NULL,hInst,NULL);
    if(!g_hOverlay) return 4;
    /* Click-through, done properly.
       WS_EX_TRANSPARENT only makes a window transparent to the mouse when the
       window is layered, and WM_NCHITTEST=HTTRANSPARENT only passes the hit to
       windows of the SAME thread. Neither helps a non-layered DirectComposition
       overlay: it kept eating every click on the desktop, which is exactly what
       "the game moves but does not shoot, other windows are dead" was.
       An empty window region removes the window from hit-testing for real,
       across processes. The overlay itself never needs mouse input - the menu is
       a separate window - so nothing is lost. */
    /* A window with WS_EX_LAYERED is not actually layered until a layered
       attribute is set, and only a real layered window is skipped by the mouse
       hit-test. Without this call the overlay stayed in the input path and ate
       every click - the game moved but would not shoot, and windows underneath
       could not be clicked. Alpha 255 here means "window content opaque"; the
       window has no content of its own, the visual is composited separately. */
    SetLayeredWindowAttributes(g_hOverlay, 0, 255, LWA_ALPHA);
    /* D3D11 + DirectComposition surface; per-pixel alpha lives in the swap
       chain, so DWM composites it on the GPU instead of scanning the window for
       a transparency key. */
    g_use_ulw = 0;
    /* UpdateLayeredWindow with a 32-bit premultiplied-alpha surface instead of
       SetLayeredWindowAttributes(LWA_COLORKEY). Color-key mode forces DWM to
       scan every pixel of the 1440p window on every frame for the transparent
       colour, on the same composition path as the mouse cursor, and it makes
       "erase to black" impossible (black IS the hole). A real alpha channel
       erases properly, so partial repaints become safe again. */
    g_use_ulw = 1;

#if MIRROR_ON
    /* Mirror the overlay to a second PC. Nothing is sent unless the build
       defines MIRROR_HOST, so a normal build has no socket at all. */
    if (!ws_send_init(MIRROR_HOST, WS_PORT))
        MessageBoxA(NULL, "mirror: UDP init failed - running local only", "ESP", 0);
#endif

#if BRIDGE_ON
    /* Publish world state for a remote renderer. */
    {
    #ifdef BRIDGE_TOKEN
        int bok = wd_state_init_tcp_auth(BRIDGE_HOST, WDS_DEFAULT_PORT, BRIDGE_TOKEN);
    #elif defined(BRIDGE_TCP)
        int bok = wd_state_init_tcp(BRIDGE_HOST, WDS_DEFAULT_PORT);
    #else
        int bok = wd_state_init_udp(BRIDGE_HOST, WDS_DEFAULT_PORT);
    #endif
        if (!bok) MessageBoxA(NULL, "bridge: link init failed - local only", "ESP", 0);
    }
#endif

    if(!menu_only){
        CreateThread(NULL,0,ReadThread,NULL,0,NULL);
        CreateThread(NULL,0,RenderThread,NULL,0,NULL);
    }

    MSG msg;
    while(GetMessage(&msg,NULL,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}void esp_dirty_rects(RECT* out, int max, int* count);

