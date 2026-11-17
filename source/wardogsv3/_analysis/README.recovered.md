# Wardog Launcher —�?�?PGR、后借物理内存映射加载异体驱动（C++ 复现�?
本目录是�?`E:\CodexData\exports\FGTRACK-完整资料-20261026-01` 里的资料与证书原件做的复现工程：
**不含任何卡密逻辑**，只保留「先让微软嵌套签名驱�?PGR 被内核接�?�?打开 PGR 设备 �?用它的物理内存映射能力读写内核内�?�?定位代码完整性目�?�?读取并保存原内容 �?写入临时内容 �?申请加载后续驱动 �?恢复原内容」这一条链�?
复现对象�?*流程与内核机�?*，被加载的「后续驱动」是我们自己的�?*真正不同**的未签名驱动
`WardogProbe.sys`，用来证明该模式对我们自己的 C++ 应用同样成立，而不是把 FGTRACK 再打包一遍�?
---

## 1. 交付�?
| 路径 | 说明 |
| --- | --- |
| `src/main.cpp` | 编排：加�?PGR、开设备、自检、定位目标、打补丁、加载、恢复、对�?|
| `src/pgr.h/.cpp` | `\Device\PGRHostControl` �?IOCTL 封装（映�?解除映射/端口 IO/握手�?|
| `src/translate.h/.cpp` | 内核虚拟地址 �?物理地址（扫�?PML4 + 四级页表遍历），�?VA �?�?|
| `src/ci.h/.cpp` | �?ci.dll / ntoskrnl 的可写映像里定位代码完整性目标，�?�?恢复 |
| `src/service.h/.cpp` | 临时服务�?+ `NtLoadDriver` / `NtUnloadDriver` + `SeLoadDriverPrivilege` |
| `src/selftest.cpp` | 无需驱动、无需管理员的自检（合成页�?+ 真实 ci.dll 字节�?|
| `driver/WardogProbe.c` | 未签名测试驱动：创建 `\Device\WardogProbe`，用于证明镜像真的跑起来�?|
| `build.cmd` | 一条命令构建（VS2022；无 WDK 也能构建驱动，见 §4�?|
| `tools/verify-run.py` | 解析运行日志，逐条断言对照实验与恢复是否成�?|
| `build/drivers/PGRHostControl.sys` | 固定身份�?PGR 原件副本（SHA256 校验后才允许加载�?|

---

## 2. 从资料里确认到的、本实现直接使用的事�?
### 2.1 PGR 为什么能先被接纳（照抄结论，不重新论证）

`SECURITY-REPORT.txt` 的三次实机对照已经给出因果：

* 原样本（SHA256 `076cfa7d…f720a4`）→ `NtLoadDriver=0`，设备出现；
* 只破�?*微软嵌套签名**（文件偏�?27879，`EE→EF`）→ `0xC0000428`，设备不出现�?* 只破�?*外层主签�?*（文件偏�?20492，`E4→E5`）→ `NtLoadDriver=0`，设备出现�?
即当前主机的接纳**依赖嵌套的微软硬件兼容性签�?*，而不依赖外层主签名。因此启动器�?「PGR 原件」当成唯一的准入凭据：加载前先核对整文�?SHA256，等�?`076cfa7d…`，否则拒绝运行�?这与报告里「不要用证书显示名放行、按字节身份放行」的建议一致�?
### 2.2 PGR 设备�?IOCTL 契约（本次从驱动字节重新恢复�?
`PGRHostControl.original.sys`�?4 024 字节）的派发函数�?`0x140016D8`，读
`IRP->Tail.Overlay.CurrentStackLocation` 后比�?`Parameters.DeviceIoControl.IoControlCode`
（x64 �?`POINTER_ALIGNMENT` 使其位于 +0x18）。本次恢复出的完整契约：

| IOCTL | 方向 | 缓冲�?| 语义 |
| --- | --- | --- | --- |
| `0x80102040` | in/out | 5×u64�?x28�?| `{size, phys, handle_out, base_out, object_out}`：把**物理地址**映射进调用进�?|
| `0x80102044` | in | 5×u64 | `{�? �? handle, base, object}`：解除映�?|
| `0x80102050` | in/out | 7 字节 | `{u16 port; u32 value; u8 width}`：`in al/ax/eax, dx` |
| `0x80102054` | in | 7 字节 | 同上：`out dx, al/ax/eax` |
| `0x0022218C` | in/out | `{u32 1; u32 �?xC}` �?`{u32 1; u32 0xC}` | 握手/版本 |

