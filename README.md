# Ra3 FrameLab / FpsTest

> 本研究分支为 **0.2.2-dev1** 跨帧率联机候选，提供FpsTest便携预发行。新版强制60/90模拟相位门，并分离私有诊断的UI与战场核心捕获。无限岛、全部真实客户端原生观战、两个凶残AI、50000资金的13组双开/三开及房主交换已实际结束，READY后合计95.023分钟；11组取得闭合有限载荷匹配，2组采集UNKNOWN保留。三开长窗包含原生30/60/90及FrameLab30/60/90，30、60、90档房主均有实际记录。旧不同步与889.593秒采集超时保留；完整状态、实际呈现率与跨机器联机仍未验证。详细结果见 [跨帧率研究记录](docs/CROSSFPS_RESEARCH.md)。

红警 3（Red Alert 3）1.12 的实验性客户端帧率工具。当前源码版本 **0.2.2-dev1**，图形界面提供 **30 / 60 / 90 FPS**，包含帧率、动画时钟、车身悬挂及诊断工具。DLL 通过运行时内存补丁工作。

**90 FPS 是当前上限。120 / 240 FPS 尚未实现。跨帧率联机仍在验收，本候选用于研究与测试，不能据此认定联机安全。** 新版为规范零售 30 / 逻辑 15 基线的 60 / 90 目标强制安装三处模拟相位边界门，并加强安装核验、接口串行化与失败还原记录；未改变六阶段调度上限。

## 下载与运行

