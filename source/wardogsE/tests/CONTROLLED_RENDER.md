# 受控内存读取与透明悬浮绘制验证 — 2026-10-23

## 交付形态与结论

最终软件的主绘制层必须**透明、无边框、置顶悬浮**，只有独立设置页面可以有边框。正式入口 `2_src_cheat/wd_esp_full.c` 已使用 `WS_POPUP | WS_VISIBLE`、`WS_EX_TOPMOST` 和 DirectComposition；本轮未修改正式入口、驱动或签名。

本轮增加独立受控测试程序。`--preview` 和 `--self-test` 均默认透明悬浮；`--overlay-preview` / `--overlay-test` 为同模式别名。`--diagnostic-preview` / `--diagnostic-test` 是显式选择的深色带框诊断工具，不是最终软件的主绘制层，也不作为透明悬浮验收结果。

**受控子进程 → 真实用户态内存读取 → 原有投影/绘制函数 → 实际 GPU 像素已跑通。**透明模式六个场景通过，另有六个诊断场景通过。不能据此宣称真实游戏、真实内核接口或正式覆盖层已完整验收。

## 实现范围

- `build_controlled_render.py` 在独立输出目录生成测试副本。仅替换读取适配器和启动入口，并提供私有菜单配置；不会改写生产文件。驱动打开/基址/写入路径在测试副本中被替换为中止函数，避免误走未测路径。
- 子进程分配自己的场景内存，通过管道只交换命令、地址、长度和校验值。父进程使用 `ReadProcessMemory` 读取该地址，核对序号和整个快照的校验值。场景字节不通过管道传输。
- 复用生产 `ENT`、`rot_matrix`、`w2s_a`、`project_all`、`DrawOverlay`。构建脚本逐字比较四个函数并记录 SHA-256。`gpu.cpp` 保持原内容，只在生成副本末尾追加读回探针。
- 在实际 D3D11/Direct2D 交换链上绘制；探针检查 `EndDraw`，从 GPU 后备缓冲复制到 staging texture 后读回像素，再调用原 `gpu_present`。
- 二次复查加强了血量像素断言，检查血条中段在 90% 时着色、35% 时留空；随后补齐透明默认模式、实际窗口样式和透明 Alpha 断言。

## 实测记录

本机 x64 MSVC 14.44 / Windows SDK 10.0.26100.0 编译链接退出码为 0；保留原源码的编译警告，未扩展成告警清理。二次阅读了生成器、GPU 读回及测试源码，生产源码无改动。

| 检查 | 结果及证据边界 |
|---|---|
| 真实跨进程读取 | PASS：父子 PID 不同；每轮六场景合计 14 次读取、34952 字节。包含原绘制函数对受控 pawn 数据的额外读取。 |
| 正常画面 | PASS：三个构造实体中两个在前方；投影坐标与独立公式误差小于 0.01 像素，头部/骨架/血条区域出现实际 GPU 像素；后方实体被剔除。 |
| 位置与血量变化 | PASS：人物 A 移动超过 100 像素，旧位置不残留；血条中段区分 90% 和 35%。 |
| 相机变化 | PASS：构造相机平移后投影按预期变化。只覆盖零旋转、90 度 FOV。 |
| 不可读页面 | PASS：子进程实际设置自己的页面为 `PAGE_NOACCESS`；读取失败，错误 299，实体区清空。不是返回假的错误码。 |
| 恢复 | PASS：子进程恢复页保护，重新读取并显示。 |
| 源进程退出 | PASS：子进程正常退出后再次读取失败，实体区清空，父进程正常退出；复查无遗留测试进程。 |
| 透明悬浮样式 | PASS：窗口创建后读回确认可见、`WS_POPUP`、无 `WS_CAPTION/WS_THICKFRAME`、有 `WS_EX_TOPMOST`。 |
| GPU 透明度 | PASS：指定空白像素 Alpha 为 0，且各场景超过 90% 的画面像素 Alpha 为 0；不依靠黑色色键或伪造桌面背景。 |
| 普通诊断窗口视觉 | 已观察人物、骨架、血条、位移、相机变化、清屏及恢复。它不能替代透明悬浮的视觉验收。 |
| 透明预览运行 | 15 秒自动结束；最后一次记录 318 帧、954 次真实读取、2778684 字节，子进程退出正常。此为运行记录，不是性能基准或逐帧 GPU 断言。 |
| 透明悬浮桌面视觉 | **未完成最终核对**：前两次 Computer Use 被用户 Escape 中止；最后一次窗口枚举未返回该工具窗口，之后用户明确要求不再打开。不能把 GPU Alpha / 窗口样式检查写成已目视确认桌面透出。 |

