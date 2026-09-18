// framelab.cpp — Ra3FrameLab:红警3 客户端帧率改造(运行时内存补丁,可开可关,带完整日志)
//
// 日期  :2026-09-16   会话:平台联机会话 68ee9b9d(Claude)   目录:G:\Ra3 FrameLab(与其它成果隔离)
// 为什么:社区 60 帧补丁只认它自带的 2012 构建主程序,且改法有缺陷(插值系数被引擎后续代码覆盖 ⇒ 顿挫)。
//         我们自己做一份:认我们这份 2009 构建、特征码定位、失败整体回滚、可运行时开关、**出事能靠日志定位**。
// 不做什么:不改游戏文件、不劫持同名 DLL、不自动安装、默认不进联机。见 HANDOFF.md 第 0 节。
//
// 改哪几处(全部运行时改内存,卸载时逐字节还原):
//   P1 客户端帧率全局 30 → 目标帧率,并同步引擎由它派生的三个浮点量(帧数/毫秒、毫秒/帧、帧率浮点值)
//   P2 逻辑调度批次块(41 字节)→ 调用我们的调度函数:保证每 r 个客户端帧正好跑满 6 个阶段(逻辑帧率不变)
//   P3 批次循环之后的插值系数(42 字节)→ 调用我们的系数函数:步长均匀(= 本逻辑帧内已过帧数 ÷ r)
//   P4 粒子生成用的「帧/毫秒」乘数 → 指向我们的常量(钉在原版 30 帧域,防特效寿命减半)
//   P5 粒子过期用的帧率乘数 → 指向我们的常量 30(同上)
//   P6 脚本模型视觉步长 1/30 → 指向我们的 1/目标帧率
// 参考补丁还改了 sub_6027D0 里算系数的那段(我们叫 P3b),我们**故意不改**:那一处的结果在同一帧内会被 P3 覆盖。
//   若 LIVE 观察到阶段步进过程中仍有顿挫,再补 P3b —— 改动越少风险越小。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "schedule.h"

// ───────────────────────────── 版本与状态码 ─────────────────────────────
#define FL_VERSION "0.1.0"

enum FlStatus {
    FL_OK = 0,
    FL_ERR_NOT_INSTALLED = 1,
    FL_ERR_ALREADY = 2,
    FL_ERR_NO_MODULE = 10,
    FL_ERR_BAD_PE = 11,
    FL_ERR_SIG_MISS = 20,        // 某个特征没找到
    FL_ERR_SIG_AMBIGUOUS = 21,   // 某个特征命中多处
    FL_ERR_FPS_GLOBALS = 22,     // 帧率全局量的值不对(不是 15 / 30)
    FL_ERR_RATIO = 23,           // 目标帧率不是逻辑帧率的整数倍,或倍数超出支持范围
    FL_ERR_WRITE = 30,           // 写内存失败(已回滚)
    FL_ERR_SUSPEND = 31,         // 挂起其它线程失败
};

// ───────────────────────────── 日志 ─────────────────────────────
// 用户报错时让他上传 logs\ 里的文件:里面有版本、进程、模块、每一处特征的命中地址与前后字节、失败原因。
namespace log {
static CRITICAL_SECTION g_lock;
static bool g_ready = false;
static char g_path[MAX_PATH * 2] = {0};

static void timestamp(char* out, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(out, n, _TRUNCATE, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void write_raw(const char* text) {
    HANDLE h = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        // ★新建文件先写 UTF-8 BOM:日志里有中文,没有 BOM 时记事本 / 部分工具会按 GBK 解码成乱码 ——
        //   这份日志将来是给玩家上传、给我们排障用的,读不了等于没有。
        LARGE_INTEGER size;
        size.QuadPart = 0;
        if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
            static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
            WriteFile(h, kBom, 3, &written, NULL);
        }
        WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(h);   // 每条都关:崩溃也不丢日志(平台 ShellLog 的同款纪律)
    }
    OutputDebugStringA(text);
}

static void line(const char* level, const char* fmt, ...) {
    if (!g_ready) return;
    char body[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof body, _TRUNCATE, fmt, args);
    va_end(args);
    char ts[32];
    timestamp(ts, sizeof ts);
    char out[1200];
    _snprintf_s(out, sizeof out, _TRUNCATE, "[%s][t%lu] %-5s %s\r\n", ts, GetCurrentThreadId(), level, body);
    EnterCriticalSection(&g_lock);
    write_raw(out);
    LeaveCriticalSection(&g_lock);
}

#define FL_INFO(...) log::line("INFO", __VA_ARGS__)
#define FL_WARN(...) log::line("WARN", __VA_ARGS__)
#define FL_ERR(...)  log::line("ERROR", __VA_ARGS__)

// 把一段字节转成十六进制,日志里能直接比对
static void hex(const void* p, size_t n, char* out, size_t outN) {
    static const char* d = "0123456789ABCDEF";
    size_t used = 0;
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n && used + 3 < outN; ++i) {
        out[used++] = d[b[i] >> 4];
        out[used++] = d[b[i] & 0xF];
        if (i + 1 < n) out[used++] = ' ';
    }
    out[used] = 0;
}

