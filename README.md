# WarDogs ESP（透视辅助）源码与分离资料归档

[English introduction](README.en.md) · [下载全部归档 / Downloads](https://github.com/simplerjiang/wardogs-research/releases/tag/archive-20261117)

本仓库是**《WarDogs》的 ESP（透视辅助）项目归档**，收录 CoreWD/Wardogs ESP 源码、FGTRACK 分离组件与分析资料，以及 Kong助手界面改版源码。ESP 代码包含读取游戏目标数据，并在游戏画面上叠加人物方框、骨骼、名称、血量等信息的实现。归档日期为 **2026-11-17，Asia/Singapore**。

源码可直接浏览和克隆；体积较大的软件样本、证书、汇编、静态分析记录和历史取证资料放在 Releases。这里的“源码”包括本地自写工程与提取的网页模板，**不代表已经取得 FGTRACK 等第三方闭源程序的完整原始源码**。

## 源码目录

| 路径 | 内容与来源 |
| --- | --- |
| [source/wardogsE](source/wardogsE) | CoreWD / WarDogs ESP 原工程当前快照：ESP 覆盖层、内核驱动、历史 mapper、受控读取与绘制测试及原有文档。来源提交 `7c73400a4873c0c86f54fa78e5eeba238c52e193`。 |
| [source/wardogsE-reference](source/wardogsE-reference) | 保留的 reference-reconstruction 独立实验分支，包含 `reconstruction/`。来源提交 `09114169ce5ba61f18c28091da54c0b64b102a32`；不表示已合入主工程。 |
| [source/wardogsv3](source/wardogsv3) | WardogLauncher C++ 复现工程、测试驱动、自测工具、案例配置和覆盖层代码。包含归档时本地尚未提交的修改，并非仅复制旧远端提交。 |
| [source/kong-ui](source/kong-ui) | Kong助手启动页与菜单 HTML/CSS、提取的原模板、界面构建脚本、设计素材和已有 UI 验证记录。来自 `codex/kong-ui-20261113` 的当前文件。 |

各工程的历史说明与构建脚本保留在对应目录。硬编码的 VS/SDK/WDK 路径及原始取证绝对路径需要结合自己的环境理解；本次归档没有重新构建或执行程序。

## 分离内容与完整下载

在 [archive-20261117 Release](https://github.com/simplerjiang/wardogs-research/releases/tag/archive-20261117) 中提供：

| 文件 | 内容 |
| --- | --- |
| `wardogs-sources-20261117.zip` | 与本仓库 `source/` 对应的完整源码快照。 |
| `fgtrack-complete-20261026.zip` | FGTRACK 完整资料目录：调查原始归档、软件原样本、公开证书、FgFrame 汇编与绘制证据、历史加载记录、来源输入及 UI 静态导出。 |
| `fgtrack-outer-20261113.zip` | 原样分离的新版外层助手 `FgGear32404.exe`、来源说明及哈希记录。文件名中的数字不代表已核验的产品版本。 |
| `kong-assistant-20261113.zip` | Kong助手早期界面派生程序、UI 源码包与已有验收记录。 |
| `kong-assistant-20261114.zip` | Kong助手文案修订版、UI 源码包、使用教程及已有验收截图。 |
| `fgtrack-attribution-20261026.zip` | 后续归因研究、静态审计、汇编对照、检查点记录与分析脚本；排除整机文件系统目录清单。 |
| `FILE-MANIFEST.json` / `SHA256SUMS.txt` | 发布文件的逐文件身份记录和下载包 SHA-256。 |

归档包使用 ZIP 格式；大包建议通过 Release 下载。普通 `git clone` 只获取仓库内容，不自动下载 Release 附件。

## 已验证的边界

- 本次整理核对了源码复制与 SHA-256，另对 ZIP 内容和发布文件进行完整性检查。此类检查只能证明归档完整性。
- 原工程已有受控用户态读取与绘制测试记录；真实内核加载、完整游戏功能和长期稳定性没有因此获得新的验证。
- 分离外层助手时确认的是文件身份与复制一致性；没有证明它可以脱离原始授权及启动链完整独立运行。
- Kong助手的已记录验证主要覆盖界面与静态修改范围；完整授权启动、游戏接入及原生菜单捕获仍有未验证事项。
- 历史文档可能描述不同时间、主机或分支。请结合每份文件的日期、来源与验证范围阅读，不将旧结论当作当前所有环境的保证。

## 来源、排除项与权利

[逐文件清单](docs/FILE-MANIFEST.json)、[附件清单](docs/RELEASE-ASSETS.json)、[排除项](docs/EXCLUSIONS.json) 和 [归档说明](docs/ARCHIVE-NOTES.md) 记录此次整理范围。排除 Git 内部数据、WebView 登录/Cookies/测试缓存、本地构建输出、Python 缓存、快捷方式、私人凭据容器和整机磁盘目录记录；原始本地资料没有因此被删除。

第三方程序、驱动、证书、SDK、图标与素材的权利归其原权利人；本仓库没有给这些内容追加统一的 MIT 或其他开源许可。已有许可文件随相应素材保留。公开可访问不等于所有第三方内容均可自由再许可。

This repository archives **WarDogs ESP** source code, extracted FGTRACK components, research evidence, and the Kong Assistant UI revision. The ESP code implements player boxes, skeletons, names, and health overlays for WarDogs. Source snapshots are browsable here; large archives are available in [Releases](https://github.com/simplerjiang/wardogs-research/releases/tag/archive-20261117). See the [full English introduction](README.en.md) for scope, provenance, and verification limits.
