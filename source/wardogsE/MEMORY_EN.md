# How the memory read works

From the outside it looks like "the cheat sees players". Inside there are four links: the driver mapping, the IOCTL protocol, the copy inside the kernel, and the user-mode layer that turns bytes into world coordinates.

---

## 1. Why not ReadProcessMemory

`ReadProcessMemory` from our process means an open handle on the game plus a call the anti-cheat sees three different ways: the handle table, the ETW provider, the hooked `NtReadVirtualMemory`. One such call is a flag.

So there is no read in user mode at all. The process reads **its own driver**, and the driver reads the game from ring 0. Our process holds no handle on `Wardogs.exe` and not a single `ReadProcessMemory` call in its IAT — only `NtDeviceIoControlFile` against its own device.

---

## 2. Mapping: how the driver gets into the kernel

`START.bat` → UAC → `kd2.exe`.

`kd2.exe` is a kdmapper fork. It carries a signed vulnerable Intel driver in its resources, uses it to drop DSE and map our image into kernel memory. Our image is `kernel-mode.sys` **from kd2.exe's own folder** (the path is hardcoded, see `BUILD_RU.md`). No `sc create`, nothing written to disk: the image lives in non-paged pool and never appears in the loaded-module list.

Then `DriverEntry`:

- creates `\Device\AbiComm` with an SDDL (system — full, admins/world — RW);
- creates the symlink `\??\AbiComm`;
- computes the session key;
- writes it to `HKLM\SYSTEM\AbiComm\SessionKey`.

The device is created through `IoCreateDriver`, not through a normal load — that path is outside PatchGuard's scope.

---

## 3. Session key

The driver reads `BootId` from `\Registry\Machine\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters`, computes `Xorshift(BootId ^ 0x8F2A9C4B)` and that is the `SessionKey`.

The key **changes every boot**. At startup the client reads it back from `HKLM\SYSTEM\AbiComm\SessionKey` and puts it into every IOCTL.

The point: you cannot fire an IOCTL "blind". The driver checks `magic_key` on every request and returns `STATUS_INVALID_PARAMETER` on a mismatch (no crash, no log — just a refusal). A static scanner that finds the device still cannot read a single byte without the key.

---

## 4. Protocol: one IOCTL = one memory range

The device is opened through `NtCreateFile` from ntdll rather than `CreateFileW`: fewer visible imports and no Win32 path parsing. The client walks `\Device\AbiComm0` … `AbiComm4` in case the name is still taken from a previous mapping.

The request is `ABI_COMMAND`, 24 bytes, `#pragma pack(1)`:

```
ULONG     magic_key;    // session key
ULONG     process_id;   // game PID, known up front
ULONG_PTR address;      // where to look
SIZE_T    size;         // how many bytes
```

Four codes:

| IOCTL | Code | What it does |
|---|---|---|
| READ | `0x222004` | copies `size` bytes out of the game into our buffer |
| GET_BASE | `0x222008` | reads `PEB->ImageBaseAddress` (peb + 0x10) |
| UNLOAD | `0x22200C` | deletes the symlink, the device becomes unreachable |
| WRITE | `0x222010` | writes into the game (chams need it) |

The transport is `METHOD_BUFFERED`. The subtlety that bites: when `inputLength < outputLength` the kernel takes the SystemBuffer from the **output** buffer. So on a read we send 24 bytes of command and receive the data straight into our own buffer — no intermediate copy. On a write it is the other way round: input is longer than output, and command + payload share one buffer.

Driver-side limits, all checked before any copy:

- `size` from 1 to `0x10000` (64 KB);
- `size` no larger than the output buffer;
- `magic_key` matches the session key;
- the whole dispatcher is wrapped in `__try/__except` — a bad pointer returns an exception code instead of taking the system down.

`GET_BASE` saves a whole branch: the module base comes from the process PEB instead of enumerating modules from user mode.

---

## 5. What happens in the kernel on a read

```
ReadTargetMemory(pid, addr, kernelBuf, size)
  ├─ PsLookupProcessByProcessId(pid) → PEPROCESS
  ├─ MmCopyVirtualMemory(game, addr, current, kernelBuf, size, KernelMode)
  └─ ObDereferenceObject(process)
```

`MmCopyVirtualMemory` is the kernel's own routine — Windows uses it too. It faults in the pages it needs, deals with non-resident ones (paged out to the pagefile), handles guard pages correctly, and does not leave the process attached if something throws inside the copy.

Why not `KeStackAttachProcess` + `memcpy`: an attach leaves the system in a half-switched state on any error inside the copy, and `memcpy` over a non-resident page is a bugcheck. `MmCopyVirtualMemory` does the same job with all those cases handled.

If fewer bytes arrive than were requested, it returns `STATUS_PARTIAL_COPY`. For the client that means "the page is not mapped": the object was streamed out or already freed.

The anti-cheat angle: `MmCopyVirtualMemory` takes the real page tables from `EPROCESS`. Ghost CR3–class tricks (swapping CR3 in user mode to hide memory from a scanner) do nothing to a kernel read — it never goes through the user-mode address space.

---

## 6. User mode: a thin layer over IOCTL

All memory access comes down to five functions:

```c
static int rd   (uint64_t a, void* o, size_t sz);  // the base call
static uint64_t rd64(uint64_t a);   // 8 bytes
static uint32_t rd32(uint64_t a);   // 4 bytes
static float    rdf (uint64_t a);   // float
static double   rdd (uint64_t a);   // double
```