映射实现（`0x140014A0`）依次调�?`ZwOpenSection("\Device\PhysicalMemory", SECTION_ALL_ACCESS)` �?`ObReferenceObjectByHandle` �?`HalTranslateBusAddress`（起止各一次）�?`ZwMapViewOfSection(NtCurrentProcess, SectionOffset=phys, ViewSize=size,
PAGE_READWRITE|PAGE_NOCACHE)`，遇�?`STATUS_OBJECT_NAME_COLLISION` �?`PAGE_READWRITE` 重试�?最后把 `base + (translated �?SectionOffset)` 写回调用者缓冲区。也就是说：
**它把指定物理区间直接映射成调用进程可读写的用户态地址**，这就是资料里说的「PGR 提供的物理内存映射能力」�?`src/pgr.cpp` 的请求结构体与这些偏移逐字节对应（`static_assert(sizeof(MapRequest)==0x28)`）�?
### 2.3 参照加载器的「读→存→写→加载→恢复」形�?
`loader-memory-flow-rea.json` �?`0x14000B120` 的反编译（evidence `ev_681df8f5…`/`ev_9c2164c2…`�?与读写包装函�?`0x14000BDA0` / `0x14000C520`（`ev_f0ef2725…` / `ev_79057e76…`）给出的顺序是：

```
read4(target1) -> 保存                       ; FUN_14000bda0
write4(target1, 0xC3C03148)                  ; 48 31 C0 C3 = xor eax,eax; ret
read4(target2) -> 保存
write4(target2, 0)                            ; 4 字节清零
call  0x14000AF30                             ; 申请加载后续驱动
write4(target1, 保存�?                       ; 恢复
write4(target2, 保存�?                       ; 恢复
```

而定位目标的那一段（同函数前半部）先打开 `System32\ntoskrnl.exe` �?`System32\ci.dll`、解析导出，
再在 `.text` 里按指令形状扫描（如 `48 8B 05` �?rip 相对引用的数据地址、并校验该地址落在
`PAGE*` 可写节内），最后把「文件内偏移」换算成「运行时内核地址」再交给读写包装函数�?本工程把这段等价地实现为�?
1. `NtQuerySystemInformation(SystemModuleInformation)` �?ntoskrnl / ci.dll �?*运行时基址**�?2. 扫描 PML4 并做四级遍历，得到任意内�?VA 的物理地址（`src/translate.cpp`）；
3. �?ci.dll、ntoskrnl �?*可写�?*里按「运行时 CI 选项值」定位候�?DWORD，并按被代码
   rip 相对引用的次数排序（`src/ci.cpp`）；
4. 对每个候选执�?*试写—试加载—恢�?*，由 `NtLoadDriver` 的真实返回值判定它是不是那道闸门；
5. 命中后：保存原�?�?写临时�?�?申请加载 �?恢复原�?�?再跑一次对照�?
---

## 3. 运行

需�?**管理�?*（`SeLoadDriverPrivilege` + 物理内存映射）。命令提示符示例�?
```bat
:: 0) 构建
build.cmd

:: 1) 只做自检：加�?PGR、开设备、握手、证明物理内存真的可读可写，然后卸干净
WardogLauncher.exe --mode probe

:: 2) 只看定位结果，不写内核内�?WardogLauncher.exe --mode scan

:: 3) 完整复现（默认）：对�?�?定位 �?试写 �?加载异体驱动 �?恢复 �?再对�?WardogLauncher.exe --mode run

:: 4) 用「代码补丁」变体：�?48 31 C0 C3 写到 ci.dll+X（X 由你给出，需自行确认�?WardogLauncher.exe --mode run --patch-rva 0x12345

:: 5) 日志断言
python tools\verify-run.py wardog-launcher.jsonl --mode run
```

可选参数：`--pgr <路径>`、`--driver <路径>`、`--max-phys <字节>`、`--dry-run`、`--log <路径>`�?
退出码：`0` 成功；`2` 参数/权限/镜像缺失；`3` PGR 加载失败；`4` 物理读写自检失败�?`10` 定位阶段失败；`11` 基线（未打补丁）竟然加载成功（说明本机未强制 CI，结果不可归因）�?`12` 所有候选都改变不了准入结论；`14/15/16/17` 后续加载失败 / 驱动未真正运�?/ 未恢�?/ 恢复后仍被接纳�?
---

## 4. 构建细节（没�?WDK 也能出驱动）

