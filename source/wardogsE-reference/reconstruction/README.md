# 路线 B：参考行为的独立模块重建

日期：2026-10-22。分支 `codex/reference-reconstruction`，基于本地 `main` 的
`63cd7de`。仓库暂无远端；没有合入路线 A，也没有更改原有三个源码目录。

第十二轮：来源摘要读回及最多两次尝试接入受控会话，短读/切源/mesh变化等九场景通过；
8个测试程序及透明窗口验收通过。ABA、同地址复用和内容变化仍存在检测限制，见
[SOURCE_CONSISTENCY.md](SOURCE_CONSISTENCY.md)。没有操作现场助手、游戏或驱动。

第十一轮历史：动态标识映射、384槽映射缓存和192槽姿态缓存接入受控会话及透明窗口。
冷/热帧、换数组、换上下文、停止/重启和失败清屏测试通过；真实数据及参考完整卸载
仍未验证。见 [DYNAMIC_MAPPING_CACHE.md](DYNAMIC_MAPPING_CACHE.md)。

第十轮历史：新增无底板实际桌面复测，约98.29%的稳定桌面区域Alpha合成通过；动态区域
明确排除。修复全屏尺寸覆盖窗口触发Shell任务栏退后的问题。详见下方集成说明。

第九轮历史：新增受控姿态选择/后处理、参考投影和独立透明置顶窗口 `overlay.exe`。
严格编译、姿态/投影测试、字段/规则回归与三轮窗口自测通过；透明背景、线条合成及
鼠标目标穿透已有桌面验证。动态映射发现、实体缓存存储和真实游戏仍未完成。
见 [CONTROLLED_INTEGRATION.md](CONTROLLED_INTEGRATION.md)，记录具体证据和限制。

第八轮历史：新增参考位置短路选择、变换有效性、骨描述/三样本接受规则及共同基址选择。
严格编译、新增五组规则测试、上轮五组字段测试回归均通过；不是完整姿态或真实读取。
模块测试无需等待驱动签名，真实端到端仍依赖加载/通信与完整数据集成。
见 [REFERENCE_RULES.md](REFERENCE_RULES.md)，收据为独立输出目录 `phase8-verification.json`。

第七轮历史：以用户人工验证可运行的参考软件为重建依据，保留旧代码作对照。新增两来源
原始字段观察器，位置、关联、骨数组描述及容器路径的五组受控测试通过；没有接入
真实游戏读取或实现完整选择算法。范围缓存轮次清空及上下文变化清缓存已有新静态
证据，完整停止/卸载仍未闭合。见 [READ_PATHS.md](READ_PATHS.md) 和数据契约。
本轮严格编译通过，收据为独立构建根目录 `phase7-verification.json`。

第六轮历史：自测窗口已增加不激活样式及显示/缩放/恢复的前台断言，首次取消自有
定时器必须成功；USER总数变化保留诊断，不单独据此判定泄漏。最终无仪器化三轮
通过，GDI/USER均为2/2。完整诊断和历史未知见 [WINDOW_DIAGNOSTICS.md](WINDOW_DIAGNOSTICS.md)。
结构、范围缓存、有效性与双路径方向见 [REFERENCE_DATA_CONTRACT.md](REFERENCE_DATA_CONTRACT.md)。
第六轮构建与窗口日志位于独立构建根目录的 `phase6-*`；收据为 `phase6-verification.json`。
下方第五轮测试数字保留为历史记录，不覆盖第六轮结果。

## 当前可运行范围

