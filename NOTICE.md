# Copyright, attribution and third-party notice

Copyright © 2026 Ra3 FrameLab contributors.

发行 GUI 署名“红警3online团队研发”。这是本项目的团队署名，不替代其他贡献者、第三方作者或原游戏权利人的署名。

项目贡献者拥有权利、且未单独标注其他许可的源码、文档及相应程序，自 2026-10-15 起按 GNU General Public License version 3 only（`GPL-3.0-only`）分发。完整、未经修改的 GPL v3 条款见根目录 [LICENSE](LICENSE)。本软件按 GPL v3 中的无保证及责任限制条款提供。

Project-owned source, documentation and resulting programs are licensed under the GNU General Public License version 3 only (`GPL-3.0-only`), except separately licensed materials. See [LICENSE](LICENSE) for the full terms, including the warranty disclaimer and limitation of liability.

[非商业使用倡议](NON_COMMERCIAL_REQUEST.md)仅表达开发者意愿，不是附加许可条件，不限制 GPL v3 授予的商业使用权。历史标签和发行包保留各自原有许可文件，不因本次调整重写。

原有技术注释中的日期与历史研究说明用于保留开发来源，不代表相应助手、平台或游戏厂商背书。

Command & Conquer、Red Alert 3 及游戏内容属于相应权利人。本项目与 EA 没有隶属或认可关系，不分发游戏主程序、游戏数据、专有调试符号或玩家回放。本项目许可不授予使用或再分发这些内容的权利。

技术报告参考了 [TheeHorse/CnC-FPS-Unlocker](https://github.com/TheeHorse/CnC-FPS-Unlocker) 的公开源码（比较基线 `7dc116f474fddee26e3b09499244b4923bffa6cb`）和发布说明。此次修改仅分析调度、计时及局部补丁的机制，未复制其 C#、x86 stub 或其他实现。该上游项目有自己的许可；本项目许可不代替第三方授权。后续若引入第三方代码，必须另行核对并保留其许可。

2026-10-28 发布核对：当前发行树、构建和内嵌载荷未直接包含 CnCFpsUnlocker / SAGE-Unlocked 原文件或 DLL；实现来源记录为参考公开机制后在本项目实现。确实阅读过上游源码，不能把这一记录表述为“完全没有参考”。上游 main 在本轮 API 核对时为 a770379，其源码头声明 GPL v3 or later。若后续采用或改编其受版权保护的代码，须遵守相应许可、保留版权与许可说明、标注修改并在分发二进制时提供对应源码及构建材料；语言转换本身不豁免这些义务。本项目继续 GPL-3.0-only 与非商业使用倡议，不增加强制禁止商用条款。

MSVC、Windows SDK、Python 及可选 Python 分析库由开发者自行安装，不随本项目源码授权。发行物使用 Windows 系统接口和静态 MSVC 运行库；具体运行库权利仍受 Microsoft 适用许可约束。

2026-10-27 highfps5 的动画 / 整体位移复核继续对照 SAGE-Unlocked a770379 的动画门、Topple 与物理显示思路。只读审查器、额外显示 alpha 适配、mode-0 包装修正、固定容量诊断和测试为本项目实现，未复制上游代码或发行其快照。可选反汇编研究依赖 capstone，开发者自行安装；该库不嵌入 GUI / DLL。范围与差异见 [复核报告](docs/HIGH_FPS_AUDIT.md)。

2026-10-27 右键卷屏修复参考同一 SAGE-Unlocked a770379 的 `PatchScrollBy` 位移归一思路。FrameLab 已有键盘 / 边缘修复，新增实现限定在右键局部乘法，避免重复缩放其他调用者；解析、C++ / x86 适配与测试为本地实现，没有复制上游虚表包装、共享临时缓冲或机器码生成器。

2026-10-27 炮塔研究另参考同一 SAGE-Unlocked a770379 提交的 `FindTurretInterp` / `PatchTurretInterp`，核对炮塔独立进度与原生角度缓存交接问题。FrameLab 以原生缓存最后一字保存显示代号，分离端点提交和连续插值，C++ 状态机及 x86 条件适配为本地实现，未复制上游补丁生成器或机器码。差异、日志与验证范围见高帧研究报告。

2026-10-26 的高帧研究参考 [SAGE-Unlocked a770379](https://github.com/TheeHorse/SAGE-Unlocked/tree/a770379e35bf814ae9a0954f192b96b60b800b33) 的默认原生批次、独立显示插值、网络等待和限帧余数设计，并对照 [CNCStuff/cnc3_fps_patch 4b8d499](https://github.com/CNCStuff/cnc3_fps_patch/tree/4b8d499ce6a11a8d774ac77d967eb9b4a9a0512f) 的时钟域及限帧说明。FrameLab 对应 C++ 状态机、x86 适配和合同测试为本轮本地实现，没有复制上游 C# 补丁生成器、其 x86 stub 或其他实现。具体机制、引用和差异见 [高帧研究报告](docs/HIGH_FPS_RESEARCH.md)。上游源码未打入发行包。

2026-10-08 的光效机制对照另参考了改名后的 [SAGE-Unlocked](https://github.com/TheeHorse/SAGE-Unlocked/tree/87d2481217379fe0592e020cdaf2b7ca0e94910d)，比较版本为 `87d2481217379fe0592e020cdaf2b7ca0e94910d`。仅分析粒子时间单位、更新节拍、绘制诊断和 Drawable pulse 的机制，没有复制其实现；第三方源码未纳入本项目发行树。

发布仓库保留正常 Git 提交历史；2026-10-03 的便携化整理从当前源码树移除了过时本机交接、私人协调记录及重复旧快照。原始记录在维护者本地另行保存，未重写早期提交。它们不属于本版发行包的使用说明。
