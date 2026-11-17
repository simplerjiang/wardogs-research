# 2026-10-22 审查修复与验证

基线：本地 `main` 的 `63cd7de`。修复分支：`fix/review-20261022`。没有配置远端。

## 缺失资源

| 项目 | 当前状态与影响 |
|---|---|
| `mesh.bin` | 缺失。Chams 的真实模型无法验证；现在从覆盖层 EXE 同目录查找。未生成占位模型。 |
| `TTNormsTrlCnd-Medium.otf` | 缺失。自定义字体不可用，现有 GPU 代码可回退到 Segoe UI。 |
| `START.bat`、`tools/drvping.exe` | 文档引用但源码包没有提供，也没有对应探针源码。完整启动包尚不齐全。 |
| `run/` 或 `1_run/` | 文档所述运行目录未提供。EXE、WebView2Loader.dll、menu.html 和驱动的部署组合未验收。 |
| `mirror/wd_stream.h`、`mirror/wd_state.h` 及实现 | 可选 MIRROR_HOST / BRIDGE_HOST 所需内容缺失；默认编译未开启这两个宏。 |
| 系统 WDK 内核头文件、库 | 本机没有安装。此次仅解压微软 `Microsoft.Windows.WDK.x64 10.0.26100.6584` 包参与编译，未安装或改系统配置。 |

`menu.html`、WebView2 SDK 和 x64 Loader 已有。覆盖层和 mapper 的 EXE 属于可编译产物，不能与缺失模型混为一谈。

## 八项修复

1. 驱动读写及取映像基址按调用者的 Windows 进程权限检查。读操作要求 VM_READ，写操作要求 VM_WRITE 和 VM_OPERATION；确认返回句柄实际获准的权限，并拒绝跨入内核地址空间的范围。使用 IoCreateDeviceSecure 实际应用已有设备 SDDL，保持原有公开设备接口，没有添加账号、版本或进程白名单门禁。
2. 删除驱动向低地址扫描并清零 PE 头的代码，避免破坏非本映像的内存。mapper 源码未改。
3. `find_world` 候选遍历由数组长度决定，消除 6 项数组按 7 项读取的问题。
4. `mesh.bin` 从 EXE 同目录查找，去掉作者电脑的绝对路径。
5. 实体压缩时同时移动骨骼数量、变换和映射，保持槽位对应。
6. 实体、相机和骨骼在同一次临界区内复制，避免读取线程切换前台缓冲后混用不同快照。
7. 补齐九个字段的保存，其中八个同时补齐读取：show_l81_users、show_vehicles、veh_armed_only、veh_show_hp、show_lead、lead_numbers、lead_zeroing、lead_vehicles、line_w。
8. 停用时唤醒并等待 keeper 线程退出，之后删除符号链接；后续 CREATE/IOCTL 被拒绝，CLOSE 仍可完成。这个接口只停用服务，**不释放手动映射的驱动映像**；已进入执行的请求仍可结束。

## 已验证

- MSVC 14.44.35207、Windows SDK 10.0.26100.0、上述 WDK 包：覆盖层四个编译单元编译及链接通过；驱动 `/kernel` 编译及 `/DRIVER` 链接通过。
- 未修改的 mapper 以 MSBuild Release/x64 重新编译、链接通过。没有执行产物。
- `menu.html` 内联 JavaScript 的 `node --check` 通过。
- `tests/offline_regression.py` 直接提取本次源码中的函数或代码段，与离线夹具编译运行：39 个菜单整数项及调色板往返、64 种实体压缩组合、前台缓冲切换、候选数组边界、模型路径、驱动权限/地址范围及逻辑停用检查通过。
- 同一组五项用户态回归运行原始基线时全部失败，运行修复后全部通过。
- 已二次阅读修改内容，`git diff --check` 通过。

离线测试可以在 x64 MSVC 开发者命令行中运行：

```bat
python tests\offline_regression.py --out <独立测试输出目录>
```

本机编译脚本和日志在 `E:\CodexData\.codex\tmp\wardogs-review-build`：`build.cmd`、`build.log`、`mapper-build.log`、`test.log`、`baseline-test.log`。实际使用了本机 E 盘 Visual Studio 路径，原有批处理的 C 盘环境默认值未修改。

产物：

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `out/WarDogsESP_v2.exe` | 380416 | `8F51D73560E0E3DC3FCE5AB13E9E5FC8AF66983342CFCDF525C99AE1B626D08C` |
| `out/driver.sys` | 23552 | `F88FDA41A5CF8BFF3B50892903758D4E3A58B7AFA97B7C285A7D44754ADC351B` |

原目录两个历史 `.sys` 保持原字节，未用新构建覆盖。

## 验证边界

这次修复验证没有启动覆盖层、mapper 或驱动，没有修改系统配置。内核测试替换了 Windows 内核 API，不能代替真实内核权限、IRQL、并发和加载验证；Chams 模型及游戏内功能未验证。此次只修复已确认的快照复制间隙，不代表原有渲染线程全部共享数据都已消除竞争。

编译仍有原有警告：menu_webview.cpp/AbiDiag 的路径转义、gpu.cpp 宏重复定义、驱动 SIZE_T 到 ULONG 的转换，以及 mapper 的编码/类型转换。未将此次范围扩展成全项目告警清理。

用户另行启动的同类软件只作参考观察，与上述构建产物的验证分开记录。REA 报告和观察记录在同一临时根目录的 `reference/`、`live-reference-20261022-1115/`，没有把第三方二进制或卡密放入仓库。

接口依据：[ObOpenObjectByPointer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-obopenobjectbypointer)、[IoCreateDeviceSecure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdmsec/nf-wdmsec-wdmlibiocreatedevicesecure)、[WDK NuGet](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget)。
