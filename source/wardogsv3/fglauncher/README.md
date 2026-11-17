# Wardog Launcher — 先 PGR、后借物理内存映射加载异体驱动（C++ 复现）

本目录是依据 `E:\CodexData\exports\FGTRACK-完整资料-20261026-01` 里的资料与证书原件做的复现工程：
**不含任何卡密逻辑**，只保留一条被完整记录的链——「微软嵌套签名映像被内核接纳 → 打开其设备 →
经物理内存映射读取数据 → 观测映像一致性校验环节 → 保存原内容 → 写入临时内容 → 请求载入后续映像 →
还原原内容」。

复现对象是**流程与机制**；被载入的「后续映像」是我们自研的、与参照物**完全不同**的测试驱动
`WardogProbe.sys`（未附签名），用来证明该模式对我们自己的 C++ 工程同样成立，而不是把 FGTRACK
再打包一遍。相关实验均在**只读优先、可回滚、可复算**的前提下进行。

> 原理与职责边界（谁加载谁、改的是哪几个字节、读取链路为何分四级）另见
> [docs/原理-驱动准入与内存读取.md](docs/原理-驱动准入与内存读取.md)。

---

## 0. 定位与范围

本工程是一份**本机实验环境的流程复现记录**。研究对象是 Windows 的驱动映像载入流程与物理内存
读取机制；全部实验对象为本机沙箱内的自有进程、自有驱动与本地样本，日志中的地址、哈希、RVA
均来自本机实测，可逐条复算。

**范围内**：驱动准入流程的观测与复现（含判定点的定位方法与对照实验）、物理内存读取通道的抽象
与自测、由案例文件驱动的读取校验，以及上述流程的日志化与可回归性。

**范围外**：网络通信、授权或计费逻辑、对外部环境的任何操作。应用层能力（目标数据读取、画面
呈现、输入控制）属于独立课题，本仓库的代码与文档不包含这部分设计与实现；如需开展，应按独立
方案另行设计、单独评估，不与本工程的驱动准入与内存读取链混在一起。

---

## 1. 交付物

| 路径 | 说明 |
| --- | --- |
| `src/provider.h` | **路线 A 的能力接口**：`IMemoryProvider`（纯物理字节访问）+ `PgrProvider` + `MakeProvider()` |
| `src/main.cpp` | 编排：加载 PGR → 打开设备 → 自检 → 定位判定点 → 临时改动 → 加载/恢复 → 案例引擎 |
| `src/case.h/.cpp` | **案例文件的装载与校验**：进程名、镜像身份、地址清单、期望字节、判定点 RVA |
| `src/json.h` | 无依赖 JSON 读取器（案例文件是数据，不是代码） |
| `src/translate.h/.cpp` | VA → PA 页表遍历、RAM 区间扫描、候选页表根、物理页缓存 |
| `src/target.h/.cpp` | 目标进程定位：路径、镜像基址（四级降级）、磁盘镜像 ground truth |
| `src/pgr.h/.cpp` | 中转驱动的设备契约：握手、物理页映射 / 解除映射 |
| `src/service.h/.cpp` | 服务项创建 + `NtLoadDriver` / `NtUnloadDriver`（含特权重新武装） |
| `src/ci.h/.cpp` | 在磁盘内核里按导出名 + 指令形状定位代码完整性判定点 |
| `src/nt.h` | `NtQuerySystemInformation` 等原生接口的最小声明 |
| `src/common.h` | `PeFile`（PE32/PE32+ 自适应）、JSONL 日志、SHA256 |
| `src/selftest.cpp` | 无管理员自检（合成页表 + 真实 `ci.dll` 字节） |
| `src/fixture.cpp` | 靶场进程 `WardogFixture.exe`：把案例镜像映射到首选基址后保持存活 |
| `driver/WardogProbe.c` | **载荷驱动源码**（唯一自写驱动）：创建 `\Device\WardogProbe` |
| `cases/case-000-reference.json` | 案例 0：完全采用导出包指定的进程与地址 |
| `tools/verify-case.py`、`tools/verify-run.py` | 日志断言（离线 / 在线 / 运行模式） |
| `src/math3d.h`、`src/draw_list.h` | 相机投影与图元列表（屏幕矩形 / 线段 / 框 / 四角） |
| `src/esp_projection.h/.cpp`、`src/esp_scene.h/.cpp` | 目标框几何（投影端点 → 框高、`0.46` 宽比、最小尺寸、缩放）与场景装配 |
| `src/esp_live.h/.cpp` | 偏移配置、相机 POV / Actor 位置读取、`--esp-probe` 标定 |
| `src/overlay_d3d12.h/.cpp` | **自有 D3D12 设备 + composition 交换链**的覆盖层（实例化四边形、回读截图） |
| `offsets/wardog-esp.json` | 目标进程偏移配置（当前为占位值，标定方法见 `docs/esp-overlay.md`） |
| `tools/verify-esp.py` | ESP 报告与回读图像的断言（比例、1.3 缩放、颜色、屏内） |
| `run-esp-demo.cmd`、`run-esp-live.cmd` | ESP 演示 / 真实链路入口脚本 |
| `run-case.cmd`、`run-read.cmd`、`run-read-base.cmd`、`run-read-cr3.cmd`、`run-probe.cmd` | 提权入口脚本 |
| `build.cmd` | 一键构建（VS2022；没有 WDK 也能出驱动） |
| `docs/原理-驱动准入与内存读取.md`、`docs/esp-overlay.md` | 原理说明 / 叠加层实现说明 |

