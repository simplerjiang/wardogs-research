# 第九轮：姿态、投影与透明覆盖窗口

2026-10-22，Asia/Singapore。当前只验证自有字节数组和自建桌面窗口，不需要驱动签名。
旧项目与 `preview.exe` 保留；新增的 `overlay.exe` 没有标题栏、背景面板或菜单。
本轮没有联系路线 A、操作共享 REA 会话、运行参考程序/游戏或改变系统配置。

第十二轮来源一致性和故障窗口验证见 [SOURCE_CONSISTENCY.md](SOURCE_CONSISTENCY.md)。
第十一轮已补上受控动态映射、两类缓存及本地停止/重启，窗口消费该会话输出。
当前结果和真实数据边界见 [DYNAMIC_MAPPING_CACHE.md](DYNAMIC_MAPPING_CACHE.md)。
下方第九/十轮数据均保留为历史记录，不覆盖第十一轮验证。

## 第十轮：无底板的实际桌面复测

用户指出此前弹出的画面像不透明窗口。第九轮 `--self-test` 确实会创建深蓝色的下层
测试窗口，而普通启动没有该底板。新增 `--desktop-test` 直接验证当前桌面，截图中
只有绿色线条覆盖原有内容。正常运行仍无标题栏、无背景面板。

发现并修复另一实际问题：屏幕大小的窗口被Shell当作全屏内容，导致任务栏退到其他
窗口后面。创建时设置 `NonRudeHWND`，销毁时移除；仅作用于自有HWND，无注册表或系统
配置改动。行为依据为[微软文档](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist2-markfullscreenwindow)。

构建与证据仍在上述独立构建根目录，新增 `phase10-*`，没有覆盖第九轮结果：

- `phase10-build-sampled.log`：MSVC严格编译通过。新代码经过逐行二读。
- `phase10-desktop-sampled.log`：1920×1080，无底板，退出码0。2,035,664个透明像素、
  1,255个实心线条像素及1,312个半透明边缘像素全部符合实际桌面合成结果（通道容差2）。
- 另有35,369个像素（约1.71%）处于动态内容及其32像素邻域，不纳入此轮像素验收。
  总计验证约98.29%屏幕；不能把动态区域描述为已验证。
- 显示覆盖层前后，线条位置的WindowFromPoint结果未变；前台窗口未变。没有模拟物理点击。
- `phase10-desktop-sampled/desktop-visible.png` 已目视确认：原有内容可见，没有不透明底板，
  任务栏正常。程序展示该帧3秒后自动关闭，不保留后台窗口。
- 初始 `phase10-desktop.log`、`phase10-desktop-settled.log` 失败记录保留；它们暴露了
  任务栏变化。修复后 `phase10-desktop-fixed.log` 尚有998个动态像素差异，随后一次邻域
  检查仍有60个周期动画像素差异；`phase10-desktop-final.log` 同样保留为失败。
- 最终测试用两个非等间隔的隐藏窗口基线和一个关闭后的基线识别动态区域，避免周期
  动画在首尾恰好同相造成误判。没有扩大颜色容差或忽略稳定区域的差异。

```powershell
# 无底板实际桌面自测；完成后自动关闭
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase10-out\overlay.exe --desktop-test E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase10-desktop-sampled
# 正常透明覆盖窗口；Ctrl+Alt+F10关闭
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase10-out\overlay.exe
```

没有重测真实游戏、独占全屏、多显示器或HDR。下面的第九轮记录保留为历史证据。

## 当前结果

| 部分 | 已实现及本轮验证 | 未覆盖 |
|---|---|---|
| 最终姿态 | 四张固定映射表、缓存提示/首选/一般三阶段选择、两种变换模式、评分与后处理；三组受控测试通过 | 动态名字映射发现、192槽实体缓存存储、外部状态读写 |
| 投影 | 参考三角多项式、相机轴、FOV/宽高比修正、近面与线长条件、缺点连接及腿部显示修复；三组测试通过 | 极端角度的机器异常行为、与参考运行结果逐帧差分 |
| 集成 | 自有字节空间 → 位置/骨候选规则 → 两个骨数组描述 → 姿态选择 → 投影 → 快照 → 绘制 | 当前mesh地址由fixture提供，未通过本场景验证pawn→mesh寻址链；没有真实游戏采集 |
| 窗口 | 置顶、无标题栏、不激活、透明背景、鼠标命中穿透；七场景×三轮通过 | 独占全屏、HDR、多屏/不同DPI、真实游戏鼠标输入验收 |

`selected` 表示曾选中候选，后处理仍可能清空mask；`postprocess_complete` 仅表示
当前已物化数据的后处理执行完成，不能据此称整个参考程序已恢复。

