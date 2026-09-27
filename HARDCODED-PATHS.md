# 写死的机器相关路径清单 —— Ra3 FrameLab

> 由 `ra3-onboarding/tools/scan_hardcoded_paths.py` 自动生成,2026-09-27。
> 扫描范围 = git 跟踪的文件 + 尚未提交但未被忽略的文件(也就是你 clone 下来拿到的那些),不含 third_party/ node_modules/ vendor/。
> 代码更新后可以自己重跑:`python <ra3-onboarding>\tools\scan_hardcoded_paths.py . -o HARDCODED-PATHS.md`

**怎么用这份清单:** 先按本仓 `SETUP.md` 把主流程跑起来 —— 那里列出的是「不改就跑不起来」的少数几处。
下面是全量清单:**「代码/数据」**一栏的行(程序、脚本、配置、会被读取的 json 等),只有当你要运行用到它的那个功能时才需要改;
**「注释/文档」**一栏不影响运行。归类是保守的:拿不准的一律算「代码/数据」(例如 Python 文档字符串里的路径)。

| 类别 | 代码/数据(文件数 / 行数) | 注释/文档(文件数 / 行数) | 怎么改 |
|---|---|---|---|
| 游戏本体路径(主人机器上的红警3安装目录) | 21 / 21 | 1 / 1 | 改成你自己的红警3安装目录(含 Data\ra3_1.12.game 的那一层);能用环境变量/设置页的优先用那个 |
| 主人的用户目录 | 22 / 26 | 3 / 3 | 改成你自己的用户目录,或改用 %USERPROFILE% / %APPDATA% / os.path.expanduser |
| 主人的盘符布局(兄弟仓、IDA 导出、参考资料在 G:/D: 盘的固定位置) | 23 / 30 | 28 / 79 | 改成你机器上对应仓/资料的实际位置;兄弟仓建议按 ra3-onboarding 的 clone-team.ps1 放在同一个父目录下 |
| 工具链安装位置 | 6 / 9 | 3 / 3 | 改成你自己装的工具链路径,或把它加进 PATH |

## 游戏本体路径(主人机器上的红警3安装目录) —— 代码/数据里的出现位置

怎么改:改成你自己的红警3安装目录(含 Data\ra3_1.12.game 的那一层);能用环境变量/设置页的优先用那个

- `archive/2026-09-17_90fps-works-animation-broken/tools/bisect.ps1`
  - 第 27 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/measure_ref.ps1`
  - 第 28 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/verify.ps1`
  - 第 37 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/watch.ps1`
  - 第 23 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/accept_90.ps1`
  - 第 66 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/animrate_ab.ps1`
  - 第 53 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/animslots_probe.ps1`
  - 第 50 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/bisect.ps1`
  - 第 27 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/chassis_probe.ps1`
  - 第 40 行:`[string]$GameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3",`
- `tools/desync_capture.ps1`
  - 第 48 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/determinism.ps1`
  - 第 48 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/flctl_probe.ps1`
  - 第 40 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/fps_capacity.ps1`
  - 第 45 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/jitter_probe.ps1`
  - 第 77 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/look.ps1`
  - 第 49 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/mask_fps_probe.ps1`
  - 第 41 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/measure_ref.ps1`
  - 第 28 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/memscan_probe.ps1`
  - 第 37 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/track_probe.ps1`
  - 第 94 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/verify.ps1`
  - 第 37 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/watch.ps1`
  - 第 23 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`

## 主人的用户目录 —— 代码/数据里的出现位置

怎么改:改成你自己的用户目录,或改用 %USERPROFILE% / %APPDATA% / os.path.expanduser

- `archive/2026-09-17_90fps-works-animation-broken/tools/bisect.ps1`
  - 第 27 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/measure_ref.ps1`
  - 第 28 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/verify.ps1`
  - 第 37 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/watch.ps1`
  - 第 23 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/accept_90.ps1`
  - 第 66 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/animrate_ab.ps1`
  - 第 53 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/animslots_probe.ps1`
  - 第 50 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/bisect.ps1`
  - 第 27 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/chassis_probe.ps1`
  - 第 40 行:`[string]$GameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3",`
  - 第 67 行:`$cand = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`
- `tools/desync_capture.ps1`
  - 第 48 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/determinism.ps1`
  - 第 48 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/flctl_probe.ps1`
  - 第 40 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/fps_capacity.ps1`
  - 第 45 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/jitter_probe.ps1`
  - 第 76 行:`$python   = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`
  - 第 77 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/look.ps1`
  - 第 49 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/mask_fps_probe.ps1`
  - 第 41 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/measure_ref.ps1`
  - 第 28 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/memscan_probe.ps1`
  - 第 37 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/re/build_msvc.sh`
  - 第 77 行:`PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"`
  - 第 136 行:`PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"`
- `tools/track_probe.ps1`
  - 第 93 行:`$python    = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`
  - 第 94 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/verify.ps1`
  - 第 37 行:`$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`
- `tools/watch.ps1`
  - 第 23 行:`$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"`