构建产物：`build\WardogLauncher.exe`、`build\WardogProbe.sys`、`build\WardogFixture.exe`；
中转驱动副本 `build\drivers\PGRHostControl.sys`（原件逐字节复制，sha256 与导出包一致）。

---

## 2. 架构：能力 / 机制 / 案例 三层（路线 A）

```
cases/*.json          进程名 + 绝对地址 + 期望字节 + 判定点 RVA      ← 可替换：加文件即加案例
      │
      ▼
broker（WardogLauncher.exe 自身）
      ├─ Translator      VA → PA（四级页表遍历），可在内核/目标进程两个地址空间之间切换
      ├─ ci::Locate…     按参照物同一套办法定位代码完整性判定点
      └─ IMemoryProvider 只提供「物理地址 → 字节」这一件事
              │
              ▼
        PgrProvider → \Device\PGRHostControl （0x80102040 / 0x80102044，参照物的第一跳）
        其它驱动    → 任何能提供物理映射/物理拷贝的已接纳驱动（新增一个类即可）
```

**为什么不是「载荷驱动直接调用中转驱动」**：中转驱动的映射操作内部是
`ZwMapViewOfSection(NtCurrentProcess, …)`，它把物理页映射进**发起请求的那个进程**。驱动发起时
「当前进程」取决于谁在跑这段代码；映射可能落进 System/Worker 线程的地址空间，用户态拿不到，
且每次读写都要 map/unmap。更关键的是：载荷驱动是**在判定点被临时改动之后**才加载的，那时它已有
完整内核权限（`__readcr3()`、`KeStackAttachProcess`、`MmCopyVirtualMemory`），**根本不需要中转驱动**。
所以正确分工是：

| 组件 | 职责 | 会不会被替换 |
| --- | --- | --- |
| `IMemoryProvider` | 第一跳能力（物理字节） | 会（换驱动只动这一层） |
| `Translator` / `ci` | 机制（VA→PA、判定点定位） | 不会 |
| 案例文件 | 业务（读哪里、期望什么字节） | 会（加/改 JSON 即可） |
| 载荷驱动（未签名，判定点之后加载） | 长期业务 | 会（换成任何自己的驱动） |

对照矩阵：

