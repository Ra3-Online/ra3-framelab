# 归档:加「车身悬挂探针 / 30 Hz 节拍门」之前的那一版

**归档时间**:2026-09-21 · 悬挂路径会话(Claude)
**归档原因**:项目规矩「要改先归档」。本批要动 `src/framelab.cpp`、`src/framelab.def`、`tools/flctl.cpp`、
`tests/test_loader.cpp`、`tools/re/ring_analyze.py`,这里是它们改动前的原样(含当时的 `build/Ra3FrameLab.dll` 与 `flctl.exe`)。

## 这一版的状态

- 60 帧成立、动画经用户人眼确认正常(三条动画时间路径已修);90 帧已实现。
- ⚠ **这份 `src/framelab.cpp` 自己编不过**:`FrameLabMemScan` 里 `staticAddr / staticVal / nStatic` 声明在首次使用之后(C2065)。
  它是 2026-09-20 11:18 的最后一次源码改动;而同目录 `build/Ra3FrameLab.dll` 是 11:17 构建的 —— **早于**那次改动,
  所以 DLL 是好的、可用作回退基准,只是不含「静态兜底」。要从这份源码重建,先把那三行声明挪到使用之前。
- 默认掩码:DLL `0x28FFF`,GUI `0x2CFFF`。

## 回退方法

把 `src/`、`tests/`、`tools/` 下的文件覆盖回去(新增的 `src/chassis_gate.h` 删掉),再 `bash tools/re/build_msvc.sh all`;
或直接用本目录 `build/` 里的 DLL 与 flctl。本批改了什么见 `HANDOFF.md §20` 与 `CHANGES.md §AA`。
