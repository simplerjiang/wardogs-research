# CoreWD — external ESP overlay for WarDogs

> 当前 main 的完成情况、验证边界、未完成事项与 worktree 清理记录见 [WORK_SUMMARY.md](WORK_SUMMARY.md)（2026-10-23）。下文保留原始项目说明；当前驱动使用标准加载/卸载流程，旧 mapper 启动方式不适用于本次构建。

> `MEMORY_EN.md` — how the memory read chain works end to end (driver mapping, IOCTL protocol, kernel copy, user-mode layer).
> Russian: `MEMORY_RU.md` (same document) and `BUILD_RU.md` (build steps, driver.sys vs kernel-mode.sys).

External overlay: reads game memory through a kernel driver, draws boxes / skeletons /
names / health / chams in a transparent DirectComposition window on top of the game.
Nothing is injected into the game process, no game file is modified.

```
run/
  START.bat                 launcher (probe -> map driver -> check game -> start overlay)
  aDKAF815afjAR.exe         the overlay (built from 2_src_cheat, renamed on purpose)
  WebView2Loader.dll        required by the overlay (WebView2 import library)
  menu.html                 the F1 menu UI (loaded from disk at runtime)
  CoreWD.cfg                saved settings (created on first run if missing)
  kd2.exe                   kernel mapper (loads a vulnerable driver, then maps kernel-mode.sys)
  kernel-mode.sys           the image kd2.exe actually maps  <-- the driver payload
  driver.sys                current driver build (kept for the Win10 diagnostic path / rebuilds)
  tools/drvping.exe         backend probe: opens \\.\AbiComm and asks for the game image base
```

---

## Requirements

| | |
|---|---|
| OS | **Windows 11 x64** (see *Why Windows 11 only*) |
| GPU | any D3D11-capable card, drivers installed |
| Game | WarDogs installed through Steam (app **1867240**) |
| Runtime | **WebView2 Evergreen Runtime** — preinstalled on Win11, the menu needs it |
| Rights | administrator for the first launch of each boot (driver mapping) |

---

## Quick start

1. Launch WarDogs through Steam. Join a server and wait until your character is
   **on the map** (not in a lobby, not in the menu). The overlay needs a live world.
2. Run **`START.bat`**.
   - It probes the kernel backend with `tools\drvping.exe` (no admin needed for the probe).
   - If the driver is already mapped this boot, it skips mapping.
   - If not, it asks for administrator rights once and maps `kernel-mode.sys` via `kd2.exe`.
   - Then it checks that `WardogsClient-Win64-Shipping.exe` is running and starts the overlay.
3. In game press **F1** to open the menu.

### Controls

| Key | Action |
|---|---|
| `F1` | open / close the menu |
| `↑` / `↓` | move between rows |
| `←` / `→` | change the value |
| `Q` / `E` | previous / next menu page |

---

## Files you can move around

The whole `run` folder is self-contained: copy it to any Win11 machine, nothing to install.
Everything it needs at runtime sits next to the exe (`WebView2Loader.dll`, `menu.html`,
`CoreWD.cfg`), and the driver chain needs `kd2.exe`, `kernel-mode.sys`, `driver.sys`,
`tools\drvping.exe` in the **same folder** — `START.bat` refuses to start if one is missing.

---

## Why Windows 11 only

The current build was validated on Windows 11 26200 and fails on Windows 10. There are
four independent gates, and only the first one is fatal:

1. **Driver mapping is blocked on Win10.** `kd2.exe` first loads a signed *vulnerable*
   driver (embedded in the mapper), then uses it to map `kernel-mode.sys`. On the Win10
   test machine that first load fails with **`0xc000009a` (STATUS_INSUFFICIENT_RESOURCES)**
   at image-load time — the signature of HVCI / the Microsoft vulnerable-driver blocklist /
   Defender blocking the driver, not of a bug in the mapper. Without the vulnerable driver
   there is no mapping, and without mapping the overlay has no memory access at all.
   On Win11 the same chain maps cleanly.
2. **PatchGuard kills a second map.** The driver may be mapped **once per Windows boot**.
   Mapping it twice in the same boot trips `0x109 CRITICAL_STRUCTURE_CORRUPTION`.
   This applies to Win10 and Win11 equally: after a failed attempt, **reboot** before retrying.
3. **The overlay compositor is Win11-tuned.** The window is a
   `WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED` window with a D3D11 flip-model swap chain
   composed through DirectComposition, drawn with Direct2D/DirectWrite. On Win10 the
   DirectComposition + Direct2D interop path and the layered-window combination behave
   differently and the window can come up black or fully transparent — no markers, no error.
4. **WebView2 runtime.** Windows 11 ships the Evergreen runtime; Windows 10 machines often
   do not have it. Without it the overlay runs, markers draw, but **the F1 menu is blank** —
   the window exists and swallows clicks in the middle of the screen. Install the
   *Microsoft Edge WebView2 Runtime* if you see that symptom.

If you must run on Win10, work through them in that order: mapping first (`drvping.exe`
must succeed), then the overlay window, then the menu runtime.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `[ERROR] Kernel backend did NOT come up within 30s` | UAC declined, Secure Boot / HVCI blocking, or the vulnerable driver is quarantined | Accept the UAC prompt; add the folder to Defender exclusions; check `kdmap.txt` |
| `kdmap.txt` mentions `ExpCov, CI` | the mapper had to clean the CI structure | **reboot** and run again — continuing risks `0x109` |
| `0x109 CRITICAL_STRUCTURE_CORRUPTION` BSOD | driver mapped twice in one boot | reboot; the driver is already live until reboot, `drvping.exe` tells you |
| `0xc000009a` while mapping | vulnerable driver blocked (HVCI / blocklist / Defender) | Win11, HVCI off, Defender exclusion — see *Why Windows 11 only* |
| Overlay runs, no markers | game not on the map yet, or offsets are stale after a game patch | join a server first; rebuild with fresh offsets (`2_src_cheat/HOW_TO_UPDATE.md`) |
| Markers draw, **F1 does nothing / menu invisible** | WebView2 runtime missing or its user-data folder locked | install WebView2 Evergreen Runtime; kill leftover `msedgewebview2.exe` |
| Overlay eats mouse clicks | the overlay window lost its layered/transparent state | restart `START.bat`; a second overlay instance is refused by design |
| Two overlays painting | started twice | `START.bat` refuses a second copy; kill `aDKAF815afjAR.exe` manually if needed |

Logs: `kdmap.txt` (mapper output), `%LOCALAPPDATA%\mrzvCore_webview2` (menu profile).

---

## Notes

- The driver is mapped, not installed: there is no service, nothing in `System32\drivers`,
  nothing to uninstall. A reboot removes it.
- Do not run two overlay instances — they paint the same markers twice and double the reads.
- The overlay never takes focus: the game keeps keyboard and mouse, the menu is drawn on top
  and the transparent window is skipped by hit-testing.

Build instructions: **`BUILD.md`**.
