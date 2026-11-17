# D3D12 人物框叠加层：实现、缩放与标定

本文说明 `WardogLauncher.exe --mode esp` 这一支的实现：用一个**自己的 D3D12 设备**在游戏窗口之上画目标框，并按投影结果缩放。全部读取走已有的物理内存通道（PGR → 分页遍历），**不写目标进程内存**，也不向游戏的交换链注入任何代码。

## 1. 与参考软件的差别

参考实现（`fgnew.dll` 的 `FgFrame`，RVA `0x12EE0`）做的是：从宿主那里拿到**游戏自己的交换链**，在游戏线程里用 `DrawInstanced(6, 记录数, 0, 0)` 把四边形画进游戏后缓冲。这要求模块先被放进游戏进程，并改掉呈现入口。

本工程走的是另一条路，原因是我们不做注入：

| 项目 | 参考软件 | 本工程 |
|---|---|---|
| 渲染位置 | 游戏交换链（进程内） | 独立覆盖窗口（进程外） |
| 窗口 | 游戏窗口本身 | `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition |
| 呈现 | 随游戏帧 | 自己的 `IDXGISwapChain1`（composition，premultiplied alpha） |
| 数据来源 | 进程内采集缓冲 | 物理内存读取 + 偏移配置 |
| 每框图元 | 4 条线段记录 + 实例化 6 顶点 | 4 条线段四边形 + 实例化 6 顶点（同为 6 顶点/实例） |

因此：几何与配色可以对齐参考，呈现机制必然是两套。

## 2. 文件与职责

| 文件 | 职责 |
|---|---|
| `src/math3d.h` | 相机基向量与世界→屏幕投影（UE 角度约定：pitch/yaw/roll，度） |
| `src/draw_list.h` | 屏幕矩形/线段/框/四角括号 → 实例化四边形列表 |
| `src/esp_projection.h/.cpp` | 方框几何：投影两端点 → 高度、宽度、最小尺寸、缩放 |
| `src/esp_scene.h/.cpp` | 目标列表 → 绘制列表，含距离上限、屏外剔除、框数预算 |
| `src/esp_live.h/.cpp` | 偏移配置读取、相机 POV 读取、Actor 位置读取、`--esp-probe` 标定 |
| `src/overlay_d3d12.h/.cpp` | 设备/队列/交换链/DComp/PSO/实例缓冲/呈现/回读截图 |
| `src/main.cpp` | `--mode esp` 参数、循环、热键、报告输出 |

## 3. D3D12 链路

```
CreateDXGIFactory2
  └ EnumAdapterByGpuPreference(HIGH_PERFORMANCE)      → RTX 5060
     └ D3D12CreateDevice(11_0)
        ├ CreateCommandQueue(DIRECT) / Allocator / CommandList
        ├ CreateFence + event
        ├ CreateDescriptorHeap(RTV, 2)
        └ CreateSwapChainForComposition(queue, FLIP_SEQUENTIAL,
                                        B8G8R8A8_UNORM, ALPHA_MODE_PREMULTIPLIED)
           └ D3D11CreateDevice(同一 adapter) → IDXGIDevice
              └ DCompositionCreateDevice → CreateTargetForHwnd(hwnd, topmost)
                 └ CreateVisual → SetContent(swapchain) → SetRoot → Commit
```

每帧：

```
WaitForGpu → Reset(allocator) → Reset(cmdlist)
  OMSetRenderTargets(RTV[i]) → ClearRenderTargetView(0,0,0,0)
  RSSetViewports / RSSetScissorRects
  SetGraphicsRootSignature / SetPipelineState
  SetGraphicsRoot32BitConstants(0, 2, {视口宽,视口高})
  IASetVertexBuffers(0, 单位四边形 6 顶点) / (1, 实例缓冲)
  DrawInstanced(6, N, 0, 0)                    ← 与参考一致的 6 顶点/实例
  Close → ExecuteCommandLists → Present(1) → DwmFlush → WaitForGpu
