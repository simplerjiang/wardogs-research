# Kong助手 UI 改版

基于用户本次运行并分离的 `FgGear32404.exe`，按选定的第三张蓝灰设计修改启动界面和菜单。

- 来源 SHA256：`55f6e9997a8ee49ebcbad534b01c581f2b0af90c59ba4ca626d71da283cb59ec`。
- 新程序：`E:\CodexData\exports\Kong助手-20261114-01\Kong助手.exe`。
- 新程序 SHA256：`2518c8dfb16d2dd7f9ff27ae3a373de5a2304dd6a4abbc48b409065bc85c55e2`。
- 两个界面均显示 `Kong助手` 和 `QQ：1013171256`。

启动窗改为 500×300，状态和参数操作在窗口内显示；去掉“更新内容”入口和授权时间线；准备阶段使用中性提示，并移除索要桌面诊断文件的文案。菜单改为窄侧栏、蓝灰分组行，标签预览可展开，原 toast 改为页脚状态文字。主题和自定义配色仍可使用。真实失败提示保留。

`original/` 是从本次新版 EXE 中提取的真实模板。反编译确认菜单从 UTF-8 字符串读取，启动页通过 `NavigateToString` 读取 UTF-16LE 字符串；这两个模板的文件槽位记录在 `original/slots.json`。修改限制在两份模板、原生窗口标题、启动窗尺寸的三个立即数，以及两条备用弹窗显示字符串。最终资源树的所有载荷哈希与来源一致。

构建命令（使用上述来源文件）：

```powershell
python prepare-launcher.py
python build-ui.py "E:\CodexData\exports\FGTRACK-外层助手-20261113-01\FgGear32404.exe" "E:\CodexData\exports\Kong助手-20261114-01\Kong助手.exe"
```

`menu.html` 已含对应样式；`menu.css` 为其可读样式源。`launcher.html` 由 `prepare-launcher.py` 根据原模板和 `launcher.css` 生成。四格图标来自 Phosphor Icons，许可证位于 `assets/PHOSPHOR-LICENSE.txt`。`operator.png` 来自本次 EXE 的普通 UI 图片资源，仅用于本地预览。

已验证原生启动窗纯 UI 入口、浏览器菜单分页、开关、滑条、标签预览、主题、自定义配色及保存反馈；脚本及改动范围经过独立二读。原生菜单是工具窗口，当前 Computer Use 无法选择该窗口，故没有取得原生菜单验收截图。完整游戏接入及正常授权启动链未重新实测，不能将界面验证当作这两项验证。

运行中的旧实例不会自动使用新文件的界面。实际试用时需先退出旧助手，再运行新文件。原始分离样本保留。

验证证据见 `evidence/verification.json`、`design-qa.md` 及 `evidence/` 中的截图。此前测试缓存清理曾被自动审批以 `blocked by policy` 拒绝。本轮保留 `ui-test-profile/` 作为纯 UI 验证环境，没有再次清理。

开发分支 `codex/kong-ui-20261113` 基于已获取的远端 `main`（`b3f36636f95db5ed613e90e61ae6564b89e9b3e4`）；获取时远端不存在 `dev`。PR 和合并等待用户确认实现。