| 要换的东西 | 需要动的代码 | 不需要动 |
| --- | --- | --- |
| 载荷业务 | `--driver` 指向新的 `.sys`，或换掉载荷的 IOCTL 实现 | broker、provider、判定点流程 |
| 第一跳驱动 | `provider.h` 新增一个 `IMemoryProvider` 实现 + `MakeProvider` 注册 | 案例、机制、编排 |
| 目标进程 / 目标地址 | 只改案例 JSON | 全部代码 |

### 2.1 `--mode read`：怎么在「读不了」的进程里定位地址空间

对受保护的客户端（实测 `WardogsClient-Win64-Shipping.exe`，VAC 在场）用户态探测全线不可信，
所以定位靠驱动，分四级，前一级失败自动降级：

| 级 | 手段 | 依赖 | 实测结果 |
| --- | --- | --- | --- |
| 1 | `--base` / 案例文件直接给基址 | 无 | 可用（最稳） |
| 2 | 内核进程链：`PsInitialSystemProcess` → 活动进程链 → 名称+PID → 页表根 | 内核地址空间可读 | 能定位到真实基址 `0x7FF653000000`，但链表偶发在某级页表指向 RAM 之外时中断 |
| 3 | `--cr3` 直接指定目标页表根 | 需先拿到 CR3 | 可用（`0x29883000` 稳定复现） |
| 4 | **用户半区头扫描**：遍历每个内核根页表的用户半区，对 2 MB 区按 64 KB 步长探测 PE 头，与磁盘的 `TimeDateStamp`/`SizeOfImage` 对齐 | 只需候选根集合 | **实测成功，无需任何用户态元数据** |

> 实测反例（重要）：`K32GetMappedFileNameW` 返回的 `0x1BDA18B0000` 在该进程自己的页表里
> **根本没有映射**（`read_unmapped`），是受保护客户端给出的假基址；只有内核侧（级 2 / 级 4）
> 给出的 `0x7FF653000000` 才能走到真实镜像。`rpm_ok=false` 也是常态。

---

## 3. 测试案例 0（`cases/case-000-reference.json`）

案例完全采用导出包里的**指定进程**与**指定地址**；地址基址统一为分析载体基址 `0x140000000`
（`01_调查原始归档/SECURITY-REPORT.txt` 第 178 行）。

| 项 | 值 |
| --- | --- |
| 进程 | `FGTRACK.exe` |
| 镜像 | `02_软件原样本\内层加载器\FGTRACK.exe.bin`，sha256 `96925a10…4f43a` |
| 基址 / SizeOfImage / TimeDateStamp | `0x140000000` / `0x9D8000` / `0x6AC90E62` |

观察点（`observations`）：

| 名称 | VA | 长度 | 说明 |
| --- | --- | --- | --- |
| `image_header` | `0x140000000` | 64 | 与磁盘同 RVA 逐字节比对 |
| `entry_point` | `0x14064C058` | 32 | 入口点（SECURITY-REPORT 第 155 行） |
| `trampoline_slot` | `0x14004AE48` | 8 | 第 260 行 |
| `encoded_unit` | `0x140075F68` | 8 | 第 258 行 |
| `protector_descriptor` | `0x14008D40E` | 4 | 第 254 行 |
| `veh_write_byte` | `0x14033E5DD` | 1 | 第 240 行 |

判定点（`gate`）：

| 项 | 值 |
| --- | --- |
| 例程 | `PsSetLoadImageNotifyRoutine`，RVA `0x787850` |
| 标志字 | RVA `0xFD9A88`，位 `0` |
| 临时改动 | `4831c0c3` = `xor eax,eax; ret`，与参照物写进例程体的 4 字节一致 |

**运行时槽**（4 个 `.themida` 地址）为什么不做字节比对：这些地址在磁盘映像里是零
（`VirtualSize` 远大于 `RawSize`，实体数据由保护器在运行时解出）。案例把它们标成
「只报告读数 + 物理地址」，这样换到真实现场时，日志里就是现场值，而不是假装有一个静态期望。

### 靶场（`WardogFixture.exe`）

