// ============================================================================
//  ra3fps_gui.cpp -- Ra3FpsTest.exe
//
//  2026-09-17 / portable-GUI session (Claude)
//
//  WHAT THIS IS
//    A single self-contained Windows GUI tool that raises Red Alert 3's RENDER frame rate
//    to 30 / 60 / 90 by patching the RUNNING game's memory. Nothing on disk is modified:
//    no game file is replaced, nothing is written into the game folder, and the patch
//    disappears the moment the game process exits.
//
//  WHY IT EXISTS
//    The dev machine is 60 Hz, so it cannot show 90 fps. Deciding whether the 90 fps path
//    is finished therefore has to happen on another machine, and that machine has nothing
//    installed -- only the game itself. So this tool must be:
//      * ONE file.     The patcher DLL is embedded as an RCDATA resource and extracted to
//                      %TEMP% at run time. There is no side file to lose or mis-place.
//      * ZERO runtime dependencies. Built /MT (static CRT) and it imports only system DLLs
//                      (kernel32/user32/gdi32/shell32/ole32/comctl32/advapi32), all of which
//                      ship with Windows. Verified after every build with tools/re/check_deps.py.
//      * NO baked-in paths. The game folder is picked by the user; our own scratch files go
//                      to %TEMP%\Ra3FrameLab. Nothing knows about the machine it was built on.
//      * x86.          RA3 is a 32-bit process, and a 64-bit process cannot LoadLibrary into
//                      it. A 32-bit exe runs fine on 64-bit Windows.
//
//  THE THREE CONTROLS (exactly what was asked for)
//    1. 帧数           -- combo box, 30 / 60 / 90
//    2. 游戏目录        -- the game folder: read-only path box + 浏览... button (one control)
//    3. 开始游戏        -- launches the game, injects the DLL, applies the patch
//    The box underneath is output only (a log you can screenshot), not a control.
//
//  WHY 30/60/90 AND NOT ANY NUMBER
//    The engine's frame scheduler has exactly 6 interpolation stages per logic frame, and
//    logic runs at 15 Hz. So the ratio r = render / logic must be a whole number in 1..6,
//    i.e. the render rate must be a multiple of 15. 30/60/90 are r = 2/4/6. 120 would be
//    r = 8 and the DLL rejects it with error 23 on purpose.
//
//  WHAT THE TOOL DOES AFTER PATCHING
//    It keeps measuring the running game every ~10 s and prints render fps, logic fps, the
//    quarantined ms-per-frame global and the logic-rate global. That is what makes it a
//    TEST tool rather than just a launcher: "did 90 fps actually happen here" is answered
//    on screen instead of by eyeball. The verdict line says which of the two things went
//    wrong when something does: the patch, or the machine.
//
//  COMMAND LINE (optional; the GUI is the point)
//    Ra3FpsTest.exe --auto "<game folder>" <fps> [cycles]
//      Headless version of the same job, for scripted runs. Writes to stdout and to
//      %TEMP%\Ra3FrameLab\gui-*.log. Exits 0 on success, 1 on failure.
//
//  ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

#include "gui_res.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// ───────────────────────────── constants ─────────────────────────────

// The proven-good change-group mask (see src/framelab.cpp FL_G_*).
//   0xC1FF  = 0x1FF (patch set) | 0x4000 (animation ruler) | 0x8000 (clock fix)
//             -- every measurement in RE-动画推进点-2026-09-17.md was taken with this.
//   0x2C1FF = the same plus FL_G_ANIMSTEP (0x20000, the P6b animation-time-step fix).
//   0x2CFFF = the same plus FL_G_DERIVED2 (0xE00) -- SHIPPED since 2026-09-17 batch 7.
//
// 2026-09-17 (batch 6): P6b was split out of FL_G_VISUAL into its own bit, so the shipped mask
// moved from 0xC1FF to 0x2C1FF.
//
// 2026-09-17 (batch 7) -- THE ACTUAL ROOT CAUSE OF THE 60 fps BUILDING-ANIMATION BUG:
//   sub_6F6CB0 (0x006F6CB0) is the progress computation of the module literally named
//   "StructureUnpackUpdate" -- the building unpack / build-up animation. It divides a
//   DISPLAY FRAME COUNTER delta by (duration_ms * flt_CDBC5C), i.e. it mixes two different
//   time sources in one ratio, so the numerator and denominator must be in the SAME unit.
//   flt_CDBC5C (0x00CDBC5C) is "frames per millisecond" = client fps * 0.001 -- retail 0.03
//   because the retail client frame rate is 30. The patch raises the client frame rate to the
//   target but never touched that constant, so the progress ran TWICE AS FAST: the animation
//   is visible for the first half of the build and then sits at "finished" -- exactly the
//   "power plant animation is fine for the first half and broken for the second half"
//   report, and why Soviet building animations seemed to vanish.
//   The same argument holds for the other two members of the triple:
//     0x00CDBC50 (float)fps (30.0) / 0x00CDBC54 ms-per-frame (33.333) / 0x00CDBC5C (0.03).
//   All three are now re-based onto the target fps by FL_G_DERIVED2 (0xE00).
//   Why we know the engine tick really did reach 60: the clock accumulator is driven once per
//   RENDER frame while the engine advances the game clock once per CLIENT tick, and the
//   measured clockrate is 66666 us/logic-frame = 4 x 16.667 -- only possible if the engine
//   tick runs exactly 4 times per logic frame (2 would read ~33000).
//   NOTE (honest status): in the automated replay harness the display frame counter read 0 and
//   flctl blendrate saw 0 calls, i.e. those windows were not presenting frames, so this fix is
//   argued from the unit-consistency of sub_6F6CB0 plus the clockrate evidence, not from a
//   direct reading of the counter. A/B masks if it ever needs re-testing:
//     0x2CFFF = fixed (shipped)   0x2C1FF = triple OFF (batch-6 behaviour)
static const unsigned kGroups = 0x2CFFFu;

// 2026-09-21 (chassis-suspension session): OPTIONAL override of the groups mask, read once from
// the environment variable RA3FL_GROUPS (decimal or 0x-hex). Absent / empty / unparsable / 0
// => the shipped default above, i.e. nothing changes for a normal user.
// Why it exists: the new chassis bits (0x80000 probe, 0x100000 30 Hz cadence gate, 0x200000
// pose interpolation) are default-OFF until they are verified on a machine that really renders
// 90 fps -- and on that machine this single exe is often the only tool at hand. With this the
// A/B is one line in a console, no rebuild:
//     set RA3FL_GROUPS=0x3ACFFF  &&  Ra3FpsTest.exe      (shipped set + probe + gate + interpolation)
//     set RA3FL_GROUPS=0x1ACFFF  &&  Ra3FpsTest.exe      (shipped set + probe + gate, hold mode)
//     set RA3FL_GROUPS=          &&  Ra3FpsTest.exe      (back to the shipped default)
// The log states the effective mask AND whether it came from the override, so a log sent back
// to us can never be mistaken for a default run.
// 2026-09-21 (later the same day): the chassis-bounce fix is a CHECKBOX on the window, default ON.
//   Bits: 0x80000 probe (feeds the black box pose axis) | 0x100000 30 Hz cadence gate |
//   0x200000 pose interpolation. In-engine readings on the dev machine at target 60: recurrence
//   steps per logic frame 2000 (vanilla) / 3993 (unfixed) / 2003 (this setting); dominant pitch
//   frequency in logic time 1.6 Hz / 3.9 Hz / 1.8 Hz. Unchecked = exactly the old shipped mask.
//   An RA3FL_GROUPS override, when present, still wins over the checkbox (expert use).
static const unsigned kChassisFixBits = 0x380000u;
static volatile LONG g_chassisFix = 1;
// 2026-09-21:卷屏归一 —— 用户点名「做成可自选的」。默认**不勾**(= 现状:卷屏随帧率变快),勾上 = 键盘 / 边缘卷屏
//   每秒走的距离与原版相同。这是手感问题不是对错问题,而且实机还没验过,所以默认保持现状。
static const unsigned kScrollFixBits = 0x800000u;
static volatile LONG g_scrollFix = 0;
// 2026-09-22:用户在 90 帧实机报的两个新现象 ——「矿场熔炉 / 电厂灯口的光闪得特别快」和
//   「帝国武士开火像机关枪(但伤害没变)」。它们与车身颠簸是**同一类 bug**:某个视觉效果
//   每显示帧推进一步、不含 dt,于是 60/90 帧下时间轴被压缩 2/3 倍。
//   这一个复选框打包三位:0x400000 第四个帧率派生量(每显示帧的秒数,影响贴花淡出 / 拖尾贴图滚动 /
//   子物体淡出 / 精灵脉动)| 0x1000000 弹道流 30 Hz 节拍门(开火密度)| 0x2000000 状态图标乒乓。
//   打包而不是三个勾:对用户它们是同一件事;要二分仍可用 flctl 逐位控制。
//   **默认不勾**:三位都还没在游戏里跑过,车身那一项刚验收通过,不要把变量混在一起。
// 2026-09-23:打包里再加 0x4000000 粒子系统节拍门 —— 用户实测「光仍然闪得快」,而同包的弹道流已修好
//   ⇒ 0x400000(RadiusDecal)不是那盏光;真凶是粒子系统管理器每显示帧推进一步(见 framelab.cpp FL_G_PSYS)。
// 2026-09-26:再加 0x8000000 过场运镜计时 —— 用户实测战役过场「镜头一下就过去、到了地方等飞机」,约快 3 倍。
static const unsigned kFxFixBits = 0xF400000u;
static volatile LONG g_fxFix = 0;
static bool g_groupsOverridden = false;
static unsigned EnvGroups();
static unsigned EffectiveGroups() {
    const unsigned env = EnvGroups();
    if (g_groupsOverridden) return env;
    return kGroups | (g_chassisFix ? kChassisFixBits : 0u) | (g_scrollFix ? kScrollFixBits : 0u)
                   | (g_fxFix ? kFxFixBits : 0u);
}
static unsigned EnvGroups() {
    static bool resolved = false;
    static unsigned value = kGroups;
    if (!resolved) {
        resolved = true;
        wchar_t buf[40] = {0};
        const DWORD n = GetEnvironmentVariableW(L"RA3FL_GROUPS", buf, 40);
        if (n > 0 && n < 40) {
            wchar_t* end = NULL;
            const unsigned long v = wcstoul(buf, &end, 0);
            while (end && (*end == L' ' || *end == L'\t')) ++end;
            if (v != 0 && end && *end == 0) { value = (unsigned)v; g_groupsOverridden = true; }
        }
    }
    return value;
}

// Base name of the patcher DLL. The file we actually write to disk carries a per-process
// suffix (see ExtractDll) so that two copies of this tool -- or two sessions on a shared
// machine -- can never fight over one file, because Windows refuses to overwrite a DLL that
// is already mapped. The MODULE name inside the target process is that same file name, so
// every lookup below must use g_dllModule (set by ExtractDll), never this constant.
static const wchar_t* kDllName = L"Ra3FrameLab.dll";

