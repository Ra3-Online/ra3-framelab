# Environment setup

在仓库根目录运行 `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -Target all`。安装 Visual Studio 2022 或 Build Tools 的 Desktop development with C++（MSVC x86 和 Windows SDK），Python 3 放到 PATH 或设置 `FLAB_PYTHON`。构建入口通过 vswhere / VsDevCmd 发现安装位置，不要求特定驱动器或固定 SDK 小版本。可用环境变量 `FLAB_VSWHERE` / `FLAB_VSDEVCMD` 指定发现程序或开发环境脚本。

默认输出为仓库 `build`；`FLAB_BUILD_DIR` 可改输出位置。GUI 资源从本次输出中的 DLL 生成，避免嵌入其他目录的旧版本。Git Bash / MSYS 的 `tools/re/build_msvc.sh` 委托 PowerShell 构建入口；实际编译仍使用 Windows MSVC，不支持用 MinGW 替代裸函数钩子。

离线自检不需要游戏。设置 `FLAB_IMAGE` 后可扫描自己持有的受支持 1.12 主程序；未提供镜像时跳过此项，不把跳过写成通过。GUI 普通运行不需要开发环境。

开发探针用 `-GameRoot` / `FLAB_GAME_ROOT`，`-Image` / `FLAB_IMAGE`，`-SkuDef` / `FLAB_SKUDEF`，`-ReplayDir` / `FLAB_REPLAY_DIR` 配置游戏。SkuDef 自动发现只接受唯一匹配，存在多个时显式指定。回放必须传 `-Replay`，不会挑选最大文件或作者以前的回放。附加型探针必须传目标 PID。

可选逆向分析工具读取 `FLAB_IDA_DUMP` 指定的本地 IDA 文本输出，缓存放在当前构建目录；没有导出时相关符号工具明确报告缺少输入。视频 / 图像分析脚本需要自行安装其实际用到的 Pillow、NumPy、OpenCV 等 Python 库，使用前查看 import 与命令行帮助；它们不属于基础构建依赖。

DLL 默认在其所在目录的 `logs` 写日志；`RA3FL_LOG` 可覆盖完整文件路径，父目录需要事先创建。GUI 的配置使用运行时发现的用户目录，日志与载荷默认位于系统临时目录下的 `Ra3FrameLab`；`FLAB_RUNTIME_DIR` 可覆盖为可写目录。路径由参数、目录、环境或系统 API 获取，仓库没有作者安装位置的默认值。

请阅读 [README](README.md) 中的运行方式和风险，以及 [技术报告](docs/TECHNICAL.md) 中的版本指纹和验收边界。所有再分发须遵守 [非商业许可](LICENSE)。