| 模块 | 实现 | 验证 | 尚未完成 |
|---|---|---|---|
| M01 二维渲染 | 原图元及诊断窗口；新增透明、无标题栏、置顶、鼠标穿透覆盖窗口 | 编译、桌面Alpha合成抽样、鼠标目标查询、三轮关开与缩放 | 原程序 D3D12 后端、字体/视觉一致性、性能、游戏集成 |
| M02 快照交接 | 同锁发布/复制；消费者取锁失败保留旧帧 | 确定性锁竞争；首次中间帧握手；50000 次发布压力测试 | 参考二进制布局、双缓冲调度、完整启动/停止/重启/心跳管理 |
| M03 配置菜单 | 38 槽解码、分组覆盖与限制；本地文件导出/读回；六个显示快捷键 | 解析测试、文件往返、颜色进入画面、无效文件保留原状态 | 未知槽语义、原菜单 UI/写入端、参考保存行为、完整字段视觉接入 |
| M04 数据适配 | 两来源字段、参考规则、固定/动态映射、两类缓存、来源摘要读回、姿态/投影接入受控透明窗口 | 8个测试程序、短读/切源/ABA负例、停止重启与无底板窗口 | 实体发现、乘员/车辆、真实读取时序与并发、实际上下文事件、参考完整停止卸载 |
| M05 进程诊断接口 | 本路线未实现 | 无内核验收 | 与路线 A 对照后另行决定，不能把 A 的测试算作 B 成果 |
| M06 交付与恢复 | 本路线未实现 | 只有模块构建脚本 | 完整部署、授权服务、资源、失败清理 |

这不是可替代原助手的应用。合成人形连线和雷达式布局只用于图元组合测试，
没有实体发现、骨骼采集、自动瞄准、模型 Chams、驱动通信或注入实现。

## 构建与复现

从该 worktree 根目录，用 PowerShell：

```powershell
& .\reconstruction\build.cmd E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\out
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\out\render2d_test.exe E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\out\fixture.png
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\out\snapshot_test.exe
```

使用现有 Visual Studio 2022、Windows SDK 和系统字体，无新增依赖下载。
`build.cmd` 可从已经初始化的 x64 MSVC 提示符运行，否则使用本机现有 VS 路径。
指定输出目录失败会返回非零，不在调用者目录继续编译。

输出、测试日志与 `verification.json` 位于上述独立 `reconstruction` 构建目录，
不会写入路线 A 的 `out`、`regression` 或 `interfaces-*` 文件。

第五轮输出目录为同级 `phase5-out`。构建同时生成 `menu_config_test.exe`、
`preview_scene_test.exe` 和 `preview.exe`。前两项直接执行即可；交互预览：

```powershell
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase5-out\preview.exe
# 自动验证会创建并关闭三轮可见窗口，截图和测试配置写入指定目录。
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase5-out\preview.exe --self-test E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase5-window
```

B/S/H/N/D/R 分别切换方框、连线、血条、名称、距离和雷达，空格暂停/继续，
F5 读取、F6 导出、Esc 关闭。默认配置是 EXE 同目录的 `preview-menu.cfg`；
可用 `--config <路径>` 指定。预览不会读写参考程序的 AppData 配置。

## API 与实现选择

`DrawList` 接受屏幕像素坐标和独立 RGBA 浮点分量，96 DPI；文本尺寸为像素，
`Canvas::pixels()` 返回自顶向下预乘 BGRA。调用者初始化 COM，Canvas 只访问自建
位图。矩形使用左上/右下坐标；第九轮 `reference_projection` 已提供受控世界坐标投影。

当前后端选择 CPU 离屏绘制以独立验证图元，不复刻参考 DLL 的 D3D12 实例化提交、
64 字节记录、8192 项缓冲上限或字体图集。绘制顺序保留，文字由系统字体渲染；
没有声称与参考着色器、字距、抗锯齿或中文字体效果相同。每帧创建文字格式，
尚未做缓存与性能优化。

`Snapshot<Frame>` 用互斥锁表达已观察到的发布/复制关系，允许调用者持有旧帧。
`Frame` 自身应拥有被复制的数据；该模板不保证外部指针内容一致。赋值抛出异常时
锁会释放，但不承诺目标强异常保证。没有增加心跳超时、自动重试或停止门禁。

`MenuConfig` 按保存伪代码中的格式解析最多 511 字节、38 个整数，至少 10 项，
按参考分组阈值应用字段；缺省值来自保存函数的模板，未证明等于运行初始全局值。
整数溢出在本地定义为 32 位回绕，尚未核实参考机器码在溢出输入下的等效性。
本地导出全部当前字段；参考保存函数主要合并旧文件并更新雷达位置，二者不同。
预览只消费槽 1、4–6、10–16、19–23、25；字体比例、距离过滤、状态颜色等未接入。
`PreviewFrame` 是人工数据结构，不兼容参考二进制布局。定时器与绘制在同一 UI
线程，窗口测试不能证明真实采集线程的同步行为。