// How long to keep retrying FrameLabEnable() while the engine finishes starting up.
// Before the engine runs, the global 0xCE176C is not yet 33, and the DLL correctly refuses
// to install (it returns 24, "state does not match expectation") rather than guessing.
// Measured: the writer runs a few seconds after the process starts.
static const int kEnableTimeoutMs = 180000;

// Measurement cadence. fpswin/logicfps each block for kMeasureWindowSec INSIDE the game.
static const int kMeasureCycleMs   = 10000;
static const int kMeasureWindowSec = 4;

// Logic rate the simulation must keep, and its tolerance. 15 Hz is the engine's logic rate
// and the whole point of the patch is that this number does NOT move.
static const int kLogicFpsX100     = 1500;
static const int kLogicFpsTolX100  = 30;

// ───────────────────────────── state ─────────────────────────────

static HINSTANCE g_hInst      = NULL;
static HWND      g_hMain      = NULL;
static HWND      g_hFps       = NULL;
static HWND      g_hDir       = NULL;
static HWND      g_hBrowse    = NULL;
static HWND      g_hStart     = NULL;
static HWND      g_hLog       = NULL;
static HWND      g_hBbDump    = NULL;   // 「立即保存现场」
static HWND      g_hBbTest    = NULL;   // 「记录器自检」
static HWND      g_hChassis   = NULL;   // 「修复载具颠簸」复选框(2026-09-21)
static HWND      g_hScroll    = NULL;   // 「卷屏保持原版速度」复选框(2026-09-21,默认不勾)
static HWND      g_hFx        = NULL;   // 「修复过快的视觉效果」复选框(2026-09-22,默认不勾)
static HFONT     g_hFont      = NULL;

static volatile LONG g_busy   = 0;   // a job is running; disable the controls
static volatile LONG g_stop   = 0;   // window is closing; the worker should bail out
// Outcome of the last job, only used by --gui-autostart to pick a process exit code.
static bool      g_lastJobOk  = false;
static bool      g_auto       = false;
// ── --gui-autostart: drive the REAL GUI controls from the command line ──
//
// Why this exists: --auto and the GUI share RunJob(), but they do NOT share the code that reads
// the controls, validates the folder and starts the worker thread. That glue is exactly what a
// user on another machine exercises first, so it needs to be testable without a human clicking.
// This mode fills the combo box and the folder box, then posts the same WM_COMMAND a click
// would, so the button handler runs verbatim.
static bool      g_selfTest   = false;
static int       g_selfTestCycles = 3;
static int       g_selfTestFps    = 60;
static std::wstring g_selfTestDir;
// Timer id used to click 开始游戏 once the window is up. Kept out of the resource header because
// it is an implementation detail, not a dialog control id.
#define kSelfTestTimer 1
static HANDLE    g_hLogFile   = INVALID_HANDLE_VALUE;

static std::wstring g_tempDir;       // %TEMP%\Ra3FrameLab
static std::wstring g_dllPath;       // where we extracted the embedded DLL this run
static std::wstring g_dllModule;     // file name of g_dllPath = the module name to look up
static int          g_dllSeq  = 0;

// ── 黑匣子(2026-09-18 第十一批)──────────────────────────────────────────────────
// 用户在**只有游戏本体**的机器上测试,GUI 是唯一入口 —— 只挂在 flctl.exe 上的记录器等于
// 没有。所以「自动开启 + 两个按钮」才是记录器真正可用的那一半。
//   g_gamePid     已打上补丁的游戏 pid(0 = 还没有)。两个按钮都要它才能工作。
//   g_bbArmed     飞行记录仪是否已经开过 —— 测量循环每 10 秒跑一轮,不能每轮都重复开
//                 (开一次会清空环形缓冲,重复开就等于永远只有 0 行)。
//   g_bbSeen      已经报过的崩溃报告文件名。每轮只报**新出现的**,否则同一份报告会被刷十几遍,
//                 把日志里真正有用的部分挤掉。
static DWORD     g_gamePid    = 0;
static bool      g_bbArmed    = false;
static std::vector<std::wstring> g_bbSeen;

// 黑匣子辅助函数的**前向声明**。它们定义在文件靠后的位置(紧挨着 AppendLog,因为要用 LogF),
// 而 JobMeasureLoop 在它们之前就要调用。签名必须与定义逐字一致。
static int  AnnounceNewReports(std::wstring* fullPath);
static void UpdateBbButtons();

// ───────────────────────────── logging ─────────────────────────────

// Append UTF-8 to the log file. The file is the authoritative record: the GUI box can be
// scrolled away and a console can garble Chinese, but the file is always readable.
static void LogFileWrite(const wchar_t* s) {
    if (g_hLogFile == INVALID_HANDLE_VALUE) return;
    char utf8[4096];
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, (int)sizeof utf8 - 1, NULL, NULL);
    if (n <= 1) return;
    DWORD wrote = 0;
    WriteFile(g_hLogFile, utf8, (DWORD)(n - 1), &wrote, NULL);
}

// Write one line to the console in --auto mode.
//
// Two things were wrong with the obvious approach (freopen "CONOUT$" + fputws):
//   * fputws converts to the console's ANSI codepage, so Chinese came out as nothing at all
//     in a non-Chinese console -- the log file was fine, the screen was not;
//   * it does nothing useful when stdout is redirected to a file.
// WriteConsoleW takes wide characters directly and does not care about the codepage, so it is
// correct on a real console. When stdout is a pipe or a file we write UTF-8 bytes instead.
static void ConsoleOut(const wchar_t* s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD w = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &w, NULL);
        return;
    }
    char utf8[4096];
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, (int)sizeof utf8 - 1, NULL, NULL);
    if (n > 1) { DWORD w = 0; WriteFile(h, utf8, (DWORD)(n - 1), &w, NULL); }
}

static void LogLine(const wchar_t* s) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamped[2200];
    _snwprintf_s(stamped, _countof(stamped), _TRUNCATE, L"[%02u:%02u:%02u] %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, s);
    LogFileWrite(stamped);

    if (g_auto) {
        ConsoleOut(stamped);
    } else {
        // UI work must happen on the UI thread, so hand the string over and let WndProc free it.
        const size_t bytes = (wcslen(stamped) + 1) * sizeof(wchar_t);
        wchar_t* heap = (wchar_t*)malloc(bytes);
        if (heap) {
            memcpy(heap, stamped, bytes);
            if (!PostMessageW(g_hMain, WM_APP_LOG, 0, (LPARAM)heap)) free(heap);
        }
    }
}

static void LogF(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogLine(buf);
}

// ───────────────────────────── small helpers ─────────────────────────────

static bool FileExistsW(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExistsW(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

static std::wstring TrimSlash(std::wstring s) {
    while (s.size() > 3 && (s[s.size() - 1] == L'\\' || s[s.size() - 1] == L'/')) s.erase(s.size() - 1);
    return s;
}

static std::wstring ParentDir(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    if (k == std::wstring::npos || k < 3) return p;
    return p.substr(0, k);
}

static std::wstring FileName(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return (k == std::wstring::npos) ? p : p.substr(k + 1);
}

static std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring r = TrimSlash(a);
    r += L"\\";
    r += b;
    return r;
}

static std::wstring Quote(const std::wstring& s) {
    std::wstring r = L"\"";
    r += s;
    r += L"\"";
    return r;
}

// Pull the LAST dotted number out of a file name and turn it into a comparable integer:
//   "ra3_1.12.game"                -> 1012
//   "RA3_chinese_t_1.12.SkuDef"    -> 1012
//   "RA3_1.0.game"                 -> 1000
// Returns -1 when there is no version-looking number. Used to pair a .game with the SkuDef
// that belongs to the same version, and to pick the newest .game when several are present.
//
// TWO traps this function has already fallen into, both fixed here:
//   * "ra3_1.12.game" -- the '3' in "ra3" is not a version. Rejecting a digit run that is glued
//     to a letter is what keeps that 3 out.
//   * the version token must be consumed AS A WHOLE. Consuming only the digits before the dot
//     and then resuming INSIDE the decimals turns "1.12" into the two tokens 1 and 12, and
//     "12" scores 12000 -- higher than the real 1012. That produced a bogus "not version 1.12"
//     warning on a perfectly normal install.
// "Last wins" (not "largest wins") is deliberate: the version is the number closest to the
// extension, and "largest" is exactly what the 12-vs-1012 confusion exploited.
static int VersionScore(const std::wstring& name) {
    int last = -1;
    size_t i = 0;
    while (i < name.size()) {
        if (!iswdigit(name[i])) { ++i; continue; }
        if (i > 0 && iswalpha(name[i - 1])) {                  // "ra3": glued to a letter, skip
            while (i < name.size() && iswdigit(name[i])) ++i;
            continue;
        }
        size_t j = i;
        while (j < name.size() && iswdigit(name[j])) ++j;
        int major = _wtoi(name.substr(i, j - i).c_str());
        int minor = 0;
        if (j < name.size() && name[j] == L'.') {
            size_t k = j + 1;
            while (k < name.size() && iswdigit(name[k])) ++k;
            if (k > j + 1) minor = _wtoi(name.substr(j + 1, k - j - 1).c_str());
            if (minor > 999) minor = 999;
            j = k;                                             // consume the decimals too
        }
        last = major * 1000 + minor;
        i = j;
    }
    return last;
}

// ───────────────────────────── game discovery ─────────────────────────────

struct GameFind {
    // NOTE: never end a // comment with a backslash -- it continues the comment onto the next
    // line and silently comments out the following declaration (this exact mistake made the
    // `exe` member vanish and produced 20 bogus compile errors the first time).
    std::wstring root;      // the folder that contains the Data subfolder
    std::wstring exe;       // the real game executable, e.g. <root>/Data/ra3_1.12.game
    std::wstring skudef;    // the -config argument (may be empty on unusual installs)
    std::wstring launcher;  // <root>/RA3.exe, used only as a fallback
};

// Highest-version match of `pattern` inside `dir`. We do our own suffix check instead of
// trusting FindFirstFile's wildcard: on Windows "*.SkuDef" also matches names that merely
// START with that extension, and the game folder holds .bak / .metabak copies of every
// SkuDef, so a loose match would happily pick a backup.
static bool FindBestIn(const std::wstring& dir, const wchar_t* pattern,
                       const wchar_t* mustEndWith, std::wstring& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    int best = -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!EndsWithNoCase(fd.cFileName, mustEndWith)) continue;
        if (wcsstr(fd.cFileName, L".bak") || wcsstr(fd.cFileName, L".metabak")) continue;
        const int score = VersionScore(fd.cFileName);
        if (score > best) {
            best = score;
            out = Join(dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best >= 0;
}

static bool FindGame(const std::wstring& userDir, GameFind& g, std::wstring& err) {
    const std::wstring dir = TrimSlash(userDir);

    // The user may point at the game root OR straight at its Data subfolder. Accept both.
    std::vector<std::wstring> roots;
    roots.push_back(dir);
    if (EndsWithNoCase(dir, L"\\data") || EndsWithNoCase(dir, L"/data")) roots.push_back(ParentDir(dir));

    for (size_t i = 0; i < roots.size() && g.exe.empty(); ++i) {
        const std::wstring& r = roots[i];
        if (FindBestIn(Join(r, L"Data"), L"*.game", L".game", g.exe) ||   // normal install
            FindBestIn(r, L"*.game", L".game", g.exe)) {                  // flat install
            g.root = r;
        }
    }
    if (g.exe.empty()) {
        err = L"在所选目录里找不到游戏主程序（*.game）。请选择包含 Data 子目录的那个文件夹，"
              L"例如 ...\\Red Alert 3。";
        return false;
    }

    // Pair the SkuDef with the .game version when possible; otherwise take the newest one.
    std::wstring anySku;
    if (FindBestIn(g.root, L"*.SkuDef", L".SkuDef", anySku)) g.skudef = anySku;
    const int exeVer = VersionScore(FileName(g.exe));
    if (exeVer >= 0) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(g.root, L"*.SkuDef").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!EndsWithNoCase(fd.cFileName, L".SkuDef")) continue;
                if (wcsstr(fd.cFileName, L".bak")) continue;
                if (VersionScore(fd.cFileName) == exeVer) { g.skudef = Join(g.root, fd.cFileName); break; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    if (FileExistsW(Join(g.root, L"RA3.exe"))) g.launcher = Join(g.root, L"RA3.exe");
    return true;
}

// ───────────────────────────── process helpers ─────────────────────────────

struct ProcEntry { DWORD pid; std::wstring name; };

static void ListProcesses(std::vector<ProcEntry>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.name = pe.szExeFile;
            out.push_back(e);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// "A game is running" = a process whose name ends in .game (ra3_1.12.game), or the launcher.
// We check BEFORE launching so we never inject into a session somebody else started -- on the
// dev box several agents share this game, and that rule is written down in HANDOFF.md.
static bool GameRunning(std::wstring& which) {
    std::vector<ProcEntry> ps;
    ListProcesses(ps);
    for (size_t i = 0; i < ps.size(); ++i) {
        if (EndsWithNoCase(ps[i].name, L".game") || _wcsicmp(ps[i].name.c_str(), L"RA3.exe") == 0) {
            wchar_t b[160];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%s (pid %lu)", ps[i].name.c_str(), ps[i].pid);
            which = b;
            return true;
        }
    }
    return false;
}

static HMODULE RemoteModuleBase(DWORD pid, const wchar_t* dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    HMODULE found = NULL;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName) == 0) { found = me.hModule; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Run one function inside the target process and return its EAX. 32-bit only, which is fine:
// both this tool and the game are x86.
static bool RemoteCall(HANDLE proc, void* fn, void* arg, DWORD* result, DWORD timeoutMs) {
    HANDLE th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (!th) return false;
    const DWORD wr = WaitForSingleObject(th, timeoutMs);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    if (wr != WAIT_OBJECT_0) return false;      // still running: treat as failure, do not free arg
    if (result) *result = code;
    return true;
}

// LoadLibraryW (not A) so a game folder or user name with non-ANSI characters still works.
static bool InjectDll(HANDLE proc, const std::wstring& dllPath, std::wstring& err) {
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { err = L"VirtualAllocEx 失败"; return false; }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, &wrote) || wrote != bytes) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        err = L"WriteProcessMemory 失败";
        return false;
    }
    void* loadLib = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    DWORD r = 0;
    const bool ok = loadLib && RemoteCall(proc, loadLib, remote, &r, 60000) && r != 0;
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (!ok) { err = L"远程 LoadLibraryW 失败（被杀软拦截？或游戏以管理员身份运行？）"; return false; }
    return true;
}