本机只有 VS2022 �?Windows SDK，没�?WDK。`build.cmd` �?`.def` + `lib.exe` 现生�?`ntoskrnl.lib`（只导出 `IoCreateDevice/IoCreateSymbolicLink/IoDeleteSymbolicLink/IoDeleteDevice`），
再用 `/driver /subsystem:native /entry:DriverEntry /nodefaultlib` 链接�?`WardogProbe.sys`�?驱动里用到的内核类型�?`driver/WardogProbe.c` 内本地声明（`DriverUnload` 位于 x64 `DRIVER_OBJECT+0x68`）�?生成�?`.sys` 刻意**不签�?*——它必须�?CI 拒绝，才能作为基线与「恢复后」的对照�?
---

## 5. 对照实验（这是本工程能自证的关键�?
| �?| 条件 | 期望 | 日志断言 |
| --- | --- | --- | --- |
| A 基线 | CI 完好，加�?`WardogProbe.sys` | `0xC0000428`（`STATUS_INVALID_IMAGE_HASH`），无设�?| `control_result/baseline_unsigned` |
| B 物理映射自检 | 取自己页面的物理地址，用 PGR 映射后比�?改写 | 读一致、写可见、可恢复 | `selftest_ok` 三个布尔 |
| C 闸门定位 | 候�?DWORD �?0 后加�?| 只有一个候选让 `NtLoadDriver` 变成 `0` | `gate_confirmed` |
| D 窗口内加�?| 保持补丁，加�?`WardogProbe.sys` | `NtLoadDriver=0`，`\DosDevices\WardogProbe` 出现 | `followup_load_result` |
| E 恢复 | 写回原值并回读 | 字节一�?| `original_content_restored.match=true` |
| F 恢复后对�?| CI 复原，再次加载同一驱动 | 又是 `0xC0000428` | `control_result/after_restore` |

自检 B 是「真实读写」的直接证据：它不依赖任何符号——先�?`QueryWorkingSetEx` 拿到自己页面的物理页号，再让 PGR 把这**同一个物理页**映射出来�?比对内容 �?通过映射改写 �?在自己的虚拟地址上看到改�?�?再改回�?若映射到的不是同一块物理内存，这三步不可能同时成立�?
---

## 6. 已经验证�?/ 尚未验证�?
已在本机（Windows 10.0.26200，非管理员会话）验证�?
* `--mode selftest` 15 项全部通过：四级遍历（2 MB 大页�?4 KB 页）、未映射返回 0�?  PML4 启发式接受真�?拒绝噪声、VA 读、VA 写、真�?`ci.dll` �?PE 解析、按�?引用次数
  定位候选、DWORD 改写与恢复往返。日�?`build/selftest.jsonl`，断言 `tools/verify-run.py`�?* 非管理员运行时立即以退出码 2 说明需要提权，不会留下任何服务键或设备�?
尚未验证（本会话是普通用户，无法加载驱动）：

* PGR 实机加载、设备打开、握手、物理映射与补丁窗口内的真实加载�?  这些需要你�?*提升权限**的终端里�?`--mode probe`、`--mode scan`、`--mode run`�?* 目标机器若开�?HVCI/VBS，物理页会受 SLAT �?W^X 约束，本方案（改代码或改只读数据页）
  可能被拦下；本机 `VirtualizationBasedSecurityStatus=0`，与资料�?SecureBoot=true�?  CI=0xC001 的环境一致，属于本方案适用的场景�?
---

## 7. 与参照软件的差异（不夸大�?
* **没有卡密**：不联网、不校验授权、不做界面。启动器只做加载链�?* **后续驱动不是 FGTRACK**：换成我们自己的 `WardogProbe.sys`，正是为了证明这是可复用的机制，
  而不是对某个样本的硬编码�?* **CI 目标的选法不同**：参照loader 用指令形状扫�?ci.dll；本实现用「运行时 CI 选项�?+ 引用次数�?  定位，并用真�?`NtLoadDriver` 结果**自证**，避免依赖特�?build 的字节签名�?  若你已经知道�?build 的验证函�?RVA，用 `--patch-rva` 即可切换到参照软件那�?  「写�?`48 31 C0 C3` 到验证函数」的形态（同样先保存、后恢复）�?* **资料仍未结论的部分不做断言**：PGR 历史首次加载时最终选中的链、证书过期后仍被接纳的具�?  策略分支，资料里仍是开放项；本工程不据现象推断这些结论�?
---

## 8. 清理与风�?
启动器每次运行都会：删除自己创建的临时服务键、卸�?PGR、确�?`\Device\PGRHostControl` 消失、恢�?`SeLoadDriverPrivilege` 原状态、把改过的内核内容写回并回读校验�?�?*�?*安装证书、不�?CI/启动/HVCI 策略、不关闭 Defender、不写磁盘上的系统文件�?即便这样，这段代码仍然在真实内核内存上做临时写入，请只在可回滚的机器上、以明确的实验目的运行�?
