# ra3-framelab —— 新队员上手

> 红警3 1.12 的纯运行时内存补丁:把渲染帧率从 30 提到 60/90,逻辑帧率保持 15,不改模拟。
> 本文由主会话(Claude)于 2026-09-27 按代码逐行核对后写成。先读团队总览 `ra3-onboarding/README.md`。
> **更详细的换机文档是仓里的 `交接-给90帧机器.md`**,本文是它的精简入口。

---

## 1. 需要的环境

| 必需 | 版本 | 说明 |
|---|---|---|
| Visual Studio 2022 **BuildTools**(C++) | MSVC **14.44.35207**、Windows SDK **10.0.26100.0** | 构建脚本写死了这两个版本号,见第 5 节 |
| Git Bash | 随 Git for Windows | 主人实际用的构建入口是 bash 脚本 |
| Python 3 | 未限定 | 构建闸门只用标准库;`tools/re/` 的反汇编工具要 `capstone`,截图比对要 `Pillow` |
| 红警3 1.12 | 主程序 SHA-256 需与团队统一版本的代码段一致 | 特征码靠它定位 |

**不用** MinGW、CMake、Ninja。**必须 32 位**(游戏是 x86)。

**只想"用"这个补丁、不开发的队员**:直接向主人要编好的 `Ra3FpsTest.exe`,双击后用文件夹对话框选游戏目录,**不依赖任何写死路径**。

## 2. 构建

两个入口:

```powershell
# 正式入口(PowerShell,调 vcvars32.bat)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target all     # test|loader|ctl|dll|all
```
```bash
# 主人实际用的入口(Git Bash),多了 gui 目标和三道构建闸门
bash tools/re/build_msvc.sh all                                           # test|loader|ctl|dll|gui|all
```

产物在 `build\`:`Ra3FrameLab.dll`、`flctl.exe`、`test_schedule.exe`、`test_loader.exe`、`Ra3FpsTest.exe`。

## 3. 运行

离线自检(不用开游戏):
```powershell
build\test_loader.exe      # 期望 34 PASS
build\test_schedule.exe    # 期望 27 PASS
```

补丁本体 —— **先开游戏进到主菜单**,再:
```powershell
build\flctl.exe dryrun
build\flctl.exe enable 60      # 或 90
```

---

## 4. 克隆后必须改的路径

| 位置 | 现在指向 | 怎么改 |
|---|---|---|
| `tools/re/build_msvc.sh` 第 16 行 `ROOT=` | `G:/Ra3 FrameLab` | 改成你的仓目录(用了团队总览的 `subst G:` 办法就不用改) |
| `tools/re/build_msvc.sh` 第 19、21 行(MSVC)、第 23 行(SDK) | MSVC 14.44.35207、SDK 10.0.26100.0 | 改成你装的版本号(看 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\` 和 `C:\Program Files (x86)\Windows Kits\10\bin\` 下的文件夹名) |
| `tools/re/build_msvc.sh` 第 77、136 行 `PY=` | 主人的 Python | 改成 `python`(用 PATH 里的)。找不到时脚本**只打警告、跳过闸门**,不会报错 —— 容易误以为闸门过了 |
| `tools/re/build_msvc.sh` 第 88 行 | `G:/IDA/RA3_1.12.game` | 设环境变量 `FLAB_IMAGE` 指向你游戏的 `Data\ra3_1.12.game`(同一个文件,不需要 IDA) |
| `tools/build.ps1` 第 10 行 | VS BuildTools 默认安装位置 | 装在别处就改这行 |
| 17 个探针脚本(`tools/look.ps1` 第 49 行等)的 `$gameRoot` | 主人的游戏根 | **写在脚本体内、不是参数**,只能改脚本。完整清单见 `HARDCODED-PATHS.md` |
| 16 个探针脚本里的 `RA3_chinese_t_1.12.SkuDef` | 繁体中文版的启动配置 | 你的游戏不是繁中版的话,改成你游戏目录里实际有的 `*_1.12.SkuDef` |

其余写死路径见 **`HARDCODED-PATHS.md`**。

## 5. 常见坑

- `.ps1` 和 **`.def`** 都必须纯英文:`.def` 里的中文注释会让链接器吞掉下一行,导出名变成带修饰的形式。
- **游戏运行中执行 `flctl disable` 会让游戏冻住**,只能直接结束进程。
- 60 Hz 显示器上要显式给 `-Fps 60`,否则游戏整体只有 66% 速度。
- 远程桌面里窗口化会被限制在约 31 帧,全屏则不画场景 —— 测帧率别用远程桌面。
- 红警3 是单实例,第二个进程会在约 12 秒内自己退出。
- Python 管道输出中文乱码时设 `PYTHONIOENCODING=utf-8-sig`。
- 探针脚本需要两份指定名字的录像(`90 FPS Test.RA3Replay`、`最近的录像_PostComm.RA3Replay`),放在 `%USERPROFILE%\Documents\Red Alert 3\Replays\`,向主人要。

## 6. 和其它仓的关系

构建和运行都**不依赖**别的仓。