从 [0.2.2-dev1 研究预发行](https://github.com/Ra3-Online/ra3-framelab/releases/tag/v0.2.2-dev1) 下载 `Ra3FrameLab-0.2.2-dev1-windows-x86.zip`，完整解压到可写目录，运行 `Ra3FpsTest.exe`。GUI内嵌对应版本DLL，普通用户无需安装Python、Visual Studio，也无需手工复制DLL到游戏目录。该候选包含本轮自动门控修复，适合按研究记录复测；有限匹配不能保证所有联机场景安全。

此前公开基线保留在 [v0.2.1](https://github.com/Ra3-Online/ra3-framelab/releases/tag/v0.2.1)，其默认模拟门配置与新版不同。两个发行的工件和验收结论分别记录。每个发行提供对应源码ZIP、技术报告、许可和SHA-256校验清单。

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

- 历史实机反馈包括60/90 FPS基础运行、动画和车身节拍修复。公开v0.2.1的便携化整理当时以重建和离线检查验收；当前候选的13组实机记录另列。
- 历史候选存在实际不同步：交换房主双端曾在3375逻辑帧分叉，NewMap三开在3150帧分叉，旧无限岛三开在7065帧出现90端与30/60端CRC差异，首个捕获字节差缩小到同一单位的武器子快照。原失败和原因覆盖限制保留；不能以新版有限匹配宣布历史原因全部排除。敌方单位偶尔不可见的旧反馈也未完成专门验收。
- 当前无限岛全观战矩阵实际完成13组、READY后合计95.023分钟，双开覆盖30/60、30/90、60/90及房主交换，并完成30/60/90三开长窗；共同完整自然CRC及所采raw未见差异、没有新的原生不同步。11组取得闭合有限记录；第4组C末scope缺失、第6组B的214.5969ms torn读取缺口均保留UNKNOWN。60档房主另有独立900秒三开，不能覆盖原缺口。此前新版889.593496秒10秒TTL失败未改判，R12独立核心采集在本轮测试中未再触发该TTL。所有客户端为原生观战、两个凶残AI、50000开局资金；逐帧完整状态、跨机器、无诊断DLL和实际Present仍未验收。
- 炉子等环境特效可能偏快。新增镜头与 Tint 修正、模拟相关实验组尚缺完整场景及联机验收。
- 目标帧率与实际帧率不一致时，现有基于显示帧计数的调度和视觉时间轴仍可能变慢。
- 公开 `v0.2.1` 的 `SIMPIN`、`SIMGATE` 默认关闭。`0.2.2-dev1` 在规范零售 30 / 逻辑 15 基线的 60 / 90 目标下，三处边界门不依赖请求掩码而自动必需；修改共享派生 FPS 时，三处逻辑高度读者也必须保持原版时间尺度。30 FPS 和 measure 模式不新增边界门。安装状态反映实际成功提交的补丁，不能作为联机安全证书。

完整机制、上游 240 FPS 对照和验收边界见 [技术报告](docs/TECHNICAL.md)。

## 从源码构建

需要 Windows、PowerShell 5.1 或更新、Visual Studio 2022 / Build Tools 的 **Desktop development with C++** 组件（MSVC x86 与 Windows SDK），以及 Python 3。普通构建与离线自检不需要游戏。

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -Target all
```

脚本自动发现 VS / SDK，以 x86 MSVC 构建。默认输出到仓库 `build`，包含 DLL、CLI、GUI 和三个测试程序。目录可以含中文和空格；CLI 和日志路径使用 Unicode 文件接口。依赖检查确保产物无需作者电脑上的环境或 VC 运行库安装路径。

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
.\build\test_sim_contract.exe
python .\tools\re\check_exports.py .\build\Ra3FrameLab.dll .\src\framelab.def
python .\tools\re\check_deps.py .\build\Ra3FpsTest.exe .\build\Ra3FrameLab.dll
# 仅在设置 FLAB_IMAGE 或提供自己的主程序时进行特征扫描：
python .\tools\re\dryscan.py .\src\framelab.cpp "$env:FLAB_IMAGE"
```

启动型探针需要明确的游戏目录和 `-Replay`，附加型探针需要明确 PID。它们会运行或操作游戏，执行前阅读脚本参数；离线检查不会启动游戏。脚本为测试回放生成唯一副本，保留输入回放。不会自动选取私人回放或假定安装语言。

`test_sim_contract` 使用自己声明的专用 PE 数据区作为模拟指令页。固定地址来自受检游戏指令的相对位移合同；使用前核验该页完整位于测试数组内且属于当前测试映像，条件不满足就拒绝运行。它不附加游戏、不覆盖未知内存，也不执行游戏函数。

CLI 必须与 `Ra3FrameLab.dll` 放在同一目录。自动化时显式提供 PID，例如 `flctl.exe status 0 <PID>`、`flctl.exe dryrun 90 <PID>`、`flctl.exe simstatus 0 <PID>`。查询命令的进程退出码就是返回值：正常 `status` 可为 30 / 60 / 90，`simstatus` 可为补丁位组合 15；不能一律把非零退出码当作查询失败。成功 `enable` 返回 0，高帧率安装后另打印 `SIMSTATUS`。原始 `disable` / `unload` 命令保留给研究，已知冻结风险见运行说明。

生成当前研究分支的候选包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -Version 0.2.2-dev1 -Rebuild
```

默认包名使用 `0.2.2-dev1`，CI 使用 `0.2.2-dev1-<提交短 SHA>`，与公开 `v0.2.1` 基线分开标识。打包检查源码、GUI 与 DLL 的实际版本一致，提取GUI的实际RCDATA/101资源并核验完整DLL字节及SHA，拒绝同版本但内容不同的载荷，并运行三项离线测试。本研究树拒绝冒用公开 `0.2.1` 标签或未经源码版本更新的正式版本号；候选标识不代表已通过联机验收。

输出便携二进制 ZIP、对应源码 ZIP 和 SHA-256 清单。内容包括本许可、版权说明、README、技术报告与当前跨帧率研究记录；不包含作者的本地归档、日志、游戏或回放。CI 在 Windows 上重建和运行离线检查，游戏镜像扫描需由拥有游戏的开发者本地完成。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `src/` | DLL、六阶段调度、车身及实验模拟门控 |
| `tests/` | 纯数学调度、非游戏进程加载、自有内存的边界门安装及故障还原合同测试 |
| `tools/gui/` | FpsTest 图形界面及嵌入资源 |
| `tools/flctl.cpp` | CLI 注入、控制与诊断 |
| `tools/*.ps1` | 构建、打包、按配置执行的开发探针 |
| `tools/re/` | 离线 PE、特征、符号与日志分析工具 |
| `docs/TECHNICAL.md` | 技术报告、240 FPS 对照、验证限制 |

## 许可与贡献

本项目采用 [Ra3 FrameLab 非商业源码许可 1.0](LICENSE)，**禁止商业使用**，允许符合许可条件的个人学习、非商业研究、修改和免费再分发。它是自定义的源码可用许可，**不是 GPL v3，也不标称 OSI 开源许可**。商业用途需要事先取得相关权利人的书面授权。

再分发源码或二进制必须保留 LICENSE、NOTICE、版权及修改说明，且不得绕过非商业限制。提交贡献即表示你有权提供该贡献，并同意按本项目许可分发；第三方材料仍受各自许可约束。EA 游戏内容及名称的权利不由本项目授予。

本次未复制 CnC-FPS-Unlocker 的 GPL 实现；技术报告仅引用其公开源码分析机制。来源和权利边界见 [NOTICE.md](NOTICE.md)。