真实现场没在跑时，案例引擎可以自己把目标地址空间造出来：`--spawn-target` 让
`WardogFixture.exe` 把案例镜像按首选基址 `0x140000000` 映射进一个独立进程，写完状态文件后保持存活。
它**不执行**任何代码、不加载驱动、不碰内核；只提供一块内容可逐字节比对的内存，用来证明
「读到的确实是那个进程、那个地址」。`WardogFixture.exe` 用 `/BASE:0x180000000 /DYNAMICBASE:NO`
链接，就是为了把 `0x140000000` 让出来。

---

## 4. 运行

需要**管理员**（`SeLoadDriverPrivilege` + 物理内存映射）才能跑 `probe/scan/run/case/read`；
案例的**离线校验**与 `selftest` 不需要管理员。

```bat
:: 0) 构建
build.cmd

:: 1) 案例离线校验：只读磁盘文件与磁盘内核，不加载驱动、不读物理内存
WardogLauncher.exe --mode case --check

:: 2) 案例在线校验（管理员）：加载 PGR → 扫描 → 打开目标地址空间 → 逐条读
WardogLauncher.exe --mode case --spawn-target

:: 3) 自检：加载 PGR、开设备、握手、证明物理内存可读，然后卸干净
WardogLauncher.exe --mode probe

:: 4) 只看判定点定位结果，不写内核内存
WardogLauncher.exe --mode scan

:: 5) 完整链（写入步骤默认关闭，必须显式 --allow-write）
WardogLauncher.exe --mode run --allow-write

:: 6) 日志断言
python tools\verify-run.py wardog-launcher.jsonl --mode run
python tools\verify-case.py build\case-check.jsonl --expect offline
python tools\verify-case.py build\case-run.jsonl   --expect online
```

一步到位的提权脚本（会先清掉上次中断留下的靶场进程与临时服务键）：

```bat
run-case.cmd        :: 右键「以管理员身份运行」，或 Start-Process -Verb RunAs
```

读取真实游戏进程（只读，全程无 `--allow-write`）：

```bat
run-read.cmd        :: 自动定位（内核进程链 → 失败则用户半区头扫描）
run-read-base.cmd   :: run-read-base.cmd <进程名> <基址>
run-read-cr3.cmd    :: run-read-cr3.cmd <进程名> <基址> <CR3>
run-probe.cmd       :: 直通任意只读参数，例如 --mode read --cr3 0x… --va 0x… --no-rpm
```

`--mode read` 的判定口径：镜像头必须与磁盘字节一致（`read_verified_disk`）；受保护程序的代码段
与磁盘不同属于正常（`read_differs_from_disk`），不计为失败；只要有一条证据成立且无矛盾即
`result=success`。

**写入路径默认关闭**：`--mode case`、`--mode scan`、`--mode read` 全程只读；`--mode run` 在没有
`--allow-write` 时会在第一次写之前退出（事件 `write_refused`，退出码 13）。

可选参数：`--case <路径>`、`--spawn-target`、`--pid <N>`、`--fixture <路径>`、`--provider pgr`、
`--pgr <路径>`、`--driver <路径>`、`--patch-rva <RVA>`、`--max-phys <字节>`、`--scan-from <PA>`、
`--base <地址>`、`--cr3 <页表根>`、`--va <地址>`、`--len <字节>`、`--allow-write`、`--dry-run`、
`--no-rpm`、`--no-kernel-route`、`--log <路径>`。

退出码：`0` 成功；`2` 参数/权限/镜像缺失；`3` PGR 加载失败；`4` 物理读写自检失败；
`10` 定位阶段失败；`11` 基线（未打补丁）竟然加载成功（本机未强制校验，结果不可归因）；
`13` 拒绝写入；`14/15/16/17` 后续加载失败 / 驱动未真正运行 / 未恢复 / 恢复后仍被接纳；
`18` 案例文件装载失败；`19` 案例镜像身份不符；`20` 目标进程不存在；
`21` 找不到目标地址空间；`22` 观察点比对失败或读取互相矛盾；`23` 目标进程定位失败。
`30` 覆盖层创建/挂载失败；`31` 没有任何一帧画出图元；`32` ESP 目标进程定位失败；
`33` ESP 配置缺失导致读取不可用。