// Remote address of one of our exports: local base + (local export offset). Same trick flctl
// uses, and the reason the DLL is also loaded into THIS process.
static void* RemoteExport(DWORD pid, HMODULE localBase, const char* name) {
    // g_dllModule, not kDllName: the extracted file name has a per-process suffix. Using the
    // constant here silently returns NULL and the whole install looks like a timeout.
    HMODULE remoteBase = RemoteModuleBase(pid, g_dllModule.c_str());
    if (!remoteBase || !localBase) return NULL;
    void* local = (void*)GetProcAddress(localBase, name);
    if (!local) return NULL;
    return (void*)((unsigned char*)remoteBase + ((unsigned char*)local - (unsigned char*)localBase));
}

// Human-readable meaning of the DLL's status codes (see enum FlStatus in src/framelab.cpp).
static const wchar_t* Explain(int rc) {
    switch (rc) {
    case 0:  return L"成功";
    case 2:  return L"之前已经装过了";
    case 10: return L"找不到游戏模块（进程还没起来？）";
    case 11: return L"游戏主程序不是有效的 PE";
    case 20: return L"特征码没找到 —— 这个游戏不是 1.12 版（本补丁只支持 1.12）";
    case 21: return L"特征码命中多处，不敢下手";
    case 22: return L"帧率全局量还不是 15/30（引擎还没初始化）";
    case 23: return L"目标帧率不是 15 的整数倍，或超出支持范围";
    case 24: return L"内存状态与预期不符（引擎还没初始化完，或版本不对）";
    case 30: return L"写内存失败（已回滚）";
    case 31: return L"挂起其它线程失败";
    default: return L"未知错误";
    }
}

// Retryable = "the engine is not ready yet" or "transient". Anything else is a real verdict.
static bool Retryable(int rc) {
    return rc == 10 || rc == 22 || rc == 24 || rc == 30 || rc == 31;
}

// ───────────────────────────── embedded DLL extraction ─────────────────────────────

// Drop the RCDATA payload into %TEMP%\Ra3FrameLab. We deliberately never write next to the
// exe (it may sit on a read-only stick) and never into the game folder (project rule).
// A fresh file name per attempt matters: Windows refuses to overwrite a DLL that some
// process has already mapped, and we map it ourselves to compute export offsets.
// Delete stale copies of our own DLL from the temp folder.
//
// Every run writes a NEW file name (see ExtractDll), so a machine that runs this tool a few
// times accumulates 144 KB files. We only remove files that are (a) ours by name and (b) older
// than an hour -- an hour is far longer than any run, so a second copy of the tool that is
// mid-run can never lose the DLL it just wrote. Failures are ignored on purpose: a DLL that
// some process still has mapped simply refuses to be deleted, which is fine.
static void SweepStaleTempDlls() {
    WIN32_FIND_DATAW fd;
    const std::wstring pat = g_tempDir + L"\\Ra3FrameLab_g*.dll";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    // "Now" in the same units as FILETIME: 100 ns ticks since 1601-01-01 UTC.
    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER now;
    now.LowPart = ftNow.dwLowDateTime;
    now.HighPart = ftNow.dwHighDateTime;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER wrote;
        wrote.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        wrote.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart <= wrote.QuadPart) continue;   // clock skew: leave it alone
        const ULONGLONG ageMs = (now.QuadPart - wrote.QuadPart) / 10000ULL;
        if (ageMs < 3600000ULL) continue;               // younger than an hour: leave it alone
        DeleteFileW((g_tempDir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool ExtractDll(std::wstring& err) {
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_FRAMELAB_DLL), RT_RCDATA);
    if (!res) { err = L"exe 里找不到内嵌的 DLL 资源"; return false; }
    const DWORD size = SizeofResource(NULL, res);
    HGLOBAL hg = LoadResource(NULL, res);
    const void* data = hg ? LockResource(hg) : NULL;
    if (!size || !data) { err = L"内嵌 DLL 资源读取失败"; return false; }

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) { err = L"取不到临时目录"; return false; }
    g_tempDir = TrimSlash(tmp);
    g_tempDir += L"\\Ra3FrameLab";
    CreateDirectoryW(g_tempDir.c_str(), NULL);
    SweepStaleTempDlls();   // keep the folder from growing one 144 KB DLL per run

    wchar_t name[64];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"Ra3FrameLab_g%lu_%d.dll",
                 (unsigned long)GetCurrentProcessId(), ++g_dllSeq);
    g_dllPath = Join(g_tempDir, name);
    // Remember the file name we chose: it is also the module name the loader will register in
    // both this process and the game, and every RemoteModuleBase/GetModuleHandle lookup needs it.
    g_dllModule = name;

    HANDLE f = CreateFileW(g_dllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"无法写入临时目录 ";
        err += g_dllPath;
        return false;
    }
    DWORD wrote = 0;
    const bool ok = WriteFile(f, data, size, &wrote, NULL) && wrote == size;
    CloseHandle(f);
    if (!ok) { err = L"写出 DLL 失败"; return false; }
    LogF(L"已释放补丁 DLL 到 %s（%lu 字节）", g_dllPath.c_str(), (unsigned long)size);
    return true;
}

// ───────────────────────────── bring the game forward ─────────────────────────────
//
// The engine only advances its client frame loop while its window is actually on screen and
// in the foreground. A game launched from a tool comes up BEHIND the tool window, and then the
// render measurement reads a flat 0.00 fps no matter what the patch does -- which looks exactly
// like a broken patch. Bringing the window forward is therefore not a cosmetic nicety, it is
// required for the measurement to mean anything.

struct FindWndCtx { DWORD pid; HWND hwnd; };

static BOOL CALLBACK FindGameWndProc(HWND h, LPARAM lp) {
    FindWndCtx* c = (FindWndCtx*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    // Visible, top-level (no owner), and actually titled: that is the game's main window, not
    // one of the many hidden message-only windows the engine creates.
    if (wpid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL &&
        GetWindowTextLengthW(h) > 0) {
        c->hwnd = h;
        return FALSE;   // stop enumerating
    }
    return TRUE;
}

// True when the window that currently has keyboard focus belongs to the game process.
//
// This is the single most useful diagnostic for a 0.00 fps reading: the engine advances its
// client frame loop only while its own window is in the foreground, so "0.00 fps" almost always
// means "the game is not in front" (alt-tabbed, or hidden behind the tool) rather than a broken
// patch. Reporting it turns an alarming zero into an obvious instruction.
static bool GameIsForeground(DWORD pid) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD wpid = 0;
    GetWindowThreadProcessId(fg, &wpid);
    return wpid == pid;
}