```

要点：

- **per-pixel 透明**只能靠 composition 交换链 + `DXGI_ALPHA_MODE_PREMULTIPLIED`；hwnd 交换链不支持 alpha，`LWA_COLORKEY` 也不作用于 flip 模型，所以没有走色键那条路。
- 混合状态为 `SrcBlend=ONE / DestBlend=INV_SRC_ALPHA`，着色器把颜色预乘 alpha 输出，符合 premultiplied 约定。
- 顶点着色器把单位四边形先按 `(半宽,半高)` 拉伸，再按角度旋转 —— 顺序反了会把竖线压成横线（这是本次实际踩到的第一个渲染 bug）。
- 实例步进 40 字节（center/half/angle/pad/color 共 10 个 float）；步进写成 32 会相邻实例互相覆盖，表现为只剩一条线。

## 4. 缩放：四条互相独立的规则

| 规则 | 取值 | 说明 |
|---|---|---|
| 框宽比例 | `width = max(2, 0.46 * height)` | 与参考 `0xDBB4` 的 `max(2, 0.46*框高)` 一致 |
| 最小框高 | `height >= 3` | 参考同处的最小 3 像素 |
| 用户缩放 | `--esp-scale 0.2 .. 4.0`（默认 1.0） | 对应参考菜单的 `espScale` 80%–130% 预览缩放；这里作用于实际框 |
| 透视 | 由投影自然产生 | 近大远小，不做人为插值 |

投影（`math3d.h`）：

```
forward = (cosP·cosY, cosP·sinY, sinP)
right0  = (-sinY, cosY, 0)
up0     = (-sinP·cosY, -sinP·sinY, cosP)
roll：right = right0·cosR + up0·sinR ；up = up0·cosR - right0·sinR
depth   = dot(rel, forward)               ；depth <= 1 视为无效
ndcX    = across / (depth·tanH)           ；ndcY = upward / (depth·tanV)
屏幕    = ((ndcX+1)/2·W, (1-ndcY)/2·H)
```

`tanH/tanV` 由 `fov` 与 `fov_horizontal` 决定：UE 的 `FOVAngle` 是水平视场角，所以默认 `tanH = tan(fov/2)`、`tanV = tanH / aspect`。

框几何（`esp_projection.cpp`）：先分别投影头顶与脚下，`height = |feet.y - head.y|`，中心取两端点中点；只有一端可见时按 `nominal_height_cm` 与「每厘米像素数」估高（`PixelsPerCentimeter`）。两端都不可见则跳过。

DPI 与窗口对齐：进程启动时设 `PER_MONITOR_AWARE_V2`，每帧用游戏窗口 `GetClientRect` + `ClientToScreen` 把覆盖层摆到客户区，并按其像素尺寸重建交换链；投影用的 `camera.width/height` 直接取覆盖层尺寸，所以 100%/125%/150% 缩放下框与画面仍对齐。

## 5. 偏移配置：为什么现在还没有真实人物地址

参考资料里能定位的是**软件自己**的地址（`fgnew.dll` 的绘制函数 RVA、`FgGear` 的 UI 资源），以及 PGR/`linklook`/`FGTRACK` 的加载链；**没有**游戏对象布局（`UWorld`/`APlayerController`/`AActor` 链）与人物、载具的对象地址。因此本工程把这条链做成可配置项，而不是写死猜测值。

配置文件：`offsets/wardog-esp.json`

```json
{
  "world_slot": "0x0",
  "owning_game_instance": "0x0",
  "local_players": "0x0",
  "local_player_controller": "0x0",
  "player_camera_manager": "0x0",
  "camera_cache": "0x0",
  "pov_location": "0x10",
  "pov_rotation": "0x1C",
  "pov_fov": "0x28",
  "world_levels": "0x0",
  "persistent_level": "0x0",
  "level_actors": "0x0",
  "actor_root_component": "0x0",
  "component_location": "0x0",
  "character_height_cm": 180,
  "vehicle_height_cm": 300
}
```

- 全为 `0x0` 表示未标定：`--esp-live` 会照常跑，但只在日志里给出 `esp_note`，不出框。
- `world_slot` 是**相对映像基址**的 RVA；其余是结构体内偏移。
- `pov_location/rotation/fov` 是相对 `camera_cache` 的偏移（UE5 的 `FCameraCacheEntry` 头 0x10 是 double 时间戳，POV 从 0x10 开始）。

标定流程：

1. `--esp-probe VA`：扫描 `VA+0x0..0x800`，找出「像指针、且指针+`component_location` 处是有限浮点」的槽位，打印候选 `root_slot`、`root`、`location`。
2. 把命中的 `root_slot` 填进 `actor_root_component`，位置偏移填进 `component_location`。
3. 相机链用同样办法从 `world_slot` 逐级验证；`--esp-live` 起来后看 `esp_report` 里 `source=live` 与 `boxes` 是否随视角变化。

## 6. 命令

```bat
:: 纯几何演示，不需要管理员、不需要目标进程
run-esp-demo.cmd 8 1.0