### 4.1 `--mode esp`：D3D12 人物框叠加层

```bat
run-esp-demo.cmd 8          :: 纯几何演示：不需要管理员、不需要目标进程
run-esp-live.cmd 8          :: 提权：驱动 + 目标定位 + 相机/对象读取 + 覆盖层
```

* 画面来源不是注入，而是**自有 D3D12 设备**：`CreateSwapChainForComposition` +
  `DXGI_ALPHA_MODE_PREMULTIPLIED` + DirectComposition，窗口为
  `WS_EX_NOREDIRECTIONBITMAP | TOPMOST | TRANSPARENT | NOACTIVATE | TOOLWINDOW`。
* 每帧 `DrawInstanced(6, 实例数, 0, 0)`，一个实例就是一个四边形；框由四条线段实例组成
  （与参考 `0xDBB4` 的四边构造呼应），`--esp-corners` 则换成四角括号。
* **缩放**：`width = max(2, 0.46 × height)`、`height ≥ 3`（照参考）、`--esp-scale` 用户倍率
  （对应参考菜单 80%–130% 的预览缩放）、屏幕大小随投影自然变化；进程按
  `PER_MONITOR_AWARE_V2` 运行，覆盖层每帧贴住游戏客户区，DPI 缩放不跑偏。
* 数据源：`--esp-demo` / `--esp-world` / `--esp-actor` / `--esp-vehicle` / `--esp-live`；
  `--esp-live` 依赖 `offsets/wardog-esp.json`。该文件目前是**占位值**：导出资料里没有
  游戏对象布局（`UWorld`/`Actor` 链），只给了参考软件自己的函数地址，所以标定流程写在
  `docs/esp-overlay.md` 第 5 节，用 `--esp-probe VA` 找出 root 槽位后填回来。
* 验证手段：`--esp-report`（上一帧几何 JSON）、`--esp-capture`（从交换链回读 BMP）、
  `tools/verify-esp.py`（比例、1.3 缩放、颜色、屏内断言）、`--mode selftest` 里的 8 项几何断言。
* 热键：`F5` 缩小、`F6` 放大、`F8` 显示/隐藏、`F9` 退出。

细节、公式与实测数据见 [docs/esp-overlay.md](docs/esp-overlay.md)。

---

## 5. 从资料里确认到的、本实现直接使用的事实

### 5.1 中转驱动为什么能先被接纳（照抄结论，不重新论证）

`SECURITY-REPORT.txt` 的三次实机对照已经给出因果：

* 原样本（SHA256 `076cfa7d…f720a4`）→ `NtLoadDriver=0`，设备出现；
* 只破坏**微软嵌套签名**（文件偏移 27879，`EE→EF`）→ `0xC0000428`，设备不出现；
* 只破坏**外层主签名**（文件偏移 20492，`E4→E5`）→ `NtLoadDriver=0`，设备出现。

即当前主机的接纳**依赖嵌套的微软硬件兼容性签名**，而不依赖外层主签名。因此启动器把
「原件」当成唯一的准入凭据：加载前先核对整文件 SHA256，等于 `076cfa7d…`，否则拒绝运行。
`src\main.cpp` 里的 `kPgrSha256` 就是这个钉值。

### 5.2 设备的 IOCTL 契约（从驱动字节恢复）

| 功能 | IOCTL | 说明 |
| --- | --- | --- |
| 握手 | `0x0022218C` | 输入 ≥ `0xC` 字节 `{1, 0x40, 0}`，返回 `{1, 0xC, 1}` |
| 映射物理页 | `0x80102040`（METHOD_BUFFERED，0x28 字节） | 请求 `{size, phys, handle, base, object}` |
| 解除映射 | `0x80102044` | 用上一步返回的 `{handle, base, object}` |
| 端口 IO | `0x80102050` / `0x80102054` | 本工程未使用 |

