# Ra3 FrameLab / FpsTest

> 本研究分支包含尚未发布的跨帧率联机候选修复。公开 `v0.2.1` 下载仍是已发布基线；候选获得了有限生产、移动和攻击场景证据，完整状态、实际呈现率与跨机器联机仍未验证。失败轮次与覆盖限制见 [跨帧率研究记录](docs/CROSSFPS_RESEARCH.md)。

红警 3（Red Alert 3）1.12 的实验性客户端帧率工具。当前版本 **0.2.1**，图形界面提供 **30 / 60 / 90 FPS**，包含帧率、动画时钟、车身悬挂及诊断工具。DLL 通过运行时内存补丁工作。

**90 FPS 是当前上限。120 / 240 FPS 尚未实现。跨帧率联机仍有已知不同步风险，本版本适合离线研究与测试，不能据此认定联机安全。** 本次发布整理了默认分支遗漏的修复、可移植构建和工具配置，未改变核心调度算法。

## 下载与运行

从 [Releases](https://github.com/Ra3-Online/ra3-framelab/releases) 下载 `Ra3FrameLab-0.2.1-windows-x86.zip`，完整解压到可写目录，运行 `Ra3FpsTest.exe`。GUI 内嵌对应版本 DLL，普通用户无需安装 Python、Visual Studio，也无需手工复制 DLL 到游戏目录。

1. 选择合法安装的红警 3 游戏目录；工具按目录识别主程序及 SkuDef。
2. 选择 30、60 或 90 FPS。先用默认选项测试，额外特效修复属于实验功能。
3. 由工具启动游戏并查看状态、日志。目标帧率需要显卡、显示器及游戏场景实际支持；垂直同步也会限制实际呈现率。
4. 测试结束退出游戏。切换补丁版本或恢复原版时，退出游戏后重新启动；游戏运行期间关闭或卸载补丁曾出现冻结，不能当作可靠的恢复流程。

使用外部 MOD、启动器、已加载的帧率补丁或不同主程序构建时，特征校验可能拒绝安装。请保留日志中的失败原因，不要绕过校验。发行包不包含游戏主程序、游戏数据或回放。

## 支持范围与已知问题

支持基线为 2009 构建的 32 位 RA3 1.12。已检查镜像为 9,306,120 字节，SHA-256：

```text
e212791928aca4fc3898c6af28f9081ffa28fc3758b009b44479037429f3b1fc
```

这是离线验证镜像的标识；运行时仍按 PE、特征码、预期操作数及数值进行检查，并非对所有标称 1.12 的版本作兼容保证。

- 已有历史实机反馈包括 60 / 90 FPS 基础运行、动画和车身节拍修复；本次便携化发布以重建和离线检查验收。
- 跨帧率联机仍有实际不同步：双端交换房主后曾在逻辑帧 3375 分叉；NewMap 三开在 3150 帧分叉。最新的无限岛三开中，原生 30 与候选 60 两端一致，90 FPS 房主在 7065 帧出现原生 CRC 差异，首个字节差缩小到同一单位的武器子快照。此前有限匹配不能覆盖这些失败。敌方单位偶尔不可见也仍需继续复核。
- 当前测试配置为无限岛、三个客户端全部观战、两名敌对凶残 AI、50000 开局资金。原生三端约 300 秒对照的 101 个自然 CRC 键及所采字节一致；同一候选的 30 / 60 / 90 混帧局随后仍发生上述实际不同步。赛后结算、采集器读取失败与原生不同步分别记录，详见 [跨帧率研究记录](docs/CROSSFPS_RESEARCH.md)。
- 炉子等环境特效可能偏快。新增镜头与 Tint 修正、模拟相关实验组尚缺完整场景及联机验收。
- 目标帧率与实际帧率不一致时，现有基于显示帧计数的调度和视觉时间轴仍可能变慢。
- 公开 `v0.2.1` 的 `SIMPIN`、`SIMGATE` 默认关闭。本研究候选在高帧率修改共享派生 FPS 时自动固定三处逻辑高度读者的原版时间尺度，`SIMGATE` 仍默认关闭；有限测试没有证明完整联机一致性。

完整机制、上游 240 FPS 对照和验收边界见 [技术报告](docs/TECHNICAL.md)。

## 从源码构建

需要 Windows、PowerShell 5.1 或更新、Visual Studio 2022 / Build Tools 的 **Desktop development with C++** 组件（MSVC x86 与 Windows SDK），以及 Python 3。普通构建与离线自检不需要游戏。

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -Target all
```

脚本自动发现 VS / SDK，以 x86 MSVC 构建。默认输出到仓库 `build`，包含 DLL、CLI、GUI 和两个测试程序。目录可以含中文和空格；CLI 和日志路径使用 Unicode 文件接口。依赖检查确保产物无需作者电脑上的环境或 VC 运行库安装路径。

可选环境变量：

| 变量 | 用途 |
| --- | --- |
| `FLAB_BUILD_DIR` | 自定义构建输出目录，默认仓库 `build`；探针使用同一设置 |
| `FLAB_PYTHON` | Python 可执行文件；构建入口从 PATH 查找 python / python3，部分探针也支持 py |
| `FLAB_IMAGE` | 自己持有的 RA3 1.12 主程序，用于可选离线特征扫描 |
| `FLAB_GAME_ROOT` | 开发探针所需的游戏安装目录 |
| `FLAB_SKUDEF` | 启动配置文件；默认发现唯一的 `*_1.12.SkuDef` |
| `FLAB_REPLAY_DIR` | 回放目录；默认当前用户 Documents 下的 RA3 Replays |
| `FLAB_IDA_DUMP` | 可选逆向分析工具所需的 IDA 文本导出 |
| `RA3FL_LOG` | DLL 日志文件完整路径；父目录应已存在且可写 |
| `FLAB_RUNTIME_DIR` | GUI 日志与内嵌 DLL 的可写运行目录；默认系统临时目录下 Ra3FrameLab |
| `RA3FL_GROUPS` | GUI 专家调试组掩码；普通用户无需设置 |

自定义 VS 安装或工具链覆盖参数见 [SETUP.md](SETUP.md) 和 `Get-Help .\tools\build.ps1 -Full`。输入路径属于使用者配置，仓库不预置作者的游戏目录、账号、Python、IDE、回放名称或符号导出位置。

## 离线检查与开发探针

```powershell
.\build\test_schedule.exe
.\build\test_loader.exe .\build\Ra3FrameLab.dll
python .\tools\re\check_exports.py .\build\Ra3FrameLab.dll .\src\framelab.def
python .\tools\re\check_deps.py .\build\Ra3FpsTest.exe .\build\Ra3FrameLab.dll
# 仅在设置 FLAB_IMAGE 或提供自己的主程序时进行特征扫描：
python .\tools\re\dryscan.py .\src\framelab.cpp "$env:FLAB_IMAGE"
```

启动型探针需要明确的游戏目录和 `-Replay`，附加型探针需要明确 PID。它们会运行或操作游戏，执行前阅读脚本参数；离线检查不会启动游戏。脚本为测试回放生成唯一副本，保留输入回放。不会自动选取私人回放或假定安装语言。

CLI 必须与 `Ra3FrameLab.dll` 放在同一目录。自动化时显式提供 PID，例如 `flctl.exe status 0 <PID>`、`flctl.exe dryrun 90 <PID>`。原始 `disable` / `unload` 命令保留给研究，已知冻结风险见运行说明。

生成当前研究分支的候选包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -Version 0.2.1-research-r1 -Rebuild
```

默认包名使用 `0.2.1-research-r1`，CI 使用 `0.2.1-research-<提交短 SHA>`，与公开 `v0.2.1` 基线分开标识。本研究树拒绝使用无研究后缀的 `-Version 0.2.1`；GUI / DLL 内部数字版本仍为 `0.2.1`，不代表候选已发布或通过联机验收。未来正式版本需明确更新源码、内部版本和发布说明。

输出便携二进制 ZIP、对应源码 ZIP 和 SHA-256 清单。内容包括本许可、版权说明、README、技术报告与当前跨帧率研究记录；不包含作者的本地归档、日志、游戏或回放。CI 在 Windows 上重建和运行离线检查，游戏镜像扫描需由拥有游戏的开发者本地完成。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `src/` | DLL、六阶段调度、车身及实验模拟门控 |
| `tests/` | 纯数学调度测试、非游戏进程加载与拒绝安装测试 |
| `tools/gui/` | FpsTest 图形界面及嵌入资源 |
| `tools/flctl.cpp` | CLI 注入、控制与诊断 |
| `tools/*.ps1` | 构建、打包、按配置执行的开发探针 |
| `tools/re/` | 离线 PE、特征、符号与日志分析工具 |
| `docs/TECHNICAL.md` | 技术报告、240 FPS 对照、验证限制 |

## 许可与贡献

本项目采用 [Ra3 FrameLab 非商业源码许可 1.0](LICENSE)，**禁止商业使用**，允许符合许可条件的个人学习、非商业研究、修改和免费再分发。它是自定义的源码可用许可，**不是 GPL v3，也不标称 OSI 开源许可**。商业用途需要事先取得相关权利人的书面授权。

再分发源码或二进制必须保留 LICENSE、NOTICE、版权及修改说明，且不得绕过非商业限制。提交贡献即表示你有权提供该贡献，并同意按本项目许可分发；第三方材料仍受各自许可约束。EA 游戏内容及名称的权利不由本项目授予。

本次未复制 CnC-FPS-Unlocker 的 GPL 实现；技术报告仅引用其公开源码分析机制。来源和权利边界见 [NOTICE.md](NOTICE.md)。