static void BringGameToFront(DWORD pid, DWORD timeoutMs) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeoutMs && !g_stop) {
        FindWndCtx c;
        c.pid = pid;
        c.hwnd = NULL;
        EnumWindows(FindGameWndProc, (LPARAM)&c);
        if (c.hwnd) {
            // Only un-minimise if it really is minimised. Calling SW_RESTORE on a game window that
            // is already up is an unnecessary window-state change, and for a fullscreen D3D9 game
            // that can force a device reset -- i.e. the tool would disturb the thing it measures.
            if (IsIconic(c.hwnd)) ShowWindow(c.hwnd, SW_RESTORE);
            // ★ Windows only lets a thread change the foreground window if that thread owns it
            //   (or meets one of a few other conditions). Our worker thread does not -- and in
            //   GUI mode the TOOL's own window is the foreground one, so a plain
            //   SetForegroundWindow here silently returns FALSE and the user gets 0.00 fps
            //   forever. Temporarily attaching our input queue to the foreground thread's makes
            //   the call legal; we detach immediately afterwards.
            DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
            const DWORD myThread = GetCurrentThreadId();
            bool attached = false;
            if (fgThread && fgThread != myThread)
                attached = AttachThreadInput(myThread, fgThread, TRUE) != 0;
            const BOOL ok = SetForegroundWindow(c.hwnd);
            if (attached) AttachThreadInput(myThread, fgThread, FALSE);

            if (ok) {
                LogLine(L"已把游戏窗口切到前台（引擎在前台才会持续渲染，否则渲染帧率会一直是 0）。");
            } else {
                LogLine(L"[注意] 游戏窗口已在，但没能自动切到前台。请手动点一下游戏窗口。");
            }
            return;
        }
        Sleep(500);
    }
    LogLine(L"[注意] 没找到游戏窗口。请手动让游戏窗口到前台，否则渲染帧率会读到 0。");
}

// Current refresh rate of the primary display, or 0 if Windows will not tell us.
//
// This is worth reporting before every run: the engine computes the simulation rate from the
// ACTUAL render rate (logic = render / r), so choosing a target the panel cannot show makes
// the entire game -- simulation and animation together -- run in slow motion instead of just
// dropping frames. Knowing the panel's rate up front turns a confusing result into an obvious
// one. See HANDOFF.md section 12.
static DWORD DisplayHz() {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 0;
}

// ───────────────────────────── the job ─────────────────────────────

struct JobCtx {
    int fps;
    std::wstring dir;
    int maxCycles;      // <= 0 means "until the game exits"
    // True only in --auto mode. A scripted run must clean up after itself, otherwise the next
    // run would be refused by the "a game is already running" check. The GUI never sets this:
    // there the game is the user's and closing the tool must not close their game.
    bool killWhenDone;
    // True only in --auto mode: re-raise the game before every measurement.
    //
    // The engine stops rendering whenever its window is not in the foreground, so an unattended
    // run loses its numbers the moment anything else takes focus. Nobody is watching an --auto
    // run, so stealing focus back is harmless there. The GUI leaves this false on purpose: the
    // user has to be able to alt-tab to the tool's log to read it.
    bool refocus;
};

// Returns true when the patch is in and the game is running.
static bool JobPatch(DWORD& outPid, HANDLE& outProc, JobCtx& ctx) {
    // The caller owns whatever we hand back here, including on failure -- --auto mode has to be
    // able to clean up a game it started even when the install later fails, otherwise every
    // failed run would leave a game behind and the next run would refuse to start.
    outPid = 0;
    outProc = NULL;

    // --- 1. refuse if a game is already up -------------------------------------------------
    std::wstring who;
    if (GameRunning(who)) {
        LogF(L"[FAIL] 已经有一个红警3在运行：%s", who.c_str());
        LogLine(L"       为避免误碰别人的对局，本工具不会去注入它。请先关闭游戏再试。");
        return false;
    }

    // --- 2. locate the game ---------------------------------------------------------------
    if (!DirExistsW(TrimSlash(ctx.dir))) {
        LogF(L"[FAIL] 目录不存在：%s", ctx.dir.c_str());
        return false;
    }
    GameFind g;
    std::wstring err;
    if (!FindGame(ctx.dir, g, err)) { LogF(L"[FAIL] %s", err.c_str()); return false; }
    LogF(L"游戏目录   : %s", g.root.c_str());
    LogF(L"主程序     : %s", g.exe.c_str());
    if (!g.skudef.empty()) LogF(L"SkuDef     : %s", FileName(g.skudef).c_str());
    if (VersionScore(FileName(g.exe)) != 1012) {
        LogLine(L"[WARN] 主程序文件名不像 1.12 版。本补丁的特征码是按 1.12 定位的，");
        LogLine(L"       其它版本会在下面报「特征码没找到」。");
    }

    // --- 3. extract our DLL ---------------------------------------------------------------
    if (!ExtractDll(err)) { LogF(L"[FAIL] %s", err.c_str()); return false; }

    // --- 4. launch ------------------------------------------------------------------------
    // Launch the real game executable directly with -config, exactly like the official
    // launcher does, so the game sees a normal command line. Working directory must be the
    // game root or the engine cannot find its .big archives.
    std::wstring cmd = Quote(g.exe);
    if (!g.skudef.empty()) cmd += L" -config " + Quote(g.skudef);
    // Log the exact command line before calling CreateProcessW. If a run ever stops here with no
    // further output, this line is what distinguishes "the launch call never returned" from
    // "CreateProcessW failed" -- two very different problems that otherwise look identical.
    LogF(L"启动命令   : %s", cmd.c_str());
    LogF(L"工作目录   : %s", g.root.c_str());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(g.exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, g.root.c_str(), &si, &pi)) {
        LogF(L"[FAIL] 启动游戏失败（错误 %lu）。", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD pid = pi.dwProcessId;
    // Grant the game the right to raise its own window later on. Harmless, and it removes one
    // more way for the measurement to come back as a meaningless zero.
    AllowSetForegroundWindow(ASFW_ANY);
    // Hand the process to the caller NOW: from here on this function never closes it, so the
    // cleanup path in RunJob always has a live handle to terminate.
    outPid = pid;
    outProc = pi.hProcess;
    LogF(L"已启动游戏 pid = %lu（补丁只改内存，进程退出即失效）", (unsigned long)pid);

    // Give the loader a moment. Injecting during the very earliest startup can deadlock the
    // remote loader, and the DLL would refuse to install anyway until the engine initialises.
    Sleep(2500);

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        LogF(L"[FAIL] 打不开游戏进程（错误 %lu）。如果游戏是以管理员身份启动的，请用管理员身份运行本工具。",
             GetLastError());
        return false;   // outProc is the caller's now; it will terminate + close.
    }

    // --- 5. inject + install ---------------------------------------------------------------
    HMODULE localBase = NULL;
    void* setGroups = NULL;
    void* enable = NULL;
    const DWORD t0 = GetTickCount();
    bool installed = false;
    int attempt = 0;
    int lastRc = -1;

    while (GetTickCount() - t0 < (DWORD)kEnableTimeoutMs) {
        if (g_stop) { LogLine(L"[STOP] 窗口已关闭，中止。"); break; }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            LogLine(L"[FAIL] 游戏进程已退出（启动失败？）。");
            break;
        }

        if (!RemoteModuleBase(pid, g_dllModule.c_str())) {
            std::wstring injErr;
            if (!InjectDll(proc, g_dllPath, injErr)) {
                LogF(L"[WAIT] 注入未成功：%s", injErr.c_str());
                Sleep(1000);
                continue;
            }
            LogLine(L"[OK]   补丁 DLL 已注入游戏进程。");
            // A freshly injected DLL starts from its defaults, so the change groups must be
            // set again before enabling.
            if (!localBase) localBase = LoadLibraryW(g_dllPath.c_str());
            setGroups = RemoteExport(pid, localBase, "FrameLabSetGroups");
            enable    = RemoteExport(pid, localBase, "FrameLabEnable");
            if (!setGroups || !enable) {
                // Say exactly which of the three lookups failed. "cannot resolve export" alone
                // cost a debugging round once: the real cause was a module-name mismatch.
                LogF(L"[FAIL] 取不到 DLL 导出地址（本进程基址 %p，游戏内基址 %p，SetGroups %p，Enable %p）。",
                     (void*)localBase, (void*)RemoteModuleBase(pid, g_dllModule.c_str()),
                     setGroups, enable);
                break;
            }
            DWORD gr = 0;
            if (RemoteCall(proc, setGroups, (void*)(uintptr_t)EffectiveGroups(), &gr, 60000)) {
                LogF(L"[OK]   改动分组 = 0x%04X（返回 %lu）%s", EffectiveGroups(), (unsigned long)gr,
                     g_groupsOverridden ? L" ★由环境变量 RA3FL_GROUPS 覆盖，不是出厂默认 0x2CFFF" : L"");
            } else {
                LogLine(L"[WARN] 设置改动分组失败，继续尝试启用。");
            }
        }

        DWORD rc = 0;
        if (!RemoteCall(proc, enable, (void*)(uintptr_t)ctx.fps, &rc, 120000)) {
            LogLine(L"[WAIT] 启用调用没返回，重试。");
            Sleep(1000);
            continue;
        }
        lastRc = (int)rc;
        if (rc == 0 || rc == 2) {
            installed = true;
            LogF(L"[OK]   补丁已生效：目标 %d 帧（返回 %lu，%s）", ctx.fps, (unsigned long)rc, Explain(lastRc));
            // 2026-09-21:载具颠簸修复到底有没有真的装上?分组位开了 ≠ 装上了(特征没命中只在 DLL 日志里留 WARN)。
            {
                void* chsStatus = RemoteExport(pid, localBase, "FrameLabChassisStatus");
                DWORD cs = 0;
                const bool wanted = (EffectiveGroups() & 0x300000u) != 0;
                if (chsStatus && RemoteCall(proc, chsStatus, NULL, &cs, 60000)) {
                    const int st = (int)cs;
                    if (st == 2)      LogLine(L"[OK]   载具颠簸修复已生效：车身悬挂递推 = 每秒 30 步 + 位姿插值。");
                    else if (st == 1) LogLine(L"[OK]   载具颠簸修复已生效：车身悬挂递推 = 每秒 30 步（回放，无插值）。");
                    // 目标 30 帧要先判:那时包装是装着的(st == 0,只当探针),但节拍门本来就不该生效 ——
                    //   照下面那条 st == 0 的话术报「未生效」会把一个正常状态说成故障。
                    else if (wanted && ctx.fps <= 30)
                        LogLine(L"[..]   目标 30 帧：就是原版节拍，载具颠簸修复不需要生效。");
                    else if (st == 0) LogLine(L"[..]   车身探针已装，但节拍门没开：载具颠簸修复【未生效】。");
                    else if (wanted)
                        LogLine(L"[!!]   载具颠簸修复【没装上】（特征未命中？）。60/90 帧下载具仍会上下颠簸 —— 请把 logs 里的日志发给开发者。");
                    else              LogLine(L"[..]   载具颠簸修复：关（复选框没勾）。60/90 帧下载具会上下颠簸。");
                } else if (wanted) {
                    LogLine(L"[WARN] 读不到载具颠簸修复的状态（DLL 太旧？）。");
                }
            }
            // 2026-09-26:过场运镜计时同样报「到底开没开成」。
            if ((EffectiveGroups() & 0x8000000u) != 0) {
                void* camStatus = RemoteExport(pid, localBase, "FrameLabCameraStatus");
                DWORD cs2 = 0;
                if (camStatus && RemoteCall(proc, camStatus, NULL, &cs2, 60000)) {
                    const int v = (int)cs2;
                    if (v >= 1000)
                        LogF(L"[OK]   过场运镜计时已生效（换算按每帧 %d 毫秒；90 帧应为 11、60 帧应为 17）。", v - 1000);
                    else if (v == -2)
                        LogLine(L"[!!]   过场运镜计时【没装上】（16 处特征没全部命中？）—— 战役过场镜头仍会跑太快。");
                    else
                        LogLine(L"[..]   过场运镜计时未生效（目标帧率不高于原版？）。");
                } else {
                    LogLine(L"[WARN] 读不到过场运镜计时的状态（DLL 太旧？）。");
                }
            }
            // 2026-09-23:粒子系统节拍门(电厂 / 矿场的光)同样报「到底开没开成」+ 放行比例。
            if ((EffectiveGroups() & 0x4000000u) != 0) {
                void* psStatus = RemoteExport(pid, localBase, "FrameLabPsysStatus");
                DWORD ps = 0;
                if (psStatus && RemoteCall(proc, psStatus, NULL, &ps, 60000)) {
                    const int v = (int)ps;
                    if (v >= 1000000)
                        LogF(L"[OK]   粒子系统节拍门已生效（放行比例 %d/1000，90 帧应约 333、60 帧约 500）。", v - 1000000);
                    else if (v == -2)
                        LogLine(L"[!!]   粒子系统节拍门【没装上】（特征未命中？）—— 电厂 / 矿场的光仍会闪得快。");
                    else
                        LogLine(L"[..]   粒子系统节拍门未生效（目标帧率不高于原版？）。");
                } else {
                    LogLine(L"[WARN] 读不到粒子系统节拍门的状态（DLL 太旧？）。");
                }
            }
            // 弹道流节拍门:报「到底开没开成」,并带上实测放行比例。
            if ((EffectiveGroups() & 0x1000000u) != 0) {
                void* trcStatus = RemoteExport(pid, localBase, "FrameLabTracerStatus");
                DWORD ts = 0;
                if (trcStatus && RemoteCall(proc, trcStatus, NULL, &ts, 60000)) {
                    const int v = (int)ts;
                    if (v >= 1000000)
                        LogF(L"[OK]   弹道流节拍门已生效（放行比例 %d/1000，90 帧应约 333、60 帧约 500）。", v - 1000000);
                    else if (v == -2)
                        LogLine(L"[!!]   弹道流节拍门【没装上】（特征未命中？）—— 开火仍会像机关枪。");
                    else
                        LogLine(L"[..]   弹道流节拍门未生效（目标帧率不高于原版？）。");
                } else {
                    LogLine(L"[WARN] 读不到弹道流节拍门的状态（DLL 太旧？）。");
                }
            }
            // 卷屏归一:同样报「到底开没开成」。
            if ((EffectiveGroups() & kScrollFixBits) != 0) {
                void* scStatus = RemoteExport(pid, localBase, "FrameLabScrollStatus");
                DWORD ss = 0;
                if (scStatus && RemoteCall(proc, scStatus, NULL, &ss, 60000)) {
                    if ((int)ss == 1) LogLine(L"[OK]   卷屏归一已生效：键盘 / 边缘卷屏速度与原版相同。");
                    else LogF(L"[!!]   卷屏归一【没开成】（状态 %d；特征未命中？）—— 卷屏保持随帧率变快。", (int)ss);
                } else {
                    LogLine(L"[WARN] 读不到卷屏归一的状态（DLL 太旧？）。");
                }
            }
            // 2026-09-22:用户实测「30 帧原版玩家 ＋ 90 帧补丁玩家」同场对战 → 不同步。
            //   锁步模型要求两端算出逐位相同的模拟状态。**原版本身允许两台机器渲染帧率不同**
            //   (慢机渲染 18 帧、快机 30 帧照样同步),所以「渲染变快」本身不应该导致不同步;
            //   实测却不同步 ⇒ 补丁改的某个量被模拟侧读到了。是哪一个还没逐字段对账清楚,
            //   所以这里只给保守建议,不把它说成「锁步的必然结果」(那是把未知说成已知)。
            if (ctx.fps > 30) {
                LogLine(L"[!!]   联机提醒：实测「30 帧原版玩家 ＋ 90 帧补丁玩家」同场对战 = 不同步。");
                LogLine(L"       在查清原因之前，请只和【同样装了这个补丁、且目标帧率相同】的玩家联机；");
                LogLine(L"       即便双方都装、帧率相同，也【从未测过】，不保证同步。");
                LogLine(L"       单机 / 战役 / 遭遇战 / 自己看自己录的录像，都不受影响。");
            }
            break;
        }
        if (Retryable(lastRc)) {
            ++attempt;
            if (attempt == 1 || attempt % 5 == 0) {
                LogF(L"[WAIT] 引擎还没准备好（返回 %d，%s），继续等…（已等 %lu 秒）",
                     lastRc, Explain(lastRc), (unsigned long)((GetTickCount() - t0) / 1000));
            }
            Sleep(1000);
            continue;
        }
        LogF(L"[FAIL] 安装被拒绝：返回 %d —— %s", lastRc, Explain(lastRc));
        break;
    }

    if (!installed) {
        // Distinguish "we ran out of time" from "we stopped early for a concrete reason" --
        // printing "timed out" after 3 seconds was actively misleading.
        const unsigned long waited = (unsigned long)((GetTickCount() - t0) / 1000);
        if (lastRc < 0) {
            LogF(L"[FAIL] 等了 %lu 秒还是没装上补丁（引擎一直没就绪）。", waited);
        } else {
            LogF(L"[FAIL] 装补丁失败：最后返回 %d（%s），等了 %lu 秒。",
                 lastRc, Explain(lastRc), waited);
        }
        LogF(L"       游戏日志在 %s\\logs\\，把它发给开发者即可定位。", g_tempDir.c_str());
        CloseHandle(proc);
        return false;   // outProc still valid: RunJob decides whether to kill it.
    }

    outPid = pid;
    outProc = pi.hProcess;
    if (localBase) {
        // Keep the local copy mapped: RemoteExport needs it on every measurement call.
        // (Intentionally not FreeLibrary'd -- the process is about to exit anyway.)
    }
    return true;
}