## 透明与鼠标穿透

覆盖窗口采用 `WS_POPUP`，扩展样式为 `WS_EX_LAYERED | WS_EX_TRANSPARENT |
WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`。每帧用RGBA零值清屏，
将预乘BGRA位图通过 `UpdateLayeredWindow(ULW_ALPHA)` 呈现。只有线条产生非零Alpha。
`WM_NCHITTEST` 返回 `HTTRANSPARENT`，显示和缩放均不激活窗口。

自测在覆盖层下创建深蓝色窗口。桌面截图中的深蓝色来自这个下层窗口，不是覆盖层背景。
实际桌面读回分别核对透明、实心和抗锯齿边缘的合成像素（通道误差最多2），并用
`WindowFromPoint` 检查透明处及实心线条处的鼠标目标都为下层窗口。这是命中路由验证，
没有模拟物理鼠标点击。读回先 `DwmFlush`，再 `BitBlt` 和 `GdiFlush`，避免CPU读取
尚未完成的DIB写入。

## 验证与复现

构建根目录：`E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction`。

```powershell
& .\reconstruction\build.cmd E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase9-out
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase9-out\reference_pose_test.exe
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase9-out\reference_projection_test.exe
& E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase9-out\overlay.exe --self-test E:\CodexData\.codex\tmp\wardogs-review-build\reconstruction\phase9-window-final
```

- 最终MSVC `/W4 /WX /O2` 编译通过，日志 `phase9-build-final-retry.log`。此前新增
  像素测试触发常量截断编译警告，已改为显式通道掩码；失败日志保留。
- 姿态、投影测试通过；原始读取路径与规则测试回归通过。
- 三轮依次验证正常、另一描述、相机背后、缺关节、拒绝候选、缩放和真实定时器。
  相机背后与拒绝候选均清除旧线条，另一个描述产生不同画面。
- `phase9-window-final.log`：21次窗口场景通过，42张PNG分别保存原始透明帧和桌面合成。
  已目视检查正常帧、缺关节和缩放画面；原始PNG查看器显示的底色不代表Alpha值。
- 每轮关闭后GDI=0、USER=5。GDI有相等断言，USER仅记录；三次记录无增长，不能推断
  任意长时间无泄漏。窗口、Canvas、DIB、DC、定时器释放均经过自测。
- 正常启动命令是不带参数执行 `overlay.exe`，覆盖主屏，使用受控动画数据；退出快捷键
  为Ctrl+Alt+F10。该快捷键代码已审阅，本轮未用真实键盘输入验收。
- 新代码经过二读与独立审阅。读回同步问题已修正，并在最终窗口测试中验证。

## 参考证据与剩余缺口

样本 `reference/kernel-backend-export/fgnew.dll.bin` SHA256：
`617bda70d67dee7ba1018a290a86929538e1a2cd67c29329c42dc8e6fa18817e`。
复用已保存REA主函数证据，并通过独立Ghidra 12.1.4项目与VS dumpbin补充静态helper；
新导出在 `phase9-static/`，不冒充新REA Evidence记录。详细映射、地址和边界见构建根目录
`phase9-pose-audit.md`、`phase9-projection-audit.md`。

仍缺5c18/9124动态映射发现、9654缓存实体索引/存储、真实源多次读取时序、完整停止/卸载。
8dc4原来可单读24字节translation；此模型用完整96字节记录的read状态，不能声称局部
读取失败情形等效。12ac对外部Mesh字段的写入没有执行。投影没有复刻极大/非有限角度
的CPU异常行为，也不保证失败输出中无意义字节完全相同。

### 参考程序的窗口与绘制方式

已保存静态证据显示，FgFrame（0x180012ee0）接收外部图形对象，通过GetDesc获取描述，
其中HWND保存到全局1ca68。draw_frame（0x180011ea8）取得当前后缓冲并向其提交D3D12
绘制命令；init（0x180010648）启用源Alpha混合。因此目前可以确认的是外部交换链/后缓冲
上的绘制流程，不能推断它一定创建了与本地相同的独立透明覆盖窗口。

现有材料未闭合CreateWindow、窗口styles、置顶和系统命中穿透链。11a70使用鼠标屏幕坐标、
ScreenToClient及左键状态判定雷达拖动，这不证明操作系统层面的鼠标穿透。准确地址、
文件与搜索边界见构建根目录 `phase9-reference-window-audit.md`。

本窗口使用自有离屏绘制和Windows分层合成，不等同于已经复刻参考程序的D3D12后端。
完整功能、视觉一致性、真实游戏可用性均未验收。
