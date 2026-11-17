# 2026-10-22 标准加载与完整卸载适配

本轮从接口补齐提交 `254a118` 继续，只修改路线 A 的驱动、构建说明和测试。已完成标准入口、初始化回滚及 DriverUnload 资源释放代码，重新编译和用户态夹具通过。**新驱动仍未签名，尚未进行 Windows 内核加载、停止和再次加载验收。**

另一个会话已确认独立使用 `reference-reconstruction/wardogsE` 工作树和 `codex/reference-reconstruction` 分支；本轮没有修改它的代码、构建目录或参考证据。原目录 `D:\wardogsE` 的 main 保留不动。

## 改动与兼容性

- `DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING)` 直接使用 Windows 提供的 DriverObject，同步完成初始化并返回实际失败状态。移除 IoCreateDriver、额外初始化线程和链接 keeper。
- 固定设备名 `\Device\AbiComm0`，匹配现有客户端的打开路径；全局链接为 `\DosDevices\Global\AbiComm`，Win32 客户端仍使用 `\\.\AbiComm`。
- 现有七个 IOCTL、请求结构、SessionKey 发现位置和 Windows 进程权限检查保持兼容。CREATE/CLOSE/CLEANUP 和设备控制请求由 rundown 保护。
- 设备、链接或状态键初始化失败时，同步清理已取得资源。链接创建碰撞不会删除他人的链接；未成功发布 SessionKey 时保留已有值；不删除此前已经存在的状态父键。
- DriverUnload 先关闭新工作入口，再等待在途请求完成，随后删除自有链接、SessionKey、必要时本次新建的状态键，关闭内核键句柄、清空会话值并删除设备。驱动映像由 Windows 加载器回收，没有自行释放正在执行的映像代码。

`ABI_IOCTL_UNLOAD` 保留为“停止接收工作”，不能在正在执行的 IOCTL 中释放本驱动。完整卸载由服务停止触发 DriverUnload；客户端应先关闭设备句柄。服务配置、磁盘包和此前已有的注册表父键属于安装/用户持有资源，与驱动卸载区分处理。

这是非 PnP 控制驱动，不是新增的 PnP 硬件设备。当前入口要求真实 DriverObject，旧 mapper 不能用于此构建；mapper 源码没有修改。构建脚本及 BUILD.md 中的当前部署说明已切换为正常的内核服务路径。

设计依据：[DriverEntry](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/writing-a-driverentry-routine)、[非 PnP 卸载](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/non-pnp-driver-s-unload-routine)、[rundown 等待](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exwaitforrundownprotectionrelease)、[全局设备链接](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/local-and-global-ms-dos-device-names)。

## 验证结果与边界

| 检查 | 结果 |
|---|---|
| MSVC /kernel 编译、/DRIVER 链接 | PASS。使用现有 portable WDK/SDK 10.0.26100.0，没有安装组件。 |
| 新加载/卸载夹具 | PASS。设备创建、链接创建、状态键创建和写入各覆盖新建/既有状态两种情况，共八种失败组合；另验成功初始化和清理。 |
| 并发排空夹具 | PASS。用事件阻塞读取，启动卸载线程，确认资源保持有效、新请求被拒绝且 CLOSE/CLEANUP 可完成；解除读取后才释放资源。 |
| 原有回归与受控子进程 | PASS。菜单/快照、权限、逻辑停用、新接口和真实子进程 PEB/数据对照继续通过。 |
| PE 检查 | PASS。AMD64、Native 子系统、有效入口；已无 IoCreateDriver、PsCreateSystemThread、PsTerminateSystemThread 或不受保护链接创建的导入。 |
| SignTool verify /kp /v | 未通过：No signature found，退出码 1。记录的是签名缺失。 |
| 真实内核设备调用、加载/卸载/重载 | NOT_TESTED。当前无 AbiComm 设备，未安装或加载本产物。 |