// Render/logic readout. This is what turns the tool into a test rather than a launcher.
static void JobMeasureLoop(DWORD pid, HANDLE gameProc, int targetFps, int maxCycles, bool refocus) {
    const int r = targetFps / 15;
    LogLine(L"");
    LogLine(L"────────── 开始测量（每 10 秒一次，关掉本窗口不影响游戏）──────────");
    LogF(L"期望：渲染 ≈ %d fps，逻辑 ≈ 15.00 Hz（r = %d）", targetFps, r);
    LogLine(L"提示：逻辑帧率只有在**对局里**才会推进，主菜单上会看到 0.00，那是正常的。");
    LogLine(L"");

    // The export addresses are stable for the lifetime of the DLL mapping, but we re-resolve
    // them here so this function does not depend on locals in JobPatch.
    HMODULE localBase = GetModuleHandleW(g_dllModule.c_str());
    if (!localBase) { LogLine(L"[WARN] 本进程没有映射补丁 DLL，无法测量。"); return; }
    void* fpswin   = RemoteExport(pid, localBase, "FrameLabFpsWindow");
    void* logicfps = RemoteExport(pid, localBase, "FrameLabLogicFps");
    void* readDw   = RemoteExport(pid, localBase, "FrameLabReadDword");
    if (!fpswin || !logicfps || !readDw) { LogLine(L"[WARN] 取不到测量用的导出地址。"); return; }

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { LogLine(L"[WARN] 打不开游戏进程，无法测量。"); return; }

    // ── 黑匣子:自动开启飞行记录仪(2026-09-18 第十一批)────────────────────────────
    // 为什么**自动**开:用户是在只有游戏本体的机器上测,没人会记得去开记录器;而「崩溃 /
    //   不同步的原因」恰恰是事后才想知道的东西 —— 出事那一刻没开,就永远补不回来了。
    //   代价很小:每渲染帧多写 44 字节,环形缓冲固定 512 行(60 帧下约 8.5 秒)。
    // 失败**不阻断测量**:记录器是附加能力,不该让帧率测量跟着挂掉。
    {
        g_gamePid = pid;
        PostMessageW(g_hMain, WM_APP_BB, 0, 0);   // 让 UI 线程把两个按钮点亮

        void* bbOn   = RemoteExport(pid, localBase, "FrameLabBlackBox");
        void* bbTest = RemoteExport(pid, localBase, "FrameLabBlackBoxSelfTest");
        if (bbOn) {
            DWORD rc = 1;
            if (RemoteCall(proc, bbOn, (void*)(uintptr_t)1, &rc, 30000) && rc == 0) {
                g_bbArmed = true;
                LogLine(L"[记录器] 飞行记录仪已开启（每渲染帧一行，512 行环形缓冲）。");
                LogLine(L"[记录器] ★看到「不同步」弹窗时，立刻点「保存现场（不同步时点）」按钮。");
            } else {
                LogF(L"[记录器] 开启失败（返回 %lu），不影响下面的帧率测量。", (unsigned long)rc);
            }
        } else {
            LogLine(L"[记录器] 取不到记录器导出（DLL 版本偏旧？），跳过。");
        }
        // 顺手自检一次:报告真的写不进磁盘时,用户必须在**现在**就知道,而不是出事之后才发现。
        // 返回码含义见 DLL 里的 FlStatus:0 好 / 1 处理链没接上 / 30 写不进磁盘 / 32 过滤失效。
        if (bbTest) {
            DWORD rc = 0xFFFFFFFF;
            if (RemoteCall(proc, bbTest, (void*)(uintptr_t)0, &rc, 60000)) {
                if (rc == 0) LogLine(L"[记录器] 自检通过：异常处理链已接上，报告能落到磁盘。");
                else         LogF(L"[记录器] ★自检未通过（返回 %lu）—— 这份记录器可能记不到东西。",
                                  (unsigned long)rc);
            }
        }
        // 先把**已经存在**的报告登记掉(可能是上一次运行留下的),这样后面报出来的都一定是
        // 本次运行期间新产生的。
        AnnounceNewReports(NULL);
    }

    int cycle = 0;
    bool sawGood = false;
    int  lastRender = -1;      // previous cycle's render reading, -1 = none yet
    int  lastGoodRender = -1;  // last cycle where the game actually rendered (>0), x100
    int  lastGoodLogic = -1;   // logic rate from that same cycle, x100
    bool warnedNoFocus = false;  // the "game is not in front" explanation was already printed
    while (!g_stop) {
        if (WaitForSingleObject(gameProc, 0) == WAIT_OBJECT_0) {
            LogLine(L"游戏已退出，测量结束。");
            break;
        }
        if (maxCycles > 0 && cycle >= maxCycles) { LogLine(L"测量次数已达上限，结束。"); break; }
        ++cycle;

        // --auto only: if the previous cycle was ruined because the game had lost the foreground,
        // take it back before measuring. Deliberately NOT unconditional -- repeatedly yanking the
        // window forward is both rude and, in a D3D9 windowed game, a good way to destabilise it.
        // Only re-raise when the last reading was actually zero AND the game is behind something.
        if (refocus && lastRender == 0 && !GameIsForeground(pid)) BringGameToFront(pid, 8000);

        const DWORD t = GetTickCount();
        DWORD render = 0, logic = 0, mspf = 0, lglob = 0;
        const bool okR = RemoteCall(proc, fpswin, (void*)(uintptr_t)kMeasureWindowSec, &render, 60000);
        const bool okL = RemoteCall(proc, logicfps, (void*)(uintptr_t)kMeasureWindowSec, &logic, 60000);
        RemoteCall(proc, readDw, (void*)(uintptr_t)0x00CE176C, &mspf, 30000);
        RemoteCall(proc, readDw, (void*)(uintptr_t)0x00CAF9D0, &lglob, 30000);

        wchar_t sR[32], sL[32];
        if (okR) _snwprintf_s(sR, _countof(sR), _TRUNCATE, L"%d.%02d", (int)render / 100, (int)render % 100);
        else     wcscpy_s(sR, L"?");
        if (okL) _snwprintf_s(sL, _countof(sL), _TRUNCATE, L"%d.%02d", (int)logic / 100, (int)logic % 100);
        else     wcscpy_s(sL, L"?");

        // Whether the game owns the foreground window right now. Only appended when the reading is
        // low, because that is the only situation where it explains anything.
        //
        // Measured carefully: the game DOES keep rendering at ~60 fps while unfocused (window
        // merely behind another window), so "not in front" is NOT by itself the cause of a zero.
        // A zero comes from the startup loading phase, or from the window being covered/minimized.
        // Printing this tag next to a healthy 60.00 would just look contradictory.
        const bool fg = GameIsForeground(pid);
        const bool lowRender = okR && (int)render < 100;

        LogF(L"#%02d  渲染 %s fps ｜ 逻辑 %s Hz ｜ 每帧毫秒 %lu ｜ 逻辑帧率全局 %lu%s",
             cycle, sR, sL, (unsigned long)mspf, (unsigned long)lglob,
             (lowRender && !fg) ? L" ｜ 焦点不在游戏" : L"");

        // 每轮扫一次报告目录。崩溃 / 不同步是**事后**才知道的,不能让用户自己去翻文件夹找 ——
        // 而且那份报告往往就是唯一能说明问题的东西。已经报过的不会重复报。
        AnnounceNewReports(NULL);

        const double rf = okR ? (double)(int)render / 100.0 : -1.0;
        const double lf = okL ? (double)(int)logic / 100.0 : -1.0;
        lastRender = okR ? (int)render : -1;
        // Remember the last reading that was actually usable, so that a later "the game is behind
        // something" cycle can still show the user a real number. The 5 fps floor keeps the noise
        // of a loading screen (0.25 fps) from being presented as a "valid reading".
        if (okR && (int)render >= 500) {
            lastGoodRender = (int)render;
            lastGoodLogic  = okL ? (int)logic : 0;
            warnedNoFocus  = false;
        }
        if (rf >= 0 && lf >= 0) {
            if (lf >= (kLogicFpsX100 - kLogicFpsTolX100) / 100.0 &&
                lf <= (kLogicFpsX100 + kLogicFpsTolX100) / 100.0 &&
                rf >= 0.95 * targetFps) {
                LogLine(L"      => [PASS] 渲染达标，且模拟保持 15 Hz。这一档在本机成立。");
                sawGood = true;
            } else if (rf < 1.0 && lf < 1.0) {
                // Right after a launch the engine is still loading and renders almost nothing,
                // which looks exactly like a broken patch. Say what it really is. The hint about
                // the window is secondary: a covered or minimised window also renders nothing,
                // and that is worth mentioning here because it is one click to fix.
                if (!warnedNoFocus) {
                    warnedNoFocus = true;
                    LogLine(L"      => [WAIT] 游戏还在加载、还没开始出画面。等主菜单出来再看。");
                    LogLine(L"         如果已经过了很久，点一下游戏窗口让它到最前面（被挡住时引擎会停渲染）。");
                }
                if (lastGoodRender >= 0) {
                    LogF(L"         上次有效读数：渲染 %.2f fps ｜ 逻辑 %.2f Hz",
                         lastGoodRender / 100.0, lastGoodLogic / 100.0);
                }
            } else if (lf < 1.0) {
                LogLine(L"      => [WAIT] 逻辑帧率还没动，大概还在主菜单。进一局再看。");
            } else if (rf < 0.95 * targetFps && lf < 13.5) {
                // Both the render rate AND the simulation rate are down: this is the real
                // "the machine cannot keep up, so the whole game slows down" signature.
                //
                // The 13.5 threshold matters. A genuine case (60 Hz panel asked for 90, r=6)
                // reads logic 9.40. A loading screen reads render ~2 while logic stays ~14.4 --
                // the simulation is NOT slowed there, and calling that [ENV] would blame the
                // monitor for a scene transition.
                LogF(L"      => [ENV] 渲染只有 %.1f fps（目标 %d），模拟也掉到 %.2f Hz。",
                     rf, targetFps, lf);
                LogLine(L"         引擎按「渲染 ÷ r」决定模拟速率，所以模拟跟着变慢、动画看起来是慢动作 ——");
                LogLine(L"         这是屏幕刷新率不够，不是补丁的问题。");
                LogLine(L"         90 帧需要 ≥90Hz 的显示器，并在游戏视频选项里关掉垂直同步。");
            } else if (rf < 0.95 * targetFps) {
                // Render dipped but the simulation did NOT slow down. Measured: this is what a
                // loading screen or a scene transition looks like (render 8.5 fps while logic
                // stayed at 14.69). Reporting [ENV] here would blame the monitor for nothing.
                LogF(L"      => [WAIT] 渲染暂时只有 %.1f fps，但模拟仍是 %.2f Hz（没被拖慢）。",
                     rf, lf);
                LogLine(L"         多半是加载或切场景的瞬间，再等一轮。");
            } else if (rf >= 0.95 * targetFps) {
                LogLine(L"      => [!!] 渲染达标但模拟速率不对，请把这份日志发给开发者。");
            }
        }
        if (sawGood && maxCycles > 0) break;

        // Keep the cycle at ~kMeasureCycleMs. The two measurements already consumed ~8 s, so we
        // only sleep the remainder.
        //
        // ★ This MUST be guarded: if a remote call runs long (observed: a first cycle taking 15 s
        //   while the game was still loading), `kMeasureCycleMs - used` underflows to ~4 billion
        //   and the tool appears to hang forever after printing exactly one reading. Sleep only
        //   when there is budget left.
        const DWORD used = GetTickCount() - t;
        if (used < (DWORD)kMeasureCycleMs) {
            const DWORD left = (DWORD)kMeasureCycleMs - used;
            for (DWORD slept = 0; slept + 200 < left && !g_stop; slept += 200) Sleep(200);
        }
    }
    CloseHandle(proc);
}