## 模拟、替换与未完成项

**构造数据**：人物名称、位置、骨骼、血量、相机、可见性字段及 pawn 占位内存。子进程按命令更新后保持快照稳定；没有模拟一个真实游戏的所有对象和并发行为。

**替换路径**：用真实 `ReadProcessMemory` 代替驱动 IOCTL；直接读取已解码的 `ENT` 快照和相机，未执行原游戏扫描/指针链/骨骼解码的 `ReadThread`。菜单使用私有固定配置，没有启动真实 WebView2 菜单。失败清空旧快照是测试适配器的行为，不能据此证明正式读取线程具备相同恢复语义。

**仍未完成**：

1. 真实内核加载/卸载/重载、PEB/批量读取等 IOCTL 的内核实测；签名、安装条件维持此前状态。
2. 真实游戏地址链、数据版本兼容、实时骨骼同步、实体发现、生产线程并发及完整端到端验收。
3. 正式覆盖层在桌面的透明合成、点击穿透、焦点不抢占、与游戏窗口相对位置、多屏/DPI、全屏兼容。测试窗口只有固定 1000×650 区域，且不完全复用正式窗口初始化：正式窗口还包含 `WS_EX_LAYERED` / `SetLayeredWindowAttributes`。设置了 `WS_EX_TRANSPARENT` 不能单独证明跨进程点击穿透。
4. WebView2 设置页、Chams、车辆、弹道、缺失模型资源、性能压力及长期资源稳定性。编译 `chams.c` 不代表运行过 Chams。
5. GPU 读回发生在 Present 之前；没有断言 DWM 最终桌面合成结果，也没有为每帧检验 Present 返回值。已观察的普通诊断窗口显示只能覆盖该模式。

## 重现方式

在 Windows x64 MSVC 开发者命令行中（需要 Python 3、Windows SDK，输出目录放仓库外）：

```bat
python tests\build_controlled_render.py --out E:\wardogs-controlled-build
mkdir E:\wardogs-controlled-test
E:\wardogs-controlled-build\controlled_render.exe --self-test E:\wardogs-controlled-test
```

这是透明、无边框、置顶的六场景测试，约三秒自动结束；日志为 `self-test.log`，成功时进程退出码 0，输出六张真实 GPU 后备缓冲 BMP。检查日志末尾 `transparent=1 kernel_tested=0`。BMP 保存原始 BGRA 数据，一些查看器会忽略 Alpha；不能根据查看器显示黑底推断程序窗口不透明。

需要观察动态受控数据时，使用 `--preview <已存在的独立输出目录>`，按不激活窗口方式显示，15 秒自动退出。只有显式 `--diagnostic-preview` 才有深色普通窗口和数字键控制；它是诊断工具，不是交付效果。

## 保存的证据

随 Git 保存：[evidence/controlled-render-20261023](evidence/controlled-render-20261023) 下的透明/诊断测试日志、最后一次预览日志、源函数清单和文件哈希记录。该记录对应本轮 worktree 构建；main 合并后只重新编译并对照生成源码，不再次打开窗口，遵循用户要求。

完整本机产物位于 `E:\CodexData\.codex\tmp\wardogs-controlled-render-20261023`：`build-final.log`、`out/`、`accepted-self-test/`、`accepted-diagnostic-test/`、`accepted-preview/`。原始 BMP 与 EXE 留在仓库外，不随 Git 自动迁移。main 重编译脚本及结果使用同目录的 `build-main.cmd`、`build-main.log`、`main-out/`。

另一个会话的 `codex/reference-reconstruction` 未合并、未改动；本轮也没有继续签名分析或发出最终签名结论。