static void init(HMODULE self) {
    InitializeCriticalSection(&g_lock);
    char dir[MAX_PATH] = {0};
    GetModuleFileNameA(self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    char envPath[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableA("RA3FL_LOG", envPath, sizeof envPath) > 0) {
        _snprintf_s(g_path, sizeof g_path, _TRUNCATE, "%s", envPath);
    } else {
        char logdir[MAX_PATH * 2];
        _snprintf_s(logdir, sizeof logdir, _TRUNCATE, "%s\\logs", dir);
        CreateDirectoryA(logdir, NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        _snprintf_s(g_path, sizeof g_path, _TRUNCATE, "%s\\framelab-%04u%02u%02u-%lu.log",
                    logdir, st.wYear, st.wMonth, st.wDay, GetCurrentProcessId());
    }
    g_ready = true;
}
}  // namespace log

// ───────────────────────────── 内存工具 ─────────────────────────────
namespace mem {

static bool read_ok(const void* addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* p = (const unsigned char*)addr;
    const unsigned char* end = p + n;
    while (p < end) {
        if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        p = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

static bool write(void* addr, const void* data, size_t n) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(addr, data, n);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    DWORD tmp = 0;
    VirtualProtect(addr, n, oldProtect, &tmp);
    return true;
}

// 除自己以外的线程全部挂起:改代码字节时别让别的线程正好执行到那里
struct ThreadFreezer {
    HANDLE handles[256];
    int count;
    ThreadFreezer() : count(0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                if (count >= 256) break;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (h && SuspendThread(h) != (DWORD)-1) handles[count++] = h;
                else if (h) CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    ~ThreadFreezer() {
        for (int i = 0; i < count; ++i) {
            ResumeThread(handles[i]);
            CloseHandle(handles[i]);
        }
    }
};
}  // namespace mem

// ───────────────────────────── 特征码 ─────────────────────────────
// 模式里 0x100 = 通配(通常是绝对地址字段)。每个特征必须**唯一命中**,否则整体不安装。
struct Pattern {
    const char* name;
    const short* bytes;
    int length;
    unsigned expectedVa;   // 我们这份 2009 构建里的已知地址(仅用于日志对账,不参与判定)
};

#define W 0x100

// P2 逻辑调度批次块(sub_616130 内,41 字节)
static const short kSigBatch[] = {
    0x8D,0x4E,0xFF, 0x0F,0xAF,0xCF, 0xB8,0xAB,0xAA,0xAA,0x2A, 0xF7,0xE9, 0x8B,0xCA, 0xC1,0xE9,0x1F,
    0x8D,0x44,0x0A,0x01, 0x8D,0x04,0x40, 0x03,0xC0, 0x99, 0xF7,0xFF,
    0xBB,0x01,0x00,0x00,0x00, 0x03,0xF3, 0x8B,0xF8, 0x03,0xFB
};
// P3 批次循环之后的插值系数(42 字节,末尾停在 pop edi 之前)
static const short kSigFraction[] = {
    0xF3,0x0F,0x2A,0x45,0x58, 0xF3,0x0F,0x59,0x05,W,W,W,W, 0x0F,0x57,0xC9, 0x0F,0x2F,0xC8,
    0xF3,0x0F,0x11,0x45,0x60, 0x77,0x0D, 0xF3,0x0F,0x10,0x0D,W,W,W,W, 0x0F,0x2F,0xC1, 0x76,0x03,
    0x0F,0x28,0xC1
};
// 帧率两个全局量所在的代码(sub_5FFAD0):mov eax,[客户端]; xor edx,edx; div [逻辑]; ...
static const short kSigFpsGlobals[] = {
    0xA1,W,W,W,W, 0x33,0xD2, 0xF7,0x35,W,W,W,W, 0x56, 0x8B,0xF0, 0x83,0xFE,0x06, 0x7D,0x14,
    0xB8,0x06,0x00,0x00,0x00, 0x99
};
// P4 粒子生成:fmul dword ptr [帧/毫秒]; test eax,eax; fild dword ptr [esi+0x28]
static const short kSigParticleFmul[] = { 0xD8,0x0D,W,W,W,W, 0x85,0xC0, 0xDB,0x46,0x28 };
// P5 粒子过期:imul eax, dword ptr [客户端帧率]; test eax,eax; mov [esp+4],eax; fild [esp+4]
static const short kSigParticleImul[] = { 0x0F,0xAF,0x05,W,W,W,W, 0x85,0xC0, 0x89,0x44,0x24,0x04, 0xDB,0x44,0x24,0x04 };
// P6 脚本模型视觉步长:addss xmm0,[1/30]; movss xmm1,[1.0]; comiss xmm0,xmm1; mov [edi+0x24C],eax
//    ★只写前三条时在主程序里命中 3 处(另外两处用的是常量 0.05),所以必须带上尾部这条;安装时还要再核对常量值 = 1/30。
static const short kSigVisualStep[] = {
    0xF3,0x0F,0x58,0x05,W,W,W,W, 0xF3,0x0F,0x10,0x0D,W,W,W,W, 0x0F,0x2F,0xC1,
    0x89,0x87,0x4C,0x02,0x00,0x00
};
// 帧率派生量:mov eax,[帧率]; fild [帧率]; test eax,eax; jge +6; fadd [常量]; <运算> ; fstp [目标]
static const short kSigDerived[] = { 0xA1,W,W,W,W, 0xDB,0x05,W,W,W,W, 0x85,0xC0, 0x7D,0x06, 0xD8,0x05,W,W,W,W };

// ── 2026-09-16 会话 68ee9b9d:改动分组开关 ────────────────────────────────────────
// 用户在 90 帧实机看到「兵营建造、熊的移动」等动画出错 ⇒ 必须能**逐组开关**来二分定位,
// 而不是靠猜。每一组都可以单独关掉;默认全开。
#define FL_G_RATIO     0x001   // R1 五处「算 r」
#define FL_G_DERIVED   0x002   // R2 三处派生量函数重定向 + 三个派生量的存量值改写(43 处读者,头号嫌疑)
#define FL_G_DIVFPS    0x004   // R3 某量÷帧率
#define FL_G_UNSURE    0x008   // R4+R5 用途未独立确认的两处
#define FL_G_BATCH     0x010   // P2 批次(r>=6 时引擎本来就跳过这块)
#define FL_G_FRACTION  0x020   // P3 批次之后的插值系数
#define FL_G_PARTICLE  0x040   // P4 粒子生成常量钉回 30 基准
#define FL_G_VISUAL    0x080   // P6 视觉步长 1/30 → 1/目标帧率
#define FL_G_PERFRAME  0x100   // P3b 逐帧推进钩子(帧计数器在这里;r>=6 时这是唯一在跑的插值路径)
// 2026-09-16 二次细分:g101 实测表明「算 r」那组根本不提升帧率(渲染仍 30,逻辑却掉到 5),
// ⇒ 真正解开帧率封顶的是派生量。但派生量有三个,合计 43 处读者,不能整组取舍 ——
// 拆成三个独立开关,让实验告诉我们哪个管帧率、哪个管动画。
#define FL_G_DER_FPS   0x200   // 派生量 (float)帧率        @ 0x00CDBC50
#define FL_G_DER_MSPF  0x400   // 派生量 1000/帧率 每帧毫秒 @ 0x00CDBC54
#define FL_G_DER_FPMS  0x800   // 派生量 帧率*0.001            @ 0x00CDBC5C
#define FL_G_DERIVED2  (FL_G_DER_FPS | FL_G_DER_MSPF | FL_G_DER_FPMS)
// ★默认集合**不包含三个派生量**(0xE00 = (float)帧率 / 1000÷帧率 / 帧率×0.001)。
// ★★理由写清楚，别拿错的那个：
//   ✗ 错的理由(我最初写的)：「实测关掉它们帧率和速度都不变 ⇒ 它们没作用」。
//     这是拿**一个录像场景下的输出指标**去论证一段代码没有下游。事实上它们共有
//     **43 处读者**(0xCDBC50 有 17 处、0xCDBC54 有 14 处、0xCDBC5C 有 12 处)，下游多得很。
//   ✓ 对的理由：**我无法证明那 43 个读者需要新值**，而本项目的铁律是「默认全不变，
//     只改我能说清楚为什么要改的」。不改时，那 43 处保持原版行为；改了却说不清，
//     就是把失效方向从「保持原样」翻成了「静默改错」。
// 实测供参考(不当理由用)：关掉后渲染 89.75 / 逻辑 14.97，与全开的 89.8 / 14.92 无差别。
// 代码保留，要做对照实验时用 FrameLabSetGroups(0xFFF) 开回来。
#define FL_G_EVERYTHING 0xFFF          // 含派生量,仅供对照实验
#define FL_G_ALL       0x1FF          // 默认:不动任何游戏数据全局

// P3b:sub_6027D0 里的逐帧推进块(0x602851..0x602876,37 字节)。
// 覆盖范围从 xorps 开始,是因为调用我们的钩子会破坏 xmm1/xmm2(32 位 MSVC 里 xmm 全是易失的),
// 而后面的钳位要用它们 —— 所以把这两个常量的装载也一起接管,调用后重新装载。
static const short kSigPerFrame[] = {
    0x0F,0x57,0xC9,                       // xorps xmm1, xmm1
    0xF3,0x0F,0x10,0x15,W,W,W,W,          // movss xmm2, [上限 1.0]
    0x8D,0x41,0x01,                       // lea eax, [ecx+1]        新阶段
    0xF3,0x0F,0x2A,0xC0,                  // cvtsi2ss xmm0, eax
    0xF3,0x0F,0x59,0x05,W,W,W,W,          // mulss xmm0, [1/6]
    0x0F,0x2F,0xC8,                       // comiss xmm1, xmm0
    0x89,0x46,0x58,                       // mov [esi+0x58], eax     存阶段
    0xF3,0x0F,0x11,0x46,0x60              // movss [esi+0x60], xmm0  存系数(后面还会再存一次钳位后的值)
};

// ── 2026-09-16 会话 68ee9b9d:「重定向读者」用到的特征 ──────────────────────────
// 为什么改成重定向:参考成品(HAOJUN0823 的 2009 版)不改共享的客户端帧率全局,只把**需要新值的
// 那几处读者**改指向另一个变量。客户端帧率全局在本构建里有 119 处读者,直接把它写成 60 等于
// 「默认全都变,除非我记得逐个冻结」;重定向则是「默认全都不变,除非我明确改」—— 漏掉一处时,
// 前者静默改错,后者保持原样。所以采用后者。
//
// R1 算 r 的地方:mov eax,[渲染帧率]; xor edx,edx; div [逻辑帧率]。本构建共 5 处,
//    五处算出的 r 都会被存进同一个对象字段(+92),所以**必须五处一起改**,否则 r 不一致。
//    ★参考成品只改了其中 2 处,它那份里另外 3 处算出的 r 与已改的两处不等 —— 这是它的缺陷之一。
static const short kSigRatio[] = { 0xA1,W,W,W,W, 0x33,0xD2, 0xF7,0x35,W,W,W,W };
// R3 某量 ÷ 渲染帧率 → 存入一个全局
static const short kSigDivFps[] = { 0xF7,0x35,W,W,W,W, 0xA3,W,W,W,W, 0xC3 };
// R4 把渲染帧率缓存进对象字段 +0x1DC(用途未独立确认,跟随参考成品)
static const short kSigCacheField[] = { 0xA1,W,W,W,W, 0x89,0x86,0xDC,0x01,0x00,0x00, 0x89,0x9E,0xE0,0x01,0x00,0x00 };
// R5 mov esi,[渲染帧率] 后跟 64000 的时间换算(用途未独立确认,跟随参考成品)
static const short kSigMovEsiFps[] = { 0x8B,0x35,W,W,W,W, 0x0F,0x8E,W,W,W,W, 0xB8,0x00,0xFA,0x00,0x00 };

#undef W

// ───────────────────────────── 全局状态 ─────────────────────────────
namespace fl {

struct Undo {
    void* address;
    unsigned char before[64];
    int size;
};

static HMODULE g_self = NULL;
static unsigned char* g_base = NULL;
static size_t g_imageSize = 0;
static unsigned char* g_text = NULL;
static size_t g_textSize = 0;

static int* g_fpsClient = NULL;    // 客户端/渲染帧率(原版 30)
static int* g_fpsLogic = NULL;     // 逻辑帧率(15),只读不改
static int g_retailFps = 30;
static int g_targetFps = 60;
static int g_ratio = 4;            // r = 客户端帧率 ÷ 逻辑帧率

// 2026-09-16 会话 68ee9b9d:容量从 16 提到 48。改成「重定向读者」的设计后改动点有 18 处,
// 原来满了之后 remember() 是**静默返回**,会留下一份残缺的撤销表 —— 回滚时还不回去,比不装还糟。
// 现在满了/超长一律返回 false,让 patch() 当场失败并整体回滚。
static Undo g_undo[48];
static const int kUndoMax = (int)(sizeof g_undo / sizeof g_undo[0]);
static int g_undoCount = 0;
static bool g_installed = false;

// 我们自己的常量:被游戏代码的操作数直接指过来(所以必须是进程内固定地址的全局)
static float g_constRetailFramesPerMs = 0.03f;   // 30 * 0.001
static int   g_constRetailFps = 30;
static float g_constVisualStep = 1.0f / 60.0f;
// 2026-09-16:我们自己的「渲染帧率」变量。被重定向的那几条游戏指令会直接读这里,
// 游戏原来的客户端帧率全局保持 30 不动(它还有一百多处读者,我们一处都不惊动)。
static int   g_fpsRender = 60;
// 分组掩码:哪些改动要真写。默认全开;二分定位时用 FrameLabSetGroups 关掉某几组。
static int   g_groups = FL_G_ALL;

// ── 自动化验证用的仪表(2026-09-16 会话 68ee9b9d)────────────────────────────────
static volatile long g_frameCount = 0;
static void* g_frameObj = NULL;            // 逐帧对象;+0x70 是引擎自己统计的 FPS(每 25 帧算一次)     // 渲染帧计数(插值系数钩子每帧 +1)
static bool  g_measureOnly = false;        // 只量不改:系数返回与原版逐位相同的值
// 引擎自带的 desync 转储需要**两个**开关,少一个只会中止对局、不产出文件:
//   0xCE2F5B = 1 (深度 CRC 总开关)  +  0xCAFF78 = 目标逻辑帧
// 用完必须还原 —— 这两个地址是共享的游戏内存,别的会话也在用。
static unsigned char* g_desyncEnable = NULL;   // 0x00CE2F5B
static int*           g_desyncFrame  = NULL;   // 0x00CAFF78
static unsigned char  g_desyncEnableOrig = 0;
static int            g_desyncFrameOrig = -1;
static bool           g_desyncArmed = false;
static int**          g_theGameLogic = NULL;   // 0x00CD8CE4,逻辑帧在 +0x50

static bool remember(void* address, int size) {
    if (g_undoCount >= kUndoMax) { FL_ERR("撤销表已满(%d 处)—— 拒绝继续改,否则回滚会残缺", kUndoMax); return false; }
    if (size > 64) { FL_ERR("单次改动 %d 字节超过撤销表格子上限 64", size); return false; }
    Undo& u = g_undo[g_undoCount++];
    u.address = address;
    u.size = size;
    memcpy(u.before, address, (size_t)size);
    return true;
}

static bool patch(void* address, const void* data, int size, const char* what) {
    char beforeHex[256], afterHex[256];
    log::hex(address, (size_t)(size > 24 ? 24 : size), beforeHex, sizeof beforeHex);
    if (!remember(address, size)) { FL_ERR("放弃改动 %s @ %08X", what, (unsigned)(uintptr_t)address); return false; }
    if (!mem::write(address, data, (size_t)size)) {
        FL_ERR("写入失败 %s @ %08X (%d 字节),已记录原字节,准备回滚", what, (unsigned)(uintptr_t)address, size);
        --g_undoCount;
        return false;
    }
    log::hex(address, (size_t)(size > 24 ? 24 : size), afterHex, sizeof afterHex);
    // ★与 rollback 同族的修正(2026-09-16):原来只凭 mem::write 返回 true 就打「已改」——
    //   那是**动作做了**,不是**字节真的成了我们要写的**。写入若静默没生效,日志照样报「已改」,
    //   于是留下一份"做对了的记录",比没有记录更坏。现在逐字节核对写入内容。
    if (memcmp(address, data, (size_t)size) != 0) {
        FL_ERR("★写入未通过核验 %s @ %08X:写完之后字节仍不是我们要写的(现在 %s)—— 当作失败处理",
               what, (unsigned)(uintptr_t)address, afterHex);
        --g_undoCount;   // 这条撤销记录作废,别让它参与回滚
        return false;
    }
    FL_INFO("已改并核验 %-24s @ %08X  %d 字节  改前 %s  改后 %s", what, (unsigned)(uintptr_t)address, size, beforeHex, afterHex);
    return true;
}

// 2026-09-16 会话 68ee9b9d(线索来自 RA3 Sim 逆向会话的复核):
// ★旧判据是「mem::write 返回 true」—— 那只说明 VirtualProtect/memcpy/FlushInstructionCache
//   三个 API 没失败,也就是**动作做了**;而我要的结论是**字节真的回到原样**。
//   判据必须就是结论本身,否则"还原成功"这四个字是空的。
// ⇒ 现在每处还原后**逐字节 memcmp 回原始备份**,末尾给出「N/N 处核验通过」。
//   原始字节本来就在撤销表里,直接比对比存哈希更强(哈希只说"大概率一样",memcmp 是逐字节)。
static bool g_selfTestNoWrite = false;   // 自检用:让写入**谎称成功**,注入「动作报成功但字节没变」这个病

static int rollback() {
    int ok = 0, bad = 0;
    for (int i = g_undoCount - 1; i >= 0; --i) {
        Undo& u = g_undo[i];
        const bool wrote = g_selfTestNoWrite ? true : mem::write(u.address, u.before, (size_t)u.size);
        const bool same = wrote && mem::read_ok(u.address, (size_t)u.size) &&
                          memcmp(u.address, u.before, (size_t)u.size) == 0;
        if (same) {
            ++ok;
            FL_INFO("已还原并核验 @ %08X (%d 字节,逐字节相同)", (unsigned)(uintptr_t)u.address, u.size);
        } else {
            ++bad;
            char nowHex[256], wantHex[256];
            const size_t show = (size_t)(u.size > 24 ? 24 : u.size);
            log::hex(u.address, show, nowHex, sizeof nowHex);
            log::hex(u.before, show, wantHex, sizeof wantHex);
            FL_ERR("★还原未通过核验 @ %08X:现在 %s | 应为 %s(%s)—— 进程状态可能不一致,建议重启游戏",
                   (unsigned)(uintptr_t)u.address, nowHex, wantHex,
                   wrote ? "写入返回成功但字节对不上" : "写入就失败了");
        }
    }
    if (bad == 0) FL_INFO("还原核验:%d/%d 处逐字节回到原样", ok, ok);
    else          FL_ERR("★还原核验:%d 处通过,**%d 处未通过** —— 请把本日志发回来,并重启游戏", ok, bad);
    g_undoCount = 0;
    return bad;
}

// 在 .text 里找唯一匹配;返回命中地址,0 = 没找到,(void*)-1 = 多处命中
// 找出**所有**匹配,并只保留「第 opIndex 个字节起的 4 字节操作数 == wantOperand」的那些。
// 2026-09-16 会话 68ee9b9d:重定向设计需要它 —— 算 r 的指令序列在本构建里有 5 处,
// 五处都得改;光用「唯一命中」的扫描器会把它们当成歧义而整体放弃。
// 判据不靠数量写死:凡是**读我们认定的那个帧率全局**的,才算数;数量只用来打日志对账。
static int scan_all(const Pattern& p, int opIndex, const void* wantOperand,
                    unsigned char** out, int outMax) {
    int found = 0, raw = 0;
    const size_t n = (size_t)p.length;
    if (g_textSize < n) return 0;
    for (size_t i = 0; i + n <= g_textSize; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            const short want = p.bytes[j];
            if (want > 0xFF) continue;
            if (g_text[i + j] != (unsigned char)want) break;
        }
        if (j != n) continue;
        ++raw;
        if (*(void**)(g_text + i + opIndex) != wantOperand) continue;
        if (found < outMax) out[found] = g_text + i;
        ++found;
    }
    FL_INFO("特征 %-22s 原始命中 %d 处,其中读目标全局的 %d 处", p.name, raw, found);
    return found > outMax ? 0 : found;   // 超过容量视为异常,交给调用方拒绝安装
}

static unsigned char* scan(const Pattern& p) {
    unsigned char* hit = NULL;
    int hits = 0;
    const size_t n = (size_t)p.length;
    if (g_textSize < n) return NULL;
    for (size_t i = 0; i + n <= g_textSize; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            const short want = p.bytes[j];
            if (want > 0xFF) continue;
            if (g_text[i + j] != (unsigned char)want) break;
        }
        if (j == n) {
            if (++hits == 1) hit = g_text + i;
            if (hits > 1) break;
        }
    }
    if (hits == 0) {
        FL_ERR("特征未找到:%s(这通常说明主程序不是我们支持的那份构建)", p.name);
        return NULL;
    }
    if (hits > 1) {
        FL_ERR("特征命中多处:%s(%d 处)—— 不敢乱改,整体放弃", p.name, hits);
        return (unsigned char*)-1;
    }
    const unsigned va = (unsigned)(uintptr_t)hit;
    if (p.expectedVa && va != p.expectedVa)
        FL_WARN("特征 %s 命中 %08X,与已知构建的 %08X 不同(仍继续,但请留意)", p.name, va, p.expectedVa);
    else
        FL_INFO("特征 %-22s 命中 %08X", p.name, va);
    return hit;
}

// ───────────────────────────── 被游戏代码调用的两个钩子 ─────────────────────────────
// 约定:__stdcall(自己清栈),MSVC 会保留 ebx/esi/edi/ebp —— 补丁片段依赖这一点。

// P2:给定当前阶段号与比值,返回引擎批次循环的上界(= 目标阶段 + 1)
extern "C" int __stdcall fl_hook_batch(int phase, int ratio) {
    const int target = fl_target_phase(phase, ratio);
    return target + 1;
}

// P3:算这一帧该显示的插值系数(= 本逻辑帧内已过的客户端帧数 ÷ r)。返回值走 x87 ST(0),
//     补丁片段再把它搬进 xmm0 —— 因为引擎函数尾部会把 xmm0 存进 [ebp+0x60]。
// 2026-09-16 会话 68ee9b9d:这个钩子每渲染一帧走一次,所以它同时兼任「渲染帧计数器」。
// ★g_measureOnly:只装这一个钩子、返回**与原版逐位相同**的系数(阶段×1/6),帧率一点不改。
// 这样基线组和实验组用的是**同一把尺子**,量出来的帧率才能直接比 —— 不然基线没有计数器,
// 就只能拿「我记得原版是 30」当分母,那是我自己都不该接受的判据。
extern "C" float __stdcall fl_hook_fraction(void* self) {
    const int phase = *(int*)((unsigned char*)self + 0x58);
    float f = g_measureOnly ? (float)phase * (1.0f / 6.0f) : fl_fraction(phase, g_ratio);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

// P3b:sub_6027D0 的逐帧推进块。**每渲染一帧走一次,与 r 无关** —— 所以帧计数器必须放这里。
// ★踩过的坑:原来把计数器放在「批次之后的系数」钩子里,而 r>=6 时引擎会跳过整个批次块,
//   那个钩子一次都不会被调用 ⇒ 90 帧那轮量出「渲染 0 帧」。坏的是尺子,不是补丁。
// g_measureOnly 时返回与原版逐位相同的「阶段 × 1/6」,只数不改。
extern "C" void __stdcall fl_hook_perframe(void* self) {
    ++g_frameCount;
    g_frameObj = self;   // 引擎自己算的 FPS 就在 self+0x70,留个指针做第二把尺子
    const int phase = *(int*)((unsigned char*)self + 0x58);
    float f = g_measureOnly ? (float)phase * (1.0f / 6.0f) : fl_fraction(phase, g_ratio);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    *(float*)((unsigned char*)self + 0x60) = f;
}

// ───────────────────────────── 安装 / 卸载 ─────────────────────────────
static void log_environment() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    FL_INFO("Ra3FrameLab %s  编译于 " __DATE__ " " __TIME__, FL_VERSION);
    FL_INFO("进程 %s (pid %lu)", path, GetCurrentProcessId());
    FL_INFO("主模块基址 %08X  大小 %08X  .text %08X+%08X",
            (unsigned)(uintptr_t)g_base, (unsigned)g_imageSize, (unsigned)(uintptr_t)g_text, (unsigned)g_textSize);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    FL_INFO("主程序编译时间戳 %08X  校验和 %08X  大地址感知 %s",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.CheckSum,
            (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "开" : "关");
}

static bool resolve_module() {
    g_base = (unsigned char*)GetModuleHandleA(NULL);
    if (!g_base) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            g_text = g_base + sec[i].VirtualAddress;
            g_textSize = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// 把错误码表原样打进日志。用户上传日志时不用再问我们「错误码 20 是什么意思」,日志自己说清楚。
// 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
static void log_error_legend() {
    FL_INFO("错误码对照: 0=成功 1=本来就没装 2=已经装过 10=拿不到主模块 11=不是32位主程序");
    FL_INFO("            20=特征没找到(主程序不是我们支持的那份构建,或已被别的补丁改过)");
    FL_INFO("            21=特征命中多处(不敢下手) 22=帧率全局量异常 23=目标帧率不是逻辑帧率的整数倍");
    FL_INFO("            30=写内存失败(已整体回滚) 31=挂起线程失败");
    FL_INFO("            请把本目录 logs 下的这个 .log 文件整份发回来,以上信息足以定位。");
}

// 六个补丁点在内存里的位置。
// 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
// 为什么单独拆出来:干扫(只查不改)和正式安装必须走**同一套判据**,否则用户日志里的
// 「干扫通过」和「安装失败」会自相矛盾,排障就没法只看日志了。
struct Sites {
    unsigned char* fps;
    unsigned char* batch;
    unsigned char* fraction;
    unsigned char* pfmul;
    unsigned char* pimul;
    unsigned char* visual;

    // 要改指向我们 g_fpsRender 的 4 字节操作数(游戏那条指令原本读的是客户端帧率全局)
    struct { void** operand; const char* name; int group; } redirect[24];
    int redirectCount;
    unsigned char* perFrame;   // P3b 逐帧推进块

    // 引擎在启动时算好的三个派生量(浮点帧率 / 每帧毫秒 / 每毫秒帧数):
    // 我们注入时它们早算完了,得按同样公式按新帧率写回去;顺便把算它们的那条 fild 也改指向。
    struct { float* target; float value; void** fildOperand; int group; } derived[8];
    int derivedCount;
};

static bool add_redirect(Sites& s, void** operand, const char* name, int group) {
    if (s.redirectCount >= (int)(sizeof s.redirect / sizeof s.redirect[0])) {
        FL_ERR("要改指向的地方超过上限 —— 放弃安装");
        return false;
    }
    s.redirect[s.redirectCount].operand = operand;
    s.redirect[s.redirectCount].name = name;
    s.redirect[s.redirectCount].group = group;
    ++s.redirectCount;
    return true;
}

// 只读:定位六处补丁点 + 核对它们现在的数值。返回 FL_OK 表示「可以安装」。全程不写游戏内存。
static int resolve_and_verify(int targetFps, Sites& s) {
    if (!resolve_module()) { FL_ERR("拿不到主模块或不是 32 位 PE"); return FL_ERR_BAD_PE; }
    log_environment();

    Pattern pFps      = {"帧率全局量所在代码", kSigFpsGlobals,    (int)(sizeof kSigFpsGlobals / sizeof(short)),    0x005FFAD0};
    Pattern pBatch    = {"逻辑调度批次块",     kSigBatch,         (int)(sizeof kSigBatch / sizeof(short)),         0x0061626E};
    Pattern pFraction = {"插值系数(批次之后)", kSigFraction,      (int)(sizeof kSigFraction / sizeof(short)),      0x006162B7};
    Pattern pPFmul    = {"粒子生成 帧/毫秒",   kSigParticleFmul,  (int)(sizeof kSigParticleFmul / sizeof(short)),  0x006C17CA};
    Pattern pPImul    = {"粒子过期 帧率",      kSigParticleImul,  (int)(sizeof kSigParticleImul / sizeof(short)),  0x006CDBD9};
    Pattern pVisual   = {"脚本模型视觉步长",   kSigVisualStep,    (int)(sizeof kSigVisualStep / sizeof(short)),    0x008F3CDC};

    unsigned char* fpsSite = scan(pFps);
    unsigned char* batch = scan(pBatch);
    unsigned char* fraction = scan(pFraction);
    unsigned char* pfmul = scan(pPFmul);
    unsigned char* pimul = scan(pPImul);
    unsigned char* visual = scan(pVisual);
    unsigned char* bad = (unsigned char*)-1;
    if (!fpsSite || !batch || !fraction || !pfmul || !pimul || !visual) return FL_ERR_SIG_MISS;
    if (fpsSite == bad || batch == bad || fraction == bad || pfmul == bad || pimul == bad || visual == bad)
        return FL_ERR_SIG_AMBIGUOUS;

    g_fpsClient = *(int**)(fpsSite + 1);
    g_fpsLogic = *(int**)(fpsSite + 9);
    if (!mem::read_ok(g_fpsClient, 4) || !mem::read_ok(g_fpsLogic, 4)) {
        FL_ERR("帧率全局量地址不可读:客户端 %08X 逻辑 %08X",
               (unsigned)(uintptr_t)g_fpsClient, (unsigned)(uintptr_t)g_fpsLogic);
        return FL_ERR_FPS_GLOBALS;
    }
    g_retailFps = *g_fpsClient;
    const int logicFps = *g_fpsLogic;
    FL_INFO("帧率全局量:客户端 %08X = %d,逻辑 %08X = %d",
            (unsigned)(uintptr_t)g_fpsClient, g_retailFps, (unsigned)(uintptr_t)g_fpsLogic, logicFps);
    if (logicFps <= 0 || g_retailFps <= 0 || g_retailFps % logicFps != 0) {
        FL_ERR("帧率全局量的值不符合预期(应为逻辑 15 / 客户端 30 的整数倍关系)");
        return FL_ERR_FPS_GLOBALS;
    }
    // 传 0 = 「保持原版帧率」(只量不改模式用),这样量具和被测对象不会互相污染
    if (targetFps <= 0) targetFps = g_retailFps;
    if (targetFps % logicFps != 0) {
        FL_ERR("目标帧率 %d 不是逻辑帧率 %d 的整数倍 —— 引擎的阶段调度只支持整数倍", targetFps, logicFps);
        return FL_ERR_RATIO;
    }
    g_targetFps = targetFps;
    g_ratio = targetFps / logicFps;
    if (g_ratio < 1 || g_ratio > 6) {
        FL_ERR("倍数 r = %d 超出支持范围(1..6);引擎每逻辑帧只有 6 个插值阶段", g_ratio);
        return FL_ERR_RATIO;
    }
    FL_INFO("目标:客户端 %d 帧,逻辑 %d 帧,r = %d(每 %d 个客户端帧跑满 6 个阶段)",
            targetFps, logicFps, g_ratio, g_ratio);

    // ★改之前先核对这三处操作数现在指向什么:地址对不上或数值不对,说明我们认错了地方,宁可不装。
    {
        int* imulOperand = *(int**)(pimul + 3);
        if (imulOperand != g_fpsClient) {
            FL_ERR("P5 粒子过期:操作数指向 %08X,不是客户端帧率全局 %08X —— 放弃安装",
                   (unsigned)(uintptr_t)imulOperand, (unsigned)(uintptr_t)g_fpsClient);
            return FL_ERR_SIG_MISS;
        }
        float* fmulOperand = *(float**)(pfmul + 2);
        float* stepOperand = *(float**)(visual + 4);
        if (!mem::read_ok(fmulOperand, 4) || !mem::read_ok(stepOperand, 4)) {
            FL_ERR("P4/P6 的操作数地址不可读(%08X / %08X)—— 放弃安装",
                   (unsigned)(uintptr_t)fmulOperand, (unsigned)(uintptr_t)stepOperand);
            return FL_ERR_SIG_MISS;
        }
        const float expectFramesPerMs = (float)g_retailFps * 0.001f;
        const float expectStep = 1.0f / (float)g_retailFps;
        FL_INFO("P4 帧/毫秒常量 @ %08X = %.6f(期望 %.6f);P6 视觉步长常量 @ %08X = %.6f(期望 %.6f)",
                (unsigned)(uintptr_t)fmulOperand, *fmulOperand, expectFramesPerMs,
                (unsigned)(uintptr_t)stepOperand, *stepOperand, expectStep);
        if (*fmulOperand < expectFramesPerMs * 0.99f || *fmulOperand > expectFramesPerMs * 1.01f) {
            FL_ERR("P4 的「帧/毫秒」当前值不是 %.6f —— 可能已被别的补丁改过,放弃安装", expectFramesPerMs);
            return FL_ERR_SIG_MISS;
        }
        if (*stepOperand < expectStep * 0.99f || *stepOperand > expectStep * 1.01f) {
            FL_ERR("P6 的视觉步长常量不是 1/%d —— 认错了地方,放弃安装", g_retailFps);
            return FL_ERR_SIG_MISS;
        }
    }

    // ── 收集「要改指向我们 g_fpsRender」的操作数。全程只读,干扫也走这里。 ──────────────
    // 2026-09-16 会话 68ee9b9d
    s.redirectCount = 0;
    s.derivedCount = 0;
    {
        unsigned char* hits[16];

        // R1 算 r 的地方(本构建 5 处)。两道判据:被除数必须是客户端帧率全局、除数必须是逻辑帧率全局。
        // 五处算出的 r 会被存进同一个对象字段,所以必须**全部**改,少改一处 r 就不一致。
        Pattern pRatio = {"算 r 的 mov+div", kSigRatio, (int)(sizeof kSigRatio / sizeof(short)), 0};
        const int nRatio = scan_all(pRatio, 1, g_fpsClient, hits, 16);
        int taken = 0;
        for (int i = 0; i < nRatio; ++i) {
            if (*(int**)(hits[i] + 9) != g_fpsLogic) {
                FL_WARN("  跳过 %08X:它的除数不是逻辑帧率全局,不是算 r 的那种", (unsigned)(uintptr_t)hits[i]);
                continue;
            }
            if (!add_redirect(s, (void**)(hits[i] + 1), "R1 算 r", FL_G_RATIO)) return FL_ERR_SIG_AMBIGUOUS;
            ++taken;
        }
        if (taken == 0) { FL_ERR("一处「算 r」的代码都没找到 —— 放弃安装"); return FL_ERR_SIG_MISS; }
        FL_INFO("  R1 算 r:采用 %d 处(这份构建已知为 5 处)", taken);

        // R3 某量 ÷ 渲染帧率 → 存全局
        Pattern pDiv = {"派生 div", kSigDivFps, (int)(sizeof kSigDivFps / sizeof(short)), 0};
        const int nDiv = scan_all(pDiv, 2, g_fpsClient, hits, 16);
        for (int i = 0; i < nDiv; ++i)
            if (!add_redirect(s, (void**)(hits[i] + 2), "R3 某量÷帧率", FL_G_DIVFPS)) return FL_ERR_SIG_AMBIGUOUS;

        // R4 / R5:用途未独立确认(IDA 没给这两处出伪代码),跟随参考成品 HAOJUN0823 的做法。
        // LIVE 若出怪相,这两处是第一批该摘掉的。
        Pattern pCache = {"缓存进对象字段", kSigCacheField, (int)(sizeof kSigCacheField / sizeof(short)), 0};
        const int nCache = scan_all(pCache, 1, g_fpsClient, hits, 16);
        for (int i = 0; i < nCache; ++i)
            if (!add_redirect(s, (void**)(hits[i] + 1), "R4 缓存字段(用途未独立确认)", FL_G_UNSURE)) return FL_ERR_SIG_AMBIGUOUS;

        Pattern pMovEsi = {"mov esi + 64000", kSigMovEsiFps, (int)(sizeof kSigMovEsiFps / sizeof(short)), 0};
        const int nMov = scan_all(pMovEsi, 2, g_fpsClient, hits, 16);
        for (int i = 0; i < nMov; ++i)
            if (!add_redirect(s, (void**)(hits[i] + 2), "R5 时间换算(用途未独立确认)", FL_G_UNSURE)) return FL_ERR_SIG_AMBIGUOUS;

        // R2 三个派生量函数:既要改指向(以后再算就按新帧率),也要把**已经算好存着的值**按新帧率写回去。
        Pattern pDer = {"帧率派生量函数", kSigDerived, (int)(sizeof kSigDerived / sizeof(short)), 0};
        const size_t n = (size_t)pDer.length;
        for (size_t i = 0; i + n + 16 <= g_textSize; ++i) {
            size_t j = 0;
            for (; j < n; ++j) {
                const short want = pDer.bytes[j];
                if (want > 0xFF) continue;
                if (g_text[i + j] != (unsigned char)want) break;
            }
            if (j != n) continue;
            if (*(int**)(g_text + i + 1) != g_fpsClient) continue;      // 必须是读客户端帧率的那几个
            if (*(int**)(g_text + i + 7) != g_fpsClient) continue;      // fild 读的也必须是它
            const unsigned char* tail = g_text + i + n;
            float* target = NULL;
            float value = 0.0f;
            int kind = 0;   // 这个派生量属于哪个开关组
            if (tail[0] == 0xD8 && tail[1] == 0x3D && tail[6] == 0xD9 && tail[7] == 0x1D) {
                target = *(float**)(tail + 8);                          // fdivr [常量] → fstp [目标]
                value = **(float**)(tail + 2) / (float)targetFps;        // 1000 / 帧率 = 每帧毫秒
                kind = FL_G_DER_MSPF;
            } else if (tail[0] == 0xD8 && tail[1] == 0x0D && tail[6] == 0xD9 && tail[7] == 0x1D) {
                target = *(float**)(tail + 8);                          // fmul [常量] → fstp [目标]
                value = (float)targetFps * **(float**)(tail + 2);        // 帧率 * 0.001 = 每毫秒帧数
                kind = FL_G_DER_FPMS;
            } else if (tail[0] == 0xD9 && tail[1] == 0x1D) {
                target = *(float**)(tail + 2);                          // 直接 fstp [目标]
                value = (float)targetFps;
                kind = FL_G_DER_FPS;
            }
            if (!target || !mem::read_ok(target, 4)) continue;
            if (s.derivedCount >= 8) { FL_ERR("派生量超过 8 处 —— 放弃安装"); return FL_ERR_SIG_AMBIGUOUS; }
            s.derived[s.derivedCount].target = target;
            s.derived[s.derivedCount].value = value;
            s.derived[s.derivedCount].fildOperand = (void**)(g_text + i + 7);
            s.derived[s.derivedCount].group = kind;
            ++s.derivedCount;
        }
        FL_INFO("  R2 帧率派生量:%d 处(这份构建已知为 3 处)", s.derivedCount);
        if (s.derivedCount == 0) { FL_ERR("一个帧率派生量都没找到 —— 放弃安装"); return FL_ERR_SIG_MISS; }
        for (int i = 0; i < s.derivedCount; ++i)
            if (!add_redirect(s, s.derived[i].fildOperand, "R2 派生量 fild", s.derived[i].group)) return FL_ERR_SIG_AMBIGUOUS;

        FL_INFO("共需改指向 %d 处操作数;客户端帧率全局 %08X 本身**保持 %d 不动**(它另有上百处读者)",
                s.redirectCount, (unsigned)(uintptr_t)g_fpsClient, g_retailFps);

        // P3b 逐帧推进块。r>=6 时引擎跳过批次块,这里是唯一还在跑的插值路径,帧计数器也在这。
        Pattern pPerFrame = {"逐帧推进块", kSigPerFrame, (int)(sizeof kSigPerFrame / sizeof(short)), 0x00602851};
        s.perFrame = scan(pPerFrame);
        if (!s.perFrame) return FL_ERR_SIG_MISS;
        if (s.perFrame == (unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
        {
            float* clampTop = *(float**)(s.perFrame + 7);    // 应当 = 1.0
            float* sixth    = *(float**)(s.perFrame + 22);   // 应当 = 1/6
            if (!mem::read_ok(clampTop, 4) || !mem::read_ok(sixth, 4)) {
                FL_ERR("P3b 的两个常量地址不可读"); return FL_ERR_SIG_MISS;
            }
            FL_INFO("  P3b 常量核对:上限 %08X = %.6f(期望 1.0);步长 %08X = %.6f(期望 %.6f)",
                    (unsigned)(uintptr_t)clampTop, *clampTop,
                    (unsigned)(uintptr_t)sixth, *sixth, 1.0f / (float)FL_PHASES_PER_LOGIC_FRAME);
            const float wantSixth = 1.0f / (float)FL_PHASES_PER_LOGIC_FRAME;
            if (*clampTop < 0.99f || *clampTop > 1.01f ||
                *sixth < wantSixth * 0.99f || *sixth > wantSixth * 1.01f) {
                FL_ERR("P3b 常量不对 —— 认错了地方,放弃安装");
                return FL_ERR_SIG_MISS;
            }
        }
    }

    s.fps = fpsSite; s.batch = batch; s.fraction = fraction;
    s.pfmul = pfmul; s.pimul = pimul; s.visual = visual;
    return FL_OK;
}

// 干扫:把安装前的全部检查跑一遍,一个字节都不改。用户报错时先让他跑这个,日志直接指出断在哪一步。
static int dry_run(int targetFps) {
    FL_INFO("──── 干扫开始(只检查,不改游戏任何字节)────");
    Sites s;
    const int rc = resolve_and_verify(targetFps, s);
    if (rc == FL_OK) {
        FL_INFO("干扫通过:六处补丁点全部唯一命中且数值正确,可以安装 %d 帧", targetFps);
        FL_INFO("  P1 帧率全局 %08X | P2 批次 %08X | P3 系数 %08X",
                (unsigned)(uintptr_t)g_fpsClient, (unsigned)(uintptr_t)s.batch, (unsigned)(uintptr_t)s.fraction);
        FL_INFO("  P4 粒子生成 %08X | P5 粒子过期 %08X | P6 视觉步长 %08X",
                (unsigned)(uintptr_t)s.pfmul, (unsigned)(uintptr_t)s.pimul, (unsigned)(uintptr_t)s.visual);
    } else {
        FL_ERR("干扫未通过,错误码 %d —— 含义见下表", rc);
        log_error_legend();
    }
    return rc;
}

// measureOnly = true:只装插值系数那一个钩子,且让它返回**与原版逐位相同**的系数。
// 目的是给基线组装上和实验组**同一个**帧计数器 —— 量具必须两边一致,否则帧率对比不成立。
// 只量不改模式的轻量解析:它只装 P3b 一处,就只校对那一处。
// ★为什么要拆出来:要拿同一把尺子去量**别人已经打过补丁的二进制**(比如社区的 60 帧成品),
// 而那份二进制的粒子常量等数值已经被他改过,走完整校对会被拒装 ⇒ 就量不成了。
static int resolve_perframe_only(Sites& s) {
    if (!resolve_module()) { FL_ERR("拿不到主模块或不是 32 位 PE"); return FL_ERR_BAD_PE; }
    log_environment();
    Pattern pPerFrame = {"逐帧推进块", kSigPerFrame, (int)(sizeof kSigPerFrame / sizeof(short)), 0x00602851};
    s.perFrame = scan(pPerFrame);
    if (!s.perFrame) return FL_ERR_SIG_MISS;
    if (s.perFrame == (unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
    float* clampTop = *(float**)(s.perFrame + 7);
    float* sixth    = *(float**)(s.perFrame + 22);
    if (!mem::read_ok(clampTop, 4) || !mem::read_ok(sixth, 4)) { FL_ERR("P3b 常量地址不可读"); return FL_ERR_SIG_MISS; }
    const float wantSixth = 1.0f / (float)FL_PHASES_PER_LOGIC_FRAME;
    FL_INFO("只量不改:P3b @ %08X,上限 %.6f,步长 %.6f(期望 %.6f)",
            (unsigned)(uintptr_t)s.perFrame, *clampTop, *sixth, wantSixth);
    if (*clampTop < 0.99f || *clampTop > 1.01f || *sixth < wantSixth * 0.99f || *sixth > wantSixth * 1.01f) {
        FL_ERR("P3b 常量不对 —— 认错了地方,放弃"); return FL_ERR_SIG_MISS;
    }
    s.redirectCount = 0;
    s.derivedCount = 0;
    return FL_OK;
}

static int install(int targetFps, bool measureOnly) {
    if (g_installed) { FL_WARN("已经安装过了,忽略"); return FL_ERR_ALREADY; }
    Sites s;
    const int rcPrepare = measureOnly ? resolve_perframe_only(s) : resolve_and_verify(targetFps, s);
    if (rcPrepare != FL_OK) { log_error_legend(); return rcPrepare; }
    targetFps = g_targetFps;   // resolve_and_verify 里做过归一化(<=0 视为 60),这里要拿归一化后的值
    unsigned char* batch = s.batch;
    unsigned char* fraction = s.fraction;
    unsigned char* pfmul = s.pfmul;
    unsigned char* pimul = s.pimul;
    unsigned char* visual = s.visual;

    // 我们的常量(游戏代码的操作数会直接指过来)
    g_constRetailFps = g_retailFps;
    g_constRetailFramesPerMs = (float)g_retailFps * 0.001f;
    g_constVisualStep = 1.0f / (float)targetFps;

    mem::ThreadFreezer freeze;   // 改代码期间别让别的线程跑到这些字节上
    FL_INFO("已挂起其它线程 %d 个,开始写补丁", freeze.count);

    bool ok = true;

    // ── P2 批次块:push edi; push esi; call 我们的调度; mov edi,eax; mov ebx,1; add esi,ebx; 其余 NOP
    if (!measureOnly && (g_groups & FL_G_BATCH)) {
        unsigned char code[41];
        memset(code, 0x90, sizeof code);
        int i = 0;
        code[i++] = 0x57;                                  // push edi   (r)
        code[i++] = 0x56;                                  // push esi   (phase)
        code[i++] = 0xE8;                                  // call rel32
        const int rel = (int)((unsigned char*)&fl_hook_batch - (batch + i + 4));
        memcpy(code + i, &rel, 4); i += 4;
        code[i++] = 0x8B; code[i++] = 0xF8;                // mov edi, eax
        code[i++] = 0xBB; code[i++] = 0x01; code[i++] = 0x00; code[i++] = 0x00; code[i++] = 0x00;  // mov ebx,1
        code[i++] = 0x03; code[i++] = 0xF3;                // add esi, ebx
        ok = ok && patch(batch, code, (int)sizeof code, "P2 逻辑调度批次");
    }

    // ── P3 插值系数:push eax; push ebp; call 我们的系数; 结果从 ST(0) 搬到 xmm0; pop eax; 其余 NOP
    if (ok && !measureOnly && (g_groups & FL_G_FRACTION)) {
        unsigned char code[42];
        memset(code, 0x90, sizeof code);
        int i = 0;
        code[i++] = 0x50;                                  // push eax        (保住返回值寄存器)
        code[i++] = 0x55;                                  // push ebp        (this)
        code[i++] = 0xE8;                                  // call rel32
        const int rel = (int)((unsigned char*)&fl_hook_fraction - (fraction + i + 4));
        memcpy(code + i, &rel, 4); i += 4;
        code[i++] = 0x83; code[i++] = 0xEC; code[i++] = 0x04;                 // sub esp,4
        code[i++] = 0xD9; code[i++] = 0x1C; code[i++] = 0x24;                 // fstp dword [esp]
        code[i++] = 0xF3; code[i++] = 0x0F; code[i++] = 0x10; code[i++] = 0x04; code[i++] = 0x24;  // movss xmm0,[esp]
        code[i++] = 0x83; code[i++] = 0xC4; code[i++] = 0x04;                 // add esp,4
        code[i++] = 0x58;                                  // pop eax
        ok = ok && patch(fraction, code, (int)sizeof code, "P3 插值系数");
    }

    // ── P3b 逐帧推进块(37 字节窗口,我们用 34 字节 + 3 个 NOP)。
    //    只量不改模式**也装这一处** —— 它是唯一「每渲染帧都会走」的路径,帧计数器在里面,
    //    两组用同一把尺子才能比。
    //    ★为什么从 xorps 起接管:调用我们的钩子会破坏 xmm1/xmm2(32 位 MSVC 里 xmm 全易失),
    //      而后面两步钳位要用它们,所以调用后必须把这两个常量重新装载回去。
    if (ok && (g_groups & FL_G_PERFRAME)) {
        unsigned char* site = s.perFrame;
        float* clampTop = *(float**)(site + 7);
        unsigned char code[37];
        memset(code, 0x90, sizeof code);
        int i = 0;
        code[i++] = 0x8D; code[i++] = 0x41; code[i++] = 0x01;                 // lea eax,[ecx+1]    新阶段
        code[i++] = 0x89; code[i++] = 0x46; code[i++] = 0x58;                 // mov [esi+0x58],eax 先把阶段存好
        code[i++] = 0x56;                                                     // push esi           (this)
        code[i++] = 0xE8;                                                     // call rel32
        {
            const int rel = (int)((unsigned char*)&fl_hook_perframe - (site + i + 4));
            memcpy(code + i, &rel, 4); i += 4;
        }
        code[i++] = 0x0F; code[i++] = 0x57; code[i++] = 0xC9;                 // xorps xmm1,xmm1    重新置 0
        code[i++] = 0xF3; code[i++] = 0x0F; code[i++] = 0x10; code[i++] = 0x15;
        memcpy(code + i, &clampTop, 4); i += 4;                               // movss xmm2,[上限]  重新装载
        code[i++] = 0xF3; code[i++] = 0x0F; code[i++] = 0x10; code[i++] = 0x46; code[i++] = 0x60;  // movss xmm0,[esi+0x60]
        code[i++] = 0x8B; code[i++] = 0x46; code[i++] = 0x58;                 // mov eax,[esi+0x58] 后面 cmp eax,6 要用
        code[i++] = 0x0F; code[i++] = 0x2F; code[i++] = 0xC8;                 // comiss xmm1,xmm0   原来的钳位判断
        ok = patch(site, code, (int)sizeof code, "P3b 逐帧推进+帧计数");
    }

    // ── P4 粒子生成:派生量马上要变成 60 基准的,这里必须钉回原版 30 基准,否则粒子生成速率翻倍。
    //    ★参考成品 HAOJUN0823 独立做了同一处、同一个 0.03,互为印证。
    if (ok && !measureOnly && (g_groups & FL_G_PARTICLE)) { void* p = &g_constRetailFramesPerMs; ok = patch(pfmul + 2, &p, 4, "P4 粒子生成帧/毫秒"); }
    // ── P6 视觉步长:原版是每渲染帧推进 1/30,渲染到 60 帧就得改成 1/60,否则这处动画快一倍。
    //    ★参考成品漏了这一处 —— 它那份里这个动画是双速的。
    if (ok && !measureOnly && (g_groups & FL_G_VISUAL)) { void* p = &g_constVisualStep;        ok = patch(visual + 4, &p, 4, "P6 视觉步长"); }
    // ── P5 粒子过期:它读的是客户端帧率全局,而我们**不再改那个全局**,所以它自然还是 30,无需动它。
    //    留着扫描与数值核对,是为了确认我们没认错构建。
    (void)pimul;

    // ── R:把「需要看到新渲染帧率」的那几条指令改指向我们的 g_fpsRender ────────────────
    // 这是本轮的设计改动:不再往游戏的客户端帧率全局里写 60(那个全局有上百处读者),
    // 改成只动我们确认需要新值的这些读者。漏掉一处的后果从「静默改错」变成「保持原样」。
    g_fpsRender = targetFps;
    for (int i = 0; ok && !measureOnly && i < s.redirectCount; ++i) {
        if (!(g_groups & s.redirect[i].group)) { FL_INFO("跳过(本组被关掉) %s", s.redirect[i].name); continue; }
        void* p = &g_fpsRender;
        ok = patch(s.redirect[i].operand, &p, 4, s.redirect[i].name);
    }
    if (ok && !measureOnly) FL_INFO("已改指向 %d 处;客户端帧率全局 %08X 仍为 %d(未改动)",
                                    s.redirectCount, (unsigned)(uintptr_t)g_fpsClient, *g_fpsClient);

    // ── 派生量的**存量值**:三个小函数在启动时就算好存进全局了,我们是中途注入,
    //    光改指向不会让它们重算,所以按同样公式用新帧率写回去(与引擎重算结果逐位相同)。
    for (int i = 0; ok && !measureOnly && i < s.derivedCount; ++i) {
        if (!(g_groups & s.derived[i].group)) {
            FL_INFO("跳过(本组被关掉) 派生量存量值 @ %08X", (unsigned)(uintptr_t)s.derived[i].target);
            continue;
        }
        char oldHex[32], newHex[32];
        log::hex(s.derived[i].target, 4, oldHex, sizeof oldHex);
        log::hex(&s.derived[i].value, 4, newHex, sizeof newHex);
        ok = patch(s.derived[i].target, &s.derived[i].value, 4, "R2 派生量存量值");
        if (ok) FL_INFO("    派生量 %08X:%s → %s (%.6f)",
                        (unsigned)(uintptr_t)s.derived[i].target, oldHex, newHex, s.derived[i].value);
    }

    if (!ok) {
        FL_ERR("安装失败,正在整体回滚(不留半套补丁)");
        rollback();
        return FL_ERR_WRITE;
    }
    g_installed = true;
    g_frameCount = 0;
    if (measureOnly) {
        FL_INFO("只量不改模式就位:只装了插值系数钩子,返回值与原版逐位相同,帧率**未改**(共 %d 处改动)。"
                "它是基线组的量具,和实验组用的是同一个。", g_undoCount);
        return FL_OK;
    }
    // 措辞按证据说话:已证的是「逻辑帧率全局没动」,这一条决定了秒→帧换算(射速就是它),
    // 未证的是「我们改写的三个派生量的四十余处读者是否全在渲染侧」—— 那要靠引擎自己的
    // desync dump 逐字段对账才算数,没过那一关之前不许写「模拟未被触碰」。
    FL_INFO("安装完成:渲染 %d → %d 帧。逻辑帧率全局 %08X 仍为 %d(未改动 ⇒ 秒→帧换算、射速与原版逐帧相同)",
            g_retailFps, targetFps, (unsigned)(uintptr_t)g_fpsLogic, *g_fpsLogic);
    FL_INFO("  共 %d 处改动,可随时 FrameLabDisable 整体还原。", g_undoCount);
    FL_WARN("  尚未证实:我们改写的三个「每帧时长」派生量共有四十余处读者,是否全在渲染侧还没逐一核对,"
            "以引擎 desync dump 的逐字段对账为准。");
    return FL_OK;
}

static int uninstall() {
    if (!g_installed) return FL_ERR_NOT_INSTALLED;
    mem::ThreadFreezer freeze;
    FL_INFO("开始卸载,挂起其它线程 %d 个", freeze.count);
    rollback();
    g_installed = false;
    FL_INFO("已卸载,全部字节还原");
    return FL_OK;
}
}  // namespace fl

// ───────────────────────────── 导出接口 ─────────────────────────────
extern "C" __declspec(dllexport) int __stdcall FrameLabEnable(int targetFps) {
    FL_INFO("收到启用请求,目标帧率 %d", targetFps);
    fl::g_measureOnly = false;
    const int rc = fl::install(targetFps, false);
    FL_INFO("启用结果 = %d", rc);
    return rc;
}

// ── 自动化验证接口(2026-09-16 会话 68ee9b9d)────────────────────────────────────
// 只量不改:装上帧计数器但**不动帧率**。基线组用它,实验组用 FrameLabEnable —— 同一把尺子。
// 分组开关:二分定位用。传 0 或负数 = 全开。改动分组的位定义见源码顶部 FL_G_*。
// 典型用法(定位动画问题):先 FrameLabSetGroups(0x1FF & ~0x002) 关掉派生量那组,再 FrameLabEnable(90)。
// 离线自检:证明「还原核验」这道闸门**真的会红**。靶子是我们自己进程里的一小块内存,
// 全程不碰游戏,不需要游戏在跑。返回位掩码:bit0 = 诚实路径核验通过,bit1 = 注入故障后被抓到。
// ★为什么必须有它:新判据(逐字节 memcmp)如果从没红过,我就不知道它有没有资格当判据 ——
//   而这里注入的正是它要防的那个病:**写入报告成功,字节却没变**。
extern "C" __declspec(dllexport) int __stdcall FrameLabSelfTestRollback() {
    static unsigned char target[16];
    unsigned char original[16], modified[16];
    memset(original, 0xA5, sizeof original);
    memset(modified, 0x5A, sizeof modified);
    memcpy(target, original, sizeof target);

    int result = 0;
    const int savedUndo = fl::g_undoCount;
    fl::g_undoCount = 0;

    // ① 诚实路径:改掉再还原,核验应当通过,且字节确实回到 0xA5
    if (fl::patch(target, modified, (int)sizeof modified, "自检·诚实路径")) {
        const int bad = fl::rollback();
        if (bad == 0 && memcmp(target, original, sizeof target) == 0) result |= 1;
        else FL_ERR("自检①失败:bad=%d,字节%s回到原样", bad,
                    memcmp(target, original, sizeof target) == 0 ? "" : "未");
    }

    // ② 故障注入:让写入**谎称成功**(模拟"动作报成功但字节没变")—— 闸门必须当场报红
    fl::g_undoCount = 0;
    memcpy(target, original, sizeof target);
    if (fl::patch(target, modified, (int)sizeof modified, "自检·故障注入")) {
        fl::g_selfTestNoWrite = true;
        const int bad = fl::rollback();
        fl::g_selfTestNoWrite = false;
        if (bad == 1) result |= 2;
        else FL_ERR("★自检②:注入了故障但闸门**没红**(bad=%d)—— 这道闸门不合格", bad);
        memcpy(target, original, sizeof target);   // 自己收拾干净
    }

    fl::g_undoCount = savedUndo;
    FL_INFO("还原核验闸门自检:诚实路径=%s 故障注入=%s",
            (result & 1) ? "通过" : "★失败", (result & 2) ? "抓到了" : "★没抓到");
    return result;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabSetGroups(int mask) {
    fl::g_groups = (mask <= 0) ? FL_G_ALL : (mask & FL_G_EVERYTHING);
    FL_INFO("改动分组掩码设为 0x%03X", fl::g_groups);
    FL_INFO("  算r=%s 除帧率=%s 未确认两处=%s 批次=%s 系数=%s 粒子=%s 视觉步长=%s 逐帧=%s",
            (fl::g_groups & FL_G_RATIO)    ? "开" : "关", (fl::g_groups & FL_G_DIVFPS)   ? "开" : "关",
            (fl::g_groups & FL_G_UNSURE)   ? "开" : "关", (fl::g_groups & FL_G_BATCH)    ? "开" : "关",
            (fl::g_groups & FL_G_FRACTION) ? "开" : "关", (fl::g_groups & FL_G_PARTICLE) ? "开" : "关",
            (fl::g_groups & FL_G_VISUAL)   ? "开" : "关", (fl::g_groups & FL_G_PERFRAME) ? "开" : "关");
    FL_INFO("  派生量三路: (float)帧率=%s  1000/帧率=%s  帧率*0.001=%s",
            (fl::g_groups & FL_G_DER_FPS)  ? "开" : "关", (fl::g_groups & FL_G_DER_MSPF) ? "开" : "关",
            (fl::g_groups & FL_G_DER_FPMS) ? "开" : "关");
    return fl::g_groups;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabMeasureOnly() {
    FL_INFO("收到「只量不改」请求(基线组)");
    fl::g_measureOnly = true;
    const int rc = fl::install(0, true);
    FL_INFO("只量不改结果 = %d", rc);
    return rc;
}

// 取渲染帧计数。调用方两次取值 + 自己的墙钟 ⇒ 渲染帧率。
extern "C" __declspec(dllexport) int __stdcall FrameLabFrameCount() {
    return (int)fl::g_frameCount;
}

// ★★ 2026-09-16:下面两个「在进程内部等待」的接口,是为了**让量具别再压着被测对象**。
// 之前编排脚本每 200ms 从外面调一次 flctl 问帧号 —— 每次都要开进程、枚举模块、往游戏里
// CreateRemoteThread,直接把游戏拖到逻辑 4 帧/秒(应为 15)。量出来的帧率全是废数据。
// 改成一次远程调用在里面等,外面几百次轮询变成一次。

// 引擎自己算的渲染 FPS x 100(来源 self+0x70,引擎每 25 帧自己算一次)。
// 它与我们的计数器是**两条独立来源**,对不上就说明有一把尺子坏了。
extern "C" __declspec(dllexport) int __stdcall FrameLabEngineFps() {
    if (!fl::g_frameObj || !mem::read_ok((unsigned char*)fl::g_frameObj + 0x70, 4)) return -1;
    const float f = *(float*)((unsigned char*)fl::g_frameObj + 0x70);
    if (f < 0.0f || f > 10000.0f) return -1;
    return (int)(f * 100.0f);
}

extern "C" __declspec(dllexport) int __stdcall FrameLabLogicFrame();   // 定义在下面

// 阻塞到逻辑帧 >= absFrame(或超时),返回到达的帧号;-1 = 取不到帧号。
extern "C" __declspec(dllexport) int __stdcall FrameLabWaitFrame(int absFrame) {
    const DWORD deadline = GetTickCount() + 180000;
    int cur = FrameLabLogicFrame();
    if (cur < 0) return -1;
    while (cur < absFrame && GetTickCount() < deadline) {
        Sleep(20);
        cur = FrameLabLogicFrame();
        if (cur < 0) return -1;
    }
    return cur;
}

// 在进程内部量 seconds 秒的渲染帧率,返回 帧率×100(避免浮点跨线程返回)。
extern "C" __declspec(dllexport) int __stdcall FrameLabFpsWindow(int seconds) {
    if (seconds <= 0 || seconds > 120) seconds = 10;
    const long  c0 = fl::g_frameCount;
    const DWORD t0 = GetTickCount();
    Sleep((DWORD)seconds * 1000);
    const long  c1 = fl::g_frameCount;
    const DWORD dt = GetTickCount() - t0;
    if (!dt) return 0;
    const int fps100 = (int)((double)(c1 - c0) * 100000.0 / (double)dt);
    FL_INFO("窗口内渲染帧率:%d 帧 / %lu 毫秒 = %d.%02d 帧每秒", (int)(c1 - c0), dt, fps100 / 100, fps100 % 100);
    return fps100;
}

// 取引擎自己的逻辑帧号(TheGameLogic + 0x50)。这是判「游戏速度有没有被改」的权威计数:
// 逻辑帧率必须恒为 15,不管渲染到多少帧。返回 -1 = 还没进对局 / 取不到。
extern "C" __declspec(dllexport) int __stdcall FrameLabLogicFrame() {
    if (!fl::g_base && !fl::resolve_module()) return -1;
    if (!fl::g_theGameLogic) fl::g_theGameLogic = (int**)0x00CD8CE4;
    if (!mem::read_ok(fl::g_theGameLogic, 4)) return -1;
    int* gl = *fl::g_theGameLogic;
    if (!gl || !mem::read_ok((unsigned char*)gl + 0x50, 4)) return -1;
    return *(int*)((unsigned char*)gl + 0x50);
}

// 让引擎自己吐出逐字段 sim 状态。★参数是**绝对逻辑帧号**,不是增量 ——
// A/B 两组必须在同一个绝对帧上转储才有可比性,用增量会因为两组进对局的时机不同而错位。
// ★必须两个开关都开,只开触发帧的话对局会中止但一个文件都不产出(踩过)。
// 产物在 <游戏根>\Data\DESYNC-Frame*.txt,约 31 个文件;用完必须 FrameLabDesyncRestore 还原。
extern "C" __declspec(dllexport) int __stdcall FrameLabDesyncDump(int absoluteFrame) {
    if (!fl::g_base && !fl::resolve_module()) return FL_ERR_NO_MODULE;
    const int now = FrameLabLogicFrame();
    if (now < 0) { FL_ERR("desync 转储:取不到当前逻辑帧(还没进对局?)"); return FL_ERR_NO_MODULE; }
    if (absoluteFrame <= now) {
        FL_ERR("desync 转储:目标帧 %d 已经过去了(当前 %d)—— 拒绝,否则两组会在不同帧转储", absoluteFrame, now);
        return FL_ERR_RATIO;
    }
    fl::g_desyncEnable = (unsigned char*)0x00CE2F5B;
    fl::g_desyncFrame  = (int*)0x00CAFF78;
    if (!mem::read_ok(fl::g_desyncEnable, 1) || !mem::read_ok(fl::g_desyncFrame, 4)) {
        FL_ERR("desync 转储:两个开关的地址不可读");
        return FL_ERR_NO_MODULE;
    }
    if (!fl::g_desyncArmed) {
        fl::g_desyncEnableOrig = *fl::g_desyncEnable;
        fl::g_desyncFrameOrig = *fl::g_desyncFrame;
    }
    const unsigned char on = 1;
    const int at = absoluteFrame;
    if (!mem::write(fl::g_desyncEnable, &on, 1) || !mem::write(fl::g_desyncFrame, &at, 4)) {
        FL_ERR("desync 转储:写开关失败");
        return FL_ERR_WRITE;
    }
    // 同族修正:写完必须复读。「已武装」这四个字要有字节撑着。
    if (*fl::g_desyncEnable != on || *fl::g_desyncFrame != at) {
        FL_ERR("★desync 武装未通过核验:深度CRC 现为 %d(应 1),触发帧现为 %d(应 %d)",
               (int)*fl::g_desyncEnable, *fl::g_desyncFrame, at);
        return FL_ERR_WRITE;
    }
    fl::g_desyncArmed = true;
    FL_INFO("desync 转储已武装并核验:当前逻辑帧 %d,触发帧 %d(复读 深度CRC=%d 触发帧=%d;原值 %d / %d)",
            now, at, (int)*fl::g_desyncEnable, *fl::g_desyncFrame,
            (int)fl::g_desyncEnableOrig, fl::g_desyncFrameOrig);
    return FL_OK;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabDesyncRestore() {
    if (!fl::g_desyncArmed) return FL_ERR_NOT_INSTALLED;
    bool ok = mem::write(fl::g_desyncEnable, &fl::g_desyncEnableOrig, 1);
    ok = mem::write(fl::g_desyncFrame, &fl::g_desyncFrameOrig, 4) && ok;
    fl::g_desyncArmed = false;
    // 原来只是把复读值打出来给人看,没有断言 —— 打印 ≠ 检查。
    const bool restored = (*fl::g_desyncEnable == fl::g_desyncEnableOrig) &&
                          (*fl::g_desyncFrame == fl::g_desyncFrameOrig);
    if (restored && ok)
        FL_INFO("desync 开关已还原并核验:深度CRC=%d 触发帧=%d(与原值逐一相同)",
                (int)*fl::g_desyncEnable, *fl::g_desyncFrame);
    else
        FL_ERR("★desync 开关还原未通过核验:现在 深度CRC=%d 触发帧=%d,应为 %d / %d",
               (int)*fl::g_desyncEnable, *fl::g_desyncFrame,
               (int)fl::g_desyncEnableOrig, fl::g_desyncFrameOrig);
    return (ok && restored) ? FL_OK : FL_ERR_WRITE;
}

// 干扫:排障第一步。不改游戏任何字节,只把「能不能装、卡在哪一步」写进日志。
// 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
extern "C" __declspec(dllexport) int __stdcall FrameLabDryRun(int targetFps) {
    FL_INFO("收到干扫请求,目标帧率 %d", targetFps);
    const int rc = fl::dry_run(targetFps);
    FL_INFO("干扫结果 = %d", rc);
    return rc;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabDisable() {
    FL_INFO("收到关闭请求");
    const int rc = fl::uninstall();
    FL_INFO("关闭结果 = %d", rc);
    return rc;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabStatus() {
    return fl::g_installed ? fl::g_targetFps : 0;
}

extern "C" __declspec(dllexport) const char* __stdcall FrameLabVersion() {
    return FL_VERSION;
}

// 出问题时让用户点一下:把当前实况(帧率全局量、派生量、各补丁点当前字节)全打进日志再上传
extern "C" __declspec(dllexport) int __stdcall FrameLabDumpDiagnostics() {
    FL_INFO("──── 诊断快照 ────");
    if (!fl::g_base && !fl::resolve_module()) { FL_ERR("拿不到主模块"); return FL_ERR_NO_MODULE; }
    fl::log_environment();
    // 没安装时不要报 g_targetFps —— 那只是默认值,写进日志会被误读成「已经设成 60 了」。
    if (fl::g_installed)
        FL_INFO("状态:已安装,目标帧率 %d,r = %d,记录在案的改动 %d 处",
                fl::g_targetFps, fl::g_ratio, fl::g_undoCount);
    else
        FL_INFO("状态:未安装(游戏仍是原版帧率),记录在案的改动 %d 处", fl::g_undoCount);
    if (fl::g_fpsClient && mem::read_ok(fl::g_fpsClient, 4))
        FL_INFO("当前客户端帧率全局量 = %d(原版 %d)", *fl::g_fpsClient, fl::g_retailFps);
    for (int i = 0; i < fl::g_undoCount; ++i) {
        char now[256], before[256];
        log::hex(fl::g_undo[i].address, (size_t)(fl::g_undo[i].size > 24 ? 24 : fl::g_undo[i].size), now, sizeof now);
        log::hex(fl::g_undo[i].before, (size_t)(fl::g_undo[i].size > 24 ? 24 : fl::g_undo[i].size), before, sizeof before);
        FL_INFO("改动 %d @ %08X:现在 %s | 原始 %s", i, (unsigned)(uintptr_t)fl::g_undo[i].address, now, before);
    }
    return FL_OK;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        fl::g_self = module;
        log::init(module);
        DisableThreadLibraryCalls(module);
        FL_INFO("════ Ra3FrameLab %s 已载入(默认不安装,等 FrameLabEnable 调用)════", FL_VERSION);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (fl::g_installed) {
            FL_WARN("进程退出时补丁仍在,尝试还原");
            fl::uninstall();
        }
    }
    return TRUE;
}
