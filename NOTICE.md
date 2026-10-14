# Copyright, attribution and third-party notice

Copyright © 2026 Ra3 FrameLab contributors.

项目贡献者拥有权利、且未单独标注其他许可的源码、文档及相应程序，自 2026-10-15 起按 GNU General Public License version 3 only（`GPL-3.0-only`）分发。完整、未经修改的 GPL v3 条款见根目录 [LICENSE](LICENSE)。本软件按 GPL v3 中的无保证及责任限制条款提供。

Project-owned source, documentation and resulting programs are licensed under the GNU General Public License version 3 only (`GPL-3.0-only`), except separately licensed materials. See [LICENSE](LICENSE) for the full terms, including the warranty disclaimer and limitation of liability.

[非商业使用倡议](NON_COMMERCIAL_REQUEST.md)仅表达开发者意愿，不是附加许可条件，不限制 GPL v3 授予的商业使用权。历史标签和发行包保留各自原有许可文件，不因本次调整重写。

原有技术注释中的日期与历史研究说明用于保留开发来源，不代表相应助手、平台或游戏厂商背书。

Command & Conquer、Red Alert 3 及游戏内容属于相应权利人。本项目与 EA 没有隶属或认可关系，不分发游戏主程序、游戏数据、专有调试符号或玩家回放。本项目许可不授予使用或再分发这些内容的权利。

技术报告参考了 [TheeHorse/CnC-FPS-Unlocker](https://github.com/TheeHorse/CnC-FPS-Unlocker) 的公开源码（比较基线 `7dc116f474fddee26e3b09499244b4923bffa6cb`）和发布说明。此次修改仅分析调度、计时及局部补丁的机制，未复制其 C#、x86 stub 或其他实现。该上游项目有自己的许可；本项目许可不代替第三方授权。后续若引入第三方代码，必须另行核对并保留其许可。

MSVC、Windows SDK、Python 及可选 Python 分析库由开发者自行安装，不随本项目源码授权。发行物使用 Windows 系统接口和静态 MSVC 运行库；具体运行库权利仍受 Microsoft 适用许可约束。

2026-10-08 的光效机制对照另参考了改名后的 [SAGE-Unlocked](https://github.com/TheeHorse/SAGE-Unlocked/tree/87d2481217379fe0592e020cdaf2b7ca0e94910d)，比较版本为 `87d2481217379fe0592e020cdaf2b7ca0e94910d`。仅分析粒子时间单位、更新节拍、绘制诊断和 Drawable pulse 的机制，没有复制其实现；第三方源码未纳入本项目发行树。

发布仓库保留正常 Git 提交历史；2026-10-03 的便携化整理从当前源码树移除了过时本机交接、私人协调记录及重复旧快照。原始记录在维护者本地另行保存，未重写早期提交。它们不属于本版发行包的使用说明。