映射返回的是 `view + (translated − SectionOffset)`，即**指向物理字节 `start` 的指针**；
所以访问 `phys` 的地址是 `returned + (phys − start)`。这一点在早期实现里写成减法，
只有页对齐访问才「看起来正常」，已修正并加入全跨度 `VirtualQuery` 校验。

### 5.3 参照加载器的「读 → 存 → 写 → 加载 → 恢复」形状

1. 读出现场字节（判定点例程体、以及它操作的标志字）；
2. 保存原值；
3. 写入临时值（例程体 `4831c0c3`、标志字 0）；
4. 在窗口期请求加载后续驱动；
5. 恢复原值；
6. 再请求一次加载作为反向对照（应重新被拒）。

本工程的 `RunFull()` 就是这六步，事件名依次为
`control_begin / control_result / original_content_saved / temporary_content_written /
followup_load_begin / followup_load_result / restore_* / control_after_restore`。

---

## 6. 构建细节（没有 WDK 也能出驱动）

`build.cmd` 调用 `E:\Apps\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`，
用 `cl.exe` 编译用户态程序；驱动侧只有 `driver\WardogProbe.c`，用 `build\ntoskrnl.def`
（只列出 `IoCreateDevice` / `IoCreateSymbolicLink` / `IoDeleteDevice` / `IoDeleteSymbolicLink`）
经 `lib.exe` 生成导入库 `ntoskrnl.lib`，再以 `/DRIVER /SUBSYSTEM:NATIVE` 链接成 `.sys`。
这样在没装 WDK 的机器上也能复现构建。

---

## 7. 对照实验（这是本工程能自证的关键）

| 实验 | 期望 | 实测 |
| --- | --- | --- |
| 未打补丁时请求加载载荷驱动 | 被拒 `0xC0000428` | 被拒 |
| 临时改动后请求加载 | `NtLoadDriver=0` 且 `\DosDevices\WardogProbe` 出现 | 待打开 `--allow-write` |
| 恢复后再次请求 | 重新被拒 | 待打开 `--allow-write` |
| 中转驱动哈希不匹配 | 拒绝运行 | 已实现 |
| 物理读越界（MMIO） | 拒绝（RAM 区间判定） | 已实现 |

---

## 8. 已经验证的 / 尚未验证的

已在本机（Windows 10.0.26200，KONG\jiang）验证：

* `--mode selftest` 23 项全部通过（合成页表 + 真实 `ci.dll` 字节 + 8 项 ESP 几何断言），无需管理员。
* `--mode case --check`（无需管理员）：案例 JSON 装载、镜像 SHA256 命中、`image_header` 与
  `entry_point` 逐字节一致、4 个运行时槽被正确识别、判定点导出 RVA `0x787850` 与钉值一致、
  `flag_rva 0xFD9A88` 落在有实体数据的节内。`tools\verify-case.py --expect offline` **14/14 通过**。
* `--mode case --spawn-target`（一次 UAC，只读）实机全链路：PGR `NtLoadDriver=0x0` + 设备出现 +
  握手 `0xC`；RAM 扫描 14.7 s（2298 MiB/s，37783 个候选页）；内核 `cr3=0x1AE000`；
  目标地址空间 `cr3=0x13CD6B000`（57 次尝试命中）；`image_header` 读到 PA `0x7DB5C6000`、
  `entry_point` 读到 PA `0x18620C058`，两条 `match=true`；清理 `pgr_unload=0x0`、服务键已删、
  设备消失。`tools\verify-case.py --expect online` **21/21 通过**。
