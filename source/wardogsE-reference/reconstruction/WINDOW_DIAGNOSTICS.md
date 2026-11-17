# 独立窗口故障复核

2026-10-22，Asia/Singapore。这里只运行自建预览和诊断变体，不运行参考助手、游戏或驱动。

## 定时器断言

已定点复现原错误：启动真实定时器后向自己的窗口投递空格消息，暂停状态生效；
随后 WM_TIMER 确实送达，但 tick 因暂停而保持序号，测试继续进入下一阶段并报
“Real timer did not advance controlled data”。因此，该错误并不必然表示系统定时器失效。

原自测窗口使用会激活窗口的显示、缩放和恢复方式，存在接收用户正在输入的按键的条件。
首次失败日志没有记录按键及状态，不能证明历史上确实收到空格，也不能把上一轮
生产/消费变量拆分当作已证明的根因修复。

本轮只改变自测窗口：WS_EX_NOACTIVATE、显示/恢复不激活、缩放不激活，并核对
三处前台窗口状态。真实定时器阶段记录 running、基线和生产/消费序号，暂停干扰
单独报错。普通交互窗口仍可激活、暂停和操作配置，没有屏蔽用户输入。

## USER 对象增长

GetGuiResources 是进程级汇总，不能只归属给 Preview 的 HWND。
本轮只对自己启动的诊断预览做调用追踪，观察到：

- 自有 33 ms 定时器成功创建，并在测试结束前成功取消。
- CoreMessaging.dll 为 SystemUserAdapterWindowClass 创建消息窗口和定时器。
- MSCTF.dll/IMM32 的窗口及回调定时器会随窗口激活/输入法初始化出现。
- OleMainThreadWndClass 在 CoUninitialize 后消失；窗口关闭时并不等于 COM 已退出。
- 六种共享光标首次及重复加载的微实验均没有增加本进程 USER 计数，当前环境下
  “首次加载调整光标导致增长”的假设没有支持。
- 关闭后追加 350 ms 消息派发的诊断实验没有让所有框架对象消失；没有把等待或
  强行销毁框架对象加入产品以压低计数。

这些事实证明总计数混入了框架对象，并解释了为什么不能把“暖机后三轮严格相等”
作为自有资源释放的唯一判据。它们没有逐一闭合最早 6/6/7 的每个对象；历史那一个
增量的身份仍未知。仪器化也会影响时序，不能把其计数与无仪器化运行直接相减。

本轮保留 USER 变化诊断，保留 GDI 暖机后变化断言，增加首次 KillTimer 必须成功
的断言，并继续要求自有窗口和 Canvas 已释放。该范围的测试通过不等于全进程零泄漏。

## 原始证据

证据根目录：`E:/CodexData/.codex/tmp/wardogs-review-build/reconstruction/`。

- `phase5-window-first-failure.log`、`phase5-timer-debug.log`：历史失败，保持不变。
- `phase6-diagnostics/pause-repro.log`：定点暂停复现，预期退出码1。
- `phase6-diagnostics/cursor-probe.log`：共享光标对照。
- `phase6-diagnostics/current-run.log`：阶段、消息、序号和窗口记录。
- `phase6-diagnostics/timer-hooks.jsonl`、`hooked-output.log`：定时器调用来源。
- `phase6-diagnostics/drain-hooks.jsonl`、`drain-hooked-output.log`：关闭后消息派发对照。
- `phase6-diagnostics/object-hooks.jsonl`、`object-hooked-output.log`：对象来源及不激活
  标志不足时的一次前台状态断言失败；之后才补上 WS_EX_NOACTIVATE。
- `phase6-window-first.log`：初版不激活修改后，USER 从2到5使旧总数断言失败。
- `phase6-window-final.log`、`phase6-verification.json`：最终无仪器化运行结果和代码身份。

Frida 17.24.3 只安装在诊断临时目录的 python 子目录，不是项目运行依赖；只由
脚本 spawn 并附加其自己的 preview 子进程。CDB 未安装，尝试未执行目标，没有据此
产生调试结论。部分诊断源码在迭代中重新生成；历史未保存可执行文件哈希的记录
不声称具备逐字节二进制重现性，源码基线和生成脚本可供检查。

官方接口依据：[GetGuiResources](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguiresources)、
[ShowWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)、
[LoadCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadcursorw)。

当前结论是可复现机制及自有资源验证；首次失败的唯一根因、全量 USER 对象归因和
长时间运行泄漏仍未闭合。