// Returns true when the patch is installed and the game is up. The return value is what makes
// --auto usable from a script: an always-zero exit code would make a failing run look fine.
static bool RunJob(JobCtx ctx) {
    LogF(L"===== 目标 %d 帧 ｜ 目录 %s =====", ctx.fps, ctx.dir.c_str());
    LogF(L"补丁版本 %s ｜ 改动分组 0x%04X%s", L"0.2.0", EffectiveGroups(),
         g_groupsOverridden ? L"（★环境变量 RA3FL_GROUPS 覆盖；出厂默认是 0x2CFFF）" : L"");
    if (!g_groupsOverridden)
        LogF(L"载具颠簸修复 = %s", g_chassisFix
             ? L"开（车身悬挂递推钉回每秒 30 步 + 位姿插值；分组多出 0x380000）"
             : L"关（与旧版逐位相同的出厂分组 0x2CFFF —— 60/90 帧下载具会上下颠簸）");
    if (!g_groupsOverridden)
        LogF(L"卷屏速度 = %s", g_scrollFix
             ? L"保持原版（键盘 / 边缘卷屏每秒距离不随帧率变；分组多出 0x800000；右键拖拽不在此列）"
             : L"随帧率变快（现状：60 帧约 2 倍、90 帧约 3 倍）");
    if (!g_groupsOverridden)
        LogF(L"过快视觉效果修复 = %s", g_fxFix
             ? L"开（弹道流密度 + 粒子发射 + 光效 / 淡出 / 图标 + 战役过场运镜钉回原版；分组多出 0xF400000）"
             : L"关（现状：弹道流、贴花淡出、拖尾滚动等在 60/90 帧下快 2/3 倍）");

    // Warn about a mismatch between the requested rate and the panel BEFORE launching, so the
    // reading further down cannot be misread as a patch bug.
    const DWORD hz = DisplayHz();
    const int   r  = ctx.fps / 15;
    if (hz) {
        LogF(L"显示器刷新率 : %lu Hz", (unsigned long)hz);
        if ((DWORD)ctx.fps > hz && r > 0) {
            LogF(L"[注意] 目标 %d 帧高于显示器刷新率 %lu Hz。引擎的模拟速率 = 实际渲染帧率 ÷ r，",
                 ctx.fps, (unsigned long)hz);
            LogF(L"       这一档预计会整体变慢（模拟约 %d.%02d Hz），看起来是慢动作，不是掉帧。",
                 (int)(hz * 100 / r) / 100, (int)(hz * 100 / r) % 100);
            LogLine(L"       想真正测这一档，请换一台刷新率 ≥ 目标帧率的机器，并在游戏视频选项里关掉垂直同步。");
        }
    } else {
        LogLine(L"显示器刷新率 : 取不到（继续，不影响下面的测量）");
    }

    DWORD pid = 0;
    HANDLE gameProc = NULL;
    if (!JobPatch(pid, gameProc, ctx)) {
        LogLine(L"");
        LogLine(L"结果：失败。上面的 [FAIL] 一行是原因。");
        LogF(L"完整日志：%s\\gui-*.log", g_tempDir.c_str());
        // --auto must not leave a game behind: a failed run would then block the next one.
        // In GUI mode killWhenDone is false, so a game the user started by hand stays up.
        if (ctx.killWhenDone && gameProc) {
            TerminateProcess(gameProc, 0);
            LogLine(L"已关闭本次启动的游戏进程（--auto 模式的清理步骤）。");
        }
        if (gameProc) CloseHandle(gameProc);
        return false;
    }
    // Raise the game before measuring. Without this the engine sits behind our window and
    // reports a flat 0.00 fps, which reads as "the patch did nothing".
    BringGameToFront(pid, 30000);

    JobMeasureLoop(pid, gameProc, ctx.fps, ctx.maxCycles, ctx.refocus);
    if (ctx.killWhenDone && gameProc) {
        // Only ever our own child: we hold this handle from CreateProcessW in JobPatch, and the
        // pid was never searched for by name. Killing it also removes the patch, since the patch
        // only ever existed in that process's memory.
        TerminateProcess(gameProc, 0);
        LogLine(L"已关闭本次启动的游戏进程（--auto 模式的清理步骤）。");
    }
    CloseHandle(gameProc);
    LogLine(L"");
    LogLine(L"说明：补丁只存在于游戏进程的内存里。关掉本窗口、甚至关掉游戏，磁盘上都不会留下改动。");
    return true;
}