* `--mode read` 对**真实受保护客户端**（`WardogsClient-Win64-Shipping.exe`，VAC 在场）全自动命中，
  全程只读、无 `--allow-write`（两次复现，退出码 0）：整机 32 GB 扫描 `scan_skipped_windows=0`、
  候选页表 37 万级；真实基址 `0x7FF653000000`（`SizeOfImage=0xE15D000`、`TimeDateStamp=0xCA4563FA`）、
  目标页表根 `CR3=0x29883000`；镜像头读到 PA `0x813AD6000` 且与磁盘 sha256 完全一致；
  入口点读到 PA `0x213522850` 的现场代码 `48 83 EC 28 E8 87 96 00 00`；
  `rpm_ok=false`（该进程拒绝 `ReadProcessMemory`，符合预期）。
* `--mode esp --esp-demo`（无需管理员）97 帧 / 6 目标 / 24 个四边形，交换链回读图像里出现
  **且仅出现**两种颜色（人物 `(110,155,255)`、载具 `(190,160,255)`），几何与 `--esp-report`
  逐框一致（例：`[915.8,454.7,1004.2,646.7]`，宽 88.3 = 0.46 × 高 192.0）；
  `--esp-scale 1.3` 后逐框宽高比恰为 `1.3000`；`tools/verify-esp.py` 判定 `esp_report_ok`。
* `--mode esp --esp-live`（一次 UAC，只读）锚点命中真实游戏窗口（`esp_anchor=game_window`，
  视口 1920×1080），目标基址 `0x7FF653000000`（`user_scan`）、`cr3=0x29883000`，
  6 秒 195 帧、24 个四边形、退出码 0。

尚未验证（需要显式 `--allow-write`，本轮按要求默认关闭）：

* 临时改动窗口内的后续驱动加载（`WardogProbe.sys` 被接纳、`\DosDevices\WardogProbe` 出现），
  以及恢复后的反向对照。代码路径与对照实验都在，只是写入开关未打开。
* 目标机器若开启 HVCI/VBS，物理页会受 SLAT 的 W^X 约束，本方案（改代码页或只读数据页）
  可能被拦下；本机 `VirtualizationBasedSecurityStatus=0`，与资料里 SecureBoot=true、
  CI 选项 `0xC001` 的环境一致。
* ESP 的**目标偏移尚未标定**：`offsets/wardog-esp.json` 是占位值，所以 `--esp-live` 目前只
  出相机链以外的框（`esp_note` 会写明未标定），真实人物/载具框要按 `docs/esp-overlay.md`
  第 5 节填完偏移才会出现。
* 覆盖层是独立窗口：游戏若用**独占全屏**（不是无边框全屏）则看不到叠加上去的内容；
  另外 composition 交换链的内容不在 GDI `BitBlt`/`PrintWindow` 的抓取范围里，
  屏幕级验证只能靠交换链回读。

---

## 9. 与参照软件的差异（不夸大）

* **没有卡密**：不联网、不校验授权、不做界面。启动器只做加载链与案例读取。
* **后续驱动不是 FGTRACK**：换成我们自己的 `WardogProbe.sys`，正是为了证明这是可复用的机制。
  `--driver` 指向什么，`run` 模式就在窗口期加载什么。
* **判定点的选法与参照物一致**（导出名 + `bts` 指令形状），但**不声称**已证明中转驱动首次被
  接纳的根因；资料里那条仍是开放项，本工程不据此推断。
* **运行时槽不做静态断言**：`.themida` 地址的真实值只能由保护器解包后产生，案例只报告现场读数。

---

## 10. 清理与风险

* `--mode cleanup`：删除遗留的临时服务键、卸载仍在内存中的驱动、确认设备消失。
* 每个提权脚本开头都会先 `cleanup`，避免上次中断留下 `WardogLaunch_<pid>_<tick>` 服务键。
* 物理读只走 RAM 区间（`0x1000-0x9F000`、`0x100000-0x80000000`、`0x100000000-0x840000000`），
  越界一律拒绝——盲目遍历物理地址会读到 MMIO 区域导致整机卡死，本工程早期吃过这个亏。
* 载荷驱动是自写未签名驱动，只有在窗口期才会被接纳；失败即中止，不存在「半加载」状态。