`rd` throws nothing and logs nothing: it returns 0 and a zero in `out` means "did not read". That is why **every** value used as a pointer goes through `pl()`:

```c
static int pl(uint64_t v){ return v >= 0x10000ULL && v < 0x00007FFFFFFFFFFFULL; }
```

A cheap but mandatory guard: without it one freed actor becomes a chain of reads at garbage addresses — the frame stutters and markers fly off to infinity. `pl()` rejects zeros, kernel addresses and leftovers from recycled memory.

Exactly three things are cached, each for its own reason:

- **the `UWorld` pointer** — lives 5 seconds; otherwise the search would walk millions of addresses every 8 ms;
- **the bone map** (`BONEMAP` keyed by `USkinnedAsset`) — built once per model, because bone names are identical for every player;
- **the entity list** — double-buffered, published under a critical section.

---

## 7. What is read, and how often

The read thread (`ReadThread`) runs on `Sleep(8)` with `timeBeginPeriod(1)` — roughly 120 Hz. Without the raised timer `Sleep(8)` actually sleeps 20–25 ms and every marker trails the body by a full period.

The tick looks like this:

| What | How often |
|---|---|
| player roster (`PlayerArray`) | every tick |
| local pawn, camera, FOV, recoil | every tick |
| entity positions and health | every tick |
| skeletons | every tick, but only for those closer than `eff_skel_dist()` |
| deep sweep of 4600 actors | every 40 ticks (~320 ms) |
| `UWorld` lookup | every 150 ticks (~1.2 s), and whenever the probe fails |

The pointer chains that produce all of it:

```
UWorld
 ├─ +0x1B0 GameState
 │    └─ +0x2D0 PlayerArray → [i] APlayerState
 │         ├─ +0x330 Pawn
 │         └─ +0x360 PlayerNamePrivate
 └─ +0x228 GameInstance
      └─ +0x38 LocalPlayers[0]
           └─ +0x30 PlayerController
                ├─ +0x360 Pawn (mine)
                ├─ +0x2C0 PlayerState
                └─ +0x370 CameraManager → +0x1560 CameraCache → +0x10 POV
```

From the pawn: `+0x1C0` root component (world position), `+0x3A0` mesh, `+0x748` vitality component, `+0x780` weapon, `+0x758` inventory.

**A skeleton is one IOCTL, not 400.** The transform array is read whole: `(last_needed_bone + 1) * 0x60` bytes, 64 KB at most. The heaviest model carries 583 bones — about 55 KB, so it fits one call with room to spare. Previously each bone cost its own IOCTL, and at 80 players the scan could not finish before the frame drew — that was the flicker, worse the fuller the raid.

Bones are stored in **component** space (raw values around the model origin), so `ComponentToWorld` is applied to them; without it the skeleton draws at the map origin.

---

## 8. Cost

- One IOCTL is a synchronous round trip into the kernel, single-digit microseconds. With 80 players whose skeletons are in range that is thousands of calls per second.
- Counters are built in: `perf.txt` in the cheat folder records `ioctl/s`, `bytes/s` and a per-source breakdown (`span`, `relloc`, `bones`, `wep`, `sweep`, `parr`, `findw`). It shows which part of the scan burns the calls.
- `PsLookupProcessByProcessId` runs on **every** read, with no `PEPROCESS` cache. A deliberate trade: a cache would have to be validated on each call, and the saving is fractions of a microsecond against the risk of reading a freed process.
- Rendering after the GPU move: 120–125 fps at ~14.4% of one core. Memory reads run on their own thread and never enter the frame.

---

## 9. When a read "does not work" — what it means

| Symptom | Cause |
|---|---|
| `rd` returns 0 for everything | wrong session key — check `HKLM\SYSTEM\AbiComm\SessionKey` |
| empty markers while the process is alive | offsets went stale after a game patch |
| some fields zero, others fine | `STATUS_PARTIAL_COPY`: page streamed out, object dead |
| skeleton frozen, positions live | reading the LOD bone cache instead of the live array |
| `0xC000009A` while mapping | vulnerable driver blocked (HVCI / blocklist / Defender) |
| `0x109` in a BSOD | second mapping in the same boot — PatchGuard |

---

## 10. What it looks like from outside

Our process never opens the game, never reads its memory and never loads a driver through the SCM. What is visible is different:

- `\Device\AbiComm` appears in the Object Manager — the object exists even with no file behind it;
- the kernel holds an image with no module in the loaded list (mapped, not loaded) — that shows up when comparing `SystemBigPoolInformation` against `PsLoadedModuleList`;
- IOCTL traffic to our device is visible to anyone who filters `IRP_MJ_DEVICE_CONTROL` — the session key stops them reading our data, it does not hide the fact of the conversation;
- the Threat-Intelligence ETW provider catches the vulnerable driver load before it does anything.

So the stealth here is not "invisibility". It is the absence of every user-mode marker an anti-cheat normally looks for: a handle on the game, `ReadProcessMemory`, a module in the list, a file on disk.

---

## 11. Quick check that the read is alive

1. `1_run\tools\drvping.exe` — opens the device, reads the key from the registry, does a test read.
2. `perf.txt` — if `ioctl/s` is above zero and `ents` grows, reads are flowing.
3. If `drvping` cannot open the device, the driver is not mapped: reboot and run `START.bat` again.
