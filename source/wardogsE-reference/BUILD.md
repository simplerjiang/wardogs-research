# Building CoreWD from source

> Russian short version, focused on the two `.sys` files: **BUILD_RU.md**.

Four pieces, in build order: **driver → mapper → overlay → deploy**.

```
1_run/              ready-to-run build (nothing to compile)
2_src_cheat/        overlay: ESP, chams, GPU backend, WebView2 menu
3_src_driver/       kernel driver (WDM, kdmapper-mapped)
4_src_mapper/       kdmapper fork used to map the driver
```

---

## Prerequisites

| Tool | Version used | Needed for |
|---|---|---|
| Visual Studio 2022 | MSVC **14.44.35207** | everything |
| Windows 11 SDK | **10.0.26100.0** | user-mode code |
| Windows Driver Kit | matching the SDK above | driver only (`km` headers/libs) |
| WebView2 SDK | bundled in `2_src_cheat/WebView2SDK` | overlay only — nothing to install |
| Windows 11 x64 | — | running/testing (see README, *Why Windows 11 only*) |

No package manager, no vcpkg, no NuGet restore. Every path is inside the archive except
Visual Studio and the Windows SDK.

---

## 1. Overlay — `2_src_cheat\build_cheat.bat`

```bat
cd 2_src_cheat
build_cheat.bat
```

Compiles four translation units and links `WarDogsESP_v2.exe`:

| File | Role |
|---|---|
| `wd_esp_full.c` (~6000 lines) | the cheat: attach to the game, driver IOCTL reads, entity scan, marker drawing (boxes/skeleton/name/health/distance/weapon), config load/save, aim assist, hotkeys |
| `gpu.cpp` / `gpu.h` | rendering backend: D3D11 device + flip-model swap chain + DirectComposition + Direct2D/DirectWrite. Exposes `gpu_*` primitives; `gpu_chams_draw()` uploads the chams texture and runs the silhouette-outline shader |
| `menu_webview.cpp` | WebView2 host on its own thread: COM apartment, controller, JS↔C++ bridge, show/hide without stealing focus |
| `menu_bridge.h`, `wv2_cb.h`, `wv2_cb_impl.h` | WebView2 callback glue and the JS bridge functions |
| `chams/chams.c` / `chams.h` | CPU rasterizer of the player mesh into its own ARGB buffer, with dirty-rect tracking (`ch_bbox`) |
| `chams_hook.h` | owns the chams texture on the GPU side, uploads only the changed rectangle |
| `menu.html` | the F1 menu UI (HTML/CSS/JS, loaded from disk at runtime — **not** embedded) |
| `WebView2SDK/` | Microsoft WebView2 SDK: `include/` + `lib/x64/` (import lib + loader DLL) |

Build flags: `/O2 /EHsc /D_CRT_SECURE_NO_WARNINGS`, `cl` per TU, then

```
link /SUBSYSTEM:WINDOWS /MACHINE:X64
     kernel32 user32 gdi32 comctl32 advapi32 ole32 oleaut32 uuid
     d3d11 dxgi d2d1 dwrite dcomp d3dcompiler WebView2Loader
```

**Deploy = 3 files next to each other:** `WarDogsESP_v2.exe`, `WebView2Loader.dll`,
`menu.html`. `WebView2Loader.dll` is an import-library dependency (not optional) and
`menu.html` is read from the exe's folder at startup — miss it and the menu comes up blank.

The exe is renamed to a random-looking name in the run folder (`aDKAF815afjAR.exe`); that is
only a name, `START.bat` looks for exactly that file.

---

## 2. Kernel driver — `3_src_driver\build_driver.bat`

```bat
cd 3_src_driver
build_driver.bat
```

Compiles with the WDK headers directly (`cl /kernel`, then `link /DRIVER /SUBSYSTEM:NATIVE
/ENTRY:DriverEntry /KERNEL`), no msbuild and no WDK VS integration required.
Output: `x64\Release\driver.sys`, copied to `driver.sys`.

What the driver is:

- WDM driver. `DriverEntry` only spawns a system thread; `IoCreateDriver` + `IoCreateDevice`
  run there at PASSIVE with a clean context. (kdmapper calls entry on a hijacked thread —
  doing object-manager work there page-faulted: `0x50` BSOD.)
- Creates its **own** device `\Device\AbiComm` + symlink `\??\AbiComm`. It does not hook
  `null.sys` — that approach tripped PatchGuard `0x109` twice.
- ABI: `METHOD_BUFFERED`, request struct `ABI_COMMAND { magic_key, pid, addr, size }`.

| IOCTL | Value | Meaning |
|---|---|---|
| `ABI_IOCTL_READ_MEMORY` | `0x222004` | read `size` bytes from `pid+addr` |
| `ABI_IOCTL_GET_BASE` | `0x222008` | image base of `pid` (used by `tools\drvping.exe`) |
| `ABI_IOCTL_UNLOAD` | `0x22200C` | unload |
| `ABI_IOCTL_WRITE_MEMORY` | `0x222010` | write `size` bytes |