:: 真实链路：驱动 + 目标解析 + 相机/对象读取
run-esp-live.cmd 8
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--esp-demo` | 固定合成场景（6 个目标） |
| `--esp-camera X,Y,Z,PITCH,YAW[,FOV]` | 手工给相机，跳过内存读取 |
| `--esp-world X,Y,Z[,HEIGHT][,1]` | 追加一个世界坐标点，末尾 1 表示载具配色 |
| `--esp-actor VA` / `--esp-vehicle VA` | 按 Actor 地址出框（读 root + location） |
| `--esp-live` | 按配置扫描 Actor 列表 |
| `--esp-probe VA` | 标定用：打印候选 root 槽位 |
| `--esp-scale F` / `--esp-thickness F` | 缩放 / 线宽（像素） |
| `--esp-corners` | 画四角括号代替整框 |
| `--esp-seconds F` | 定时退出（默认按 F9 退出） |
| `--esp-report PATH` | 输出上一帧几何为 JSON |
| `--esp-capture PATH` | 把渲染结果从交换链回读成 BMP |
| `--esp-vsync` | 每帧等合成器（`DwmFlush`） |

运行时热键：`F5` 缩小、`F6` 放大、`F8` 隐藏/显示、`F9` 退出。

## 7. 实测（2026-10-27，1920×1080，RTX 5060）

演示场景（相机 `(0,0,100)`，yaw 0，fov 90，1920×1080，`--esp-scale 1.0`）：

| 目标 | 序列化框 `[left,top,right,bottom]` | 宽×高 |
|---|---|---|
| 人 #1（600,−300） | `[413.8, 412.0, 546.2, 700.0]` | 132.5 × 288.0 |
| 人 #2（900,0） | `[915.8, 454.7, 1004.2, 646.7]` | 88.3 × 192.0 |
| 人 #3（1400,350） | `[1171.6, 485.1, 1228.4, 608.6]` | 56.8 × 123.4 |
| 人 #4（2000,−500） | `[700.1, 501.6, 739.9, 588.0]` | 39.7 × 86.4 |
| 人 #5（2600,120） | `[989.0, 510.5, 1019.6, 576.9]` | 30.6 × 66.5 |
| 载具（1100,700） | `[1510.7, 365.5, 1631.1, 627.3]` | 120.4 × 261.8 |

- `--esp-scale 1.3` 后逐框宽高比恰为 `1.3000`，屏幕非黑包围盒由 `x 413..1631` 变为 `393..1649`。
- 回读截图里颜色只有两种：人物 `(110,155,255)`、载具 `(190,160,255)`，`esp_report_ok`。
- 提权真实链路：目标基址 `0x7FF653000000`（`user_scan` 定位），`cr3 0x29883000`，覆盖层锚点 `game_window`，视口 1920×1080，6 秒 195 帧、24 个四边形、`exit 0`。
- `--mode selftest` 新增 8 项几何断言（参考框、0.46 比例、1.3 缩放、最小尺寸、背后剔除、两框 8 四边形、四角 16 四边形、框数预算），`failures=0`。

## 8. 已知限制

1. **没有游戏对象偏移**：`offsets/wardog-esp.json` 目前是占位值，`--esp-live` 不出框是预期结果，要先按第 5 节标定。
2. **独占全屏**：覆盖层是独立窗口，游戏若用独占全屏（而非无边框全屏）就看不到；无边框/窗口化正常。
3. **屏幕截图抓不到**：composition 交换链的内容不在 GDI `BitBlt`/`PrintWindow` 里，所以验证用的是交换链回读（`--esp-capture`）；`DwmGetWindowAttribute(DWMWA_CLOAKED)=0` 说明窗口确实被合成，未被合成器隐藏。
4. **只有框**：还没有文字、骨骼、血条、雷达；`esp_scene` 已按「四边形列表」抽象，加文字需要字体图集与另一条管线。
5. **不做瞄准**：没有任何写入目标内存的路径，`--allow-write` 与本模式无关。
6. **框只按原点 + 固定身高**：没有骨点，远端目标的高度是估计值（`nominal_height_cm`）。

## 9. 与其它文档的关系

- 驱动准入与读取链路见 [原理-驱动准入与内存读取.md](原理-驱动准入与内存读取.md)。
- 目标解析的四条路径（`--base`/EPROCESS/`--cr3`/用户半区扫描）见 `README.md` 第 2 节。
