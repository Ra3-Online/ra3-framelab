#!/usr/bin/env bash
# tools/re/build_msvc.sh -- 直接调 cl.exe 的构建路径(绕开 cmd/vcvars)
#
# 2026-09-17 动画定位会话:本机沙箱禁止从 Bash/PowerShell 工具调 cmd.exe,而 tools\build.ps1 靠
# `cmd /c` 跑 vcvars32.bat ⇒ 那条路在本环境走不通。vcvars32.bat 自己还会调 reg.exe(同样被禁)。
# 于是这里手工摆出 vcvars 会设的三样东西(PATH / INCLUDE / LIB),直接跑 cl.exe。
# **产物与 build.ps1 逐位等价**:同样的编译/链接开关,同样的输出路径与文件名。
# 保留 build.ps1 作为正式入口;本脚本只是本机沙箱下的等价替代。
#
# 用法: bash "G:/Ra3 FrameLab/tools/re/build_msvc.sh" [test|loader|ctl|dll|gui|all]
#   gui = Ra3FpsTest.exe(带界面的可移植测试工具)。它把 Ra3FrameLab.dll **内嵌**进自己,
#   所以必须先把 dll 构建出来;gui 目标会自己检查这一点。
set -u

TARGET="${1:-all}"
ROOT="G:/Ra3 FrameLab"
OUT="$ROOT/build"

MSVCU="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
SDKU="/c/Program Files (x86)/Windows Kits/10"
MSVCW='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
SDKW='C:\Program Files (x86)\Windows Kits\10'
VER='10.0.26100.0'

export PATH="$MSVCU/bin/Hostx64/x86:$SDKU/bin/$VER/x86:$PATH"
export INCLUDE="$MSVCW\\include;$SDKW\\Include\\$VER\\ucrt;$SDKW\\Include\\$VER\\um;$SDKW\\Include\\$VER\\shared;$SDKW\\Include\\$VER\\winrt"
export LIB="$MSVCW\\lib\\x86;$SDKW\\Lib\\$VER\\ucrt\\x86;$SDKW\\Lib\\$VER\\um\\x86"
# 2026-09-17:下面两个检查脚本会打中文。管道下 Python 默认按 locale(cp936)编码输出,
# 而这里的终端是 UTF-8 ⇒ 全是乱码,看着像失败。钉死成 UTF-8。
# ⚠ 2026-09-17 二次订正(踩了三轮才定位):
#   ① 光用 utf-8 不够 —— 没有 BOM 的 UTF-8 会被读取方按本地代码页(cp936)解码,中文照旧乱码。
#   ② 改用 utf-8-sig 也不够!实测发现:**Python 的 utf-8-sig 只在「它是这个文件的第一个
#      写入者」时才写 BOM**。本脚本里 cl.exe 先往同一个 stdout 写了东西 ⇒ 位置不在 0
#      ⇒ BOM 被跳过(隔离实验:先 `echo hi` 再跑 python → BOM@-1;python 先写 → BOM@0)。
#   ③ 于是改成:检查脚本各自写**自己的**日志文件(它们就是第一写入者 ⇒ 有 BOM),
#      主 stdout 只打纯 ASCII 摘要。`build\logs\_checks.txt` 才是给人读的检查报告。
export PYTHONIOENCODING=utf-8-sig

mkdir -p "$OUT"
cd "$OUT" || exit 2

fail=0

# 编译并核对「产物时间戳真的刷新了」—— 项目铁律:退出码 0 不算数。
# 参数: 产物名 然后是一条 cl 命令行(用 eval 展开)
build_one() {
    local artifact="$1"; shift
    local before=0
    [ -f "$artifact" ] && before=$(stat -c %Y "$artifact")
    MSYS_NO_PATHCONV=1 eval "$@"
    local rc=$?
    if [ $rc -ne 0 ]; then echo "FAIL($rc): $artifact"; fail=1; return; fi
    if [ ! -f "$artifact" ]; then echo "FAIL(no artifact): $artifact"; fail=1; return; fi
    local after; after=$(stat -c %Y "$artifact")
    if [ "$after" -le "$before" ]; then echo "FAIL(not refreshed): $artifact"; fail=1; return; fi
    echo "built: $artifact ($(stat -c %s "$artifact") bytes)"
}

if [ "$TARGET" = "test" ] || [ "$TARGET" = "all" ]; then
    build_one test_schedule.exe cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:test_schedule.exe "\"$ROOT/tests/test_schedule.cpp\""
fi

if [ "$TARGET" = "loader" ] || [ "$TARGET" = "all" ]; then
    build_one test_loader.exe cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:test_loader.exe "\"$ROOT/tests/test_loader.cpp\""
fi

if [ "$TARGET" = "ctl" ] || [ "$TARGET" = "all" ]; then
    build_one flctl.exe cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:flctl.exe "\"$ROOT/tools/flctl.cpp\"" /link advapi32.lib
fi