## 验证证据边界

- 像素测试覆盖一个越界矩形、50% 红覆盖不透明蓝、线宽内外采样、透明清屏、
  非空文字像素与布局裁剪、空心圆采样及 48 段闭合；不是全分辨率/字体测试。
- `fixture.png` 已目视检查：合成线条、方框、文字、填充条和点位存在，未发现截断；
  这不证明参考程序视觉一致性。
- 快照竞争测试用测试负载在复制内部持锁，保证另一消费者遇到竞争；压力测试以
  握手保证至少一次非最终帧读取，检查各字段与序列号一致和最终发布可见。
  不是穷尽所有调度的证明，也未做吞吐或延迟承诺。
- 新代码经过逐文件二次阅读及独立审查；已修正输出目录失败继续构建的问题，
  并补上压力测试的中间帧覆盖。只有自建测试程序被运行。
- 独立窗口验证三轮关闭/重开，抽样比较窗口 DC 和离屏缓冲，覆盖六个显示开关、
  配置读回、640×480/1280×720、最小化恢复、暂停恢复及真实 Windows 定时器。
  最终三轮自有窗口/Canvas 均释放，GDI/USER 计数均为 9/7。计数仅能判断
  这些对象在三轮后的增长情况，不能证明所有资源无泄漏。最终结果见
  `phase5-verification.json` 和对应日志。
- 首次新增定时器测试序号断言失败，日志不足确定根因；后续调试运行定时器通过，
  但 USER 计数 6/6/7 未归因。两份失败日志保留。最终拆分生产/消费状态并加强
  两阶段断言后通过；这不证明已定位早期全部不稳定原因。
- 极端雷达尺寸配置允许覆盖其他合成内容；没有做全分辨率布局或参考画面对齐验收。

## 参考证据与剩余工作

分析材料位于：
`E:/CodexData/.codex/tmp/wardogs-review-build/reference/kernel-backend-export/`。

- `BACKEND_COMPARISON.md`：完整功能范围、mesh.bin 依赖、配置索引与未知项。
- `lifecycle-notes-20261022.md`：静态发布/消费和生命周期；以文末新增证据修正为准。
- `CONFIG_BEHAVIOR_AUDIT.md`：配置语法、分组更新、默认模板、读写差异及未知项。
- `ADDRESS_LAYOUT_AUDIT.md`：地址类型、指针槽、内部缓存、字形和图元布局边界。
- `PHASE6_STRUCTURE_LIFECYCLE.md`：快照分区、96→80字节变换、失败输出契约、范围缓存及新增静态校验规则。
- `PHASE7_LIFECYCLE_CACHE.md`：停止/卸载资源矩阵、范围缓存和上下文缓存失效；修正早期未找到清空路径的状态。
- `phase8-analysis/`、`phase8-evidence-20261022.json`：位置接受谓词、距离固定迭代及常量；累计139条证据。
- `phase5-analysis/`、`phase5-evidence-20261022.json`：本轮资源与范围检查的静态证据。
- `render-analysis/0x18000ccac.pseudo.c.txt`：48 段圆。
- `feature-gap-analysis/0x18000b750.asm.txt`、`0x18000c948.asm.txt`：消费尝试取锁及发布顺序。
- `lifecycle-analysis/`、`lifecycle-evidence-20261022.json`：入口汇编、初始化分配、线程空入口及引用。

参考样本 SHA256：`617bda70d67dee7ba1018a290a86929538e1a2cd67c29329c42dc8e6fa18817e`。
目前所查二维链没有旧 `mesh.bin` 依赖；等效三维模型效果仍未确认。
下一步优先解决配置未知字段、成功初始化后的资源销毁与参考画面逐项对照，
再扩大模块实现。完整功能复原和真实游戏验收仍未完成。