## 主人的盘符布局(兄弟仓、IDA 导出、参考资料在 G:/D: 盘的固定位置) —— 代码/数据里的出现位置

怎么改:改成你机器上对应仓/资料的实际位置;兄弟仓建议按 ra3-onboarding 的 clone-team.ps1 放在同一个父目录下

- `archive/2026-09-17_90fps-works-animation-broken/tools/compare_shots.py`
  - 第 15 行:`ROOT = sys.argv[1] if len(sys.argv) > 1 else r"G:\Ra3 FrameLab\build\bisect"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/diff_shots.py`
  - 第 16 行:`ROOT = sys.argv[1] if len(sys.argv) > 1 else r"G:\Ra3 FrameLab\build\bisect"`
- `archive/2026-09-17_90fps-works-animation-broken/tools/measure_ref.ps1`
  - 第 19 行:`[string]$Exe = "G:\参考源码\红警3平台增强\red-alert-3-60fps-mod-main\Data\60FPS\ra3_1.12.game",`
- `tools/accept_90.ps1`
  - 第 58 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_accept.txt"`
- `tools/animrate_ab.ps1`
  - 第 38 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_ab.txt",`
- `tools/animslots_probe.ps1`
  - 第 33 行:`[string]$OutDir = "G:\Ra3 FrameLab\build\logs"`
  - 第 47 行:`$lab       = "G:\Ra3 FrameLab"`
- `tools/compare_shots.py`
  - 第 15 行:`ROOT = sys.argv[1] if len(sys.argv) > 1 else r"G:\Ra3 FrameLab\build\bisect"`
- `tools/diff_shots.py`
  - 第 16 行:`ROOT = sys.argv[1] if len(sys.argv) > 1 else r"G:\Ra3 FrameLab\build\bisect"`
- `tools/flctl_probe.ps1`
  - 第 32 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_probe.txt",`
- `tools/fps_capacity.ps1`
  - 第 37 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_fpscap.txt"`
- `tools/jitter_probe.ps1`
  - 第 57 行:`[string]$OutDir = "G:\Ra3 FrameLab\build\logs"`
- `tools/live_probe.ps1`
  - 第 22 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_live.txt"`
- `tools/look.ps1`
  - 第 42 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_look.txt"`
- `tools/measure_ref.ps1`
  - 第 19 行:`[string]$Exe = "G:\参考源码\红警3平台增强\red-alert-3-60fps-mod-main\Data\60FPS\ra3_1.12.game",`
- `tools/memscan_probe.ps1`
  - 第 27 行:`[string]$OutDir = "G:\Ra3 FrameLab\build\logs"`
  - 第 34 行:`$lab       = "G:\Ra3 FrameLab"`
- `tools/re/build_msvc.sh`
  - 第 16 行:`ROOT="G:/Ra3 FrameLab"`
  - 第 88 行:`IMG="${FLAB_IMAGE:-G:/IDA/RA3_1.12.game}"`
- `tools/re/check_deps.py`
  - 第 25 行:`"G:\\Ra3 FrameLab\\build\\Ra3FpsTest.pdb". Harmless functionally, but the user's`
  - 第 160 行:`needles = [b"G:\\Ra3", b"C:\\Users", b"Mithlan", b".pdb"]`
- `tools/re/disasm.py`
  - 第 24 行:`IMAGE = r"G:\IDA\RA3_1.12.game"`
  - 第 25 行:`DUMP  = r"G:\IDA\RA3_1.12.game.c"`
- `tools/re/encl.py`
  - 第 13 行:`PATH = r"G:\IDA\RA3_1.12.game.c"`
- `tools/re/refs.py`
  - 第 8 行:`d=open(r'G:\IDA\RA3_1.12.game','rb').read()`
  - 第 16 行:`for line in open(r'G:\IDA\RA3_1.12.game.c',encoding='utf-8',errors='replace'):`
- `tools/read_series.ps1`
  - 第 29 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_series.txt"`
- `tools/track_probe.ps1`
  - 第 82 行:`[string]$OutDir = "G:\Ra3 FrameLab\build\logs"`
  - 第 89 行:`$lab       = "G:\Ra3 FrameLab"`
- `tools/watch_game.ps1`
  - 第 33 行:`[string]$Out = "G:\Ra3 FrameLab\build\logs\_watch.txt"`

## 工具链安装位置 —— 代码/数据里的出现位置

怎么改:改成你自己装的工具链路径,或把它加进 PATH

- `archive/2026-09-17_90fps-works-animation-broken/tools/build.ps1`
  - 第 10 行:`$bt = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"`
- `tools/build.ps1`
  - 第 10 行:`$bt = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"`
- `tools/chassis_probe.ps1`
  - 第 67 行:`$cand = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`
- `tools/jitter_probe.ps1`
  - 第 76 行:`$python   = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`
- `tools/re/build_msvc.sh`
  - 第 19 行:`MSVCU="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"`
  - 第 21 行:`MSVCW='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'`
  - 第 77 行:`PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"`
  - 第 136 行:`PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"`
- `tools/track_probe.ps1`
  - 第 93 行:`$python    = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"`