`MAX_TRANSFER = 0x10000` — 64 KB per call, the client batches reads accordingly.

Session key: `PrefetchParameters\BootId ^ 0x8F2A9C4B`, run through `xorshift32`; the driver
also publishes it at `HKLM\SYSTEM\AbiComm\SessionKey` so the client can pick it up.

### Deploying a new driver build

`kd2.exe` maps the file named **`kernel-mode.sys` from its own folder**. The path you pass on
the command line is not the image that gets mapped (`kernel_mapper.cxx:79`). So:

```bat
copy /Y driver.sys ..\1_run\kernel-mode.sys
copy /Y driver.sys ..\1_run\driver.sys
```

Keep the previous pair until the new one is verified in game — the mapped image is what
matters, and a bad one costs a reboot.

**Never map twice in the same Windows boot.** The second map trips PatchGuard
`0x109 CRITICAL_STRUCTURE_CORRUPTION`. `tools\drvping.exe` tells you whether the driver is
already live this boot (it opens `\Device\AbiComm` and asks for the game image base — a real
liveness probe, unlike the `SessionKey` registry value which survives until reboot).

---

## 3. Mapper — `4_src_mapper\kd2_src\kdmapper.sln`

Open in Visual Studio 2022, platform **x64 / Release**, build. Output `kdmapper.exe` →
deployed as **`kd2.exe`** (`START.bat` expects that name).

What this fork does, in order:

1. Loads a signed *vulnerable* Intel driver (sources and resources under
   `workspace/intel/`) — this is what buys kernel execution.
2. Caches and cleans its load traces.
3. Maps `kernel-mode.sys` from its own directory (`allocation_mode::allocate_pool`).
4. Unloads the vulnerable driver.

Two deliberate deviations from upstream kdmapper:

- **The CI hook is disabled** — `perform_load_trace_cleanup(false)`. Patching `ci.dll` code
  (a `xor rax,rax; ret` stub over a CI function) is a direct kernel-code modification and
  produced `0x109 CRITICAL_STRUCTURE_CORRUPTION` on Win11 26200. Do not re-enable it.
- Strings are wrapped in `skCrypt` (compile-time encryption), so `strings kd2.exe` shows
  nothing useful. Read the source, not the binary.

---

## 4. Offsets after a game patch

A game patch moves three globals; everything else is derived from them at runtime.
The full procedure (in Russian) is `2_src_cheat/HOW_TO_UPDATE.md`. Short version:

| Global | How to find it | Must verify |
|---|---|---|
| `GWorld` | scan for an RVA where `rd64(base+RVA)` yields a world pointer | `world+0x30` → Level, `Level+0xA0`/`+0xA8` → actors data/count (100..400000), `world+0x1B0` → GameState, `GameState+0x2D0` → PlayerArray, **count = 100** on a live server |
| `GObjects` | `rd64(base+RVA)` → chunk table; element = `chunk + idx*0x18 + 8` | first objects are `/Script/CoreUObject`, `Object`, `/Script/Engine` |
| `GNames` | `rd64(base+RVA)` → table of blocks | `block = rd64(table + (idx>>16)*8)`, `entry = block + (idx&0xFFFF)*8`, `len = uint16@(entry+8) >> 6` |

Counts like 132181 / 207518 are false positives from a wrong base — the real PlayerArray
count on a live server is ~100. Offsets live at the top of `wd_esp_full.c`.

---

## 5. Shipped artifacts

| Artifact | Size | MD5 (as shipped) |
|---|---|---|
| `aDKAF815afjAR.exe` (the overlay) | 379 904 | `8b233d5dffd12007b12176102c34ecbd` |
| `kernel-mode.sys` | 7 168 | `2d199590b79e4317b8afc5f9d2b3741f` |
| `driver.sys` | 8 704 | `8bae0e6a38aa4efd5260078c76035848` |
| `kd2.exe` | 217 088 | `d0c2ef36959aff9271de8e7781ace4c4` |

Toolchain: MSVC 14.44.35207, Windows SDK 10.0.26100.0, x64 Release, `/O2 /EHsc`.

A fresh `build_cheat.bat` produces an exe of exactly the same size (379 904 bytes) and the
same code — only 96 bytes differ, and those are the PE header timestamp/checksum, so the MD5
will never match byte-for-byte between two builds. Compare size and behaviour, not hashes.

---

## 6. Known limits

- **Windows 10 is unsupported** — driver mapping is blocked there (`0xc000009a`), and the
  composition window is Win11-tuned. Details in `README.md`.
- 64 KB per driver read; the client splits larger reads.
- The menu needs the WebView2 Evergreen Runtime. On a machine without it the overlay still
  draws markers, but the menu window stays blank — and that blank window still swallows
  clicks in the middle of the screen.
- The driver is mapped once per boot; there is no service to restart. Reboot to remap.