if [ "$TARGET" = "dll" ] || [ "$TARGET" = "all" ]; then
    build_one Ra3FrameLab.dll cl.exe /nologo /EHsc /W4 /O2 /MT /utf-8 /LD /Fe:Ra3FrameLab.dll "\"$ROOT/src/framelab.cpp\"" /link /DEF:"\"$ROOT/src/framelab.def\"" kernel32.lib user32.lib
    # 2026-09-17: 构建后逐名核对导出表。项目铁律「退出码 0 不算数」再补一条:
    # .def 里被吃掉一行时,链接照样成功、时间戳照样刷新,只有运行期 GetProcAddress 才炸。
    # 见 tools/re/check_exports.py 顶部的完整事故记录。
    if [ $fail -eq 0 ]; then
        PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"
        if [ -x "$PY" ]; then
            # 检查报告写独立文件(理由见上面的编码说明):truncate 之后 Python 就是第一写入者,
            # BOM 落在偏移 0 ⇒ 整个文件被正确识别为 UTF-8。第二段检查追加在后面,不需要再写 BOM。
            CHK="$OUT/logs/_checks.txt"
            : > "$CHK"
            if ! "$PY" "$ROOT/tools/re/check_exports.py" "$OUT/Ra3FrameLab.dll" "$ROOT/src/framelab.def" >> "$CHK" 2>&1; then fail=1; fi
            # 离线干扫:验证每一条特征码在原始 .game 里能不能定位。
            # 2026-09-17 加:那次 kSigClockAdvance 漏了一个字节,签名 0 命中,而构建、导出表、
            # 时间戳全部"通过" —— 只有真起游戏才会发现。这个检查把它提前到构建阶段。
            # 镜像路径可用 FLAB_IMAGE 覆盖;默认用 IDA 那份(与游戏主程序 SHA256 相同)。
            IMG="${FLAB_IMAGE:-G:/IDA/RA3_1.12.game}"
            if [ -f "$IMG" ]; then
                if ! "$PY" "$ROOT/tools/re/dryscan.py" "$ROOT/src/framelab.cpp" "$IMG" >> "$CHK" 2>&1; then fail=1; fi
            else
                echo "WARN: image not found: $IMG (skip dryscan; set FLAB_IMAGE)"
            fi
            # 主 stdout 只打 ASCII 摘要 —— 中文报告在 _checks.txt 里,避免编码来回踩。
            if [ $fail -eq 0 ]; then
                echo "checks OK (exports + dryscan) -> build/logs/_checks.txt"
            else
                echo "checks FAILED -> build/logs/_checks.txt (see below)"
                sed -n '1,40p' "$CHK"
            fi
        else
            echo "WARN: venv python not found, skip export check + dryscan"
        fi
    fi
fi

# ───────────────────────────── 可移植 GUI 测试工具 ─────────────────────────────
# 2026-09-17 新增。产物 Ra3FpsTest.exe 要拿到别的机器上跑,那台机器上什么环境都没有。
# 因此这里有三条与其它目标不同的要求,全部由 tools/re/check_deps.py 强制:
#   ① /MT(静态 CRT)  —— 不加的话 exe 依赖 vcruntime140.dll,在干净机器上直接起不来;
#   ② DLL 必须**内嵌**(.rc 里的 RCDATA) —— 否则"一个文件"的承诺就是假的;
#   ③ exe 里不能出现构建机的绝对路径 —— 链接器的 PDB 引用是最常见的泄漏源。
if [ "$TARGET" = "gui" ] || [ "$TARGET" = "all" ]; then
    if [ ! -f "$OUT/Ra3FrameLab.dll" ]; then
        echo "FAIL: build the DLL first -- bash tools/re/build_msvc.sh dll"
        fail=1
    else
        # rc.exe 在 SDK 的 bin 里(上面已经加进 PATH)。工作目录必须是 tools/gui:
        # .rc 里 RCDATA 的路径是相对**当前目录**解析的,站错地方 rc 仍然退出 0,
        # 只是产出一个没有载荷的 .res —— 那个故障只在"开始游戏"时才现形。
        ( cd "$ROOT/tools/gui" && MSYS_NO_PATHCONV=1 \
          rc.exe /nologo /fo "../../build/ra3fps_gui.res" ra3fps_gui.rc ) || { echo "FAIL: rc.exe"; fail=1; }
    fi

    if [ $fail -eq 0 ]; then
        # ★ .res 的路径必须**带引号传进去**:build_one 用 eval 展开,而这个仓库路径里有空格,
        #   不转义的话 "G:/Ra3 FrameLab/build/..." 会被切成两段,cl 报
        #   「无法识别的源文件类型"G:/Ra3"」并把它当目标文件 —— 链接照样可能成功,于是静默丢载荷。
        build_one Ra3FpsTest.exe cl.exe /nologo /EHsc /W4 /O2 /MT /utf-8 /DUNICODE /D_UNICODE \
            /Fe:Ra3FpsTest.exe "\"$ROOT/tools/gui/ra3fps_gui.cpp\"" "\"$OUT/ra3fps_gui.res\"" \
            /link /SUBSYSTEM:WINDOWS
    fi

    # 依赖 + 内嵌载荷 + 绝对路径三项一起查(理由见 check_deps.py 顶部的事故记录)。
    if [ $fail -eq 0 ]; then
        PY="C:/Users/Mithlan/.workbuddy-ai/binaries/python/envs/default/Scripts/python.exe"
        if [ -x "$PY" ]; then
            CHK2="$OUT/logs/_deps.txt"
            : > "$CHK2"
            if "$PY" "$ROOT/tools/re/check_deps.py" "$OUT/Ra3FpsTest.exe" "$OUT/Ra3FrameLab.dll" >> "$CHK2" 2>&1; then
                echo "deps OK (portable, self-contained) -> build/logs/_deps.txt"
            else
                echo "deps FAILED -> build/logs/_deps.txt (see below)"
                # tr -d 去掉 UTF-8 BOM:检查脚本是 _deps.txt 的第一写入者,PYTHONIOENCODING=utf-8-sig
                # 会给它写 BOM(文件本身是对的,记事本能正确识别中文),但回显到终端时那个字节
                # 会显示成乱码 —— 这个项目已经因为「输出看着像失败」白查过三轮,不留这种噪声。
                sed -n '1,40p' "$CHK2" | tr -d '\357\273\277'
                fail=1
            fi
        else
            echo "WARN: venv python not found, skip dependency check"
        fi
    fi
fi

exit $fail