夹具直接编译提取的当前驱动入口、分发和清理函数；注册表、设备和内核 rundown 调用被用户态实现替代，阻塞/等待使用真实 Windows 线程。它验证控制流程与资源顺序，不能代替真实内核的 IRQL、请求者身份、对象管理器、签名策略、Driver Verifier 或 HVCI 验收。真实子进程测试也没有使用内核驱动。

本轮仅重新编译变更的驱动；覆盖层源代码未修改。现存 SIZE_T 到 ULONG 的警告保留，本轮移除的旧 AbiDiag 路径警告不再出现。

本机记录目录：`E:\CodexData\.codex\tmp\wardogs-review-build`。

- `standard-build.cmd`、`standard-build.log`：实际编译命令和日志。仓库 build_driver.bat 的 VS/WDK 默认路径仍需按使用环境配置。
- `standard-tests.log`、`standard-device-test.log`：回归及设备测试结果。
- `standard-pe.json`、`standard-signature.log`、`standard-host-prerequisites.json`：产物结构、签名和主机前提条件。
- `out/driver.sys`：23040 字节，SHA-256 `9A21DD0468CC38EDE7442075671D3AC6435FF704882B33A40B8F86CAD7AACFAE`。旧接口报告中的哈希不是当前产物身份。

## 签名与安装还缺的条件

| 条件 | 当前证据与剩余事项 |
|---|---|
| 当前系统接受的内核签名 | driver.sys 未签名；Authenticode 与 SignTool 均确认。当前 Windows 11 10.0.26200 开启 Secure Boot/代码完整性，未启用测试签名。正常安装需要满足其内核签名政策的产物。 |
| 签名提交身份和权限 | 当前用户及本机 My 存储中，没有发现具备私钥且未过期的代码签名证书；这不排除外部 HSM/云签名。尚无已提供并核验的 Hardware Developer Program 账号、已注册 EV 证书或可执行的签名流程。 |
| 所选签名路线的提交包 | 本轮提供可编译 SYS，没有生成面向硬件中心提交的 INF、CAT、PDB 和 CAB，也没有完成门户提交或 HLK 认证。应根据所选流程准备，不能把编译成功等同签名审核通过。 |
| 安装操作权限 | 当前执行令牌未提升为管理员。使用 SCM 注册、启动及停止内核服务需要相应管理权限；本轮没有创建服务。 |
| 内核验收条件 | 需要已能合法加载该产物的受控 Windows 环境。取得签名后，先核验已签文件身份，再运行受控进程设备测试、关闭所有句柄、停止服务并检查链接/自有状态清理，随后验证再次加载。上述验收尚未完成。 |

Windows 正常签名政策及门户条件见[微软内核签名要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-requirements--windows-vista-and-later-)和[证书管理](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-cert-manage)。测试用 attestation 与正式兼容性认证的用途不同，其适用范围和包内容见[当前 attestation 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)，不能仅用“拿到一张普通证书”替代当前系统所需签名。

对于本地非 PnP 服务安装，可使用正常 SCM demand-start kernel service 路径，签名合格的驱动二进制不等于必须额外枚举一个 PnP 硬件。若选择门户/INF 分发，则还需对应提交包。本轮只准备适配代码和验证材料，没有修改 Secure Boot、代码完整性、证书信任或 Defender，没有运行历史加载器。

## 参考样本的签名不能用于推断成本或加载方法

本轮再次只读核对 `FGTRACK.original.sys`，SHA-256 仍为 `5A99931DE2F173E57040FC64ACB30324A1F1C6B96F511D8BFA6288A02732B8A2`。Authenticode 显示签名存在，状态为 UnknownError，证书链终止于不受信任的根；叶子证书 Subject 与 Issuer 不同。当前证据不足以确定它是否使用自建根、是否正规签发、由谁支付成本或通过何种方式进入内核。证书名称中出现 Microsoft 不代表来源已获认证。

参考文件与运行中内核映像的身份关联没有完成验证，观察到参考助手运行不能证明 Windows 正常加载流程接受了这个文件。代码适配和本轮编译不需要购买证书；后续可以在已有的隔离测试签名环境继续开发验收。当前主机保持现有策略进行正常安装时，仍需合格签名和管理权限。