// ───────────────────────────── UI ─────────────────────────────

static void SetBusy(bool busy) {
    InterlockedExchange(&g_busy, busy ? 1 : 0);
    const BOOL e = busy ? FALSE : TRUE;
    EnableWindow(g_hFps, e);
    EnableWindow(g_hDir, e);
    EnableWindow(g_hBrowse, e);
    EnableWindow(g_hChassis, e);
    EnableWindow(g_hScroll, e);
    EnableWindow(g_hFx, e);
    EnableWindow(g_hStart, e);
    SetWindowTextW(g_hStart, busy ? L"运行中…" : L"开始游戏");
}

struct ThreadArg { JobCtx ctx; };

static DWORD WINAPI JobThread(LPVOID p) {
    ThreadArg* a = (ThreadArg*)p;
    g_lastJobOk = RunJob(a->ctx);
    delete a;
    PostMessageW(g_hMain, WM_APP_DONE, 0, 0);
    return 0;
}

// One logical control ("choose the game folder") rendered as a path box plus a browse button.
static void BrowseFolder(HWND h) {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof bi);
    bi.hwndOwner = h;
    bi.lpszTitle = L"选择红警3的安装目录（就是包含 Data 子目录的那个文件夹）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return;
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(idl, path)) SetWindowTextW(g_hDir, path);
    CoTaskMemFree(idl);
}

static void AppendLog(const wchar_t* text) {
    const int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
}

// ───────────────────── 黑匣子:界面侧(2026-09-18 第十一批)─────────────────────
//
// 为什么这两个动作必须走「远程调用」:黑匣子的状态全在**游戏进程内**(环形缓冲、VEH 命中
// 计数、最近报告路径),GUI 在自己的进程里一个都看不到。所以只能把导出函数扔进游戏进程执行。
//
// 为什么报告路径靠**扫文件**而不是 FrameLabBlackBoxLast:那个导出是 (char*, int) 两个参数,
// 而 RemoteCall 只传得了一个参数。与其为它再加一条远程调用通道,不如直接看文件系统 ——
// 报告本来就落在磁盘上,而且**磁盘上的文件才是证据**,进程内那个路径字符串不是。

// 列出 <g_tempDir>\logs 下的报告文件。返回 (完整路径, 文件名) 对。
static void ListReports(std::vector<std::pair<std::wstring, std::wstring> >* out) {
    out->clear();
    std::wstring dir = g_tempDir + L"\\logs";
    WIN32_FIND_DATAW fd;
    const wchar_t* pats[2] = { L"\\crash-*.log", L"\\snapshot-*.log" };
    for (int k = 0; k < 2; ++k) {
        std::wstring pat = dir + pats[k];
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out->push_back(std::make_pair(dir + L"\\" + fd.cFileName, std::wstring(fd.cFileName)));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// 报告新出现的报告文件(每轮只报一次,否则同一份会被刷十几遍,把有用的日志挤掉)。
// 返回新报告的数量。fullPath 非空时把最后一个新报告的完整路径写进去。
static int AnnounceNewReports(std::wstring* fullPath) {
    std::vector<std::pair<std::wstring, std::wstring> > all;
    ListReports(&all);
    int fresh = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < g_bbSeen.size(); ++j)
            if (g_bbSeen[j] == all[i].second) { seen = true; break; }
        if (seen) continue;
        g_bbSeen.push_back(all[i].second);
        ++fresh;
        if (fullPath) *fullPath = all[i].first;
        // 「异常报告」这四个字要说清它**不**等于游戏崩了:VEH 看到的永远是第一次机会异常,
        // 游戏自己的处理程序可能把它吞掉并继续跑。这条提醒写在报告头部,这里再写一遍 ——
        // 用户很容易把「有报告」直接读成「游戏崩了」。
        const bool isCrash = (all[i].second.compare(0, 6, L"crash-") == 0);
        LogF(L"[记录器] %s：%s\r\n",
             isCrash ? L"★新的异常报告(不一定等于崩溃，见报告头部说明)" : L"新的现场快照",
             all[i].second.c_str());
    }
    return fresh;
}

// 「立即保存现场」。**看到引擎弹「不同步」对话框时马上点它** —— 那个对话框会冻住模拟,
// 而环形缓冲里那 512 帧就是出事前唯一的记录。
static void BbDump() {
    if (!g_gamePid) { AppendLog(L"[记录器] 还没有在运行的游戏，先点「开始游戏」。\r\n"); return; }
    HMODULE localBase = GetModuleHandleW(g_dllModule.c_str());
    if (!localBase) { AppendLog(L"[记录器] 本进程没有映射补丁 DLL，无法调用。\r\n"); return; }
    void* fn = RemoteExport(g_gamePid, localBase, "FrameLabBlackBoxDump");
    if (!fn) { AppendLog(L"[记录器] 取不到 FrameLabBlackBoxDump 导出地址。\r\n"); return; }
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_gamePid);
    if (!proc) { AppendLog(L"[记录器] 打不开游戏进程。\r\n"); return; }
    DWORD rc = 0;
    const bool called = RemoteCall(proc, fn, NULL, &rc, 20000);
    CloseHandle(proc);
    if (!called) {
        AppendLog(L"[记录器] 保存现场调用没有返回（游戏可能已卡住或已退出）。\r\n");
        return;
    }
    if (rc != 0) { LogF(L"[记录器] 保存现场失败，返回 %lu。\r\n", (unsigned long)rc); return; }
    AppendLog(L"[记录器] 现场已保存。\r\n");
    std::wstring p;
    if (AnnounceNewReports(&p)) LogF(L"[记录器] 文件：%s\r\n", p.c_str());
}

// 「记录器自检」:走**真实的向量化异常路径**(导出内部抛一个自定义异常码再自己吞掉),
// 判断「报告有没有真的落到磁盘上」。返回码的含义与 DLL 里的 FlStatus 一一对应。
static void BbTest() {
    if (!g_gamePid) { AppendLog(L"[记录器] 还没有在运行的游戏，先点「开始游戏」。\r\n"); return; }
    HMODULE localBase = GetModuleHandleW(g_dllModule.c_str());
    if (!localBase) { AppendLog(L"[记录器] 本进程没有映射补丁 DLL，无法调用。\r\n"); return; }
    void* fn = RemoteExport(g_gamePid, localBase, "FrameLabBlackBoxSelfTest");
    if (!fn) { AppendLog(L"[记录器] 取不到 FrameLabBlackBoxSelfTest 导出地址。\r\n"); return; }
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_gamePid);
    if (!proc) { AppendLog(L"[记录器] 打不开游戏进程。\r\n"); return; }
    DWORD rc = 0xFFFFFFFF;
    const bool called = RemoteCall(proc, fn, (void*)(uintptr_t)0, &rc, 60000);
    CloseHandle(proc);
    if (!called) { AppendLog(L"[记录器] 自检调用没有返回。\r\n"); return; }
    switch (rc) {
    case 0:  AppendLog(L"[记录器] 自检通过：异常处理链已接上，报告确实写到了磁盘。\r\n"); break;
    case 1:  AppendLog(L"[记录器] 自检失败(1)：异常处理链没接上 —— 这份记录器现在什么都记不到。\r\n"); break;
    case 30: AppendLog(L"[记录器] 自检失败(30)：异常被看到了，但报告没写进磁盘（目录不可写/磁盘满）。\r\n"); break;
    case 32: AppendLog(L"[记录器] 自检失败(32)：良性异常过滤失效 —— 它会把普通调试通知也记成崩溃。\r\n"); break;
    default: LogF(L"[记录器] 自检返回了意料之外的码 %lu。\r\n", (unsigned long)rc); break;
    }
    std::wstring p;
    if (AnnounceNewReports(&p)) LogF(L"[记录器] 自检产生的文件：%s\r\n", p.c_str());
}

// 两个黑匣子按钮只有在「游戏已经在跑、而且补丁已经装上」之后才有意义 —— 一个点了没反应的
// 按钮比没有按钮更让人困惑,所以没装上之前是灰的。
// ★刻意**不受 g_busy 影响**:测量循环会一直跑,而用户需要在游戏运行中随时点「保存现场」;
//   g_busy 只该锁住「帧率 / 目录 / 开始游戏」这三个。
static void UpdateBbButtons() {
    const BOOL on = (g_gamePid != 0) ? TRUE : FALSE;
    if (g_hBbDump) EnableWindow(g_hBbDump, on);
    if (g_hBbTest) EnableWindow(g_hBbTest, on);
}

static void CreateUi(HWND h) {
    g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    const DWORD st = WS_CHILD | WS_VISIBLE | SS_LEFT;
    const DWORD ed = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;

    HWND l1 = CreateWindowExW(0, L"STATIC", L"帧数：", st, 14, 19, 60, 20, h, NULL, g_hInst, NULL);
    g_hFps = CreateWindowExW(0, L"COMBOBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                             80, 15, 220, 220, h, (HMENU)IDC_FPS_COMBO, g_hInst, NULL);
    SendMessageW(g_hFps, CB_ADDSTRING, 0, (LPARAM)L"30 帧（原版速度基线，r = 2）");
    SendMessageW(g_hFps, CB_ADDSTRING, 0, (LPARAM)L"60 帧（r = 4）");
    SendMessageW(g_hFps, CB_ADDSTRING, 0, (LPARAM)L"90 帧（r = 6，需要 ≥90Hz 屏幕）");
    SendMessageW(g_hFps, CB_SETCURSEL, 1, 0);   // default 60: correct on every machine

    // 2026-09-21:载具颠簸修复的开关,放在帧数下拉框右边,默认勾上。取消勾选 = 与旧版逐位相同的出厂掩码(做 A/B 用)。
    g_hChassis = CreateWindowExW(0, L"BUTTON", L"修复载具上下颠簸（60/90 帧，实验）",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 312, 8, 290, 20, h, (HMENU)IDC_CHASSIS_FIX, g_hInst, NULL);
    SendMessageW(g_hChassis, BM_SETCHECK, g_chassisFix ? BST_CHECKED : BST_UNCHECKED, 0);
    // 卷屏:可自选,默认不勾。放在上一个复选框正下方。
    g_hScroll = CreateWindowExW(0, L"BUTTON", L"卷屏保持原版速度（不勾 = 随帧率变快）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                312, 29, 290, 20, h, (HMENU)IDC_SCROLL_FIX, g_hInst, NULL);
    SendMessageW(g_hScroll, BM_SETCHECK, g_scrollFix ? BST_CHECKED : BST_UNCHECKED, 0);
    // 2026-09-22:过快视觉效果的打包开关,默认不勾。
    g_hFx = CreateWindowExW(0, L"BUTTON", L"修复过快的特效与过场运镜（实验）",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                            312, 50, 290, 20, h, (HMENU)IDC_FX_FIX, g_hInst, NULL);
    SendMessageW(g_hFx, BM_SETCHECK, g_fxFix ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND l2 = CreateWindowExW(0, L"STATIC", L"游戏目录：", st, 14, 81, 70, 20, h, NULL, g_hInst, NULL);
    g_hDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             ed | ES_READONLY, 92, 78, 400, 24, h, (HMENU)IDC_DIR_EDIT, g_hInst, NULL);
    g_hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览…",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                502, 77, 100, 26, h, (HMENU)IDC_DIR_BROWSE, g_hInst, NULL);

    g_hStart = CreateWindowExW(0, L"BUTTON", L"开始游戏",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               92, 120, 170, 36, h, (HMENU)IDC_START, g_hInst, NULL);

    // 黑匣子的两个按钮(2026-09-18 第十一批)。**只有游戏已经在跑并且打上补丁之后才有意义**,
    // 所以初始状态是禁用的 —— 一个点了没反应的按钮比没有按钮更让人困惑。它们由
    // UpdateBbButtons() 在补丁装上之后打开。
    g_hBbDump = CreateWindowExW(0, L"BUTTON", L"保存现场（不同步时点）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                270, 120, 178, 36, h, (HMENU)IDC_BB_DUMP, g_hInst, NULL);
    g_hBbTest = CreateWindowExW(0, L"BUTTON", L"记录器自检",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                456, 120, 100, 36, h, (HMENU)IDC_BB_TEST, g_hInst, NULL);

    HWND l3 = CreateWindowExW(0, L"STATIC", L"运行日志（可截图发给开发者）：", st, 14, 170, 400, 20, h, NULL, g_hInst, NULL);
    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             14, 192, 588, 290, h, (HMENU)IDC_LOG, g_hInst, NULL);

    HWND all[] = { l1, l2, l3, g_hFps, g_hDir, g_hBrowse, g_hStart, g_hLog, g_hBbDump, g_hBbTest, g_hChassis, g_hScroll, g_hFx };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); ++i) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
    UpdateBbButtons();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateUi(h);
        return 0;

    case WM_APP_LOG: {
        wchar_t* s = (wchar_t*)lp;
        if (s) { AppendLog(s); free(s); }
        return 0;
    }

    case WM_APP_DONE:
        SetBusy(false);
        // In self-test mode there is nobody to click 关闭, so end the process here. This also
        // makes the mode usable from a script: exit code comes from the message loop.
        if (g_selfTest) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;

    // 工作线程已经知道游戏 pid 了 ⇒ 两个黑匣子按钮可以点亮(2026-09-18 第十一批)。
    case WM_APP_BB:
        UpdateBbButtons();
        return 0;

    case WM_TIMER:
        if (wp == kSelfTestTimer) {
            KillTimer(h, kSelfTestTimer);
            // Fill the controls exactly the way a user would, then click the real button.
            // Nothing here bypasses the button handler -- that is the whole point.
            static const int kFpsSelfTest[3] = { 30, 60, 90 };
            int sel = 1;
            for (int i = 0; i < 3; ++i) if (kFpsSelfTest[i] == g_selfTestFps) sel = i;
            SendMessageW(g_hFps, CB_SETCURSEL, (WPARAM)sel, 0);
            SetWindowTextW(g_hDir, g_selfTestDir.c_str());
            AppendLog(L"[自测] 已填好控件，模拟点击「开始游戏」\r\n");
            PostMessageW(g_hMain, WM_COMMAND, MAKEWPARAM(IDC_START, BN_CLICKED), (LPARAM)g_hStart);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DIR_BROWSE:
            BrowseFolder(h);
            return 0;
        // 黑匣子两个按钮(2026-09-18 第十一批)。**在 UI 线程上同步做** —— 每次远程调用
        // 都是几十毫秒级,而「保存现场」的全部价值就在于用户看到「不同步」弹窗之后能马上按下去,
        // 再切线程反而多一层可能出错的时序。
        case IDC_BB_DUMP:
            BbDump();
            return 0;
        case IDC_BB_TEST:
            BbTest();
            return 0;
        case IDC_START: {
            if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return 0;

            wchar_t dir[MAX_PATH * 2] = {0};
            GetWindowTextW(g_hDir, dir, _countof(dir));
            if (!dir[0]) {
                MessageBoxW(h, L"请先选择游戏目录。", L"红警3 帧率测试", MB_ICONINFORMATION);
                InterlockedExchange(&g_busy, 0);
                return 0;
            }
            const int sel = (int)SendMessageW(g_hFps, CB_GETCURSEL, 0, 0);
            static const int kFps[3] = { 30, 60, 90 };
            if (sel < 0 || sel > 2) { InterlockedExchange(&g_busy, 0); return 0; }

            // 复选框状态在 UI 线程上读一次,工作线程只读这个全局(EffectiveGroups 在工作线程里被调)。
            InterlockedExchange(&g_chassisFix,
                                SendMessageW(g_hChassis, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            InterlockedExchange(&g_scrollFix,
                                SendMessageW(g_hScroll, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            InterlockedExchange(&g_fxFix,
                                SendMessageW(g_hFx, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            AppendLog(L"\r\n");
            SetBusy(true);
            ThreadArg* a = new ThreadArg;
            a->ctx.fps = kFps[sel];
            a->ctx.dir = dir;
            a->ctx.maxCycles = 0;             // measure until the game exits
            a->ctx.killWhenDone = false;      // never close the user's game for them
            a->ctx.refocus = false;           // and never steal focus from them either
            // Self-test mode (--gui-autostart) is the one exception: it is a scripted run that
            // nobody is watching, so it must clean up the game it started. Everything else about
            // this path is byte-for-byte the same as a real button click.
            if (g_selfTest) {
                a->ctx.killWhenDone = true;
                a->ctx.maxCycles = g_selfTestCycles;
            }
            HANDLE th = CreateThread(NULL, 0, JobThread, a, 0, NULL);
            if (!th) { delete a; SetBusy(false); MessageBoxW(h, L"创建线程失败。", L"错误", MB_ICONERROR); }
            else CloseHandle(th);
            return 0;
        }
        default: break;
        }
        break;

    case WM_CLOSE:
        InterlockedExchange(&g_stop, 1);
        // The game is deliberately left running: the patch lives in ITS memory and the user
        // may still want to play. Only our measuring stops.
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ───────────────────────────── entry point ─────────────────────────────

static void OpenLogFile() {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return;
    g_tempDir = TrimSlash(tmp);
    g_tempDir += L"\\Ra3FrameLab";
    CreateDirectoryW(g_tempDir.c_str(), NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"gui-%04u%02u%02u-%02u%02u%02u.log",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring p = Join(g_tempDir, name);
    g_hLogFile = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hLogFile != INVALID_HANDLE_VALUE) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };   // so Notepad shows Chinese correctly
        DWORD w = 0;
        WriteFile(g_hLogFile, bom, 3, &w, NULL);
        LogF(L"日志文件 %s", p.c_str());
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR /*lpCmdLine*/, int) {
    g_hInst = hInst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // ── optional headless mode: Ra3FpsTest.exe --auto "<dir>" <fps> [cycles] ──
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // ── self-test mode: Ra3FpsTest.exe --gui-autostart "<dir>" <fps> [cycles] ──
    // Builds the real GUI, pre-fills the real controls and clicks the real button. Does NOT
    // return here -- it falls through to the normal GUI path on purpose.
    if (argc >= 4 && _wcsicmp(argv[1], L"--gui-autostart") == 0) {
        g_selfTest = true;
        g_selfTestDir = argv[2];
        g_selfTestFps = _wtoi(argv[3]);
        g_selfTestCycles = (argc >= 5) ? _wtoi(argv[4]) : 3;
    }

    if (argc >= 4 && _wcsicmp(argv[1], L"--auto") == 0) {
        g_auto = true;
        // Attach to the console we were started from so the output lands where the caller can
        // see it. Note there is deliberately no freopen("CONOUT$") here: see ConsoleOut.
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        OpenLogFile();
        JobCtx c;
        c.dir = argv[2];
        c.fps = _wtoi(argv[3]);
        c.maxCycles = (argc >= 5) ? _wtoi(argv[4]) : 3;
        c.killWhenDone = true;
        c.refocus = true;   // unattended run: nobody minds if we steal focus back
        if (c.fps != 30 && c.fps != 60 && c.fps != 90) {
            LogF(L"[FAIL] 帧数只支持 30 / 60 / 90，收到 %d", c.fps);
            if (g_hLogFile != INVALID_HANDLE_VALUE) CloseHandle(g_hLogFile);
            return 1;
        }
        const bool ok = RunJob(c);
        if (g_hLogFile != INVALID_HANDLE_VALUE) CloseHandle(g_hLogFile);
        return ok ? 0 : 1;
    }
    if (argv) LocalFree(argv);

    // ── GUI ──
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Ra3FpsTestWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    // 2026-09-22:高度 472 → 496,给第三个复选框腾出那 24 px(实测截图发现它压在「游戏目录」行上)。
    RECT rc = { 0, 0, 616, 496 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hMain = CreateWindowExW(0, wc.lpszClassName, L"红警3 帧率测试工具（只改内存，不动游戏文件）",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    OpenLogFile();
    ShowWindow(g_hMain, SW_SHOW);
    UpdateWindow(g_hMain);

    // Self-test: give the window a moment to lay out, then click 开始游戏 for the user.
    if (g_selfTest) SetTimer(g_hMain, kSelfTestTimer, 700, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hMain, &msg)) {   // so Tab moves between controls
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_hLogFile != INVALID_HANDLE_VALUE) CloseHandle(g_hLogFile);
    CoUninitialize();
    // Self-test reports the job's outcome as the process exit code so a script can check it.
    return g_selfTest ? (g_lastJobOk ? 0 : 1) : 0;
}
