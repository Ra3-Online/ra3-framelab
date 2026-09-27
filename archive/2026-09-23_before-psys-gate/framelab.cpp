// framelab.cpp — Ra3FrameLab:红警3 客户端帧率改造(运行时内存补丁,可开可关,带完整日志)
//
// 日期  :2026-09-16   会话:平台联机会话 68ee9b9d(Claude)   目录:G:\Ra3 FrameLab(与其它成果隔离)
// 为什么:社区 60 帧补丁只认它自带的 2012 构建主程序,且改法有缺陷(插值系数被引擎后续代码覆盖 ⇒ 顿挫)。
//         我们自己做一份:认我们这份 2009 构建、特征码定位、失败整体回滚、可运行时开关、**出事能靠日志定位**。
// 不做什么:不改游戏文件、不劫持同名 DLL、不自动安装、默认不进联机。见 HANDOFF.md 第 0 节。
//
// 改哪几处(全部运行时改内存,卸载时逐字节还原):
//   P1 客户端帧率全局 30 → 目标帧率,并同步引擎由它派生的三个浮点量(帧数/毫秒、毫秒/帧、帧率浮点值)
//      ⚠ 2026-09-16 起描述已过时:现在**不写**那个全局(它另有上百处读者),改成把
//        11 处「需要看到新帧率」的指令操作数逐个改指我们的 g_fpsRender。漏一处的后果
//        从「静默改错」变成「保持原样」。详见 install() 里 R 段与 resolve_and_verify()。
//   P2 逻辑调度批次块(41 字节)→ 调用我们的调度函数:保证每 r 个客户端帧正好跑满 6 个阶段(逻辑帧率不变)
//   P3 批次循环之后的插值系数(42 字节)→ 调用我们的系数函数:步长均匀(= 本逻辑帧内已过帧数 ÷ r)
//   P4 粒子生成用的「帧/毫秒」乘数 → 指向我们的常量(钉在原版 30 帧域,防特效寿命减半)
//   P5 粒子过期用的帧率乘数 → 指向我们的常量 30(同上)
//   P6 脚本模型视觉步长 1/30 → 指向我们的 1/目标帧率
//   P8 (2026-09-17 新增,**实验组,默认关**) 客户端帧率全局 30 → 目标帧率。
//      方向与上面所有改动相反,只为验证「动画快 3 倍 = 那批「秒→帧」换算没跟着改」这个假设,
//      用 flctl groups 0x21FF 开。见 FL_G_FPSGLOBAL 处的完整推理(★那个推理后来被证伪,原注释已标 ⛔)。
//   P9 (2026-09-17 新增,**默认开,这是修复本体**) 游戏时钟推进量。
//      引擎主 tick 里 `dword_CE1388 += dword_CE176C`(每客户端帧加一次「每帧毫秒」),
//      而 dword_CE176C 恒为原版值 33 ⇒ 90 帧时时钟跑 3 倍快 ⇒ **动画快 3 倍**
//      (动画的 dt 直接取自这个时钟:见 sub_90ECF0)。
//      修法:把时钟推进那**两条互斥分支**读 dword_CE176C 的操作数都改指我们的
//      g_msPerFrame(= 1000 ÷ 目标帧率),并把 dword_CE176C 的两个写入者也隔离到
//      我们自己的变量里(它另有 14 处读者,必须冻结)。
//      ⚠ 反编译只显示其中一条分支;漏掉另一条会残留约 5.6% 偏快 —— 见 kSigClockAdvance2。
//      见 FL_G_CLOCK 处的完整证据链与 RE-动画推进点-2026-09-17.md §11。
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
#include "chassis_gate.h"   // 2026-09-21:车身外观 30 Hz 节拍门的纯逻辑(与离线自检共用)

// ───────────────────────────── 版本与状态码 ─────────────────────────────
#define FL_VERSION "0.2.0"

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
    FL_ERR_SIG_MISMATCH = 24,    // 特征命中了,但现场的值/操作数不是我们预期的那份构建
    FL_ERR_WRITE = 30,           // 写内存失败(已回滚)
    FL_ERR_SUSPEND = 31,         // 挂起其它线程失败
    FL_ERR_BB_FILTER = 32,       // 黑匣子:良性异常码过滤失效(它把通知类异常也记成崩溃了)
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

// 给黑匣子用:崩溃报告必须和主日志落在**同一个目录**里。让 bb 自己再算一遍路径的话,
// 两处迟早会漂开(比如有人改了 RA3FL_LOG 只改了一边)⇒ 提供一个只读访问器。
static const char* path() { return g_path; }
}  // namespace log

// 黑匣子(完整实现在文件末尾的 bb 命名空间)。这里先声明,因为逐帧钩子在文件中间就要用它,
// 而它又要用到 fl 里的那些全局量 ⇒ 只能定义在后面。声明和定义的签名必须逐字一致。
namespace bb { void tick(void* frameObj); }
// 坐标跟踪通道 0 的设置入口。★必须走这个 setter:g_trkAddr 是 bb 命名空间里的 static,
//   而 FrameLabTrack 这个导出函数在**全局**作用域、且位置在 bb 的完整定义**之前**
//   (第一版直接在导出里写 bb::g_trkAddr,编译报 C2039「不是 bb 的成员」——
//    那时 bb 里只有上面这一行前向声明)。返回 0 成功 / -1 地址被拒。
namespace bb { int set_track_addr(unsigned addr); }

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

// ── ★★★ 2026-09-17(第六批):P6b 动画时间步 —— 本轮找到的**建筑动画回归真因** ────────────
// 现场字节(从原始二进制读的,不是转储推的):
//   00903938  F3 0F 5E 05 50 BC CD 00   divss xmm0,[0x00CDBC50]  ; ★ 要改的操作数在 +4
//   00903940  8B CE                     mov ecx,esi
//   00903942  F3 0F 11 04 24            movss [esp],xmm0
//   00903947  E8 ...                    call sub_8EA220
// 反编译 sub_903910 完整语义(逐行核对过,不是推测):
//   *(_DWORD *)(this + 84) = *(_DWORD *)(this + 88);        // 上次帧号 = 当前帧号
//   v2 = (vtbl[116])(dword_CDB750);                          // ★ 当前帧号(与主循环同一个计数器)
//   *(_DWORD *)(this + 88) = v2;
//   v3 = v2 - *(_DWORD *)(this + 84);                        // 帧增量
//   if ( v3 ) sub_8EA220((_DWORD *)this, (float)v3 / flt_CDBC50);   // ★ dt(秒)
// 而 sub_8EA220 是**混合斜坡推进器**(逐行核对):
//   v3 = (this[2] - this[1]) / 80;                           // 80 字节一条的条目数组
//   do { *v6 = (float)(a2 / *(v6 - 1)) + *v6; ... }          // ★ 进度 += dt / 时长
// ⇒ dt 大一倍 ⇒ 建筑建造/揭幕这类**混合斜坡动画半速跑完** ⇒
//   用户实测的「苏联建筑动画消失、发电厂前一半正常后一半坏」正是这个形态。
// 为什么原版对、60 帧错:
//   vtbl[116] 按**实际渲染帧率**递增;原版渲染 30 帧/秒 + 除数 30 ⇒ dt = 1/30 秒/帧 ✓
//   本补丁渲染 60 帧/秒而除数仍是 30 ⇒ dt = 1/30 秒/帧,而真实间隔是 1/60 ⇒ **快一倍**。
// ⇒ 修法与 P6 同类、同组(FL_G_VISUAL):把除数改指我们自己的「目标帧率浮点值」。
// ★ 与调用频率无关:dt = Δ帧号 ÷ 帧率,只要帧率 = 计数器 tick 速率,dt 就恒等于真实经过时间。
// 已用 tools/re/xref.py 验过:0x00CDBC50 在主程序里共 17 处引用,本条特征(含尾部 8B CE
//   F3 0F 11 04 24)唯一命中 0x00903938(1 处)。
static const short kSigAnimTimeStep[] = {
    0xF3,0x0F,0x5E,0x05,W,W,W,W,          // divss xmm0, [客户端帧率浮点]   ← 操作数在 +4
    0x8B,0xCE,                            // mov ecx, esi
    0xF3,0x0F,0x11,0x04,0x24              // movss [esp], xmm0
};
// ── ★★★ 2026-09-17(第六批):混合斜坡量尺的钩子点(P6b 的客观读数)──────────────────
// 就在上面那条特征后面再挂 5 个字节,把范围从「除数的操作数」扩到「那次调用本身」:
//   00903938  F3 0F 5E 05 50 BC CD 00   divss xmm0,[0x00CDBC50]   ; +4  = P6b 改的操作数
//   00903940  8B CE                     mov ecx,esi               ; +9
//   00903942  F3 0F 11 04 24            movss [esp],xmm0          ; +11 = dt 放到栈上
//   00903947  E8 20 48 FE FF            call sub_8EA220           ; ★ +15 = 要拦的 call
//   ⇒ call 的 rel32 在 +16,下一条指令在 +20;原目标 = hit+20+rel32,必须等于 0x008EA220。
// ★ 为什么把特征做长(而不是复用 kSigAnimTimeStep):调用点必须被**唯一**钉住。
//   尾部的 `8B CE F3 0F 11 04 24 E8` 把「这次调用」和主程序里任何其它 divss 彻底分开。
// ★ 顺序很要紧:这段安装必须排在 P6b 把 +4 操作数改指**之前** —— 那时 +4 还是引擎的
//   0x00CDBC50,可以用它核对「我们没认错构建」。安装时另有兜底:也接受已经改指过的
//   &g_constRenderFps,这样即使将来顺序被改动,量尺也不会因为「已经被改过」而失效。
static const short kSigBlendStepCall[] = {
    0xF3,0x0F,0x5E,0x05,W,W,W,W,          // divss xmm0, [客户端帧率浮点]
    0x8B,0xCE,                            // mov ecx, esi
    0xF3,0x0F,0x11,0x04,0x24,             // movss [esp], xmm0
    0xE8,W,W,W,W                          // call sub_8EA220           ← rel32 在 +16
};
// 帧率派生量:mov eax,[帧率]; fild [帧率]; test eax,eax; jge +6; fadd [常量]; <运算> ; fstp [目标]
static const short kSigDerived[] = { 0xA1,W,W,W,W, 0xDB,0x05,W,W,W,W, 0x85,0xC0, 0x7D,0x06, 0xD8,0x05,W,W,W,W };

// ── 2026-09-17 动画定位会话:动画标尺的钩子点(只读探针,默认关)──────────────────
// 位置:sub_90C1B0(W3DScriptedModelDraw 的「切动画」)里那条 `call sub_8EDED0`。
// 现场字节(从原始二进制读的,不是转储推的):
//   0090C240  8B 7E 40        mov edi,[esi+0x40]   ; 旧动画
//   0090C243  8B CE           mov ecx,esi          ; ecx = drawModule
//   0090C245  E8 86 1C FE FF  call sub_8EDED0      ; ★ 取「上一个动画分数」
//   0090C24A  8B 44 24 1C     mov eax,[esp+0x1c]
// 我们只把这条 call 的 rel32 改指到 DLL 里的同约定包装函数(ecx=this,返回 double 走 ST(0)),
// **长度不变、行为逐位不变**,只是顺手记一笔「这个 drawModule 是谁」。
// 已用 tools\re\pescan.py 验过:主程序里唯一命中 0x0090C240(1 处)。
static const short kSigAnimFractionCall[] = {
    0x8B,0x7E,0x40, 0x8B,0xCE, 0xE8,W,W,W,W, 0x8B,0x44,0x24,0x1C
};

// ── 2026-09-17 动画定位会话:游戏时钟推进量(默认开,见 FL_G_CLOCK 的完整证据链)──────
// 位置:sub_5FE9D0(引擎主 tick)里唯一那处时钟推进。现场字节(从原始二进制读的):
//   005FEB9D  A1 8813CE00    mov eax,[0x00CE1388]   ; 时钟
//   005FEBA2  03 05 6C17CE00  add eax,[0x00CE176C]   ; ★ 我们要改的就是这条的操作数
//   005FEBA8  50             push eax               ; 新时钟,下面两次调用都拿它当参数
//   005FEBA9  A3 8813CE00    mov [0x00CE1388],eax
//   005FEBAE  E8 ..          call 0x936910          ; WW3D 时间 = 新时钟
//   005FEBB9  52             push edx
//   005FEBBA  E8 ..          call 0x99bf90          ; APT(UI) 时间 = 新时钟
// 我们**只改操作数**(0x005FEBA4 起的 4 字节,原本是 0x00CE176C),指令本身不动 ⇒
// 长度不变、时钟推进的语义不变,只是推进量改由我们控制。
// 特征带上后面的 `mov [0xCE1388],eax`,是为了把范围钉死在「推进时钟」这一处 ——
// 主程序里读 0xCE176C 的地方不止一处,单看 add 会误伤。
// ⚠ 踩过的坑:第一版签名写成 `03 05 W W W W A3 88 13 CE 00`,漏掉了中间那个 `50`(push eax),
//   于是离线干扫 0 命中。当时是靠 tools\re\dryscan.py 在**起游戏之前**发现的。
//   教训:读字节要用 pedump 整段 dump,别用 grep 按符号名过滤 —— 过滤会把中间那几行藏掉。
// 已用 tools\re\pescan.py + dryscan.py 验过:主程序里唯一命中 0x005FEBA2(1 处)。
static const short kSigClockAdvance[] = {
    0x03,0x05,W,W,W,W, 0x50, 0xA3,0x88,0x13,0xCE,0x00
};

// ── 2026-09-17 当天第二次定位:时钟推进的**第二条(互斥)分支** ────────────────────────
// ⚠ 这是本轮最重要的订正之一:sub_5FE9D0 里**有两个**写 dword_CE1388 的地方,不是一处。
//   反编译(Hex-Rays)只把其中一条写进 C 代码里,另一条藏在 else 分支 ⇒ 只读 C 会漏掉。
//   两条分支互斥,由这个条件选:
//     if ( v10 || !v6 || *(BYTE*)(TheGameLogic+368) || !(vtbl[116]() % 0x1E) )   ← 分支 B
//       dword_CE1388 += v5 * dword_CE176C;      // v5 = 帧增量;每 30 帧走一次这条
//     else                                                                        ← 分支 A(常态)
//       dword_CE1388 += dword_CE176C;
//   现场字节(原始二进制):
//     005FEBE1  8B 0D 6C17CE00   mov  ecx,[0x00CE176C]   ; ★ 要改的操作数
//     005FEBE7  A1 8813CE00     mov  eax,[0x00CE1388]   ; 时钟
//     005FEBEC  0F AF CE         imul ecx,esi           ; × 帧增量
//     005FEBEF  03 C1            add  eax,ecx
//     005FEBF1  50               push eax
//     005FEBF2  A3 8813CE00     mov  [0x00CE1388],eax
//   **为什么必须一起改**(不改就是残留误差,已量化):
//     90 帧下每 30 次调用里有 29 次走 A、1 次走 B,而 30 次调用只花 30/90 秒 = 0.333 秒。
//       只改 A: 29×11 + 33        = 352 毫秒 / 0.333 秒 = **1.056×**(仍有 5.6% 快)
//       两条都改: 30×11            = 330 毫秒 / 0.333 秒 = 0.99×(与原版同样的 1% 偏慢)
//     这与「原版 30 帧下 30×33 = 990 毫秒 / 1.000 秒」是同一套取整特性。
//   特征里把 `mov ecx,[0x00CE176C]` 的**操作数通配掉**(W,W,W,W),让它和分支 A 一样
//   由代码在运行期核对「读的确实是 0x00CE176C」再改 —— 两条分支用同一套判据。
// 已用 tools\re\pescan.py + dryscan.py 验过:主程序里唯一命中 0x005FEBE1(1 处)。
static const short kSigClockAdvance2[] = {
    0x8B,0x0D,W,W,W,W, 0xA1,0x88,0x13,0xCE,0x00, 0x0F,0xAF,0xCE, 0x03,0xC1, 0x50,
    0xA3,0x88,0x13,0xCE,0x00
};

// ── 2026-09-17:「每帧毫秒」的第二个写入者 sub_5B81A0(虚表 0x00C09A20 槽 105)──────────
//   005B81A0  F3 0F 2C 44 24 04   cvttss2si eax, [esp+4]   ; 参数(浮点)= 每帧毫秒
//   005B81A6  A3 6C 17 CE 00      mov [0x00CE176C], eax    ; ★ 要改的操作数
//   005B81AB  C2 04 00            ret 4
// 这是个带参数的 setter。它和上面那个小函数是两个独立的写入者,两处都要挡(理由见
// g_msPerFrameQuarantine 的注释)。特征带上后面的 `ret 4`,把范围钉死在这个 setter 上。
// 已用 tools\re\pescan.py 验过:主程序里唯一命中 0x005B81A0(1 处)。
static const short kSigMsPerFrameSetter[] = {
    0xF3,0x0F,0x2C,0x44,0x24,0x04, 0xA3,W,W,W,W, 0xC2,0x04,0x00
};

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
#define FL_G_VISUAL    0x080   // P6 视觉步长 1/30 → 1/目标帧率(仅 sub_8F3850 的揭幕斜坡)
#define FL_G_PERFRAME  0x100   // P3b 逐帧推进钩子(帧计数器在这里;r>=6 时这是唯一在跑的插值路径)
// 2026-09-16 二次细分:g101 实测表明「算 r」那组根本不提升帧率(渲染仍 30,逻辑却掉到 5),
// ⇒ 真正解开帧率封顶的是派生量。但派生量有三个,合计 43 处读者,不能整组取舍 ——
// 拆成三个独立开关,让实验告诉我们哪个管帧率、哪个管动画。
#define FL_G_DER_FPS   0x200   // 派生量 (float)帧率        @ 0x00CDBC50
#define FL_G_DER_MSPF  0x400   // 派生量 1000/帧率 每帧毫秒 @ 0x00CDBC54
#define FL_G_DER_FPMS  0x800   // 派生量 帧率*0.001            @ 0x00CDBC5C
// ★★★ 2026-09-17(第七批):默认集合**改为包含**三个派生量。这是本批找到的
//   「60 帧建筑动画回归」的**真因修复**。下面把「为什么以前关着」和「为什么现在必须开」
//   两件事都写清楚 —— 因为这一段曾经被一个**听起来很有道理但错误**的理由关掉过。
//
//   ── 以前为什么关着(第六批及以前)──
//   ✗ 错的理由(最初写的)：「实测关掉它们帧率和速度都不变 ⇒ 它们没作用」。
//     这是拿**一个录像场景下的输出指标**去论证一段代码没有下游。
//   ✗ 也不算对的理由：「我无法证明那 43 个读者需要新值,按铁律默认不改」。
//     这条在**当时**是守纪律的,但它把「不知道」当成了「不需要」——
//     真相是:那 43 个读者**全都要**新值,只是当时没有证据。
//
//   ── 现在为什么必须开(第七批的证据)──
//   真凶 `sub_6F6CB0`(0x006F6CB0,模块名写死在代码里:`"StructureUnpackUpdate"`,
//   即**建筑解包/建造动画**)把**两种不同来源的时间**放进同一个式子相除:
//       分子 = 显示器帧计数器 − 起始帧号        (单位:渲染帧,按 60/秒涨)
//       分母 = 时长(秒) × 1000 × flt_CDBC5C     (靠这个常量把毫秒折算成渲染帧)
//   ⇒ 分子分母必须同单位 ⇒ **flt_CDBC5C 必须等于「渲染帧/毫秒」**。
//   原版 0.03 = 30÷1000,对应的正是原版客户端帧率 30;我们把客户端帧率提到 60
//   却不动它 ⇒ 进度**双倍速跑完** ⇒ 用户实测的「发电厂动画前一半正常、后一半卡死」
//   「苏联建筑动画消失」。
//
//   ── 「引擎 tick 真的到 60 了吗」这条唯一的前提,怎么证 ──
//   FL_G_CLOCK 的时钟累加器是**每渲染帧**驱动一次(fl_hook_perframe),而引擎的游戏时钟是
//   **每客户端 tick** 推进一次。实测 clockrate = 66666 微秒/逻辑帧 = 4 × 16.667,
//   这**只有**在引擎 tick 恰好每逻辑帧跑 4 次时才成立(若仍只跑 2 次 = 30/秒,会读 ~33000)。
//   ⇒ 引擎的客户端 tick 确实是 60/秒,帧率三兄弟必须整体按 60 重算。
//   (另有三条旁证见 kEngineFramesPerMsVa 上方的注释。)
//
//   ── 与 P6b 的关系 ──
//   FL_G_DER_FPS 会把 0x00CDBC50 整体写成目标帧率,效果**覆盖** P6b 的单点重定向;
//   两者同时开都指向 60,结果一致,不冲突。
//
//   要做对照实验时:`0x281FF` = 关掉三兄弟(第六批的配置),`0x28FFF` = 本批修复。
#define FL_G_DERIVED2  (FL_G_DER_FPS | FL_G_DER_MSPF | FL_G_DER_FPMS)
// 实测供参考(历史,不当理由用):关掉后渲染 89.75 / 逻辑 14.97,与全开无差别 ——
// **那条读数看不见建筑解包动画**,所以它对这个问题没有鉴别力。
#define FL_G_ANIMGATE  0x1000        // P7 逐帧绘制更新的**次数门**(修动画过快;见下面 kSigFrameUpdate)
#define FL_G_EVERYTHING 0x3FFFFFF      // 含派生量 + P8 + 动画标尺 + 混合斜坡量尺 + 时钟修正 + P6b + 原版帧率下的时钟修正
                                      // + 车身外观探针/节拍门/插值(2026-09-21,0x80000 / 0x100000 / 0x200000)+ 第四个派生量(0x400000),仅供对照实验
// ⛔ 2026-09-17 P7「动画次数门」已从默认集合**移除**,理由见 kSigFrameUpdate 上方的订正。
//    实测:开了它,录像回放**当场不同步** —— 它删的是模拟步骤,不是动画。
// ★ 2026-09-17 动画定位会话:默认集合加入 FL_G_CLOCK(0x8000)。
//   它不是「可选优化」,是**修复** —— 不加它,渲染到 90 帧而动画快 3 倍,就是用户报的那个 bug。
//   0x1FF 这个历史值保持可复现:要做「有时钟修正 vs 没时钟修正」的对照实验时,
//   显式用 FrameLabSetGroups(0x1FF) 关掉它即可(此时 0x1FF 就是老默认集合,含义不变)。
// ★★ 2026-09-17(第六批订正):再加入 FL_G_ANIMSTEP(0x20000,P6b 动画时间步)。
//   ⇒ 默认集合 = 0x281FF。**注意这不是历史值**:历史上默认是 0x81FF、测量配置是 0xC1FF,
//     那两份都**不含** P6b(位 0x20000 那时还不存在)。要做历史对照实验时显式给 0xC1FF 即可。
// ★★★ 2026-09-17(第七批):再加入 FL_G_DERIVED2(0xE00,帧率三兄弟)—— 这是
//   「60 帧建筑动画回归」的真因修复,理由见上面 FL_G_DERIVED2 定义处那一整段。
//   ⇒ 默认集合 = 0x28FFF。
//   ⚠ 对照实验的两个掩码(务必记牢):
//       0x28FFF = 第七批修复(当前默认)
//       0x281FF = 关掉帧率三兄弟(第六批的配置 ⇒ 建筑解包动画会快一倍)
//   若某一批建筑动画**变慢**一倍而不是变好,说明「引擎 tick 到 60」这个前提不成立,
//   把 FL_G_DERIVED2 从默认里去掉即可回退(证据会同时出现在 displayclock 的读数里)。
#define FL_G_ALL       (0x1FF | FL_G_CLOCK | FL_G_ANIMSTEP | FL_G_DERIVED2)   // 默认 = 0x28FFF

// ── 2026-09-17 动画定位会话:P8 客户端帧率全局写入(实验组,默认**关**) ──────────────
// 为什么要做这个实验(完整推理见 RE-动画推进点-2026-09-17.md):
//   引擎里有一大批「秒 → 帧」换算写成 `(double)客户端帧率 * 秒`,还有一批「每帧步长」写成
//   `1.0 / X / (double)客户端帧率`。它们算出来的是**帧数 / 每帧步长**,而消费它们的东西
//   (逐帧推进的动画、过渡、计时器)是按**渲染帧**走的。
//   本补丁的设计是「那个全局一个字节都不动,只重定向读者」—— 于是这些读者仍按 30 算,
//   而实际渲染已经是 90 帧/秒 ⇒ 帧数只够原版的 1/3、每帧步长却是 3 倍 ⇒ **一切按渲染帧
//   推进的东西都快 3 倍**。
//   ⛔ 2026-09-17 订正:上面这段**推理本身是错的**(见 RE-动画推进点-2026-09-17.md 第 12 节),
//     但它的结论「快 3 倍」碰巧对了 —— 真因是游戏时钟(见下面的 FL_G_CLOCK),不是这些换算。
//     这里保留原文只为记录当时的思路,不要照它推理。
//   ⛔ 另一处错:我原先写「`dword_CE176C` 已经被 R3 重定向到新帧率了」。**没有。**
//     R3 改的是 `sub_5B81A0` 里那条 `div` 的**操作数**,而这个函数只在启动时跑一次
//     (虚表槽 0x00C09BC4,GameClient),我们是中途注入 ⇒ 数据上 `dword_CE176C` 始终是 33。
//     实测读数:groups=0x41FF 那一局 `read 0x00CE176C` = 33(原版值),不是 11。
//     这个「指令改了、数据没改」的错配正是 3 倍的真因,详见 FL_G_CLOCK。
//   ⇒ 本组就是把这个错配消掉:直接把客户端帧率全局写成目标帧率(等价于把那批读者一次性全改对)。
//   默认关闭的理由:这会把全局翻成「默认全都变」,与项目铁律相反 —— 所以它只作**实验**,
//   用 FrameLabSetGroups(0x21FF) 开;若实验证明动画被修好,再逐个审计哪些读者必须留在 30。
//
// ── ★★★ 2026-09-17(第八批):本组的**定性被推翻了** —— 它不是「方向相反的实验」, ──
//   而是第七批修复的**另一半**。理由(证据链见 CHANGES.md §Z / RE 文档 §23):
//     第七批修的是**派生浮点常量** 0xCDBC50/54/5C(FL_G_DERIVED2):它们由这个整数全局
//     在启动时算出来(`fild [0x00CAF9D4]`)。真凶 sub_6F6CB0(建筑解包动画)把帧计数器
//     除以「时长 × 0xCDBC5C」,所以 0xCDBC5C 必须 = 呈现帧/毫秒 —— 那一半已实测修好。
//     但**整数全局本身**没动,于是所有直接读它的读者仍按 30 算:
//       · 0x00CAF9D4 初值就是 30,全程序**无赋值、无取地址** ⇒ 静态名义值,语义 = 当前客户端帧率
//       · 0x00CAF9D0 初值 15,语义 = 逻辑 tick 频率(与 clockrate 实测 66667µs = 1/15 秒互证)
//       · 56 处读者里,已逐条读过的三处都因为「仍按 30 算」而快 2 倍(60 帧下):
//           sub_5EB5E0(W3DView 等 N 帧倒计时:30.0 ÷ 该全局,因子恒为 1)、
//           sub_6CDBD0(帧预算 该全局 × ms × 0.001)、
//           sub_6293D0(实测帧率被钳到 [15,30],60 帧下仍报 30)
//     ⇒ 所以本组与 FL_G_DERIVED2 是**同一个修复的两半**,不是两个互相排斥的方向。
//   仍然**默认关**的理由(没有变):56 处读者里还有没逐条核对的,而且至少一处
//     (P5 粒子过期,pimul)读它**就是**为了保持 30 基准 —— 写 60 会让那处翻倍。
//   ⇒ 处理办法:install() 收尾会打一条**量化的残余风险告警**(只在 targetFps≠30 且本组关时),
//     把「哪些读者、快几倍、怎么开」直接写进日志。要做 A/B 时:
//       0x2CFFF = 当前默认(P8 关,本组所覆盖的读者仍按 30 算)
//       0x2EFFF = 当前默认 + P8(= 0x2CFFF | 0x2000)
//     两者都跑一遍 FrameLabFpsWindow / FrameLabLogicFps / FrameLabClockPerLogicFrame,
//     三条不变量必须都不变(渲染 = 目标、逻辑 = 15.00、clockrate ≈ 66667)。
#define FL_G_FPSGLOBAL 0x2000

// ── ★★ 2026-09-17 动画定位会话:游戏时钟推进量修正(默认**开**)★★ ────────────────────
// 这是本轮找到的**真因**,也是整个「90 帧但动画快 3 倍」的答案。完整证据链:
//
// 1) 时钟在哪里被推进 —— 全局只有一个写入点(sub_5FE9D0 引擎主 tick):
//      005FEB9D  A1 8813CE00    mov eax, [0x00CE1388]     ; 时钟(毫秒)
//      005FEBA2  03 05 6C17CE00  add eax, [0x00CE176C]     ; ★ 时钟 += 每帧毫秒
//      005FEBA9  A3 8813CE00    mov [0x00CE1388], eax
//      005FEBAE  E8 ..          call 0x936910             ; WW3D 时间 = 时钟
//      005FEBBA  E8 ..          call 0x99bf90             ; APT(UI) 时间 = 时钟
//    ⚠ 订正(2026-09-17,用 tools/re/xref.py 整段线性反汇编穷尽枚举):
//      写它的有 **3 处**,不是 1 处 —— 两处在 sub_5FE9D0(本组要改的那两条分支),
//      第三处是 sub_5F4310 里的 `dword_CE1388 = 0`(**重置清零**,开局/换地图时走,
//      不需要改:重置之后时钟照旧按我们的 g_msPerFrame 推进)。
//      读它的有 **12 处**(早先按 pattern 扫描只数到 7 —— `mov edx,[时钟]`/`mov ecx,[时钟]`
//      这两种编码被 `A1` 那条 pattern 漏掉了)。
//
// 2) 时钟的推进量是**每客户端帧加一次固定毫秒数**(不是按真实经过时间):
//      dword_CE176C = 1000 ÷ 客户端帧率,原版 = 1000÷30 = 33。
//      原版:30 帧/秒 × 33 毫秒 = 990 毫秒/秒 ⇒ 实时(已知原版 1% 偏慢,是它自己的取整)。
//      本补丁:渲染真到 90 帧/秒,而 dword_CE176C 仍是 33 ⇒ 90 × 33 = 2970 毫秒/秒
//      ⇒ **时钟跑 3 倍快**。
//      ★为什么 R3 没救回来:见上面 FL_G_FPSGLOBAL 的订正 —— R3 只改了指令操作数,
//        而算这个值的 sub_5B81A0 在启动时(我们注入之前)就跑完了,数据仍是 33。
//      实测(2026-09-17,groups=0x41FF,pid 16556):Δ时钟 52569 毫秒 / Δ墙钟 21.75 秒
//        = 2.42×,而同期渲染 76.33 帧/秒 × 33 毫秒 = 2.54× —— 吻合。
//        同一录像 stock 组:11748 毫秒 / 16.2 秒 = 0.73×,渲染 ~22 帧/秒 × 33 = 0.73× ✓
//
// 3) 动画的时间步长直接取自这个时钟 —— 所以时钟快 3 倍,动画就快 3 倍:
//      sub_90ECF0(动画推进)开头:
//        0090ED67  mov ecx, [0x00CE1388]        ; 当前时钟
//        0090ED6F  sub eax, [esi+0xC8]          ; ★ dt = 当前时钟 − 上次时钟(毫秒)
//        0090ED80  mov [esi+0xC8], ecx          ; 记下本次时钟
//      然后 dt 换算成秒并乘以动画帧率,累加到当前帧:
//        0090EED0  fild dword [esp+0x18]        ; dt(毫秒)
//        0090EEEE  fmul [0x00BE660C]            ; × 0.001 ⇒ dt(秒)
//        0090EF1E  addss xmm0, [esp+0x14]       ; ★ 当前帧 += dt秒 × 动画帧率
//        0090EF27  movss [edi], xmm0            ; ★ 写回当前帧
//      三个动画更新函数(sub_90F820 / sub_90FD20 / sub_90FF70)都先
//      `mov eax,[0xCE1388]; cmp eax,[esi+0xC8]; je 跳过` 再 `call sub_90ECF0`
//      —— 时钟不变就完全不推进,进一步证明动画是**按时钟增量**走的。
//
// 4) 逐条对上用户观察:步兵腿快 3 倍 ✓(动画帧正比于 dt);建筑建造动画「直接跳完工、
//    而 sim 上还在建造中」✓(动画状态机在 1/3 的时间里走完,sim 由阶段计数驱动、不受影响)。
//    射速/数值不变 ✓(逻辑帧率全局 0x00CAF9D0 仍是 15,一个字节没动)。
//
// 修法:把时钟推进那两处读 `dword_CE176C` 的**操作数**改指向我们自己的 g_msPerFrame
//   (= 1000 ÷ 目标帧率)。这就是本项目一贯的做法:
//   **不写游戏全局,只重定向需要新值的那个读者。**
//   ⚠ 2026-09-17 订正:时钟推进有**两条互斥分支**(见 kSigClockAdvance / kSigClockAdvance2),
//     反编译只显示一条。只改一条会留下约 5.6% 的残留偏快 —— 必须两条都改。
//   为什么不用「往 dword_CE176C 写 11」那种更省事的做法:那个全局还有 14 处别的读者
//   (两种方向:毫秒÷它→帧数、它×帧数→毫秒,后者含音效时长),我**无法证明**它们都需要新值
//   (原版音效时长可能按 30 帧/秒的帧数算)。只改时钟这两处,其余 14 处保持原版行为 ——
//   失效方向仍然是安全的。
// 为什么默认**开**:它不是实验,它就是修复本身。关掉它 = 回到「90 帧但动画快 3 倍」。
// 为什么用整数 1000÷目标帧率(90→11):原版也是整数除法(1000÷30→33),保持同一套取整
//   特性 ⇒ 90 帧下 990 毫秒/秒,与原版同样 1% 偏慢,而不是引入新的偏差。
// 本组包含**四处**操作数重定向:
//   ① 0x005FEBA4  时钟推进(常态分支)  `add eax,[0x00CE176C]`   → 改指 &g_msPerFrame
//   ①'0x005FEBE3  时钟推进(每30帧分支) `mov ecx,[0x00CE176C]`   → 也改指 &g_msPerFrame
//   ② 0x00BB3C5E  写入者①(小函数 `div [帧率]` 之后那条 mov)→ 改指 &g_msPerFrameQuarantine
//   ③ 0x005B81A7  写入者②(setter sub_5B81A0 里那条 mov)→ 也改指 &g_msPerFrameQuarantine
// 为什么②③也要改:那两个写入者算的/收的都是「每帧毫秒」,它们若在运行中重跑(分辨率或
//   窗口模式变化时有可能),原本会把新值写进共享全局 dword_CE176C —— 而那个全局另有 14 处
//   读者(两种方向,含音效时长),它们必须永远保持启动时那份 33。改指之后,它们只会写隔离区,
//   共享全局一个字节都不会被碰。②③互相独立,任一失败都只 WARN 不中断安装。
#define FL_G_CLOCK     0x8000

// ── 2026-09-17 动画定位会话:动画标尺(只读探针,默认**关**)───────────────────────
// 为什么需要它:整个项目的卡点是「高帧率下动画快 3 倍」,而判据一直只有「用眼睛看」——
// 静帧像素 diff 已被证伪(见 tools\watch.ps1 顶部注释)。没有客观读数,既没法验收,
// 也没法二分:你能看出「动画不对」,但没法说出「快了多少」。
// 这个探针把「动画快不快」变成数字。它读的是**引擎自己的当前动画帧**:
//   位置 = [[drawModule+0xC] + 0xB0](sub_8EDED0 反汇编核实:lea esi,[eax+0xA8] 后取 [esi+8])
//   含义 = 当前帧(float),同函数的 `+0x14` 是总帧数 ⇒ 分数 = 当前帧 / (总帧数-1)
// 默认关闭的理由:它会改一条 call 的目标。虽然包装函数先记录再原样转发、对调用方逐位无差别,
// 但按本项目铁律「默认一个字节都不动」,它必须显式开(见 FrameLabAnimRate 的注释)。
#define FL_G_ANIMPROBE 0x4000

// ── ★★★ 2026-09-17(第六批):P6b 混合斜坡量尺(只读探针,默认**关**)──────────────────
// 为什么必须再有一把尺子:本轮定位出的建筑动画真因是**第二条动画时间路径** ——
//   sub_903910 里 `dt = 帧号增量 ÷ 客户端帧率`,消费者是 sub_8EA220(混合斜坡推进器)。
//   而项目已有的两把尺子都量**第一条**路径(游戏时钟,sub_90ECF0):
//     · clockrate(FrameLabClockPerLogicFrame):量「每逻辑帧推进多少毫秒**游戏时钟**」
//     · animrate (FrameLabAnimRate):         量「每逻辑帧动画**帧号**推进多少」
//   它们对 sub_903910→sub_8EA220 这条路径**完全看不见** —— 除数从 30 改成 60,
//   这两个读数一个数都不会变。所以 P6b 修得对不对,此前只能靠眼睛。
//
// 判据(与 clockrate 同源,是同一类**与渲染帧率无关的不变量**):
//   每逻辑帧推进的「混合斜坡动画时间」必须恒等于 1/15 秒 = 66.67 毫秒。
//     原版 30 帧(r=2):2 次调用 × dt(1/30 秒) = 66.67 毫秒 ✓
//     修好后 60 帧(r=4):4 次调用 × dt(1/60 秒) = 66.67 毫秒 ✓  ← P6b 生效
//     没修 60 帧(r=4):4 次调用 × dt(1/30 秒) = 133.33 毫秒 ✗  ← 动画时间跑一倍
//   ⇒ 同一个 60 帧配置下,开/关 FL_G_VISUAL 两个读数应当差 2 倍。这就是 P6b 的直接判据。
//
// 额外两个读数用来**验证前提假设**,而不是只报一个数(不然读错了也不知道):
//   · 每次调用的平均 dt 应当 = 1 ÷ 实际渲染帧率(60 帧 ⇒ 16.67 毫秒)。
//     它同时验证了「vtbl[116] 确实按渲染帧率递增」这个前提 —— 前提错了这个数会露馅。
//   · 每逻辑帧的调用次数应当 = r(60 帧 ⇒ 4 次)。
//
// 默认关闭的理由与动画标尺相同:它要改一条 call 的目标。虽然包装先记录再原样转发、
//   对调用方逐位无差别,但按本项目铁律「默认一个字节都不动」,它必须显式开。
// ★ sub_8EA220 全程序**只有一个调用点**(就是 sub_903910 里那条)⇒ 拦这一条即完整覆盖。
#define FL_G_STEPPROBE 0x10000

// ── ★★★ 2026-09-17(第六批·订正):P6b 动画时间步**独立成组** ──────────────────────────
// 我最初把 P6b 归进 FL_G_VISUAL,理由是「与 P6 同一个失效模式」。**那个理由不成立**,已订正:
//   · 它们是**两个不同的补丁点**(P6 = `8F3CE0` 的常量 1/30;P6b = `90393C` 的操作数),
//     归成一组 ⇒ 二分时只能「一起开 / 一起关」,无法分辨是哪一个在起作用。
//   · 更糟的是:P6b 所在的 `sub_903910` 在**正常对局回放里根本不跑**
//     (实测:三次窗口共约 40 秒、窗口可见、逻辑帧在推进,`sub_8EA220` 调用数 = 0)。
//     同组会把「这一处压根没被触发」这件事**藏起来** —— 关掉 FL_G_VISUAL 看起来「有效果」,
//     其实效果全部来自 P6。
// ⇒ 拆开。`FL_G_VISUAL` 恢复它本来的含义(只有 P6),P6b 用下面这个新位。
// 分组掩码对照(60 帧、含两把量尺):
//   0x1C1FF = P6 开、P6b 关   ← 历史上所有测量用的配置(位 0x080 开、0x20000 关)
//   0x2C1FF = P6 开、P6b 开   ← 现在的默认
//   0x1C17F = P6 关、P6b 关
//   0x2C17F = P6 关、P6b 开   ← 这个 2×2 才能把两处分清
#define FL_G_ANIMSTEP  0x20000
// ── 2026-09-18(第十一批):「在原版帧率下也修正时钟」────────────────────────────────
// 背景:用户报「30 帧下载具上下抖动剧烈」,而 30 帧正是**原版帧率** —— 补丁在那里本该
//   什么都不做。用黑匣子飞行记录仪实测两臂(measure30 = 原版基线 / fps30 = 打补丁到 30):
//       插值系数 frac : 两臂都是精确的 0.5 / 1.0,各 256 次 —— 完全一致
//       逻辑帧推进    : 两臂都是 +0 / +1 各约 50% —— 完全一致
//       游戏时钟增量  : 原版恒 33;**补丁是 33,33,34 循环** ← 唯一的差异
//   ⇒ 30 帧下补丁与原版**只差这一个量**。而这个量来自 FL_G_CLOCK 的定点累加器:
//     它的职责是修「r 个客户端帧 × 整数毫秒 ≠ 66.667 毫秒」的漂移(r=4 时 −4%,r=2 时 −1%),
//     代价是逐帧增量在原值附近摆动。
//   ⇒ 既然 30 帧下它带来的是一个 ±1 毫秒、10 Hz 的锯齿,而收益只有 −1% 的漂移修正,
//     就把它**在原版帧率下关掉**:g_msPerFrame 保持原版的恒定 33,补丁在 30 帧下与原版逐位一致。
//     60/90 帧下 r 更大、漂移大得多,累加器照旧生效。
//   这个位 = **强制**在原版帧率下也修正(只为做对照实验,默认关)。
//   判据可测:开它 ⇒ 时钟增量出现 33,33,34;不开 ⇒ 恒 33。用 ring_analyze.py 第 5 节看。
#define FL_G_CLOCK_AT_RETAIL 0x40000

// ── ★★★ 2026-09-21(悬挂路径会话):车身外观(悬挂 / 俯仰 / 侧倾)—— 探针 + 30 Hz 节拍门 ──────
// 背景:用户实机(170 Hz 屏,真 90 帧)录像:蓝色海啸坦克行驶时**上下颠簸剧烈**,原版 30 帧正常。
//   此前两把量具(动画帧号、世界坐标)都测不出差异 —— 因为它们在结构上就看不见这条路径:
//     sub_53F080 (Drawable::draw)
//       → sub_535DD0 (applyPhysicsXform:平移视觉 Z,再依次转 俯仰 / −侧倾 / 偏航)
//         → sub_532CA0 (calcPhysicsXform:按**显示帧号**去重 loco+0xBC,再按 locomotor 外观分派)
//           → 外观 1/7 sub_526BD0(轮式带悬挂)| 2/3/4 sub_51E800 | 8 sub_5262D0
//   与 EA 公开的 Generals `Drawable.cpp`(calcPhysicsXformWheels 等)近乎逐行对应。这一族函数是
//   **每显示帧递推一次、不含 dt** 的迭代(逐条见 RE-车身悬挂-2026-09-21.md):
//     · 俯仰/侧倾弹簧阻尼      rate += −k·(角 − 地面角) − c·rate;  角 += rate·u
//     · 随机颠簸 sub_5269C0    速度 > 10% 最大速度、且角速度已衰减到阈值以下才再踢一次
//     · 加速点头/侧倾的第二套弹簧、sub_51A220 摆动相位累加、sub_51A2D0 偏航衰减、
//       四个轮位偏移 ×0.5 平滑、轮角 ×0.1 平滑
//   原版每秒递推 30 次;渲染到 60/90 帧后每秒 60/90 次 ⇒ 整套车身运动的**时间轴被压缩 2/3 倍**。
//   它只改渲染矩阵、不动逻辑坐标;随机数走**客户端**种子(sub_600AB0 → 0x00CB01F0),
//   不碰逻辑随机数;也不读任何帧率常量 —— 所以帧率读者普查(0xCAF9D4 / 0xCDBC5x / 0xCE176C)查不到它。
//
// 补丁点:sub_535DD0 里**唯一**那条 `call sub_532CA0`。现场字节(原始二进制读的):
//     00535E7B  F3 0F 11 44 24 14    movss [esp+0x14], xmm0      ; 四个输出清零(后两个)
//     00535E81  F3 0F 11 44 24 18    movss [esp+0x18], xmm0
//     00535E87  E8 14 CE FF FF       call  sub_532CA0            ; ★ 只改这条 call 的 rel32(+13)
//     00535E8C  84 C0                test  al, al
//     00535E8E  0F 84 B4 00 00 00    je    不施加任何姿态        ; ★ 返回 0 ⇒ 这一帧车身没有姿态
//   原目标 = hit + 17 + rel32,必须等于 0x00532CA0(绝对 VA,别再加基址 —— 见 kEngineBlendStepVa 的坑)。
//   与动画标尺 / 混合斜坡量尺同一种手法:5 字节原地改写、长度不变、进撤销表、可整体回滚。
//
// 两个位,**都默认关**(不在 FL_G_ALL 里;GUI 的 0x2CFFF 也不含):
//   FL_G_CHASSISPROBE 0x80000   只读探针:包装先记账再**原样**转发,行为逐位不变。
//                               给 `flctl chassisrate / chassisslots` 和黑匣子「姿态轴」用。
//                               与动画标尺一样,只量不改(measureex)模式下也可装 —— 原版基线要用同一把尺子。
//   FL_G_CHASSIS30    0x100000  修复候选:节拍门。只在「30 Hz 虚拟帧号」变化的显示帧放行原函数,
//                               其余显示帧回放缓存的四个输出(见 src/chassis_gate.h)。
//                               目标帧率 == 原版帧率时等于直通;只量不改模式下**不生效**。
//   任一位开着就会装包装函数;对照实验用:
//       0x28FFF | 0x080000 = 0x0A8FFF   默认集合 + 探针(量「修之前」)
//       0x28FFF | 0x180000 = 0x1A8FFF   默认集合 + 探针 + 节拍门(量「修之后」)
// 判据(与 clockrate / blendrate 同类的不变量):**同一辆车每逻辑帧的递推步数恒为 2**
//   (原版 30 帧:2;没修的 60/90 帧:4/6;修好后任何帧率:2)。读数 = `flctl chassisrate` ×1000。
// ⚠ 状态:2026-09-21 只做了离线构建与自检,**实机一次都没跑过**;默认关,转正前必须实测 A/B。
#define FL_G_CHASSISPROBE 0x80000
#define FL_G_CHASSIS30    0x100000
// ── 2026-09-21(同日追加):节拍门的**插值**变体,为 90 帧准备 ──────────────────────────────
//   FL_G_CHASSIS30 单开 = 「回放」:车身姿态每秒只变 30 次(60 帧停 2 帧、90 帧停 3 帧),而车的平移每帧都在动
//   ⇒ 车身相对自己的平移有 30 Hz 台阶,帧率越高越显眼。再开这一位 = 递推仍严格 30 Hz,
//   但**显示**时在相邻两个 30 Hz 位姿之间按相位线性插值(90 帧 φ = 1/3, 2/3, 1)⇒ 每个显示帧姿态都在变、
//   动力学与原版逐步相同,代价是显示晚至多 33 毫秒。细节与边界见 src/chassis_gate.h。
//   单开这一位没有意义(它只是节拍门的一个模式),install() 会把它当成「节拍门 + 插值」。
//       0x28FFF | 0x180000 = 0x1A8FFF   探针 + 节拍门(回放)
//       0x28FFF | 0x380000 = 0x3A8FFF   探针 + 节拍门(插值)  ← 90 帧的首选候选
#define FL_G_CHASSISLERP  0x200000

// ── ★★★ 2026-09-21(显示帧递推普查):帧率派生量其实有**四个**,第七批只管了三个 ────────────────
//   第四个 = 0x00CDBD34 = 1.0 ÷ flt_CDBC50 = **每显示帧的秒数**(原版 1/30)。初始化器紧跟在
//   0x00CDBC50 的后面(原始字节已核):
//       00BB3150  F3 0F 10 05 00 62 BE 00   movss xmm0,[0x00BE6200]   ; 1.0
//       00BB3158  F3 0F 5E 05 50 BC CD 00   divss xmm0,[0x00CDBC50]   ; ÷ 帧率        ← 用它核对没认错地方
//       00BB3160  F3 0F 11 05 34 BD CD 00   movss [0x00CDBD34],xmm0   ; ★要改写的存量值
//       00BB3168  C3
//   全镜像 1 个写者(就是它,只在启动时跑一次 —— 我们是中途注入,改指令没用,只能改**存量值**)+ 8 个读者,
//   逐个读过,全部是「一个显示帧 = 多少秒」:sub_52C4B0(精灵旋转/脉动,t = 帧号 × 它)、
//   sub_5EA080 / sub_5EA110(贴花淡出,每帧 alpha −= (1÷时长) × 它)、sub_6BDE60(拖尾粒子贴图滚动)、
//   sub_9080B0 → sub_8E3130(子物体计时,每帧 t += 它)、sub_8E2070 / sub_8E42E0(动画时间比较里的「一帧以内」阈值)。
//   渲染到 60/90 帧而它仍是 1/30 ⇒ 这些效果快 2/3 倍。单位一致性的论证与 FL_G_DERIVED2 完全相同。
//   为什么**默认关**(不进 FL_G_ALL,GUI 的默认集合也不含):2026-09-21 才发现,实机一次都没跑过;
//   而且此刻正在验收车身颠簸修复,变量不要混。对照掩码:0x28FFF | 0x400000 = 0x428FFF。
//   完整普查见 RE-按显示帧递推普查-2026-09-21.md。
#define FL_G_DER_SPF      0x400000

// ── ★ 2026-09-21(用户点名「卷屏做成可自选的」):卷屏速度按帧率归一,**默认关、可自选** ─────────────
//   键盘 / 屏幕边缘卷屏(sub_AC0070,状态对象 vtable off_C80FA8 的 +8 槽;加速段走 sub_ABC9C0)的位移是
//       位移 = 方向 × 卷屏系数(flt_CB0338 / flt_CB033C) × **100.0** × (显示帧号 − 上次帧号) × 选项里的卷屏速度
//   帧号差**没有**换算成时间(只有「加速段多长」用了 flt_CDBC50 × 0.25)⇒ 每个显示帧走固定的一步
//   ⇒ 卷屏速度正比于渲染帧率:60 / 90 帧下快 2 / 3 倍。这是手感问题而不是对错问题,所以做成开关:
//       关(默认) = 现状:卷屏随帧率变快;      开 = 每秒走的距离与原版 30 帧相同。
//   做法(仍是「只重定向读者」):那个 100.0 是 .rdata 里的共享常量(0x00C7FF44,全镜像 4 个读者),
//   只把**这两条按帧推进的**读指令的操作数改指我们自己的 g_scroll100 = 100 × 原版帧率 ÷ 目标帧率:
//       00AC0273  F3 0F 10 1D 44 FF C7 00   movss xmm3,[0x00C7FF44]   ; sub_AC0070 稳态段
//       00ABCA04  F3 0F 10 1D 44 FF C7 00   movss xmm3,[0x00C7FF44]   ; sub_ABC9C0 加速段
//   另外两个读者 sub_ABC640 / sub_ABC690 是**事件驱动**的一次性位移(没有帧号差),不动。
//   为什么不去缩放卷屏系数本身:flt_CB0338 / flt_CB033C 各有 3 个写入点(设置 / 选项菜单),运行中会变,
//   我们的副本会过期;而 100.0 是常量。两处必须**一起**改(只改一处,加速段与稳态段速度会接不上)。
//   ⚠ 不含右键拖拽卷屏(sub_ABFF40):它是「每调用一次走 系数 ×(鼠标 − 锚点)」,没有帧号差;
//     它的调用节拍(每帧 / 每次鼠标消息)静态没定下来,先不碰,见 RE-按显示帧递推普查-2026-09-21.md。
//   状态:2026-09-21 静态定位 + 离线构建;**实机一次都没跑过**。
#define FL_G_SCROLL       0x800000

// ── ★★★ 2026-09-22(用户 90 帧实机报告 #2):弹道流(TheTracerManager)30 Hz 节拍门,**默认关** ──────
//   用户原话:帝国武士(JapanAntiInfantryInfantry)开火「原速一轮射击开不了几发子弹,60/90 帧下像机关枪,
//   射速极高」,**但总伤害量没变** ⇒ 模拟侧是对的,快的是纯视觉。
//   真因与车身悬挂是**同一个形状**:`dword_CDB79C`(由 sub_98D690(..., "TheTracerManager") 确证)的
//   update(vtable +20 = sub_5F4960)开头就按显示帧号去重:
//       v2 = (*(vtbl+116))(dword_CDB750);                    // 显示帧号
//       if (v2 != this[42]) { this[42] = v2; sub_5F48C0(this, v2); ... }   // 一次调用 = 推进一步
//   ⇒ 原版每秒推进 30 步,90 帧下每秒 90 步。而生成密度(sub_5F1D30,转储 419149 起)是
//   「每次推进生成 rand(模板+36, 模板+40) 条」(带小数进位器 this[34]),伸展进度 this[38] 同样每次推进一步
//   ⇒ **每秒生成的弹道流条数是原版的 2 / 3 倍** —— 正是「看起来子弹多得像机关枪」。
//   弹道流是纯客户端视觉(不碰模拟),与「总伤害没变」完全一致。
//
//   补丁点 = 客户端逐帧更新总入口 sub_542DF0 里那次调用,11 字节窗口(现场字节已核):
//       00542E49  8B 0D 9C B7 CD 00   mov  ecx,[0x00CDB79C]   ; TheTracerManager ← 让签名唯一的就是它
//       00542E4F  85 C9               test ecx, ecx           ; ★ 窗口从这里开始
//       00542E51  74 07               je   0x542E5A
//       00542E53  8B 01               mov  eax,[ecx]
//       00542E55  8B 50 14            mov  edx,[eax+0x14]
//       00542E58  FF D2               call edx
//   ⚠ 紧邻的上一处(dword_CDB794)后 11 个字节**完全相同**,所以特征必须带上 `mov ecx,[0xCDB79C]`。
//   换成 `call fl_wrap_tracer`(5 字节)+ 6 个 NOP;包装函数拿 ecx = 管理器,只在「30 Hz 虚拟帧号变化」
//   时才转发 vtbl[0x14]。**不需要插值**:原版本来就是 30 Hz 步进,这样与原版逐步相同。
#define FL_G_TRACER       0x1000000
static const short kSigTracerUpdate[] = {
    0x8B,0x0D,W,W,W,W,                    // mov  ecx,[TheTracerManager]   ← 操作数在 +2,必须是 0x00CDB79C
    0x85,0xC9,                            // test ecx, ecx                 ← 窗口起点在 +6
    0x74,0x07,                            // je   +7
    0x8B,0x01,                            // mov  eax,[ecx]
    0x8B,0x50,0x14,                       // mov  edx,[eax+0x14]
    0xFF,0xD2                             // call edx
};

// ── ★ 2026-09-22:单位头顶状态图标的乒乓动画,**默认关** ────────────────────────────────────────
//   sub_51A4D0:`帧 = 显示帧号 × dword_CAD5F4(=15) ÷ dword_CAF9D4(=30) % (2N)`,再折回来做乒乓。
//   分子 15 = 逻辑帧率、除数 30 = **原版**客户端帧率,而补丁故意不写 0xCAF9D4(它仍是 30)
//   ⇒ 90 帧下这组图标动画快 3 倍。调用点在 sub_527900 / sub_528860,取的是 dword_CDB7A0 那组共享图标。
//   修法是一处 4 字节:把那条 `div [0x00CAF9D4]` 的操作数改指 g_fpsRender ⇒ `帧号 × 15 ÷ 目标帧率`
//   = 逻辑帧数,与原版逐帧相同。**不是**用户说的熔炉光,只是顺手修掉的同类。
#define FL_G_ICONANIM     0x2000000
static const short kSigIconPingPong[] = {
    0x8B,0xD0,                            // mov  edx, eax                 (显示帧号)
    0xA1,W,W,W,W,                         // mov  eax,[dword_CAD5F4]
    0x0F,0xAF,0xC2,                       // imul eax, edx
    0x33,0xD2,                            // xor  edx, edx
    0xF7,0x35,W,W,W,W,                    // div  dword [0x00CAF9D4]       ← 操作数在 +16
    0x8D,0x0C,0x36                        // lea  ecx,[esi+esi]
};
static const short kSigScrollStep1[] = {                 // sub_AC0070 稳态段,命中 0x00AC0263
    0x89,0x4C,0x24,0x10,                  // mov  [esp+0x10], ecx         帧号差
    0xDB,0x44,0x24,0x10,                  // fild dword [esp+0x10]
    0x7D,0x06,                            // jge  +6
    0xD8,0x05,W,W,W,W,                    // fadd [2^32]                  无符号修正
    0xF3,0x0F,0x10,0x1D,W,W,W,W,          // movss xmm3,[100.0]           ← 操作数在 +20
    0xD9,0x5C,0x24,0x10,                  // fstp dword [esp+0x10]
    0xF3,0x0F,0x59,0xC3,                  // mulss xmm0, xmm3
    0xF3,0x0F,0x59,0xCB                   // mulss xmm1, xmm3
};
static const short kSigScrollStep2[] = {                 // sub_ABC9C0 加速段,命中 0x00ABC9FC
    0x7D,0x06,                            // jge  +6
    0xD8,0x05,W,W,W,W,                    // fadd [2^32]
    0xF3,0x0F,0x10,0x1D,W,W,W,W,          // movss xmm3,[100.0]           ← 操作数在 +12
    0xD9,0x5C,0x24,0x08,                  // fstp dword [esp+8]
    0x8B,0x44,0x24,0x04,                  // mov  eax,[esp+4]
    0xF3,0x0F,0x59,0xC3,                  // mulss xmm0, xmm3
    0xF3,0x0F,0x59,0xCB                   // mulss xmm1, xmm3
};
static const short kSigSecPerFrameInit[] = {
    0xF3,0x0F,0x10,0x05,W,W,W,W,          // movss xmm0, [1.0]
    0xF3,0x0F,0x5E,0x05,W,W,W,W,          // divss xmm0, [客户端帧率浮点]   ← 操作数在 +12,必须是 0x00CDBC50
    0xF3,0x0F,0x11,0x05,W,W,W,W,          // movss [每显示帧秒数], xmm0    ← 操作数在 +20 = 要改写的全局
    0xC3                                  // ret
};
static const short kSigChassisCall[] = {
    0xF3,0x0F,0x11,0x44,0x24,0x14,        // movss [esp+0x14], xmm0
    0xF3,0x0F,0x11,0x44,0x24,0x18,        // movss [esp+0x18], xmm0
    0xE8,W,W,W,W,                         // call sub_532CA0          ← rel32 在 +13
    0x84,0xC0,                            // test al, al
    0x0F,0x84,W,W,W,W                     // je   (返回 0 ⇒ 不施加姿态)
};
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


// P7:`sub_6027D0` 里那次「每客户端帧一回」的绘制/动画更新调用(0x602942,13 字节,唯一命中):
//     mov edx,[esi] / push eax(阶段) / mov eax,[edx+0x90](槽 144) / mov ecx,esi / call eax
// ★为什么要拦它:原版 r=2 时每逻辑帧只走 **2 次**(阶段 1、4);我们把 r 抬到 6 之后变成 **6 次**
//   ⇒ 凡是「按这次调用推进固定量」的动画就快 r/2 倍。用户 2026-09-17 实机看到:
//   步兵腿速明显过快、建筑建造动画直接跳到完工(而 sim 仍在建造中)—— 正好是 3 倍。
// ★注意区分:按**阶段**推进的东西不受影响(两种情况下每逻辑帧都是 6 个阶段),
//   只有按**调用次数**推进的才会快。所以把转发次数钉回「每逻辑帧 2 次」即可。
// ⛔⛔ 2026-09-17 实机订正:**这个门是错的,已从默认集合移除。**
//   实测(用户实机 + 录像回放):开门后动画**依旧过快**,而且过一会儿**直接不同步**。
//   病因:槽 144 指向 `0x616130`(批次函数)。我看到它「每客户端帧调一次」就假设它是绘制侧的 ——
//   **但它是推进阶段的**。而且 `sub_616130` 里的追赶循环写在 `if (r < 6)` 内,
//   **r=6 时不追赶**,每次调用只推进一个阶段 ⇒ 我跳掉 6 次里的 4 次,
//   等于每逻辑帧**丢掉 4 个阶段的模拟更新** ⇒ 锁步回放当场不同步。
//   ★教训:我用「调用频率看起来像绘制」代替了「它到底做什么」,没先确认语义就动手。
//   代码保留仅供后人看这条弯路;要复现请显式 FrameLabSetGroups(0x11FF)。
static const short kSigFrameUpdate[] = {
    0x8B,0x16, 0x50, 0x8B,0x82,0x90,0x00,0x00,0x00, 0x8B,0xCE, 0xFF,0xD0
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
// ★ 2026-09-17:引擎的「每客户端帧毫秒数」全局。地址从 sub_5FE9D0 反汇编读出:
//     005FEBA2  03 05 6C17CE00   add eax, [0x00CE176C]   ; A3 6C 17 CE 00 → 0x00CE176C
//   我们**绝不写它**(它另有 14 处读者,方向还不一致),只在定位「时钟推进点」时核对
//   操作数确实指着它 —— 防止特征码撞到别的 `add eax,[某全局]` 上。
static const uintptr_t kEngineMsPerFrameVa = 0x00CE176C;
// 游戏时钟全局(毫秒)。只读,**我们一处都不改**:它是修复的判据(见 FrameLabClockPerLogicFrame)。
// ⚠ 计数订正(2026-09-17,tools/re/xref.py 穷尽枚举):写它 3 处、读它 12 处。
//   早先按 pattern 扫描估的「读者 7 处、写它 1 处」都不准 —— 见 kSigClockAdvance 上方的订正。
static const uintptr_t kEngineClockVa = 0x00CE1388;
// ★★ 2026-09-17(第六批,建筑动画回归定位):引擎的「客户端帧率**浮点**值」全局。
//   地址来源不是猜的 —— 用 tools/re/xref.py 穷尽枚举 0x00CDBC50 的引用得到 17 处,
//   其中三处在 W3D/动画区间(0x90xxxx),且 sub_903910 里那条是
//      00903938  F3 0F 5E 05 50 BC CD 00   divss xmm0,[0x00CDBC50]
//   反编译 sub_903910 给出量纲:dt(秒) = (帧号增量) ÷ flt_CDBC50 ⇒ 它必然是「帧/秒」。
//   ⇒ 原版 = 30.0。本补丁**不改它**(它另有 16 处读者),只改需要新值的那些读者。
static const uintptr_t kEngineFpsFloatVa = 0x00CDBC50;
// ★★ 2026-09-17(第六批):混合斜坡推进器 sub_8EA220 的地址 —— 用来核对「那条 call 真的调去了它」。
//   ⚠ 这是**绝对 VA**,不是 RVA。本项目所有引擎地址常量都是这个约定,成立的前提是
//     「主程序加载在自己的首选基址 0x00400000」—— 日志每次都打印
//     `主模块基址 00400000 … 大地址感知 关`,所以 VA = IDA 里看到的那个数,不需要再加基址。
//   ⚠ 踩过的坑(2026-09-17):这里最初写成 `g_base + 0x008EA220`,而 g_base 已经是 0x00400000,
//     于是期望值算成 0x00CEA220,量尺自己把自己拒装了 —— 日志里那条
//     `call @00903947 的目标是 008EA220,期望 00CEA220` 就是它。**别再加基址。**
static const uintptr_t kEngineBlendStepVa = 0x008EA220;
// ═══════════════════════════════════════════════════════════════════════════════════════════
// ★★★ 2026-09-17(第七批,建筑动画回归的**真因**):引擎「客户端帧率派生量」的另外两个。
//   · 0x00CDBC5C = 「帧/毫秒」= 帧率 × 0.001   (原版 0.03)
//   · 0x00CDBC54 = 「每帧毫秒」= 1000 ÷ 帧率   (原版 33.333…)
//   它们与 0x00CDBC50(帧/秒,原版 30.0)是同一个「帧率三兄弟」,由同一段 init 代码算出。
//
//   ── 为什么现在**敢**说它们必须跟着帧率走(推翻第五批「无法证明那 43 个读者需要新值」)──
//   取证在 sub_6F6CB0(0x006F6CB0),它是 **"StructureUnpackUpdate"(建筑解包/建造动画)** 的
//   进度算法。它把**两种不同来源的时间**放进同一个式子相除:
//       分子 = 显示器帧计数器 − 起始帧号                        ← 单位:**渲染帧**
//              v5 = (vtbl[116](dword_CDB750)) - v7
//       分母 = 时长(秒) × 1000 × flt_CDBC5C × 速率             ← 单位:渲染帧(靠它换算)
//              v11 = (int64)(sub_6D3D30(clock, this[56]) * 1000.0 * flt_CDBC5C * v12)
//       进度 = v5 ÷ v11                                          ← 无量纲,必须自洽
//   分子分母必须同单位 ⇒ **flt_CDBC5C 必须等于「渲染帧/毫秒」**。
//
//   ── 那么帧计数器按多快 tick?三条独立证据都指向「渲染帧率」──
//   ① dword_CDB750 经 vtbl[104] 取得,再由它经 vtbl[132] 取出并显式命名为 "TheDisplay"
//      (sub_4xxxxx 全局初始化 @ 465885/465886/465887)⇒ 它数的是**显示器呈现帧**。
//   ② 本补丁 FL_G_CLOCK 的既有结论:引擎每**客户端帧**把游戏时钟推进 g_msPerFrame 毫秒,
//      而实测 clockrate = 66666 微秒/逻辑帧 = 4 × 16.667 ⇒ 一个逻辑帧含 **4 个客户端帧**
//      ⇒ 60 客户端帧/秒。帧计数器与客户端帧同源。
//   ③ 原版自洽性:原版 flt_CDBC5C = 0.03 = 30 帧 ÷ 1000,而原版客户端帧率正是 30。
//   ⇒ 我们把客户端帧率提到 60 却不动这三个派生量,进度就**以双倍速跑完** ——
//     这正是用户实测的「发电厂动画前一半正常、后一半卡死」与「苏联建筑动画消失」。
//
//   ⚠ 修法遵守本项目铁律「不写共享全局,只重定向读者」:这三处由 **FL_G_DERIVED2(0xE00)**
//     组统一处理(重定向 init 里 fild 的操作数 + 写回已算好的存量值),这里只登记地址。
//   ⚠ 与 P6b 的关系:FL_G_DER_FPS 会把 0x00CDBC50 整体写成目标帧率,效果**覆盖** P6b
//     那一条重定向;两者同时开时都指向 60,结果一致,不冲突。
static const uintptr_t kEngineFramesPerMsVa = 0x00CDBC5C;
static const uintptr_t kEngineMsPerFrameFloatVa = 0x00CDBC54;
// ★★★ 2026-09-17(第七批):显示器/渲染帧计数器的对象全局。`vtbl[116]` 就是「当前帧号」。
//   取证见上面 ① —— dword_CDB71C 被显式命名为 "TheDisplay",而它是从 dword_CDB750 派生的。
//   本常量只给 FrameLabDisplayClock 那把**只读**尺子用,用来实测帧计数器的 tick 速率。
//   ★ 这是整个「建筑动画」修复的前提数字:若实测 = 60 帧/秒,则 flt_CDBC5C 必须改;
//     若实测仍是 30 帧/秒,则我上面的推论就是错的,必须回头重查(所以必须先量、再改)。
static const uintptr_t kDisplayClockVa = 0x00CDB750;
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
// ★★★ 2026-09-17(第六批):我们自己的「渲染帧率**浮点**」常量。P6b(动画时间步)把
//   sub_903910 里 `divss xmm0,[0x00CDBC50]` 的操作数改指到这里 —— 那条指令算的是
//   dt(秒) = 帧号增量 ÷ 帧率,除数必须跟着实际渲染帧率走,否则 dt 差 r/2 倍。
//   为什么用**目标**帧率而不是实测帧率:与 P6(g_constVisualStep = 1/目标帧率)、
//   R1(g_fpsRender = 目标帧率)同一套约定,保持一致;本机验收判据要求实测渲染
//   ≥ 0.95×目标(见 GUI 工具的 [PASS] 条件),两者在通过时相差 ≤5%。
//   ⚠ 已知边界:若渲染被负载压到目标之下(例如 60Hz 面板硬跑 90),这里仍按目标算,
//     dt 会偏小 ⇒ 混合斜坡偏慢。那种配置本来就整体变慢,不是本处引入的偏差。
static float g_constRenderFps  = 60.0f;
// 2026-09-16:我们自己的「渲染帧率」变量。被重定向的那几条游戏指令会直接读这里,
// 游戏原来的客户端帧率全局保持 30 不动(它还有一百多处读者,我们一处都不惊动)。
static int   g_fpsRender = 60;
// ★ 2026-09-17 动画定位会话:我们自己的「每客户端帧毫秒数」。
// 引擎主 tick 里那条 `add eax,[0x00CE176C]`(推进游戏时钟)被改指到这里(见 FL_G_CLOCK)。
// 必须与游戏原版同一套取整:原版 = 1000÷客户端帧率 的**整数除法**(30→33,不是 33.33),
// 所以我们同样用整数除法 ⇒ 90 帧下 990 毫秒/秒,与原版同样 1% 偏慢,不引入新偏差。
// 为什么不用 float 精确值:游戏那条指令是 `add eax, dword`(整数加法),喂浮点位模式会得到垃圾。
static int   g_msPerFrame = 33;            // 1000 ÷ 60,install() 里按目标帧率重算
// ★★ 2026-09-17(续):时钟推进量的**精确定点累加器**。
//   上面那句注释说「与原版同样 1% 偏慢,不引入新偏差」—— 本轮实机测试证明它只对 r=2 和 r=6 成立,
//   r=4 时偏到 4%,而且这个误差**肉眼可见**:
//       r=2(原版 30 帧) 1000÷30 = 33 → 2×33 = 66.0 毫秒/逻辑帧,目标 66.667 ⇒ −1.0%
//       r=4(60 帧)      1000÷60 = 16 → 4×16 = 64.0                ⇒ −4.0%  ← 实测 63.73(−4.4%)
//       r=6(90 帧)      1000÷90 = 11 → 6×11 = 66.0                ⇒ −1.0%
//   根因:引擎是每**客户端帧**推进 g_msPerFrame 一次,而一个逻辑帧恰好含 r 个客户端帧,
//   所以每逻辑帧推进 r × g_msPerFrame —— 整数截断的相对误差被 r 放大了。
//   修法:不取 1000÷帧率,改用累加器把「每逻辑帧 66.667 毫秒」精确摊到每个客户端帧上。
//   数学与边界条件见 clock_accumulator_step() 上方的注释。
static int   g_clockAcc  = 0;              // 累加器(见 clock_accumulator_step)
static int   g_clockWant = 0;              // 上次算出的「累计应推进毫秒数」
// ★ 2026-09-17:「每帧毫秒」写入的隔离区。
// 引擎里**有两处**会写共享全局 dword_CE176C(= 1000 ÷ 客户端帧率):
//   ① 0x00BB3C50 那个小函数:`div [0x00CAF9D4]` 之后 `mov [0x00CE176C],eax`
//   ② sub_5B81A0(虚表 0x00C09A20 槽 105):`mov [0x00CE176C], cvttss2si(参数)`
// 这两处的写入操作数都被改指到这里(见 FL_G_CLOCK)。于是:
//   · 共享全局 dword_CE176C **永远保持启动时那份 33** ⇒ 它另外 14 处读者(两种方向,含
//     音效时长)行为与老版本逐位一致,不会因为我们在运行中改了它而出怪相;
//   · g_msPerFrame 由我们自己完全掌控,时钟推进量不会被人偷偷改回 33。
// 为什么不直接写 dword_CE176C:那样就动了共享全局,违背项目铁律「只重定向读者」。
// 为什么不用 g_msPerFrame 当这个隔离区:①的写入依赖 R3 把它的除法操作数改指 g_fpsRender
//   才等于目标帧率;②的写入取决于调用方传什么,不可控。隔离区把这两个不确定都挡在外面。
static int   g_msPerFrameQuarantine = 0;
// 分组掩码:哪些改动要真写。默认全开;二分定位时用 FrameLabSetGroups 关掉某几组。
static int   g_groups = FL_G_ALL;

// ── 自动化验证用的仪表(2026-09-16 会话 68ee9b9d)────────────────────────────────
static volatile long g_frameCount = 0;
static bool  g_animGate = true;            // P7 是否启用「次数门」
// 2026-09-17 动画采样:引擎自己的脚本 API `CurDrawablePrevAnimFraction` 注册的是 sub_555D80,
// 它读的是 *(float*)(*(int*)(0xCDD17C + 296) + 20) —— 所以**动画进度就在这个位置**,
// 这是引擎自报的,不是我猜的。采样它就能回答「按渲染帧推进还是按阶段推进」。
static volatile long g_sampleLeft = 0;
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

// ── 2026-09-17 动画标尺的状态(钩子点见 kSigAnimFractionCall)──────────────────────
static unsigned char* g_animSite   = NULL;   // 被改的那条 call 的地址(应为 0x0090C245)
static void*          g_animOrigFn = NULL;   // 它原来的目标 sub_8EDED0(应为 0x008EDED0)
static volatile long  g_animCalls  = 0;      // 探针一共被走了多少次(判「到底有没有采到东西」)
#define FL_ANIM_OWNERS 12                    // 最多同时盯 12 个动画对象
static void*          g_animOwner[FL_ANIM_OWNERS];   // = [drawModule+0xC]
static float*         g_animFrame[FL_ANIM_OWNERS];   // = owner + 0xB0,即当前帧(float*)
static volatile long  g_animHit[FL_ANIM_OWNERS];     // 各自被看到多少次(用来淘汰最不活跃的)
// ★ 2026-09-18(第十一批·续):每个槽**最后一次被确认**的 GetTickCount。
//   为什么需要它:黑匣子要在热路径上读 g_animFrame[i] 指向的 float,而那个指针可能
//   因为对象被换掉而短暂悬垂。第一版我在 bb::tick 里用 __try/__except 兜底 ——
//   **那是错的**:bb::tick 是被手工构造的 hook(fl_hook_perframe)调用的,SEH 会在栈上
//   建立异常注册记录、改变栈帧布局,破坏那个手工栈平衡。实测后果:黑匣子自检从
//   23 PASS 掉到 21 PASS,crash 报告再也写不到磁盘上(返回 FL_ERR_WRITE=30)。
//   ⇒ 改用这个时间戳:bb::tick 只读「最近 2 秒内被探针确认过」的槽。探针每秒被走
//   约 24 次,所以悬垂窗口被压到 2 秒以内,而且**完全不需要 SEH**。
static volatile long  g_animTick[FL_ANIM_OWNERS];    // 0 = 从没确认过

// ── ★★★ 2026-09-17(第六批):混合斜坡量尺的状态(钩子点见 kSigBlendStepCall)──────────
// 只累加三个数,不做任何判断 —— 判断留给导出函数(它才知道区间边界)。
static unsigned char* g_stepSite   = NULL;   // 被改的那条 call 的地址(应为 0x00903947)
static void*          g_stepOrigFn = NULL;   // 它原来的目标 sub_8EA220(应为 0x008EA220)
static volatile long  g_stepCalls  = 0;      // 区间内被调用了多少次(应 ≈ r × 逻辑帧数)
static double         g_stepDtSum  = 0.0;    // 区间内收到的 dt 之和,单位**秒**
static volatile long  g_stepDtCnt  = 0;      // dt 的样本数(与 g_stepCalls 一致,分开只为可读性)
static volatile long  g_stepMaxDt  = 0;      // 见过的最大 dt × 1000000(用来发现异常大的步长)

// ── ★★★ 2026-09-21(悬挂路径会话):车身外观探针 / 30 Hz 节拍门的状态(钩子点见 kSigChassisCall)──
// calcPhysicsXform(sub_532CA0)的绝对 VA —— 用来核对「那条 call 真的调去了它」。
// ⚠ 绝对 VA,不是 RVA;**不要再加 g_base**(同 kEngineBlendStepVa 的坑)。
static const uintptr_t kEngineChassisXformVa = 0x00532CA0;
// 2026-09-22:TheTracerManager 的全局(sub_98D690(..., "TheTracerManager") 确证)。只用来核对特征没认错。
static const uintptr_t kEngineTracerMgrVa = 0x00CDB79C;
// 2026-09-21:卷屏归一(见 FL_G_SCROLL)。被改指过来的两条 movss 直接读这个常量。
static float g_scroll100   = 100.0f;      // = 100 × 原版帧率 ÷ 目标帧率,install() 里算
static int   g_scrollSites = 0;           // resolve 时定位并核对通过的补丁点个数(应为 2;不是 2 就一处都不登记)
static bool  g_scrollOn    = false;       // 两处是否真的改指了(给 FrameLabScrollStatus 用)
// 2026-09-22:弹道流节拍门(见 FL_G_TRACER)。只需要一个「上次转发时的 30 Hz 虚拟帧号」,不需要表 ——
//   全进程只有一个 TheTracerManager。
static unsigned char* g_trcSite   = NULL;    // 被改的那 11 字节的起点(应为 0x00542E4F);NULL = 没装
static unsigned       g_trcVFrame = 0xFFFFFFFFu;  // 上一次放行时的虚拟帧号
static bool           g_trcOn     = false;   // 节拍门是否真的生效
static volatile long  g_trcCalls  = 0;       // 包装被进入的次数
static volatile long  g_trcFwd    = 0;       // 真的转发给引擎的次数(放行 ÷ 调用 应 ≈ 30 ÷ 目标帧率)
static unsigned char* g_chsSite   = NULL;    // 被改的那条 call 的地址(应为 0x00535E87);NULL = 没装
static void*          g_chsOrigFn = NULL;    // 它原来的目标 sub_532CA0
static int            g_chsMode   = FL_CHS_MODE_PROBE;   // 0 探针 / 1 节拍门·回放 / 2 节拍门·插值(只量不改或目标帧率 <= 原版时恒为 0)
static FlChassisEnt   g_chsTab[FL_CHS_SLOTS];            // 每辆车一格:缓存的输出 + 记账(见 chassis_gate.h)
static volatile long  g_chsCalls  = 0;       // 包装函数被进入的总次数
static volatile long  g_chsAdv    = 0;       // 原函数被放行且返回非 0 的总次数(= 递推总步数)
static volatile long  g_chsHold   = 0;       // 回放缓存的总次数
static volatile long  g_chsZero   = 0;       // 原函数返回 0 的总次数(无 locomotor / 外观不处理 / 同帧去重)
static volatile long  g_chsNoSlot = 0;       // 表满退回直通的总次数(正常应为 0)

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


// P7 的钩子:决定这一次「逐帧绘制更新」要不要真的转发给引擎。
// 原版每逻辑帧走 2 次(6 个阶段里的第 1、4 个)⇒ 只在 (阶段-1) % 3 == 0 时转发,
// 动画推进速率就与原版逐次相同,与我们把渲染抬到多少帧无关。
static volatile long g_fwdCalls = 0, g_skipCalls = 0;
extern "C" void __stdcall fl_hook_frameupdate(void* self, int phase) {
    if (g_animGate && ((phase - 1) % 3) != 0) { ++g_skipCalls; return; }
    ++g_fwdCalls;
    // 原调用是 __thiscall(this 在 ecx,阶段在栈上)⇒ 用 __fastcall 形状转发
    typedef void (__fastcall *FnSlot144)(void* ecx, void* edx, int phase);
    void** vt = *(void***)self;
    FnSlot144 fn = (FnSlot144)vt[36];        // 0x90 / 4 = 36
    fn(self, 0, phase);
}

// ── 时钟推进量的定点累加器(2026-09-17 动画定位会话,Claude)───────────────────────
//
// 要解决的问题
//   引擎每**客户端帧**把游戏时钟推进 g_msPerFrame 毫秒(sub_5FE9D0 里两处,见 FL_G_CLOCK),
//   而一个逻辑帧恰好含 r 个客户端帧 ⇒ 每逻辑帧推进 r × g_msPerFrame。
//   原版把 g_msPerFrame 取成 1000÷客户端帧率(整数除法),于是误差被 r 放大:
//       r=2 → 2×33 = 66.0(目标 66.667,−1.0%)   r=4 → 4×16 = 64.0(−4.0%)   r=6 → 6×11 = 66.0(−1.0%)
//   r=4 那 4% 就是 2026-09-17 实机测到的 63.73 毫秒/逻辑帧 —— 动画相对模拟慢 4%。
//
// 做法:定点累加,数学上精确
//   目标每逻辑帧 66.667 毫秒 = 200/3 毫秒。把分子放大到整数:
//       acc += 200;                       // 每客户端帧贡献 200/(3r) 毫秒
//       want = acc / (3 * r);             // 累计**应**推进的毫秒数(整数)
//       g_msPerFrame = want - 上次 want;  // 本帧实际推进量(必为整数,可正可负)
//   于是 3r 个客户端帧(= 3 个逻辑帧)的增量之和恰为 200 毫秒 ⇒ 每逻辑帧 66.667 毫秒,**精确**。
//   逐帧增量在两个相邻整数之间摆动(±0.5 毫秒),但**三个逻辑帧的平均值精确无偏**。
//
//   验算 r=4(3r=12):acc 每步 +200 ⇒ want = 16,33,50,66,83,100,116,133,150,166,183,200
//                    增量 = 16,17,17,16,17,17,16,17,17,16,17,17,和为 200 ✓
//   验算 r=6(3r=18):want = 11,22,…,200(共 18 项),增量之和 200 ✓
//   验算 r=2(3r=6): 增量 = 33,33,34,33,33,34,和为 200 ✓(连原版那 −1% 也一并修掉)
//
// 前提与边界(必须知道,不然会误判)
//   · 调用点是 fl_hook_perframe —— **每渲染帧**走一次,而时钟是**每客户端帧**推进一次。
//     两者在「实际渲染帧率 ≥ 目标帧率」时是 1:1(实机目标 60 时渲染恰好 60.00,已验证)。
//     若渲染被卡在目标之下(例如 60 Hz 面板上硬要 90),钩子会比时钟少跑几次,累加器随之偏慢 ——
//     但那种配置本来就已经因为 r 过大而整体变慢(实测 66% 速度),不是这个修正引入的偏差。
//   · r 必须 ≥ 1(install 已把 r 限制在 [2,6]);3*r 不会为 0。
//   · 只在 FL_G_CLOCK 且非「只量不改」时更新 —— 关掉时钟修正时这个值没人读。
static void clock_accumulator_step() {
    if (g_ratio < 1) return;
    g_clockAcc += 200;                          // 200/3 毫秒的分子,每客户端帧加一次
    const int want = g_clockAcc / (3 * g_ratio);
    g_msPerFrame = want - g_clockWant;
    g_clockWant  = want;
}

// P3b:sub_6027D0 的逐帧推进块。**每渲染一帧走一次,与 r 无关** —— 所以帧计数器必须放这里。
// ★踩过的坑:原来把计数器放在「批次之后的系数」钩子里,而 r>=6 时引擎会跳过整个批次块,
//   那个钩子一次都不会被调用 ⇒ 90 帧那轮量出「渲染 0 帧」。坏的是尺子,不是补丁。
// g_measureOnly 时返回与原版逐位相同的「阶段 × 1/6」,只数不改。
extern "C" void __stdcall fl_hook_perframe(void* self) {
    ++g_frameCount;
    g_frameObj = self;
    // ★ 2026-09-18(第十一批):黑匣子 —— 每渲染帧记一行状态(飞行记录仪)。
    //   默认关,关着时只是一次 bool 判断。开着时崩溃/不同步现场就能倒出「出事前最后 512 帧」。
    bb::tick(self);
    // ★ 2026-09-17(续):先把「本帧该推进多少毫秒游戏时钟」算出来。
    //   这个钩子每渲染帧走一次,而时钟每客户端帧推进一次;在「实际渲染帧率 ≥ 目标帧率」时两者一一对应。
    //   放在函数最前面:时钟推进发生在主 tick 里,越早更新越不会读到上一帧留下的旧值。
    //   只量不改时不更新(那时 g_msPerFrame 没人读,更新只会让日志更难对账)。
    //   ★★ 2026-09-18(第十一批):**原版帧率下跳过累加器**。理由见 FL_G_CLOCK_AT_RETAIL 定义处:
    //   实测 30 帧下补丁与原版唯一的差异就是累加器带来的 33,33,34 锯齿,而 30 帧是原版帧率,
    //   补丁在那里本该什么都不做 ⇒ 保持原版的恒定 33,让 30 帧与原版逐位一致。
    //   60/90 帧下 r 更大、漂移大得多(4%/10%),累加器照旧生效。
    const bool clockAtRetail = (g_targetFps == g_retailFps);
    if (!g_measureOnly && (g_groups & FL_G_CLOCK)
        && (!clockAtRetail || (g_groups & FL_G_CLOCK_AT_RETAIL)))
        clock_accumulator_step();
    if (g_sampleLeft > 0) {
        --g_sampleLeft;
        const int phase = *(int*)((unsigned char*)self + 0x58);
        int logicFrame = -1;
        int** gl = (int**)0x00CD8CE4;
        if (mem::read_ok(gl, 4) && *gl && mem::read_ok((unsigned char*)*gl + 0x50, 4))
            logicFrame = *(int*)((unsigned char*)*gl + 0x50);
        float frac = -1.0f;
        unsigned char* mgr = *(unsigned char**)0x00CDD17C;
        if (mgr && mem::read_ok(mgr + 296, 4)) {
            unsigned char* ctx = *(unsigned char**)(mgr + 296);
            if (ctx && mem::read_ok(ctx + 20, 4)) frac = *(float*)(ctx + 20);
        }
        FL_INFO("采样 渲染帧#%ld 逻辑帧=%d 阶段=%d 动画进度=%.6f",
                g_frameCount, logicFrame, phase, frac);
    }   // 引擎自己算的 FPS 就在 self+0x70,留个指针做第二把尺子
    const int phase = *(int*)((unsigned char*)self + 0x58);
    float f = g_measureOnly ? (float)phase * (1.0f / 6.0f) : fl_fraction(phase, g_ratio);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    *(float*)((unsigned char*)self + 0x60) = f;
}

// ──────────────────── 2026-09-17 动画标尺:探针钩子 ────────────────────
// 约定:`sub_8EDED0` 是 __thiscall(this 走 ECX),返回 double 走 ST(0)。
// 在 32 位 MSVC 里,**单参数的 __fastcall 与 __thiscall 的 ABI 完全一样**(第一个参数在 ECX、
// 没有栈参、调用方不需要清栈)—— 所以下面用 __fastcall 形状来当这个钩子。这不是取巧:
// 上面 fl_hook_frameupdate 转发虚表槽时用的就是同一招。返回 double 也同样是走 ST(0)。
// 行为保证:先记录、再原样转发给原函数并把它返回的 double 原封不动传回去 ⇒ 对调用方逐位无差别。
//   (顺序很要紧:原函数的返回值在**转发调用**里产生,所以我们记录时就算弄脏了浮点栈也没关系。)
typedef double (__fastcall *FnAnimFraction)(void* ecx, void* edx);

static void anim_probe_record(void* drawModule) {
    ++g_animCalls;
    if (!drawModule) return;
    if (!mem::read_ok((unsigned char*)drawModule + 0x0C, 4)) return;
    void* owner = *(void**)((unsigned char*)drawModule + 0x0C);
    if (!owner || !mem::read_ok((unsigned char*)owner + 0xB0, 4)) return;
    // ★ 只取一次时钟。这个函数在热路径上(实测约 24 次/秒),不该多调。
    //   它的用途见 g_animTick 的注释:给黑匣子一个「这个槽还新鲜」的凭据,
    //   这样 bb::tick 读分数时**不需要 SEH**(SEH 会破坏 hook 的手工栈平衡)。
    const long now = (long)GetTickCount();
    int slot = -1;
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
        if (g_animOwner[i] == owner) {
            ++g_animHit[i];
            g_animTick[i] = now;                 // ★续期
            return;                              // 见过的对象:只加计数
        }
        if (!g_animOwner[i] && slot < 0) slot = i;
    }
    if (slot < 0) {                    // 满了:踢掉命中次数最少的那个(最不活跃的)
        slot = 0;
        for (int i = 1; i < FL_ANIM_OWNERS; ++i)
            if (g_animHit[i] < g_animHit[slot]) slot = i;
    }
    g_animOwner[slot] = owner;
    g_animFrame[slot] = (float*)((unsigned char*)owner + 0xB0);
    g_animHit[slot] = 1;
    g_animTick[slot] = now;
}

extern "C" double __fastcall fl_wrap_animfraction(void* self, void* unused) {
    (void)unused;                      // edx 是调用方的残留值,我们不关心
    anim_probe_record(self);
    FnAnimFraction orig = (FnAnimFraction)g_animOrigFn;
    return orig(self, NULL);
}

// ── ★★★ 2026-09-17(第六批):混合斜坡量尺的包装(只读探针)──────────────────────────
// sub_8EA220 的形状:`int __thiscall sub_8EA220(_DWORD *this, float a2)` ——
//   this 在 ECX,dt 是**栈上**的 float(调用方用 `movss [esp],xmm0` 放好再 call)。
// 要转发就得让我们的钩子也把 this 放 ECX、dt 留在栈上 ⇒ 声明成
//   `__fastcall(self, 占位, dt)`:
//     self → ECX(与 __thiscall 的 this 同寄存器)
//     占位 → EDX(原函数不用 EDX,随便它是什么)
//     dt   → 栈(第一个栈参,与原函数的栈布局一致)
// 这是本项目已经在用的同一招(见上面 fl_wrap_animfraction:单参数 __fastcall ≡ __thiscall)。
// ★ 行为保证:只累加三个计数,然后**原样**把 (this, dt) 转发给原函数,返回值原封不动传回。
//   不碰游戏内存、不改任何数据 ⇒ 对调用方逐位无差别。
typedef int (__thiscall *FnBlendStep)(void* self, float dt);

extern "C" void __fastcall fl_wrap_blendstep(void* self, void* unused, float dt) {
    (void)unused;                                  // edx 是调用方的残留值
    ++g_stepCalls;
    g_stepDtSum += (double)dt;
    ++g_stepDtCnt;
    // 只记录「最大 dt」用来发现异常步长(例如掉帧时帧号增量 > 1)。
    // 用整数比较,避免在渲染线程里做浮点比较带来的额外开销与不确定性。
    const long dtMicro = (long)(dt * 1000000.0f);
    if (dtMicro > g_stepMaxDt) g_stepMaxDt = dtMicro;
    FnBlendStep orig = (FnBlendStep)g_stepOrigFn;
    orig(self, dt);                                // 原样转发
}

// ── ★★★ 2026-09-21(悬挂路径会话):车身外观包装(探针 + 30 Hz 节拍门)────────────────────
// sub_532CA0 的形状:`char __thiscall sub_532CA0(Drawable* this, float* out4)` ——
//   this 在 ECX,out(四个 float:俯仰/侧倾/偏航/视觉 Z)是唯一的栈参,被调方 `ret 4`,结果在 AL。
// 我们的钩子声明成 `__fastcall(self, 占位, out)`:self→ECX、占位→EDX(原函数不用)、out→栈,
//   被调方同样清 4 字节 —— 与上面 fl_wrap_blendstep 是同一招,栈布局与原调用逐位一致。
// ★ 这是被**正常编译的 call** 调进来的(只改了 rel32),不是手工机器码桩,所以没有「手工栈平衡」
//   的约束;但它在渲染热路径上(每辆可见载具每显示帧一次),所以照样:不用 SEH、不调 read_ok
//   (VirtualQuery 是系统调用)、不分配内存。表是定长静态数组,满了就退回直通。
// 行为保证:
//   · 只开探针(FL_G_CHASSISPROBE):每次都原样转发 ⇒ 对引擎逐位无差别,只多记账。
//   · 开节拍门(FL_G_CHASSIS30):见 src/chassis_gate.h —— 只在 30 Hz 虚拟帧变化时放行,其余帧回放缓存。
typedef char (__thiscall *FnChassisXform)(void* self, float* out);
typedef int  (__thiscall *FnClientFrame)(void* client);

// 适配器:把引擎的 __thiscall 原函数包成 chassis_gate.h 要的形状(ctx 不用)。
static char chassis_call_engine(void* ctx, void* self, float* out) {
    (void)ctx;
    return ((FnChassisXform)g_chsOrigFn)(self, out);
}

extern "C" char __fastcall fl_wrap_chassis(void* self, void* unused, float* out) {
    (void)unused;                                  // edx 是调用方的残留值
    ++g_chsCalls;
    // 显示帧号:和原函数用**同一个来源**(TheGameClient 的 vtbl+0x74,字节偏移)。原函数自己
    //   紧接着也会对同一个指针做同样的解引用,所以这里与原代码同样安全。全局还是空的
    //   (极早期)就直接直通。
    void* client = *(void**)kDisplayClockVa;
    if (!client) return chassis_call_engine(NULL, self, out);
    const unsigned frame = (unsigned)((FnClientFrame)(*(void***)client)[0x74 / 4])(client);

    FlChassisEnt* ent = NULL;
    int did = FL_CHS_DID_ZERO;
    const char r = fl_chs_gate(g_chsTab, self, out, frame, g_targetFps, g_retailFps,
                               g_chsMode, (unsigned)(2 * (g_targetFps > 0 ? g_targetFps : 60)),
                               chassis_call_engine, NULL, &ent, &did);
    if (did == FL_CHS_DID_ADVANCE)      ++g_chsAdv;
    else if (did == FL_CHS_DID_HOLD)    ++g_chsHold;
    else                                ++g_chsZero;
    if (!ent) ++g_chsNoSlot;
    // 外观枚举只读一次,而且只在「原函数刚刚放行且返回非 0」之后读:那一刻引擎自己刚沿着
    //   同一条指针链走过一遍(Drawable+0x138 → Object+0x374 → AI+0x21C → locomotor → 模板+100),
    //   链上每一环都刚被它自己用过。它告诉我们这辆车走的是哪个分支:
    //   1/7 → sub_526BD0(轮式带悬挂)  2/3/4 → sub_51E800  8 → sub_5262D0。
    if (ent && ent->appearance < 0 && did == FL_CHS_DID_ADVANCE) {
        unsigned char* obj  = *(unsigned char**)((unsigned char*)self + 0x138);
        unsigned char* ai   = obj  ? *(unsigned char**)(obj + 0x374) : NULL;
        unsigned char* loco = ai   ? *(unsigned char**)(ai + 0x21C)  : NULL;
        unsigned char* lt   = loco ? *(unsigned char**)(loco + 4)    : NULL;
        unsigned char* tmpl = lt   ? *(unsigned char**)(lt + 4)      : NULL;
        if (tmpl) ent->appearance = *(int*)(tmpl + 100);
    }
    return r;
}

// ── ★★★ 2026-09-22:弹道流 30 Hz 节拍门的包装(见 FL_G_TRACER)────────────────────────────────
// 被改的 11 字节里,ecx 在进入时**已经**装着 TheTracerManager(上一条 `mov ecx,[0xCDB79C]` 没被动),
// 所以这里用 __fastcall 接住它;原代码的 null 判断由我们自己做(那条 test/je 被我们覆盖掉了)。
// 只读我们自己的两个全局 + 一次虚拟帧号计算,不分配、不用 SEH;转发就是原来那条 `call [vtbl+0x14]`。
// 节拍门关着(只读计数)时每次都转发 ⇒ 行为逐位不变。
typedef void (__thiscall *FnTracerUpdate)(void* self);
extern "C" void __fastcall fl_wrap_tracer(void* mgr, void* unused) {
    (void)unused;
    if (!mgr) return;                                  // 原代码的 test ecx,ecx / je
    ++g_trcCalls;
    if (g_trcOn) {
        void* client = *(void**)kDisplayClockVa;
        if (client) {
            typedef int (__thiscall *FnClientFrame2)(void*);
            const unsigned frame = (unsigned)((FnClientFrame2)(*(void***)client)[0x74 / 4])(client);
            const unsigned vf = fl_chs_vframe(frame, g_targetFps, g_retailFps);
            if (vf == g_trcVFrame) return;             // 这个 30 Hz 虚拟帧已经推进过了
            g_trcVFrame = vf;
        }
    }
    ++g_trcFwd;
    ((FnTracerUpdate)(*(void***)mgr)[0x14 / 4])(mgr);
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
    FL_INFO("            24=特征命中了但现场值不对(不是我们支持的那份构建)");
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

    // 要改指向的 4 字节操作数(游戏那条指令原本读的是某个全局)。
    // newTarget = 改指到的**我们自己的**全局地址;为 NULL 时默认指 &g_fpsRender(老行为)。
    // 2026-09-17 新增 newTarget:时钟那处要指 &g_msPerFrame,不能一律指帧率。
    struct { void** operand; const char* name; int group; void* newTarget; } redirect[24];
    int redirectCount;
    unsigned char* perFrame;   // P3b 逐帧推进块
    unsigned char* frameUpdate; // P7 逐帧绘制更新调用点

    // 引擎在启动时算好的三个派生量(浮点帧率 / 每帧毫秒 / 每毫秒帧数):
    // 我们注入时它们早算完了,得按同样公式按新帧率写回去;顺便把算它们的那条 fild 也改指向。
    struct { float* target; float value; void** fildOperand; int group; } derived[8];
    int derivedCount;

    // 2026-09-21:第四个派生量 0x00CDBD34(每显示帧秒数)的存量值。NULL = 没定位到 / 核对没过(只 WARN,不拒装)。
    float* spfTarget;
};

static bool add_redirect(Sites& s, void** operand, const char* name, int group, void* newTarget = NULL) {
    if (s.redirectCount >= (int)(sizeof s.redirect / sizeof s.redirect[0])) {
        FL_ERR("要改指向的地方超过上限 —— 放弃安装");
        return false;
    }
    s.redirect[s.redirectCount].operand = operand;
    s.redirect[s.redirectCount].name = name;
    s.redirect[s.redirectCount].group = group;
    s.redirect[s.redirectCount].newTarget = newTarget;
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
    s.spfTarget = NULL;
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
        // ★ 2026-09-17:这里的小函数(0x00BB3C50)算的是 **1000 ÷ 客户端帧率,存进 dword_CE176C**,
        //   也就是「每客户端帧毫秒」。它只在启动时跑过一次(我们注入之前),所以 R3 改了指令操作数
        //   也改不动数据 —— 实测运行时 `dword_CE176C` 始终是 33。**这个「指令改了数据没改」
        //   的错配就是「动画快 3 倍」的真因**,修法见 FL_G_CLOCK。
        //   这里额外把它的**写入操作数**也改指到我们自己的 g_msPerFrame,理由:
        //   万一它以后再跑(分辨率/窗口模式变化时有可能),就只会写我们的变量,而**绝不会**
        //   动到 dword_CE176C —— 那个全局另有 14 处读者(两种方向,含音效时长),它们的值
        //   必须永远保持启动时那份。这样就把「R3 可能在运行中改坏共享全局」这个隐患堵死了。
        //   ⚠ 归属订正:这个 div 函数**不是** sub_5B81A0 —— 后者是另一个独立的 setter(见下面写入②)。
        //   一开始我按 dump 里 `dword_CE176C = (int)a1` 那句把两者混为一谈了。
        //   写法上直接从 R3 的命中点推偏移:特征 = div[disp32](6 字节) + mov[disp32],eax(5) + ret
        //   ⇒ 除法操作数在 +2,写入操作数在 +7。不再单独立一条特征,免得两处判据漂移。
        Pattern pDiv = {"派生 div", kSigDivFps, (int)(sizeof kSigDivFps / sizeof(short)), 0};
        const int nDiv = scan_all(pDiv, 2, g_fpsClient, hits, 16);
        for (int i = 0; i < nDiv; ++i) {
            if (!add_redirect(s, (void**)(hits[i] + 2), "R3 某量÷帧率", FL_G_DIVFPS)) return FL_ERR_SIG_AMBIGUOUS;
            const uintptr_t storeVa = (uintptr_t)*(int**)(hits[i] + 7);
            if (storeVa != kEngineMsPerFrameVa) {
                FL_WARN("R3 后面那条 mov 写的不是每帧毫秒全局 %08X(实际 %08X)—— "
                        "跳过写入隔离(只影响『它以后重跑』这个隐患的兜底)",
                        (unsigned)kEngineMsPerFrameVa, (unsigned)storeVa);
            } else if (!add_redirect(s, (void**)(hits[i] + 7), "C 每帧毫秒写入①隔离",
                                     FL_G_CLOCK, (void*)&g_msPerFrameQuarantine)) {
                return FL_ERR_SIG_AMBIGUOUS;
            }
        }

        // C 每帧毫秒写入②:setter sub_5B81A0(见 kSigMsPerFrameSetter)。同样隔离。
        {
            Pattern pSet = {"每帧毫秒 setter", kSigMsPerFrameSetter,
                            (int)(sizeof kSigMsPerFrameSetter / sizeof(short)), 0x005B81A0};
            unsigned char* setSite = scan(pSet);
            if (!setSite || setSite == (unsigned char*)-1) {
                FL_WARN("每帧毫秒 setter 没找到(或命中多处)—— 跳过它的写入隔离。"
                        "不影响时钟修正本身,只是少一层兜底。");
            } else if ((uintptr_t)*(int**)(setSite + 7) != kEngineMsPerFrameVa) {
                FL_WARN("每帧毫秒 setter 写的不是 %08X —— 跳过它的写入隔离",
                        (unsigned)kEngineMsPerFrameVa);
            } else if (!add_redirect(s, (void**)(setSite + 7), "C 每帧毫秒写入②隔离",
                                     FL_G_CLOCK, (void*)&g_msPerFrameQuarantine)) {
                return FL_ERR_SIG_AMBIGUOUS;
            }
        }

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

        // ── ★★★ P6b 动画时间步(2026-09-17 第六批:建筑动画回归的真因)───────────────
        // 把 sub_903910 里 `divss xmm0,[0x00CDBC50]` 的除数改指我们自己的
        // g_constRenderFps(= 目标帧率浮点)。判据与理由见 kSigAnimTimeStep 上方。
        // ★ 归属 FL_G_ANIMSTEP(0x20000) —— **不是** FL_G_VISUAL。
        //   最初归 FL_G_VISUAL,理由是「与 P6 同一个失效模式」。**那个理由不成立,已订正**:
        //   归成一组就只能一起开/一起关,而 P6b 所在路径在正常对局里根本不跑(见 FL_G_ANIMSTEP
        //   定义处的实测),同组会把这件事藏起来。拆开之后 2×2 才分得清。
        // ★ 只改操作数、长度不变;现场核对 operand 确实指着引擎的浮点帧率全局,否则拒装。
        // ★ 命中 0 处时**只 WARN 不拒装**:万一换构建导致这条特征漂了,不该让整个补丁装不上
        //   (与动画标尺探针同一套「尺子坏了不拖垮其它改动」的取舍)。
        {
            Pattern pAnimStep = {"动画时间步(帧增量÷帧率)", kSigAnimTimeStep,
                                 (int)(sizeof kSigAnimTimeStep / sizeof(short)), 0x00903938};
            const int nStep = scan_all(pAnimStep, 4, (void*)kEngineFpsFloatVa, hits, 16);
            if (nStep <= 0) {
                FL_WARN("P6b 动画时间步:特征未命中(或 operand 不是 %08X)—— 跳过。"
                        "后果:建筑建造/揭幕这类混合斜坡动画在 60 帧下会快一倍。",
                        (unsigned)kEngineFpsFloatVa);
            } else {
                for (int i = 0; i < nStep; ++i)
                    if (!add_redirect(s, (void**)(hits[i] + 4), "P6b 动画时间步",
                                      FL_G_ANIMSTEP, (void*)&g_constRenderFps))
                        return FL_ERR_SIG_AMBIGUOUS;
                FL_INFO("  P6b 动画时间步命中 %d 处 @ %08X(除数 %08X → 我们的目标帧率)",
                        nStep, (unsigned)(uintptr_t)hits[0], (unsigned)kEngineFpsFloatVa);
            }
        }

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

        // ── R2b 第四个派生量:每显示帧秒数 0x00CDBD34(2026-09-21,默认关,见 FL_G_DER_SPF 定义处)──────
        //   这种「movss / divss / movss / ret」形状的静态初始化器在主程序里不止一个,所以用 scan_all
        //   按**除数操作数 == 0x00CDBC50** 过滤,过滤后必须恰好 1 处;再核对被除数常量 = 1.0、
        //   存量值 ≈ 1 ÷ 原版帧率。任何一条对不上 ⇒ 只 WARN、跳过(它是可选项,不该拖垮整个补丁)。
        {
            Pattern pSpf = {"每显示帧秒数 init", kSigSecPerFrameInit,
                            (int)(sizeof kSigSecPerFrameInit / sizeof kSigSecPerFrameInit[0]), 0};
            const int nSpf = scan_all(pSpf, 12, (void*)kEngineFpsFloatVa, hits, 16);
            if (nSpf != 1) {
                FL_WARN("R2b 每显示帧秒数:按「除数 = %08X」过滤后命中 %d 处(应恰好 1 处)—— 跳过。",
                        (unsigned)kEngineFpsFloatVa, nSpf);
            } else {
                float* one = *(float**)(hits[0] + 4);
                float* spf = *(float**)(hits[0] + 20);
                const float want = 1.0f / (float)g_retailFps;
                if (!mem::read_ok(one, 4) || !mem::read_ok(spf, 4)) {
                    FL_WARN("R2b 每显示帧秒数:操作数地址不可读(%08X / %08X)—— 跳过。",
                            (unsigned)(uintptr_t)one, (unsigned)(uintptr_t)spf);
                } else if (*one < 0.999f || *one > 1.001f || *spf < want * 0.99f || *spf > want * 1.01f) {
                    FL_WARN("R2b 每显示帧秒数:现场值不对(被除数 %.6f 应为 1.0;存量值 @%08X = %.6f 应为 %.6f)"
                            "—— 认错了地方或已被改过,跳过。",
                            *one, (unsigned)(uintptr_t)spf, *spf, want);
                } else {
                    s.spfTarget = spf;
                    FL_INFO("  R2b 每显示帧秒数 @ %08X = %.6f(init @ %08X);FL_G_DER_SPF 开着才会改写成 1/%d",
                            (unsigned)(uintptr_t)spf, *spf, (unsigned)(uintptr_t)hits[0], targetFps);
                }
            }
        }

        // ── S 卷屏步长常量(可自选,默认关;见 FL_G_SCROLL 定义处)。两处必须一起登记:只改一处,
        //    加速段与稳态段的速度会接不上。任何一处没命中 / 常量不是 100.0 ⇒ 两处都不登记,只 WARN。
        g_scrollSites = 0;
        {
            Pattern pScroll1 = {"卷屏步长①(键盘/边缘·稳态段)", kSigScrollStep1,
                                (int)(sizeof kSigScrollStep1 / sizeof kSigScrollStep1[0]), 0x00AC0263};
            Pattern pScroll2 = {"卷屏步长②(键盘/边缘·加速段)", kSigScrollStep2,
                                (int)(sizeof kSigScrollStep2 / sizeof kSigScrollStep2[0]), 0x00ABC9FC};
            unsigned char* sc1 = scan(pScroll1);
            unsigned char* sc2 = scan(pScroll2);
            const bool found = sc1 && sc2 && sc1 != (unsigned char*)-1 && sc2 != (unsigned char*)-1;
            float* c1 = found ? *(float**)(sc1 + 20) : NULL;
            float* c2 = found ? *(float**)(sc2 + 12) : NULL;
            if (!found || !mem::read_ok(c1, 4) || !mem::read_ok(c2, 4)
                || *c1 < 99.99f || *c1 > 100.01f || *c2 < 99.99f || *c2 > 100.01f) {
                FL_WARN("S 卷屏步长:两处特征没有同时命中,或常量不是 100.0 —— 两处都不登记(卷屏保持现状)。");
            } else if (!add_redirect(s, (void**)(sc1 + 20), "S 卷屏步长①(稳态段)", FL_G_SCROLL, (void*)&g_scroll100)
                    || !add_redirect(s, (void**)(sc2 + 12), "S 卷屏步长②(加速段)", FL_G_SCROLL, (void*)&g_scroll100)) {
                return FL_ERR_SIG_AMBIGUOUS;
            } else {
                g_scrollSites = 2;
                FL_INFO("  S 卷屏步长:两处就位(%08X / %08X,共享常量 %08X = %.1f);FL_G_SCROLL 开着才会改指",
                        (unsigned)(uintptr_t)sc1, (unsigned)(uintptr_t)sc2, (unsigned)(uintptr_t)c1, *c1);
            }
        }

        // ── ★ 时钟推进量(2026-09-17 动画定位会话)—— 这是「动画快 3 倍」的真因,默认开。
        // 完整证据链见 FL_G_CLOCK 上方的注释。这里只**定位 + 登记**重定向,真正写内存由 install() 统一做。
        // 干扫(dryrun)也走这里,所以「能不能装」的判据与实际要改的东西永远一致。
        Pattern pClock = {"时钟推进量", kSigClockAdvance, (int)(sizeof kSigClockAdvance / sizeof(short)), 0x005FEBA2};
        unsigned char* clockSite = scan(pClock);
        if (!clockSite || clockSite == (unsigned char*)-1) {
            FL_ERR("时钟推进点没找到(或命中多处)—— 放弃安装。"
                   "没有它,90 帧下动画会快 3 倍(见 FL_G_CLOCK)");
            return FL_ERR_SIG_MISS;
        }
        {
            const uintptr_t operandVa = (uintptr_t)*(int**)(clockSite + 2);
            if (operandVa != kEngineMsPerFrameVa) {
                FL_ERR("时钟推进点读的不是每帧毫秒全局 %08X(实际 %08X)—— 认错了地方,放弃安装",
                       (unsigned)kEngineMsPerFrameVa, (unsigned)operandVa);
                return FL_ERR_SIG_MISMATCH;
            }
            if (!mem::read_ok((const void*)kEngineMsPerFrameVa, 4)) {
                FL_ERR("每帧毫秒全局 %08X 读不到 —— 放弃安装", (unsigned)kEngineMsPerFrameVa);
                return FL_ERR_SIG_MISMATCH;
            }
            const int engineMs = *(int*)kEngineMsPerFrameVa;
            if (engineMs != 1000 / g_retailFps) {
                FL_ERR("每帧毫秒全局 %08X 现在是 %d,不是原版的 %d —— 状态与预期不符,放弃安装",
                       (unsigned)kEngineMsPerFrameVa, engineMs, 1000 / g_retailFps);
                return FL_ERR_SIG_MISMATCH;
            }
            FL_INFO("  时钟推进点 @ %08X:add eax,[%08X]=%d —— 将改指本补丁的 g_msPerFrame",
                    (unsigned)(uintptr_t)clockSite, (unsigned)kEngineMsPerFrameVa, engineMs);
        }
        if (!add_redirect(s, (void**)(clockSite + 2), "C 时钟推进量(动画快 3 倍的真因)",
                          FL_G_CLOCK, (void*)&g_msPerFrame)) return FL_ERR_SIG_AMBIGUOUS;

        // ── ★ 时钟推进的**第二条互斥分支**(2026-09-17 当天第二次定位)──────────────────
        // 为什么必须一起改:两条分支互斥,90 帧下每 30 次调用里 29 次走 A、1 次走 B。
        // 只改 A 会留下 29×11+33 = 352 毫秒 / 0.333 秒 = 1.056× 的残留误差(见 kSigClockAdvance2)。
        // 判据与分支 A 完全一致:读的必须是同一个每帧毫秒全局,值必须是原版 33。
        Pattern pClock2 = {"时钟推进量②", kSigClockAdvance2,
                           (int)(sizeof kSigClockAdvance2 / sizeof(short)), 0x005FEBE1};
        unsigned char* clockSite2 = scan(pClock2);
        if (!clockSite2 || clockSite2 == (unsigned char*)-1) {
            FL_ERR("时钟推进点②没找到(或命中多处)—— 放弃安装。"
                   "只改①不改②,90 帧下时钟仍有约 5.6% 偏快(见 kSigClockAdvance2 的量化)");
            return FL_ERR_SIG_MISS;
        }
        {
            const uintptr_t operandVa2 = (uintptr_t)*(int**)(clockSite2 + 2);
            if (operandVa2 != kEngineMsPerFrameVa) {
                FL_ERR("时钟推进点②读的不是每帧毫秒全局 %08X(实际 %08X)—— 认错了地方,放弃安装",
                       (unsigned)kEngineMsPerFrameVa, (unsigned)operandVa2);
                return FL_ERR_SIG_MISMATCH;
            }
            FL_INFO("  时钟推进点② @ %08X:mov ecx,[%08X] —— 同样改指 g_msPerFrame(否则残留约 5.6%% 偏快)",
                    (unsigned)(uintptr_t)clockSite2, (unsigned)kEngineMsPerFrameVa);
        }
        if (!add_redirect(s, (void**)(clockSite2 + 2), "C 时钟推进量②(每 30 帧一次的那条分支)",
                          FL_G_CLOCK, (void*)&g_msPerFrame)) return FL_ERR_SIG_AMBIGUOUS;

        FL_INFO("共需改指向 %d 处操作数;客户端帧率全局 %08X 本身**保持 %d 不动**(它另有上百处读者)",
                s.redirectCount, (unsigned)(uintptr_t)g_fpsClient, g_retailFps);

        // P7 逐帧绘制更新调用点(13 字节,唯一命中)。开了动画次数门才会改它,但**干扫也要找到它**,
        // 否则"能不能装"的判据就和实际要改的东西不一致了。
        Pattern pFU = {"逐帧绘制更新调用", kSigFrameUpdate, (int)(sizeof kSigFrameUpdate / sizeof(short)), 0x00602942};
        s.frameUpdate = scan(pFU);
        if (!s.frameUpdate) return FL_ERR_SIG_MISS;
        if (s.frameUpdate == (unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;

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
    s.spfTarget = NULL;
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
    // ★★★ 2026-09-17(第六批):P6b 的除数 —— 目标帧率浮点值。
    //   必须与 g_constVisualStep 同一时刻就位,且**在下面重定向循环之前**设置好:
    //   循环里会把游戏那条 divss 的除数操作数改成 &g_constRenderFps,
    //   若那时它还是旧值,游戏第一帧就会读到错的 dt。
    g_constRenderFps  = (float)targetFps;
    // 2026-09-21:卷屏步长常量(见 FL_G_SCROLL)。同样必须在重定向循环之前就位。
    g_scroll100 = 100.0f * (float)g_retailFps / (float)(targetFps > 0 ? targetFps : g_retailFps);
    g_scrollOn  = false;

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

    // ── P7 动画次数门(13 字节窗口,我们用 7 字节 + 6 个 NOP)
    //    原地把 `mov edx,[esi] / push eax / mov eax,[edx+0x90] / mov ecx,esi / call eax`
    //    换成 `push eax(阶段) / push esi(this) / call 我们的钩子`,由钩子决定要不要转发。
    //    栈是平的:原来被调方(__thiscall,1 个栈参)清 4 字节,现在我们的 __stdcall 清 8 字节,各自平衡。
    if (ok && (g_groups & FL_G_ANIMGATE)) {
        unsigned char* site = s.frameUpdate;
        unsigned char code[13];
        memset(code, 0x90, sizeof code);
        int i = 0;
        code[i++] = 0x50;                       // push eax   (阶段号,__stdcall 第二个参数先压)
        code[i++] = 0x56;                       // push esi   (this)
        code[i++] = 0xE8;                       // call rel32
        const int rel = (int)((unsigned char*)&fl_hook_frameupdate - (site + i + 4));
        memcpy(code + i, &rel, 4); i += 4;
        ok = patch(site, code, (int)sizeof code, "P7 动画次数门");
        if (ok) FL_INFO("  动画次数门已启用:只在阶段 1 和 4 转发绘制更新(与原版每逻辑帧 2 次一致)");
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

    // ── P8 客户端帧率全局写入(实验组,默认关;2026-09-17 动画定位会话)
    //    与上面所有改动**方向相反**:这里是「默认全都变」。只在显式开 FL_G_FPSGLOBAL 时执行,
    //    目的单一 —— 把「帧数换算还按 30 算」这个错配消掉,看动画是否被修好。
    //    可回滚:4 字节写,进程退出或 rollback 时按撤销表还原。
    if (ok && !measureOnly && (g_groups & FL_G_FPSGLOBAL)) {
        ok = patch(g_fpsClient, &targetFps, 4, "P8 客户端帧率全局(实验:把帧数换算一次改对)");
        if (ok) FL_INFO("  ★实验组已开:客户端帧率全局 %08X 由 %d 改为 %d。"
                        "这条路径与其它组方向相反,只在做对照实验时用。",
                        (unsigned)(uintptr_t)g_fpsClient, g_retailFps, targetFps);
    }

    // ── 动画标尺(只读探针,默认关;2026-09-17 动画定位会话)────────────────────────
    //    把 sub_90C1B0 里那条 `call sub_8EDED0` 的 rel32 改指到我们的包装函数。
    //    5 字节原地改写,长度不变;包装先记录再原样转发 ⇒ 行为逐位不变。
    //    ★这一处不参与帧率,也不改任何数据 —— 它纯粹是「尺子」,给 FrameLabAnimRate 用。
    //    没命中就只记错误、不拖垮其它改动(尺子坏了不该让补丁装不上)。
    //    ★★★ 2026-09-19(动画方向会话)去掉 `!measureOnly`:这个钩子**不改任何数据**,
    //    是纯粹的「尺子」,所以在**原版基线**下也必须装 —— 否则 `-Mode measure` 那一臂
    //    的动画轴永远是「0 帧」,和 30 帧那一臂根本没法比。
    //    实测:加 `enable` 之后原版基线**仍然**是 0 帧,原因就在这里(`measureOnly` 为真
    //    时整个分支被跳过)。注释本身早就写了它「行为逐位不变」,所以放开是安全的。
    if (ok && (g_groups & FL_G_ANIMPROBE)) {
        Pattern pAnim = {"动画分数读取点", kSigAnimFractionCall,
                         (int)(sizeof kSigAnimFractionCall / sizeof kSigAnimFractionCall[0]), 0x0090C240};
        unsigned char* hit = scan(pAnim);
        if (hit && hit != (unsigned char*)-1) {
            g_animSite   = hit + 5;                                  // call 指令本身
            g_animOrigFn = (void*)(hit + 10 + *(int*)(hit + 6));     // 原目标 = call 下一条 + rel32
            unsigned char code[5];
            code[0] = 0xE8;                                          // call rel32
            const int rel = (int)((unsigned char*)&fl_wrap_animfraction - (g_animSite + 5));
            memcpy(code + 1, &rel, 4);
            ok = patch(g_animSite, code, 5, "动画标尺探针(只改 call 目标)");
            if (ok) FL_INFO("  动画标尺已装:call @%08X → 包装 %08X(原目标 %08X,行为不变)",
                            (unsigned)(uintptr_t)g_animSite,
                            (unsigned)(uintptr_t)&fl_wrap_animfraction,
                            (unsigned)(uintptr_t)g_animOrigFn);
        } else {
            FL_ERR("动画标尺:特征未命中或命中多处 —— 跳过(不影响其它改动)");
        }
    }

    // ── ★★★ 混合斜坡量尺(只读探针,默认关;2026-09-17 第六批)────────────────────────
    //    把 sub_903910 里那条 `call sub_8EA220` 的 rel32 改指到我们的包装函数。
    //    5 字节原地改写,长度不变;包装只累加计数再原样转发 ⇒ 行为逐位不变。
    //    ★这一处不参与帧率,也不改任何数据 —— 它纯粹是「尺子」,给 FrameLabBlendStepPerLogicFrame 用。
    //    ★★ 必须排在下面「R:重定向循环」**之前**:那时 +4 还是引擎的 0x00CDBC50,
    //       可以用它核对「我们没认错构建」。重定向循环跑完之后那里会变成 &g_constRenderFps。
    //       (即使顺序被改动也不会失效:下面两个值都接受。)
    //    ★没命中就只记错误、不拖垮其它改动(尺子坏了不该让补丁装不上)。
    if (ok && !measureOnly && (g_groups & FL_G_STEPPROBE)) {
        Pattern pStep = {"混合斜坡调用点", kSigBlendStepCall,
                         (int)(sizeof kSigBlendStepCall / sizeof kSigBlendStepCall[0]), 0x00903938};
        unsigned char* hit = scan(pStep);
        // 现场核对:除数操作数必须仍指着引擎的浮点帧率全局,或者已经被 P6b 改指到我们自己的
        // g_constRenderFps。两者都不是 ⇒ 我们认错了地方,拒装这把尺子(但只 WARN)。
        const void* opNow = (hit && hit != (unsigned char*)-1) ? *(void**)(hit + 4) : NULL;
        const bool opOk = (opNow == (const void*)kEngineFpsFloatVa) || (opNow == (const void*)&g_constRenderFps);
        if (hit && hit != (unsigned char*)-1 && opOk) {
            // ★ 2026-09-17 踩过的坑:这两个全局**只在真正装成功之后**才赋值。
            //   最初是在核对之前就赋了 g_stepSite,于是「目标不对 ⇒ 跳过」之后
            //   g_stepSite 仍非空 ⇒ 导出函数以为尺子装好了,报的是
            //   「区间内一次都没被调用(这段录像里没有建筑在建造/揭幕?)」——
            //   把「尺子没装」误报成「录像没数据」。**误导性日志比没有日志更坏**,
            //   所以这里用局部变量暂存,核对通过才落到全局。
            unsigned char* site = hit + 15;                                 // call 指令本身(0x00903947)
            void* origFn = (void*)(hit + 20 + *(int*)(hit + 16));           // 原目标 = call 下一条 + rel32
            // ★ 再核一次原目标:它必须是 sub_8EA220。特征里的 E8 是通配的(rel32 随加载基址变),
            //   所以只有在这里才检查得出「这条 call 真的调去了混合斜坡推进器」。
            //   ⚠ kEngineBlendStepVa 是绝对 VA(见它的定义处),**不要再加 g_base**。
            const void* want = (const void*)kEngineBlendStepVa;
            if (origFn != want) {
                FL_WARN("混合斜坡量尺:call @%08X 的目标是 %08X,期望 %08X(sub_8EA220)—— 跳过。",
                        (unsigned)(uintptr_t)site, (unsigned)(uintptr_t)origFn,
                        (unsigned)(uintptr_t)want);
            } else {
                unsigned char code[5];
                code[0] = 0xE8;                                      // call rel32
                const int rel = (int)((unsigned char*)&fl_wrap_blendstep - (site + 5));
                memcpy(code + 1, &rel, 4);
                if (patch(site, code, 5, "混合斜坡量尺探针(只改 call 目标)")) {
                    g_stepSite   = site;                             // ★ 只有装成功才登记
                    g_stepOrigFn = origFn;
                    FL_INFO("  混合斜坡量尺已装:call @%08X → 包装 %08X(原目标 %08X,行为不变)",
                            (unsigned)(uintptr_t)g_stepSite,
                            (unsigned)(uintptr_t)&fl_wrap_blendstep,
                            (unsigned)(uintptr_t)g_stepOrigFn);
                }
            }
        } else {
            FL_WARN("混合斜坡量尺:特征未命中 / 命中多处 / 除数操作数是 %08X(既不是 %08X 也不是 %08X)"
                    "—— 跳过(不影响其它改动)",
                    (unsigned)(uintptr_t)opNow, (unsigned)kEngineFpsFloatVa,
                    (unsigned)(uintptr_t)&g_constRenderFps);
        }
    }

    // ── ★★★ 车身外观包装(探针 / 30 Hz 节拍门,**都默认关**;2026-09-21 悬挂路径会话)──────────
    //    把 sub_535DD0 里那条 `call sub_532CA0` 的 rel32 改指到 fl_wrap_chassis。
    //    5 字节原地改写、长度不变、进撤销表。完整背景见 FL_G_CHASSISPROBE 定义处。
    //    ★ 只量不改(measureex)模式下**也可以装** —— 但那时只当探针用(节拍门不生效):
    //      原版基线必须用同一把尺子量,否则「修之前 / 修之后 / 原版」三臂没法比。
    //    ★ 与两把动画量尺同一套取舍:没命中 / 目标不对 ⇒ 只 WARN、跳过,不拖垮其它改动;
    //      两个全局**只在真正装成功之后**才赋值(「没装」不能伪装成「没采到」,铁律 15)。
    //    (g_chsOrigFn 不在这里清:它是绝对 VA 的常量值,清掉只会给「上一轮还停在包装里的线程」挖坑。)
    g_chsSite = NULL; g_chsMode = FL_CHS_MODE_PROBE;
    if (ok && (g_groups & (FL_G_CHASSISPROBE | FL_G_CHASSIS30 | FL_G_CHASSISLERP))) {
        Pattern pChassis = {"车身外观调用点", kSigChassisCall,
                            (int)(sizeof kSigChassisCall / sizeof kSigChassisCall[0]), 0x00535E7B};
        unsigned char* hit = scan(pChassis);
        if (hit && hit != (unsigned char*)-1) {
            unsigned char* site = hit + 12;                                 // call 指令本身(0x00535E87)
            void* origFn = (void*)(hit + 17 + *(int*)(hit + 13));           // 原目标 = call 下一条 + rel32
            if (origFn != (void*)kEngineChassisXformVa) {
                FL_WARN("车身外观包装:call @%08X 的目标是 %08X,期望 %08X(sub_532CA0)—— 跳过。",
                        (unsigned)(uintptr_t)site, (unsigned)(uintptr_t)origFn,
                        (unsigned)kEngineChassisXformVa);
            } else {
                unsigned char code[5];
                code[0] = 0xE8;                                             // call rel32
                const int rel = (int)((unsigned char*)&fl_wrap_chassis - (site + 5));
                memcpy(code + 1, &rel, 4);
                // 表与总账在改字节**之前**清零:改完那一刻渲染线程就可能进来(虽然此刻它被挂起)。
                memset(g_chsTab, 0, sizeof g_chsTab);
                g_chsCalls = g_chsAdv = g_chsHold = g_chsZero = g_chsNoSlot = 0;
                g_chsOrigFn = origFn;                                       // 包装一进来就要用,必须先就位
                {
                    const bool gate = (g_groups & (FL_G_CHASSIS30 | FL_G_CHASSISLERP)) && !measureOnly && targetFps > g_retailFps;
                    g_chsMode = !gate ? FL_CHS_MODE_PROBE
                                      : ((g_groups & FL_G_CHASSISLERP) ? FL_CHS_MODE_LERP : FL_CHS_MODE_HOLD);
                }
                ok = patch(site, code, 5, "车身外观包装(只改 call 目标)");
                if (ok) {
                    g_chsSite = site;                                       // ★ 只有装成功才登记
                    FL_INFO("  车身外观包装已装:call @%08X → 包装 %08X(原目标 %08X)",
                            (unsigned)(uintptr_t)site, (unsigned)(uintptr_t)&fl_wrap_chassis,
                            (unsigned)(uintptr_t)origFn);
                    if (g_chsMode != FL_CHS_MODE_PROBE)
                        FL_INFO("    ★30 Hz 节拍门**生效**(%s):车身递推每 %d 个显示帧放行 %d 次"
                                "(期望 chassisrate ≈ 2000)",
                                g_chsMode == FL_CHS_MODE_LERP ? "插值:每个显示帧在相邻两个 30 Hz 位姿之间按相位取值"
                                                              : "回放:其余显示帧原样重放上一次的位姿",
                                targetFps, g_retailFps);
                    else if (g_groups & (FL_G_CHASSIS30 | FL_G_CHASSISLERP))
                        FL_INFO("    节拍门位开着但**不生效**:%s ⇒ 包装只当探针用,行为逐位不变",
                                measureOnly ? "只量不改模式" : "目标帧率不高于原版帧率(本来就该直通)");
                    else
                        FL_INFO("    只读探针:每次原样转发,行为逐位不变"
                                "(没修的 %d 帧下期望 chassisrate ≈ %d)",
                                measureOnly ? g_retailFps : targetFps,
                                measureOnly ? 2000 : (targetFps * 1000) / 15);
                } else {
                    g_chsMode = FL_CHS_MODE_PROBE;
                }
            }
        } else {
            FL_WARN("车身外观包装:特征未命中或命中多处 —— 跳过(不影响其它改动)。"
                    "后果:车身悬挂递推仍按显示帧率走,60/90 帧下颠簸节奏快 2/3 倍。");
        }
    }

    // ── ★★★ 弹道流 30 Hz 节拍门(默认关;2026-09-22)──────────────────────────────────────────
    //    11 字节换成 `call fl_wrap_tracer` + 6 个 NOP。ecx 已由上一条 mov 装好,不用再取。
    //    只量不改模式下不装(那一臂要的是原版行为)。
    g_trcSite = NULL; g_trcOn = false; g_trcVFrame = 0xFFFFFFFFu; g_trcCalls = 0; g_trcFwd = 0;
    if (ok && !measureOnly && (g_groups & FL_G_TRACER)) {
        // ⚠ 把操作数通配掉之后,这段字节与紧邻的上一处(dword_CDB794 那个管理器)**完全相同** ——
        //   所以不能用 scan()(它要求唯一命中,这里必然报「命中多处」),必须像 R1 那样**按操作数过滤**:
        //   只认 `mov ecx,[0x00CDB79C]` 的那一处,过滤后必须恰好 1 处。
        unsigned char* trcHits[8];
        // 期望地址写 0:这条特征交给 scan_all(按操作数过滤),而**离线干扫不做操作数过滤** ——
        //   它只把「第一处命中」和期望地址比,于是会误报 FAIL(实测第一处在 0x004EE6E8)。
        //   与其它 scan_all 用例(R1 算 r、R2b 每显示帧秒数)一致:地址由运行期的操作数判据把关。
        Pattern pTrc = {"弹道流 update 调用点", kSigTracerUpdate,
                        (int)(sizeof kSigTracerUpdate / sizeof kSigTracerUpdate[0]), 0};
        const int nTrc = scan_all(pTrc, 2, (void*)kEngineTracerMgrVa, trcHits, 8);
        unsigned char* hit = (nTrc == 1) ? trcHits[0] : NULL;
        if (!hit) {
            FL_WARN("弹道流节拍门:按「管理器 = %08X」过滤后命中 %d 处(应恰好 1 处)—— 跳过。"
                    "后果:60/90 帧下弹道流密度仍是原版的 2/3 倍(开火看起来像机关枪)。",
                    (unsigned)kEngineTracerMgrVa, nTrc);
        } else {
            FL_INFO("  弹道流 update 调用点 @ %08X(管理器 %08X,过滤前 %d 处里唯一一处读它)",
                    (unsigned)(uintptr_t)hit, (unsigned)kEngineTracerMgrVa, nTrc);
            unsigned char* site = hit + 6;             // 从 test ecx,ecx 开始的 11 字节
            unsigned char code[11];
            memset(code, 0x90, sizeof code);
            code[0] = 0xE8;
            const int rel = (int)((unsigned char*)&fl_wrap_tracer - (site + 5));
            memcpy(code + 1, &rel, 4);
            ok = patch(site, code, (int)sizeof code, "弹道流 30 Hz 节拍门");
            if (ok) {
                g_trcSite = site;
                g_trcOn   = (targetFps > g_retailFps);
                FL_INFO("  弹道流节拍门已装 @ %08X → 包装 %08X;%s",
                        (unsigned)(uintptr_t)site, (unsigned)(uintptr_t)&fl_wrap_tracer,
                        g_trcOn ? "生效(每秒推进钉回 30 步 ⇒ 生成密度与原版相同)"
                                : "目标帧率不高于原版 ⇒ 每次都转发,行为逐位不变");
            }
        }
    }

    // ── ★ 状态图标乒乓动画(默认关;2026-09-22)。一处 4 字节:除数 30 → 目标帧率。
    if (ok && !measureOnly && (g_groups & FL_G_ICONANIM)) {
        Pattern pIcon = {"状态图标乒乓(帧号×15÷帧率)", kSigIconPingPong,
                         (int)(sizeof kSigIconPingPong / sizeof kSigIconPingPong[0]), 0x0051A4E8};
        unsigned char* hit = scan(pIcon);
        if (!hit || hit == (unsigned char*)-1) {
            FL_WARN("状态图标乒乓:特征未命中 / 命中多处 —— 跳过(图标动画在 60/90 帧下仍快 2/3 倍)。");
        } else if (*(int**)(hit + 14) != g_fpsClient) {
            // 偏移 +14 不是 +16:8B D0(2) | A1 +操作数 4(5) | 0F AF C2(3) | 33 D2(2) | F7 35(2) ⇒ 除数在 +14。
            FL_WARN("状态图标乒乓:除数指向 %08X,不是客户端帧率全局 %08X —— 跳过。",
                    (unsigned)(uintptr_t)*(int**)(hit + 14), (unsigned)(uintptr_t)g_fpsClient);
        } else {
            void* p = (void*)&g_fpsRender;
            ok = patch((void**)(hit + 14), &p, 4, "状态图标乒乓除数 → 目标帧率");
        }
    }

    // ── R:把「需要看到新渲染帧率」的那几条指令改指向我们的 g_fpsRender ────────────────
    // 这是本轮的设计改动:不再往游戏的客户端帧率全局里写 60(那个全局有上百处读者),
    // 改成只动我们确认需要新值的这些读者。漏掉一处的后果从「静默改错」变成「保持原样」。
    g_fpsRender = targetFps;
    // 时钟推进量:与 g_fpsRender 同一时刻就位 —— 那条 add 指令的两次读之间不能出现不一致。
    // ★ 2026-09-17(续):这里给的只是**起始值**(累加器还没跑起来时用它兜底,例如 FL_G_PERFRAME
    //   被关掉、逐帧钩子没装)。真正生效的是 clock_accumulator_step() 逐帧算出的精确值。
    //   累加器状态必须在这里归零,否则「换帧率重新 enable」会带着上一轮的余数。
    g_msPerFrame = 1000 / targetFps;
    g_clockAcc   = 0;
    g_clockWant  = 0;
    for (int i = 0; ok && !measureOnly && i < s.redirectCount; ++i) {
        if (!(g_groups & s.redirect[i].group)) { FL_INFO("跳过(本组被关掉) %s", s.redirect[i].name); continue; }
        void* p = s.redirect[i].newTarget ? s.redirect[i].newTarget : (void*)&g_fpsRender;
        ok = patch(s.redirect[i].operand, &p, 4, s.redirect[i].name);
        if (ok && s.redirect[i].newTarget)
            FL_INFO("    → %s 改指 %08X(不是帧率,是本补丁自己的量)",
                    s.redirect[i].name, (unsigned)(uintptr_t)p);
    }
    if (ok && !measureOnly) FL_INFO("已改指向 %d 处;客户端帧率全局 %08X 仍为 %d(未改动)",
                                    s.redirectCount, (unsigned)(uintptr_t)g_fpsClient, *g_fpsClient);
    if (ok && !measureOnly && (g_groups & FL_G_SCROLL)) {
        g_scrollOn = (g_scrollSites == 2);
        if (g_scrollOn)
            FL_INFO("★卷屏归一已开:键盘 / 边缘卷屏每帧步长 100 → %.3f(= 100 × %d ÷ %d)⇒ 每秒走的距离与原版相同。"
                    "右键拖拽卷屏不在此列。", g_scroll100, g_retailFps, targetFps);
        else
            FL_WARN("FL_G_SCROLL 开着,但两处卷屏步长没就位(见上面的 S WARN)—— 卷屏保持现状。");
    }
    // ⚠ 只在 FL_G_CLOCK 真的开着时才打这条 —— 否则上面那两处重定向被跳过,
    //   还打「时钟推进量 = 11」会让人以为修好了(2026-09-17 加)。
    if (ok && !measureOnly) {
        if (g_groups & FL_G_CLOCK) {
            // ★ 2026-09-17(续):起始值和**精确目标**都要打出来,方便跟 clockrate 实测对账。
            //   期望 clockrate ≈ 66667;对不上就说明累加器没跑(或跑多了),不要再靠推理判断。
            // ★★ 2026-09-18(第十一批):原版帧率下累加器被跳过(除非显式加 FL_G_CLOCK_AT_RETAIL),
            //   这时**不要**再打「期望 clockrate ≈ 66667」—— 那是修好之后的数,没修时是 66000。
            //   打错一行日志会把后面的实测对账带偏,这正是「量具必须自证」的用法。
            const bool clockAtRetail = (g_targetFps == g_retailFps);
            const bool accOn = !clockAtRetail || (g_groups & FL_G_CLOCK_AT_RETAIL);
            FL_INFO("时钟推进量:起始值 1000 ÷ %d = %d 毫秒/客户端帧", targetFps, g_msPerFrame);
            if (accOn) {
                FL_INFO("  累加器%s ⇒ 精确摊平到 每逻辑帧 66.667 毫秒(r=%d);期望 clockrate ≈ 66667",
                        clockAtRetail ? "(原版帧率下被 FL_G_CLOCK_AT_RETAIL 强制打开)" : "开着",
                        g_ratio);
            } else {
                FL_INFO("  ★累加器**不开**(目标是原版帧率 %d ⇒ 补丁在此不该改变任何东西):",
                        g_retailFps);
                FL_INFO("    时钟恒按原版 %d 毫秒/客户端帧推进 ⇒ 期望 clockrate ≈ %d",
                        g_msPerFrame, g_msPerFrame * g_ratio * 1000);
                FL_INFO("    这是刻意的:实测 30 帧下补丁与原版唯一的差异就是这个锯齿,去掉它 ⇒ 逐位一致");
            }
            if (!(g_groups & FL_G_PERFRAME))
                FL_WARN("  ⚠ FL_G_PERFRAME 关着 ⇒ 逐帧钩子没装,累加器跑不起来,"
                        "时钟将停留在上面的起始值(%d 毫秒/客户端帧),r=4 时偏慢约 4%%",
                        g_msPerFrame);
        } else {
            FL_WARN("FL_G_CLOCK 关着 ⇒ 时钟仍按原版 %d 毫秒/帧推进。"
                    "90 帧下**动画会快约 3 倍**,这不是可用配置,只是对照实验组。",
                    1000 / g_retailFps);
        }
    }

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

    // ── R2b 第四个派生量的存量值(默认关;见 FL_G_DER_SPF 定义处)。与上面三个同一手法:写回按新帧率重算的值。
    if (ok && !measureOnly && (g_groups & FL_G_DER_SPF)) {
        if (s.spfTarget) {
            const float v = 1.0f / (float)targetFps;
            ok = patch(s.spfTarget, &v, 4, "R2b 每显示帧秒数存量值");
            if (ok) FL_INFO("    每显示帧秒数 %08X → %.6f(= 1/%d;8 个读者:精灵脉动、贴花淡出、拖尾贴图滚动、子物体计时…)",
                            (unsigned)(uintptr_t)s.spfTarget, v, targetFps);
        } else {
            FL_WARN("FL_G_DER_SPF 开着,但每显示帧秒数没定位到(见上面的 R2b WARN)—— 这一项没改。");
        }
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
    // 以及「三个派生量必须跟着帧率走」(第七批:单位一致性 + 帧计数器实测 60/秒)。
    // 未证的只剩「那 43 处读者里有没有**不是**时间换算的」—— 已逐条看过 `0xCDBC5C` 的 11 处
    // (全是「毫秒 → 帧号」),`0xCDBC50`/`0xCDBC54` 的读者尚未逐条核对,所以这里如实说清楚。
    FL_INFO("安装完成:渲染 %d → %d 帧。逻辑帧率全局 %08X 仍为 %d(未改动 ⇒ 秒→帧换算、射速与原版逐帧相同)",
            g_retailFps, targetFps, (unsigned)(uintptr_t)g_fpsLogic, *g_fpsLogic);
    FL_INFO("  共 %d 处改动,可随时 FrameLabDisable 整体还原。", g_undoCount);
    if ((g_groups & FL_G_DERIVED2) == FL_G_DERIVED2) {
        FL_WARN("  已按帧率重算三个派生量(0xCDBC50/54/5C)。依据:sub_6F6CB0(建筑解包动画)把"
                "帧计数器除以「时长 × 0xCDBC5C」,分子分母必须同单位;帧计数器实测 ≈ 目标帧率"
                "(flctl displayclock)。剩余风险:那 43 处读者里若有不属于「毫秒↔帧号」换算的,"
                "以 desync dump 逐字段对账为准。");
    } else {
        FL_WARN("  三个帧率派生量**没开**:建筑解包动画在 60 帧下会快一倍"
                "(见 FL_G_DERIVED2 定义处)。要开就加上 0x%04X。", (unsigned)FL_G_DERIVED2);
    }

    // ★★★ 2026-09-17(第八批):残余风险告警 —— 整数客户端帧率全局 0x00CAF9D4 的读者。
    //   第七批修的是**派生浮点常量** 0xCDBC50/54/5C(FL_G_DERIVED2);这一条说的是它们的
    //   **整数源头** 0x00CAF9D4 —— 那是同一个修复的**另一半**,但我们**故意没动**它(56 处读者)。
    //   完整证据(2026-09-17 第八批,见 CHANGES.md §Z / RE 文档 §23):
    //     · 0x00CAF9D4 在镜像里的**初值就是 30**(.data 里直接可读),而整个 42MB 反编译输出里
    //       **没有任何一处**给它赋值,也**没有任何一处**取它的地址(&dword_CAF9D4 零匹配)
    //       ⇒ 它是**静态名义值**,语义 = 「当前客户端帧率」。
    //     · 0x00CAF9D0 同理,初值 15,语义 = **逻辑 tick 频率**。旁证:clockrate 实测
    //       66667 微秒/逻辑帧 = 1/15 秒,与 15 完全吻合(两个独立来源互证)。
    //     · ⇒ 所有把「秒」换算成「帧」的读者(`该全局 × 秒`,22 处)、所有
    //       `帧差 × (30.0 / 该全局)`(独苗,在 sub_5EB5E0)、所有 `1.0 / X / 该全局`
    //       (2 处,line 619363/619375),都仍按 30 算 —— 而实际呈现是 targetFps。
    //       帧数偏小 / 每帧步长偏大 ⇒ 这些**按呈现帧推进**的东西快 (targetFps/30) 倍。
    //     · 已逐条读过(全部只读分析,一处未改)的三个代表:
    //         sub_5EB5E0 case 4  W3DView 的「等 N 帧」倒计时:累加器 += 帧差 × (30/30) = 帧差
    //                            ⇒ 60 帧下倒计时用时**减半**。消费者 sub_913AE0,
    //                            上限在「定义对象 +32」;触发者 sub_559940(把显示对象链表里
    //                            带 +317&0x40 标志的对象位置求平均,交给 W3DView::vtbl[32])。
    //         sub_6CDBD0         帧预算 `该全局 × ms × 0.001` ⇒ 帧数减半 ⇒ 该计时器慢一倍。
    //         sub_6293D0         实测帧率被钳到 [0xCAF9D0, 0xCAF9D4] = [15,30]
    //                            ⇒ 60 帧下它仍认为「跑在 30」(质量档位封顶)。
    //   为什么**没有**默认改掉:56 处读者里还有没逐条核对的,而且至少一处
    //   (P5 粒子过期,见上面 pimul 那段注释)读它**就是**为了保持 30 基准 ——
    //   一旦把全局写成 60,那一处会跟着翻倍。所以按铁律「不懂的东西不盲改」先只**报告**。
    //   要一次性全改对,用 FL_G_FPSGLOBAL(0x2000):那等价于引擎自己「客户端帧率变了」的情形,
    //   也正是这个全局被设计出来的用途。先做 A/B 实测再决定,见 CHANGES.md §Z.5。
    if (ok && !measureOnly && targetFps != g_retailFps && !(g_groups & FL_G_FPSGLOBAL)) {
        FL_WARN("  ⚠ 残余风险(本局未修):整数客户端帧率全局 %08X 仍是 %d,而实际呈现是 %d 帧/秒。",
                (unsigned)(uintptr_t)g_fpsClient, *g_fpsClient, targetFps);
        FL_WARN("     所有把「秒→帧」写成 `该全局 × 秒` 的读者(22 处)、sub_5EB5E0 里那条"
                "`30.0 ÷ 该全局`(W3DView 的等 N 帧倒计时)、以及 `1.0 ÷ X ÷ 该全局`(2 处),"
                "都会快约 %.2f 倍。",
                (double)targetFps / (double)(*g_fpsClient > 0 ? *g_fpsClient : 1));
        FL_WARN("     依据:该全局初值 30 且全程序无写入点、无取地址 ⇒ 它是「静态名义帧率」。"
                "要一次性改对,加上 FL_G_FPSGLOBAL(0x%04X);"
                "注意它同时会改 P5 粒子过期(那处本意是保持 30 基准)。"
                "逐条清单与实测见 CHANGES.md §Z。",
                (unsigned)FL_G_FPSGLOBAL);
    } else if (ok && !measureOnly && (g_groups & FL_G_FPSGLOBAL)) {
        FL_INFO("  ★FL_G_FPSGLOBAL 已开:整数客户端帧率全局 %08X = %d(实际呈现 %d 帧/秒)"
                " —— 那批「秒→帧」读者已跟着改对。注意 P5 粒子过期也会跟着变。",
                (unsigned)(uintptr_t)g_fpsClient, *g_fpsClient, targetFps);
    }
    return FL_OK;
}

static int uninstall() {
    if (!g_installed) return FL_ERR_NOT_INSTALLED;
    mem::ThreadFreezer freeze;
    FL_INFO("开始卸载,挂起其它线程 %d 个", freeze.count);
    rollback();
    // 车身外观包装的 call 已被撤销表还原 ⇒ 登记也要清掉,否则 chassisrate 会把「已卸载」
    // 报成「装着但没数据」(「没装」与「没采到」必须是两句不同的话)。g_chsOrigFn 故意**不清**:
    // 万一有线程此刻正停在包装函数里,它醒来后还要用它转发一次。
    g_chsSite = NULL; g_chsMode = FL_CHS_MODE_PROBE;
    g_scrollOn = false;
    g_trcSite = NULL; g_trcOn = false;
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

// ── 时钟推进量累加器的离线自检(2026-09-17 动画定位会话,Claude)───────────────────
// 为什么需要这条自检:
//   clock_accumulator_step() 要解决的「r=4 时每逻辑帧只推进 64 而不是 66.667 毫秒」是个纯整数
//   算术问题,它**不依赖游戏里的任何东西** —— 只读写我们自己的 g_clockAcc / g_clockWant /
//   g_msPerFrame / g_ratio。而实机验证要占一台机器 + 45 秒起局,还会和别的会话抢游戏进程
//   (2026-09-17 就真的被别人的一局挡在门外)。所以给它一条不需要游戏的判据:
//   这样「累加器精不精确」这个问题永远不必靠推理回答,也不必等游戏可用。
//
// 判据(对 r ∈ [2,6] 各跑一遍,跑满 3 个逻辑帧):
//   ① 3 个逻辑帧的推进量之和必须**恰好** 200 毫秒 —— 200/3 = 66.667 每逻辑帧,精确;
//   ② **每个**逻辑帧各自的推进量必须是 66 或 67(±0.5 毫秒),不允许出现离群值。
//      ②比①更强:只看 3 帧总和的话,「一帧 50、一帧 83」这种锯齿也能骗过判据,而那在画面上是灾难。
//   ③ 自检资格:把**旧的**公式(每帧 1000÷目标帧率,整数截断)拿同一条判据去量,必须报不通过。
//      旧公式对 r=2..6 每一条都算不到 200(192~198),所以资格必然成立 ——
//      这同时也就是「这条自检真的能抓到我们修的那个 bug」的证明。
//
// 返回位掩码:bit(r-2) 置 1 = 该 r 通过;bit5(0x20) = 自检资格成立。全通过 = 0x3F。
extern "C" __declspec(dllexport) int __stdcall FrameLabSelfTestClock() {
    const int savedAcc   = fl::g_clockAcc;
    const int savedWant  = fl::g_clockWant;
    const int savedMs    = fl::g_msPerFrame;
    const int savedRatio = fl::g_ratio;

    int mask = 0;
    for (int r = 2; r <= 6; ++r) {
        fl::g_ratio      = r;
        fl::g_clockAcc   = 0;
        fl::g_clockWant  = 0;
        fl::g_msPerFrame = 0;
        int perLogic[3] = {0, 0, 0};            // 每个逻辑帧各推进了多少毫秒
        for (int i = 0; i < 3 * r; ++i) {
            fl::clock_accumulator_step();
            perLogic[i / r] += fl::g_msPerFrame;
        }
        const int total = perLogic[0] + perLogic[1] + perLogic[2];
        bool eachFrameSane = true;
        for (int k = 0; k < 3; ++k)
            if (perLogic[k] != 66 && perLogic[k] != 67) eachFrameSane = false;
        const bool ok = (total == 200) && eachFrameSane;
        if (ok) mask |= (1 << (r - 2));
        FL_INFO("时钟累加器自检 r=%d:三个逻辑帧各推进 %d/%d/%d 毫秒(应各为 66 或 67),"
                "合计 %d(应恰好 200)⇒ %s",
                r, perLogic[0], perLogic[1], perLogic[2], total, ok ? "通过" : "★失败");
    }

    // ★ 自检资格:同一条判据量**旧公式**。旧公式 = 每客户端帧推进 floor(1000÷目标帧率),
    //   目标帧率 = 15×r ⇒ 3 个逻辑帧共 3×r×floor(1000/(15r))。
    //   它必须报不通过,否则说明这条自检根本抓不到东西。
    int oldPass = 0;
    for (int r = 2; r <= 6; ++r) {
        const int oldTotal = 3 * r * (1000 / (15 * r));
        if (oldTotal == 200) oldPass |= (1 << (r - 2));
    }
    const bool gateWorks = (oldPass == 0);      // 旧公式对每个 r 都应不通过
    FL_INFO("时钟累加器自检资格:旧公式(1000÷帧率 截断)按同一判据的通过位 = 0x%02X"
            "(应为 0x00)⇒ 闸门%s", oldPass, gateWorks ? "会红,合格" : "★不会红,不合格");

    fl::g_clockAcc   = savedAcc;
    fl::g_clockWant  = savedWant;
    fl::g_msPerFrame = savedMs;
    fl::g_ratio      = savedRatio;

    const int finalMask = mask | (gateWorks ? 0x20 : 0);
    FL_INFO("时钟累加器自检结果掩码 = 0x%02X(全通过应为 0x3F)", finalMask);
    return finalMask;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabSetGroups(int mask) {
    fl::g_groups = (mask <= 0) ? FL_G_ALL : (mask & FL_G_EVERYTHING);
    FL_INFO("改动分组掩码设为 0x%04X", fl::g_groups);
    FL_INFO("  算r=%s 除帧率=%s 未确认两处=%s 批次=%s 系数=%s 粒子=%s 视觉步长=%s 逐帧=%s",
            (fl::g_groups & FL_G_RATIO)    ? "开" : "关", (fl::g_groups & FL_G_DIVFPS)   ? "开" : "关",
            (fl::g_groups & FL_G_UNSURE)   ? "开" : "关", (fl::g_groups & FL_G_BATCH)    ? "开" : "关",
            (fl::g_groups & FL_G_FRACTION) ? "开" : "关", (fl::g_groups & FL_G_PARTICLE) ? "开" : "关",
            (fl::g_groups & FL_G_VISUAL)   ? "开" : "关", (fl::g_groups & FL_G_PERFRAME) ? "开" : "关");
    // ★ 2026-09-17(第六批·订正):P6 与 P6b **已拆成两组**,各报一行。
    //   最初把它们归成一组(理由是「同一个失效模式」),结果二分时分不清是哪一个在起作用;
    //   而且 P6b 所在路径在正常对局里根本不跑(实测 40 秒 0 次调用),同组会把这件事藏起来。
    FL_INFO("  ★FL_G_VISUAL 视觉步长(P6)=%s%s",
            (fl::g_groups & FL_G_VISUAL) ? "开" : "关",
            (fl::g_groups & FL_G_VISUAL) ? "(sub_8F3850 的揭幕斜坡 1/30 → 1/目标帧率)" : "");
    FL_INFO("  ★FL_G_ANIMSTEP 动画时间步(P6b)=%s%s",
            (fl::g_groups & FL_G_ANIMSTEP) ? "开" : "关",
            (fl::g_groups & FL_G_ANIMSTEP)
                ? "(sub_903910 的除数 → 目标帧率浮点)"
                : " ⚠关着:该路径若被触发,混合斜坡在 60 帧下会快一倍");
    // ★★★ 2026-09-17(第七批):帧率三兄弟 = 「60 帧建筑动画回归」的**真因修复**。
    //   三路必须**整体**开:它们由同一段 init 从同一个整数帧率算出,拆开只会有半套新基准。
    //   这里在关着的时候把后果直接写出来 —— 免得下一个人看到「关」却不知道代价。
    FL_INFO("  ★★★帧率派生量三路: (float)帧率=%s  1000/帧率=%s  帧率*0.001=%s",
            (fl::g_groups & FL_G_DER_FPS)  ? "开" : "关", (fl::g_groups & FL_G_DER_MSPF) ? "开" : "关",
            (fl::g_groups & FL_G_DER_FPMS) ? "开" : "关");
    if ((fl::g_groups & FL_G_DERIVED2) == FL_G_DERIVED2) {
        FL_INFO("      ⇒ 三个常量已按目标帧率重算(60 帧下应为 60.0 / 16.667 / 0.06)");
    } else {
        FL_WARN("      ⚠三路没全开!建筑解包动画(模块 StructureUnpackUpdate 的 sub_6F6CB0)");
        FL_WARN("        把帧计数器除以「时长 × 0x00CDBC5C」,单位不一致 ⇒ 进度会按 30/60 的比例跑偏。");
        FL_WARN("        60 帧下正确值:0x00CDBC50=60.0  0x00CDBC54=16.667  0x00CDBC5C=0.06。");
        FL_WARN("        读数用 flctl displayclock;判据见 CHANGES.md §Y。");
    }
    // 2026-09-17 动画定位会话:时钟修正。它是**修复**,不是实验 —— 关掉就等于回到「动画快 3 倍」。
    FL_INFO("  ★★时钟推进量修正=%s%s",
            (fl::g_groups & FL_G_CLOCK) ? "开" : "关",
            (fl::g_groups & FL_G_CLOCK)
                ? "(修复:时钟 += 1000/目标帧率,而不是原版的 33)"
                : "(⚠已关:90 帧下游戏时钟会跑 3 倍快,动画随之快 3 倍 —— 只在做对照实验时这样)");
    // ★ 2026-09-18(第十一批):原版帧率下累加器默认**不跑**。理由见 FL_G_CLOCK_AT_RETAIL 定义处。
    //   在关着的时候把「这是刻意的、判据是什么」写出来,免得下一个人以为是漏装了。
    FL_INFO("  ★原版帧率下也修时钟=%s%s",
            (fl::g_groups & FL_G_CLOCK_AT_RETAIL) ? "开" : "关",
            (fl::g_groups & FL_G_CLOCK_AT_RETAIL)
                ? "(实验组:目标帧率 == 原版时也跑累加器 ⇒ 时钟增量会出现 33,33,34 锯齿)"
                : "(默认:目标帧率 == 原版时跳过累加器 ⇒ 时钟恒 33,30 帧下与原版逐位一致)");
    // 2026-09-17 动画定位会话:实验组单独一行,并且**显式警告方向相反** —— 它是唯一一处「默认全都变」。
    FL_INFO("  ★实验组 P8 写客户端帧率全局=%s%s",
            (fl::g_groups & FL_G_FPSGLOBAL) ? "开" : "关",
            (fl::g_groups & FL_G_FPSGLOBAL) ? "(方向与其它组相反:那批「秒→帧」读者会全部跟着变)" : "");
    // 2026-09-17:动画标尺(只读探针)。它不是补丁,是尺子 —— 单独一行,免得跟改动混在一起看。
    FL_INFO("  动画标尺探针=%s%s",
            (fl::g_groups & FL_G_ANIMPROBE) ? "开" : "关",
            (fl::g_groups & FL_G_ANIMPROBE) ? "(只改一条 call 的目标,行为逐位不变;给 FrameLabAnimRate 用)" : "");
    // ★ 2026-09-17(第六批):第二把动画时间尺子。同样不是补丁,是尺子。
    //   它量的是**混合斜坡**那条路径,而 clockrate / animrate 量的是**游戏时钟**那条 ——
    //   两把尺子互相看不见对方管的路径,所以必须分两行报,免得拿错读数当结论。
    FL_INFO("  混合斜坡量尺探针=%s%s",
            (fl::g_groups & FL_G_STEPPROBE) ? "开" : "关",
            (fl::g_groups & FL_G_STEPPROBE)
                ? "(只改一条 call 的目标,行为逐位不变;给 FrameLabBlendStepPerLogicFrame 用)"
                : "(关着:量不了 P6b 的效果,clockrate 看不见这条路径)");
    // 2026-09-22:弹道流与状态图标(用户 90 帧实机报的「开火像机关枪」)。
    FL_INFO("  弹道流 30 Hz 节拍门=%s%s",
            (fl::g_groups & FL_G_TRACER) ? "开" : "关",
            (fl::g_groups & FL_G_TRACER)
                ? "(生成密度 / 伸展 / 移动钉回每秒 30 步 —— 开火不再像机关枪;未经实机验证)"
                : "(默认:弹道流每秒生成条数是原版的 2/3 倍)");
    FL_INFO("  状态图标乒乓动画=%s%s",
            (fl::g_groups & FL_G_ICONANIM) ? "开" : "关",
            (fl::g_groups & FL_G_ICONANIM)
                ? "(sub_51A4D0 的除数 30 → 目标帧率;未经实机验证)" : "");
    // 2026-09-21:卷屏归一(用户点名要的可自选项)。
    FL_INFO("  卷屏速度按帧率归一=%s%s",
            (fl::g_groups & FL_G_SCROLL) ? "开" : "关",
            (fl::g_groups & FL_G_SCROLL)
                ? "(键盘 / 边缘卷屏每秒走的距离与原版相同;右键拖拽不在此列;未经实机验证)"
                : "(默认:卷屏随渲染帧率变快 —— 60/90 帧下快 2/3 倍)");
    // 2026-09-21(显示帧递推普查):第四个派生量。默认关,单独一行。
    FL_INFO("  第四个派生量 每显示帧秒数(0x00CDBD34)=%s%s",
            (fl::g_groups & FL_G_DER_SPF) ? "开" : "关",
            (fl::g_groups & FL_G_DER_SPF)
                ? "(1/30 → 1/目标帧率:精灵脉动、贴花淡出、拖尾贴图滚动、子物体计时不再快 2/3 倍;未经实机验证)"
                : "(默认:仍是 1/30 —— 60/90 帧下那 8 个读者快 2/3 倍)");
    // ★ 2026-09-21(悬挂路径会话):车身外观的两个位,**都默认关**。各报一行 ——
    //   一个是尺子、一个是修复候选,混成一行会让人分不清「量的是修之前还是修之后」。
    FL_INFO("  车身外观探针=%s%s",
            (fl::g_groups & FL_G_CHASSISPROBE) ? "开" : "关",
            (fl::g_groups & FL_G_CHASSISPROBE)
                ? "(只改一条 call 的目标,原样转发;给 chassisrate / chassisslots / 黑匣子姿态轴 用)" : "");
    FL_INFO("  ★车身外观 30 Hz 节拍门=%s%s",
            (fl::g_groups & FL_G_CHASSIS30) ? "开" : "关",
            (fl::g_groups & FL_G_CHASSIS30)
                ? "(修复候选:悬挂/俯仰/侧倾递推钉回每秒 30 步,其余显示帧回放缓存)"
                : "(默认:车身递推按显示帧率走 —— 60/90 帧下颠簸节奏快 2/3 倍)");
    FL_INFO("  ★车身外观 位姿插值=%s%s",
            (fl::g_groups & FL_G_CHASSISLERP) ? "开" : "关",
            (fl::g_groups & FL_G_CHASSISLERP)
                ? "(节拍门的插值变体:递推仍 30 Hz,显示时在相邻两个位姿之间按相位插值 —— 为 90 帧准备)" : "");
    return fl::g_groups;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabMeasureOnly() {
    FL_INFO("收到「只量不改」请求(基线组)");
    fl::g_measureOnly = true;
    const int rc = fl::install(0, true);
    FL_INFO("只量不改结果 = %d", rc);
    return rc;
}

// ★★★ 2026-09-19(动画方向会话)新增:带 groups 的「只量不改」。
// 为什么需要:`FrameLabMeasureOnly` 内部就调了 `install(0,true)`,而 `install` 开头是
//   `if (g_installed) return FL_ERR_ALREADY`。所以「先 measure 再 groups+enable」这条路
//   走不通 —— enable 会被「已经安装过了」挡掉,动画钩子永远装不上,
//   原版基线的动画轴永远是「0 帧」(实测三次都是 0)。
// 这个入口一次把两件事做完:设 measureOnly、设 groups、再 install。
// groups 里目前只期望 FL_G_ANIMPROBE(只读尺子,不改任何数据)。
extern "C" __declspec(dllexport) int __stdcall FrameLabMeasureOnlyEx(int groups) {
    FL_INFO("收到「只量不改」请求(groups=0x%X)", (unsigned)groups);
    fl::g_measureOnly = true;
    if (groups) fl::g_groups = (unsigned)groups;
    const int rc = fl::install(0, true);
    FL_INFO("只量不改(groups=0x%X)结果 = %d", (unsigned)groups, rc);
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
// 动画次数门的实际工作量:转发了多少次、跳过了多少次。比值应当 ≈ 1:2(每 3 次转发 1 次)。
// ★这是"门真的在起作用"的证据,不是"我装了它"。
// 采样 n 个渲染帧的(逻辑帧, 阶段, 动画进度),写进日志。用来测「动画按什么推进」。
// 进度字段来源:引擎脚本 API CurDrawablePrevAnimFraction → sub_555D80 → *(*(0xCDD17C+296)+20)
extern "C" __declspec(dllexport) int __stdcall FrameLabSampleAnim(int samples) {
    if (samples <= 0 || samples > 600) samples = 120;
    unsigned char* mgr = *(unsigned char**)0x00CDD17C;
    FL_INFO("开始采样 %d 个渲染帧;动画管理器 %08X", samples, (unsigned)(uintptr_t)mgr);
    fl::g_sampleLeft = samples;
    return samples;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabGateStats() {
    const long fwd = fl::g_fwdCalls, skip = fl::g_skipCalls;
    FL_INFO("动画次数门:转发 %ld 次,跳过 %ld 次(门%s)", fwd, skip,
            fl::g_animGate ? "开" : "关");
    if (fl::g_frameObj) {
        void** vt = *(void***)fl::g_frameObj;
        FL_INFO("  逐帧对象 %08X  vptr %08X  槽144 → %08X",
                (unsigned)(uintptr_t)fl::g_frameObj, (unsigned)(uintptr_t)vt,
                (unsigned)(uintptr_t)vt[36]);
    }
    return (int)(fwd ? (skip * 100 / (fwd + skip)) : -1);   // 返回跳过占比%
}

extern "C" __declspec(dllexport) int __stdcall FrameLabEngineFps() {
    if (!fl::g_frameObj || !mem::read_ok((unsigned char*)fl::g_frameObj + 0x70, 4)) return -1;
    const float f = *(float*)((unsigned char*)fl::g_frameObj + 0x70);
    if (f < 0.0f || f > 10000.0f) return -1;
    return (int)(f * 100.0f);
}

extern "C" __declspec(dllexport) int __stdcall FrameLabLogicFrame();   // 定义在下面

// ═══════════ 2026-09-17 动画定位会话:动画标尺(只读,不属于补丁本体)═══════════
// 整个项目的验收卡在「动画快 3 倍」上,而这条一直没有客观读数 —— 只能拿眼睛看。
// 下面三个接口把「动画快多少」变成数字。它们**只读**,不改游戏任何字节
// (唯一的例外是 FL_G_ANIMPROBE 那条 call 目标重定向,而且那个包装函数对调用方逐位无差别)。

// 读任意地址的 4 字节(只读诊断)。给自动化脚本量全局用:
//   0x00CE1388 = 全局视觉时钟(毫秒)   0x00CAF9D4 = 客户端帧率   0x00CAF9D0 = 逻辑帧率
//   0x00CE176C = 每帧毫秒(1000 ÷ 客户端帧率)   0x00D075C8/CC = APT 时钟对
// 返回 -1 = 该地址不可读。
extern "C" __declspec(dllexport) int __stdcall FrameLabReadDword(int va) {
    const void* p = (const void*)(uintptr_t)(unsigned)va;
    if (!mem::read_ok(p, 4)) return -1;
    return *(const int*)p;
}

// 清空动画标尺记下的对象。★换配置前后**各调一次**:两组之间的对象不能混在一起,
// 否则「哪一组快」就说不清了。
extern "C" __declspec(dllexport) int __stdcall FrameLabAnimReset() {
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
        fl::g_animOwner[i] = NULL; fl::g_animFrame[i] = NULL;
        fl::g_animHit[i] = 0;      fl::g_animTick[i] = 0;   // ★新鲜度也要清,否则新槽会被当成「旧的」
    }
    fl::g_animCalls = 0;
    FL_INFO("动画标尺:已清空(探针计数归零)");
    return 0;
}

// ★ 一个只属于本文件的极小「格式化追加」辅助。**不能用 bb::fmt_append** ——
//   它是 bb 命名空间里的内部函数(定义在本文件更靠后、落在 namespace bb 里面),
//   而这个导出函数在**全局**作用域,够不着它。第一版我在这里写了 `static void
//   fmt_append(...)` 的前向声明,编译直接报 **C2129 静态函数已声明但未定义**
//   —— 编译器把它当成「全局的另一个函数」,而不是 bb 里那一个。这是本批踩的坑,
//   记在这里免得下次再犯。
static void anim_slots_append(char* buf, int cap, int* used, const char* fmt, ...) {
    if (*used >= cap - 1) return;
    va_list a; va_start(a, fmt);
    const int n = _vsnprintf_s(buf + *used, (size_t)(cap - *used), _TRUNCATE, fmt, a);
    va_end(a);
    if (n < 0) { *used = cap - 1; buf[*used] = 0; return; }   // _TRUNCATE 截断时返回 -1
    *used += n;
}

// ── ★★ 动画槽全表(只读诊断;2026-09-18 第十一批·续)──────────────────────────────
// 为什么需要:黑匣子的动画轴只记**前 4 个**槽,而实测两次运行采到的对象**不是同一批**
//   (槽分配是「先到先得 + 淘汰最不活跃」,运行时序不同结果就不同),而且那 4 个槽
//   两次采到的都是**静态**对象(分数整段不变:通道 0 恒 4.0164、通道 1/2 恒 0)。
//   可「切动画」钩子每秒被走约 24 次,说明「在动的对象」确实存在,只是不在这 4 个槽里。
//   ⇒ 必须能把 12 个槽**全部**倒出来看。
//
// ★ 用法(关键):**连续调两次,间隔约 1 秒**,对比两次的「当前」列 ——
//   变的那个槽就是「正在动的对象」,它的 owner 才是下一步该盯的载具/建筑。
//   单次调用只能看到「谁登记过」,看不到「谁在动」。
//
// ★ 为什么只有一个参数:调用方是 flctl,它用 CreateRemoteThread 远程调用,而那里
//   只能传**一个** lpParameter。所以容量写死成下面这个常量,调用方只要在目标进程里
//   分配一块这么大的缓冲、把地址传进来即可。12 个槽 × 约 70 字节 + 头 < 1KB,4096 足够。
//
// 返回:活的槽数(>=0)。0 = 探针没装(FL_G_ANIMPROBE 关)或还没采到任何对象。
#define FL_ANIM_SLOTS_CAP 16384
extern "C" __declspec(dllexport) int __stdcall FrameLabAnimSlots(char* buf) {
    if (!buf) return -1;
    const int cap = FL_ANIM_SLOTS_CAP;
    int used = 0;
    anim_slots_append(buf, cap, &used,
                      "动画槽 %d 个,探针累计走了 %ld 次,被改的 call 在 %08X(原目标 %08X)\r\n",
                      (int)FL_ANIM_OWNERS, (long)fl::g_animCalls,
                      (unsigned)(uintptr_t)fl::g_animSite, (unsigned)(uintptr_t)fl::g_animOrigFn);
    int live = 0;
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
        void* owner = fl::g_animOwner[i];
        float* fp   = fl::g_animFrame[i];
        if (!owner) continue;
        ++live;
        // 分数用 SEH 兜住:owner 可能刚被换掉,指针短暂悬垂。
        // 这里**不用 mem::read_ok** —— read_ok 走 VirtualQuery,而 SEH 的代价是零
        // (只有真异常时才展开),更省。
        // ★★ 一次读 4 个通道(2026-09-19 动画方向会话)。为什么:
        //   `sub_99BE00` 是 `fld dword ptr [ecx + eax*4 + 8]` —— 即 **`+0xB0` 是一个 float
        //   数组**(带索引),而 `sub_8EDED0` 只取通道 0。实测通道 0 在 1 秒内不变
        //   (而且这个录像里全是 0.00000),所以「当前帧」很可能在**别的通道**上。
        //   一次把 4 个通道都打出来,就能看出哪个在动。
        //   ★ 这里可以用 SEH:本函数是**导出函数**(不是被手工 hook 调用的),不涉及
        //     栈帧布局的约定(铁律 42 只约束 hook 调用路径上的函数)。
        float v0 = -1.0f, v1 = -1.0f, v2 = -1.0f, v3 = -1.0f;
        __try {
            if (fp) {
                v0 = fp[0]; v1 = fp[1]; v2 = fp[2]; v3 = fp[3];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { v0 = v1 = v2 = v3 = -2.0f; }
        anim_slots_append(buf, cap, &used,
                          "  槽 %2d  owner=%08X  分数指针=%08X  ch0=%10.5f ch1=%10.5f ch2=%10.5f ch3=%10.5f  命中=%ld\r\n",
                          i, (unsigned)(uintptr_t)owner, (unsigned)(uintptr_t)fp,
                          v0, v1, v2, v3, (long)fl::g_animHit[i]);
    }
    anim_slots_append(buf, cap, &used,
                      "  活的槽 %d / %d\r\n", live, (int)FL_ANIM_OWNERS);
    return live;
}

// ── ★★★ 2026-09-21(悬挂路径会话):车身外观的三把只读尺子 ───────────────────────────────
// 背景与补丁点见 FL_G_CHASSISPROBE 定义处;节拍门的纯逻辑在 src/chassis_gate.h。
//
// ① FrameLabChassisSlots(buf)   倒出「被包装函数见过的载具」全表(按调用次数排前 24 个):
//      Drawable 地址、locomotor 外观枚举(告诉我们它走哪个分支)、调用/放行/回放次数、最近一次四个输出。
//      与 animslots / memscan 一样是「带输出缓冲」的命令(单参数 + 固定容量)。
//      给脚本解析的字段全部用 **ASCII 标签**(铁律 48:别让正则依赖中文)。
//      返回:活的格子数;-2 = 包装没装(这不是「没数据」)。
#define FL_CHASSIS_SLOTS_CAP 16384
#define FL_CHASSIS_SHOW      24
extern "C" __declspec(dllexport) int __stdcall FrameLabChassisSlots(char* buf) {
    if (!buf) return -1;
    const int cap = FL_CHASSIS_SLOTS_CAP;
    int used = 0;
    anim_slots_append(buf, cap, &used,
                      "车身外观包装:call @%08X(原目标 %08X) 模式=%d(0 探针 / 1 回放 / 2 插值) 目标帧率=%d 原版帧率=%d\r\n",
                      (unsigned)(uintptr_t)fl::g_chsSite, (unsigned)(uintptr_t)fl::g_chsOrigFn,
                      fl::g_chsMode, fl::g_targetFps, fl::g_retailFps);
    anim_slots_append(buf, cap, &used,
                      "TOTAL calls=%ld adv=%ld hold=%ld zero=%ld noslot=%ld gate=%d installed=%d\r\n",
                      (long)fl::g_chsCalls, (long)fl::g_chsAdv, (long)fl::g_chsHold,
                      (long)fl::g_chsZero, (long)fl::g_chsNoSlot, fl::g_chsMode,
                      fl::g_chsSite ? 1 : 0);
    if (!fl::g_chsSite) {
        anim_slots_append(buf, cap, &used,
                          "  包装**没装**(FL_G_CHASSISPROBE 0x80000 / FL_G_CHASSIS30 0x100000 都关着,"
                          "或特征没命中)—— 这不是「没数据」,是「没装」。\r\n");
        return -2;
    }
    // 取前 N 个:**先按「放行 + 回放」次数**(= 真的是载具、且在屏幕里待得久),再按调用次数。
    //   ★ 2026-09-21 首次实机(原版基线臂)的教训:只按调用次数排,前 24 名全是**建筑**
    //     (每帧都被画、但没有 locomotor ⇒ 原函数返回 0 ⇒ 放行恒为 0),真正的两辆车排不进榜。
    //   表只有 1024 格,N 次线性扫描足够,不值得为它写排序。
    static char taken[FL_CHS_SLOTS];
    memset(taken, 0, sizeof taken);
    int live = 0;
    for (int i = 0; i < FL_CHS_SLOTS; ++i) if (fl::g_chsTab[i].key) ++live;
    for (int n = 0; n < FL_CHASSIS_SHOW; ++n) {
        int best = -1;
        for (int i = 0; i < FL_CHS_SLOTS; ++i) {
            if (!fl::g_chsTab[i].key || taken[i]) continue;
            if (best < 0) { best = i; continue; }
            const long a = fl::g_chsTab[i].advances + fl::g_chsTab[i].holds;
            const long b = fl::g_chsTab[best].advances + fl::g_chsTab[best].holds;
            if (a > b || (a == b && fl::g_chsTab[i].calls > fl::g_chsTab[best].calls)) best = i;
        }
        if (best < 0) break;
        taken[best] = 1;
        const FlChassisEnt e = fl::g_chsTab[best];      // 拷一份再打:渲染线程同时在改它
        anim_slots_append(buf, cap, &used,
                          "  drawable=%08X app=%d calls=%ld adv=%ld hold=%ld moved=%ld"
                          "  pitch=%9.5f roll=%9.5f yaw=%9.5f z=%9.5f\r\n",
                          (unsigned)(uintptr_t)e.key, e.appearance, e.calls, e.advances, e.holds, e.moved,
                          e.shown[0], e.shown[1], e.shown[2], e.shown[3]);
    }
    anim_slots_append(buf, cap, &used,
                      "  活的格子 %d / %d(app: 1/7=sub_526BD0 轮式悬挂 | 2/3/4=sub_51E800 | 8=sub_5262D0)\r\n",
                      live, (int)FL_CHS_SLOTS);
    return live;
}

// ⓪ FrameLabChassisStatus()   车身外观包装到底装上没有、处于什么模式。
//      -2 = 没装(分组位没开,或特征没命中 / 目标地址不对 —— 后一种只会在 DLL 日志里留一条 WARN,
//           GUI 用户根本看不见);0 = 只读探针;1 = 节拍门·回放;2 = 节拍门·插值。
//      ★ 2026-09-21 的教训:用户第一次「验收修复」时掩码其实是出厂默认(修复关),而界面上没有任何一处
//        能看出来。凡是「默认关 / 可能静默装不上」的东西,都必须有一个上层读得到的状态。
extern "C" __declspec(dllexport) int __stdcall FrameLabChassisStatus() {
    if (!fl::g_chsSite) return -2;
    return fl::g_chsMode;
}

// FrameLabTracerStatus():弹道流节拍门的状态。-2 = 补丁点没就位;0 = 关;1 = 已生效。
//   低 16 位另带「放行 ÷ 调用 × 1000」,给脚本当判据:90 帧生效时应 ≈ 333,60 帧 ≈ 500,没装门 = 1000。
extern "C" __declspec(dllexport) int __stdcall FrameLabTracerStatus() {
    if (fl::g_installed && (fl::g_groups & FL_G_TRACER) && !fl::g_trcSite) return -2;
    if (!fl::g_trcOn) return 0;
    const long c = fl::g_trcCalls, f = fl::g_trcFwd;
    const int ratio = c > 0 ? (int)((long long)f * 1000 / c) : 0;
    FL_INFO("弹道流节拍门:调用 %ld 转发 %ld ⇒ 放行比例 %d/1000(90 帧应 ≈ 333,60 帧 ≈ 500)", c, f, ratio);
    return 1000000 + ratio;
}

// FrameLabScrollStatus():卷屏归一的状态。-2 = 两处补丁点没就位(特征没命中 / 常量不对);0 = 关;1 = 已生效。
//   与 FrameLabChassisStatus 同一个理由:可自选的东西必须让上层读得到「到底开没开成」。
extern "C" __declspec(dllexport) int __stdcall FrameLabScrollStatus() {
    if (fl::g_scrollOn) return 1;
    if (fl::g_installed && (fl::g_groups & FL_G_SCROLL) && fl::g_scrollSites != 2) return -2;
    return 0;
}

// ② FrameLabChassisRate(逻辑帧数)   ★车身路径的**主判据**:同一辆车每逻辑帧递推了几步(×1000)。
//      与 clockrate / blendrate 同一类「与渲染帧率无关的不变量」:
//        原版 30 帧(r=2):每逻辑帧 2 个显示帧 × 每帧 1 步            = 2 步 ⇒ **2000**
//        没修的 60 / 90 帧:                                           4 / 6 步 ⇒ 4000 / 6000
//        节拍门生效的任何帧率:                                        2 步 ⇒ **2000**
//      取「区间内 放行 + 回放 次数最多的那辆车」(真的是载具、且在屏幕里待得最久的那辆)。
//      ★ 2026-09-21 首次实机订正:第一版取「调用次数最多的 drawable」,结果挑中一栋**建筑**
//        (每帧都画、没有 locomotor ⇒ 放行恒 0),读数 0.000。建筑也会走到这个包装里,只是原函数返回 0。
//      ★ 同一次实机还暴露了第二个问题:自动化回放时镜头不动,车只在屏幕里待几秒(那一局 139 / 300 帧)。
//        「放行次数 ÷ 逻辑帧数」在车不可见时会偏低。所以读数改成**与可见时长无关**的形式:
//            每逻辑帧递推步数 = 放行 ÷ (放行 + 回放) × (区间渲染帧数 ÷ 区间逻辑帧数)
//        第一项 = 这辆车「每个被画到的显示帧递推几步」(没修 = 1,60 帧节拍门 = 1/2,90 帧 = 1/3),
//        第二项 = 每逻辑帧有几个显示帧(30/60/90 帧 = 2/4/6)。两项相乘与车可见多久无关。
//      同时仍然报**覆盖率**(这辆车被画到的帧数 ÷ 区间渲染帧数),太低说明样本少,读数只能当参考。
//      ⚠ 内部用静态快照缓冲,**不可重入**(flctl 一次只发一条命令,够用)。
//      返回:步数 ×1000;-1 = 区间内没有可用读数;-2 = 包装没装。
extern "C" __declspec(dllexport) int __stdcall FrameLabChassisRate(int logicFrames) {
    if (logicFrames <= 0 || logicFrames > 1800) logicFrames = 150;
    FL_INFO("车身尺子:进入(量 %d 个逻辑帧)", logicFrames);
    if (!fl::g_chsSite) {
        FL_ERR("车身尺子:包装**没装**(FL_G_CHASSISPROBE / FL_G_CHASSIS30 都关着,或特征没命中)"
               "—— 这不是「没数据」,是「没装」");
        return -2;
    }
    const int f0 = FrameLabLogicFrame();
    if (f0 < 0) { FL_ERR("车身尺子:取不到逻辑帧(还没进对局?)"); return -1; }

    static void* snapKey[FL_CHS_SLOTS];
    static long  snapCalls[FL_CHS_SLOTS], snapAdv[FL_CHS_SLOTS], snapHold[FL_CHS_SLOTS];
    for (int i = 0; i < FL_CHS_SLOTS; ++i) {
        snapKey[i]   = fl::g_chsTab[i].key;
        snapCalls[i] = fl::g_chsTab[i].calls;
        snapAdv[i]   = fl::g_chsTab[i].advances;
        snapHold[i]  = fl::g_chsTab[i].holds;
    }
    const long calls0 = fl::g_chsCalls, adv0 = fl::g_chsAdv, hold0 = fl::g_chsHold, zero0 = fl::g_chsZero;
    const long rend0 = fl::g_frameCount;

    const DWORD t0 = GetTickCount();
    int cur = f0;
    while (cur - f0 < logicFrames && (GetTickCount() - t0) < 180000) {
        Sleep(2);
        cur = FrameLabLogicFrame();
        if (cur < 0) break;
    }
    const long done = cur - f0;
    const long rend = fl::g_frameCount - rend0;
    const DWORD elapsed = GetTickCount() - t0;
    if (done <= 0) { FL_ERR("车身尺子:一个逻辑帧都没走完 —— 读数无效"); return -1; }

    int best = -1;
    long bestSeen = 0;                                        // 放行 + 回放 = 这辆车被画到、且真的是载具的帧数
    for (int i = 0; i < FL_CHS_SLOTS; ++i) {
        const FlChassisEnt e = fl::g_chsTab[i];
        if (!e.key) continue;
        // 区间内才出现的车(格子在快照里还是空的 / 是别人的)从 0 算起;被换过人的旧账不能拿来相减。
        const bool same = (e.key == snapKey[i]);
        const long da = e.advances - (same ? snapAdv[i] : 0), dh = e.holds - (same ? snapHold[i] : 0);
        if (da <= 0) continue;                                // 没放行过 = 不是载具(建筑 / 道具)
        if (da + dh > bestSeen) { bestSeen = da + dh; best = i; }
    }
    FL_INFO("车身尺子:区间 %ld 个逻辑帧 / %ld 个渲染帧 / %u 毫秒;全体载具 调用 %ld 放行 %ld 回放 %ld 返回0 %ld",
            done, rend, (unsigned)elapsed, (long)fl::g_chsCalls - calls0, (long)fl::g_chsAdv - adv0,
            (long)fl::g_chsHold - hold0, (long)fl::g_chsZero - zero0);
    if (best < 0 || bestSeen < 30 || rend <= 0) {
        FL_ERR("车身尺子:区间内没有一辆载具被画到足够多帧(最多的一辆只有 %ld 帧,至少要 30)—— "
               "镜头里没有载具?读数无效,别拿它当结论", bestSeen);
        return -1;
    }
    const FlChassisEnt e = fl::g_chsTab[best];
    const bool same = (e.key == snapKey[best]);
    const long dAdv = e.advances - (same ? snapAdv[best] : 0), dHold = e.holds - (same ? snapHold[best] : 0);
    const int out = (int)((long long)dAdv * rend * 1000 / ((long long)(dAdv + dHold) * done));
    const int coverage = (int)((long long)bestSeen * 100 / rend);
    FL_INFO("★车身尺子读数:drawable %08X(外观 %d)被画到 %ld 帧:放行 %ld 回放 %ld;区间每逻辑帧 %ld.%02ld 个显示帧 ⇒ "
            "每逻辑帧递推 %d.%03d 步(原版与修好的配置都应是 2.000;没修的 60/90 帧是 4/6);覆盖率 %d%%",
            (unsigned)(uintptr_t)e.key, e.appearance, bestSeen, dAdv, dHold,
            rend / done, (rend * 100 / done) % 100, out / 1000, out % 1000, coverage);
    if (coverage < 30)
        FL_WARN("  ⚠ 覆盖率 %d%%:这辆车只在屏幕里待了很短一段,样本少;读数的形式与可见时长无关,但请换个窗口复核", coverage);
    return out;
}

// ③ FrameLabSelfTestChassis()   节拍门的**离线自检**(纯逻辑:私有的表 + 假递推,不需要游戏)。
//      假递推与引擎同形(离散弹簧 + 确定性的「随机」踢),所以「位姿序列逐位相同」是有意义的判据。
//      返回位掩码,全通过 = 0x3FF:
//        0x01  目标 30:每帧放行、零回放(直通 ⇒ 与原版逐位一致)
//        0x02  目标 60:放行 150 / 回放 150,放行得到的位姿序列与 30 帧那一轮**逐位相同**,回放值恒等于上次放行值
//        0x04  目标 90:放行 100 / 回放 200,同上
//        0x08  非整数倍(45 / 75):放行次数 = 虚拟帧个数,同一虚拟帧绝不放行两次,连续回放不超过 ceil(fps/30)
//        0x10  只开探针(门关)在 90 帧:每帧放行、零回放;且**按同一判据它不通过**(300 ≠ 100 ⇒ 闸门会红)
//        0x20  原函数返回 0:原样返回 0、不缓存、不回放;之后恢复正常放行
//        0x40  表满:退回直通(每次都放行、不丢姿态);全部陈旧后格子可回收
//        0x80  陈旧重置:久未出现的同一地址回来时先重置(当成新车),第一帧必放行
//        0x100 插值模式(60 / 90 / 45 / 75):放行次数与回放模式相同;每个虚拟帧最后一个显示帧(相位 = 1)的输出
//              与引擎位姿**逐位相同**且该序列 == 30 帧参考序列;其余显示帧的输出落在相邻两个位姿之间
//        0x200 插值模式的两个边界:目标 30 = 直通(逐位);位姿不相邻(车离开屏幕又回来)时**不**拿过期位姿当起点
struct ChsFake { float pitch, rate, roll, rollRate; unsigned lcg; long runs; int returnZero; };
static char chs_fake_orig(void* ctx, void* self, float* out) {
    ChsFake* f = (ChsFake*)ctx;
    (void)self;
    if (f->returnZero) return 0;
    ++f->runs;
    const float ar = f->rate < 0 ? -f->rate : f->rate;
    const float arr = f->rollRate < 0 ? -f->rollRate : f->rollRate;
    if (ar < 0.0040f && arr < 0.0020f) {                     // 与 sub_5269C0 同形:衰减够了才再踢一次
        f->lcg = f->lcg * 1664525u + 1013904223u;
        const float kick = 0.016f;
        switch ((f->lcg >> 16) & 3) {
            case 0: f->rate -= kick; f->rollRate -= kick * 0.5f; break;
            case 1: f->rate += kick; f->rollRate -= kick * 0.5f; break;
            case 2: f->rate -= kick; f->rollRate += kick * 0.5f; break;
            default: f->rate += kick; f->rollRate += kick * 0.5f; break;
        }
    }
    f->rate     += -0.06f * f->pitch - 0.20f * f->rate;      // 弹簧 + 阻尼(无 dt,每次调用一步)
    f->rollRate += -0.03f * f->roll  - 0.10f * f->rollRate;
    f->pitch    += f->rate * 0.8f;
    f->roll     += f->rollRate * 0.8f;
    out[0] = f->pitch; out[1] = f->roll; out[2] = 0.0f;
    out[3] = (f->pitch < 0 ? -f->pitch : f->pitch) * 2.5f;
    return 1;
}

#define CHS_TEST_FRAMES 300
#define CHS_TEST_FIRST  1200u
static FlChassisEnt g_chsTestTab[FL_CHS_SLOTS];

// 跑 CHS_TEST_FRAMES 个显示帧。poses 收集每次**放行**得到的俯仰;返回放行次数。
static int chs_selftest_run(int fps, int gateOn, float* poses, long* holds, int* maxHoldRun,
                            int* doubleAdvance, int* holdMismatch) {
    memset(g_chsTestTab, 0, sizeof g_chsTestTab);
    ChsFake f; memset(&f, 0, sizeof f); f.lcg = 20260921u;
    void* self = (void*)(uintptr_t)0x04A51230u;
    int adv = 0, run = 0;
    unsigned lastVf = 0xFFFFFFFFu;
    float lastOut[FL_CHS_OUTS] = {0, 0, 0, 0};
    *holds = 0; *maxHoldRun = 0; *doubleAdvance = 0; *holdMismatch = 0;
    // 起点取 1200:同时被 2 和 3 整除 ⇒ 60 / 90 帧下第一个虚拟帧是完整的,放行次数恰为 300×30÷fps。
    for (unsigned frame = CHS_TEST_FIRST; frame < CHS_TEST_FIRST + CHS_TEST_FRAMES; ++frame) {
        float out[FL_CHS_OUTS] = {0, 0, 0, 0};
        int did = -1;
        const char r = fl_chs_gate(g_chsTestTab, self, out, frame, fps, 30, gateOn,
                                   (unsigned)(2 * fps), chs_fake_orig, &f, NULL, &did);
        if (did == FL_CHS_DID_ADVANCE) {
            const unsigned vf = fl_chs_vframe(frame, fps, 30);
            if (gateOn && vf == lastVf) *doubleAdvance = 1;
            lastVf = vf;
            if (adv < CHS_TEST_FRAMES) poses[adv] = out[0];
            ++adv; run = 0;
            memcpy(lastOut, out, sizeof lastOut);
        } else if (did == FL_CHS_DID_HOLD) {
            ++*holds; ++run;
            if (run > *maxHoldRun) *maxHoldRun = run;
            if (!r || memcmp(lastOut, out, sizeof lastOut) != 0) *holdMismatch = 1;
        }
    }
    return adv;
}

// 插值模式的一轮:返回是否满足 0x100 那条判据。ref = 30 帧参考位姿序列。
//   相位在这里用**自己的**整数算术重算(不调 fl_chs_phase)—— 判据必须有独立来源。
static bool chs_selftest_lerp(int fps, const float* ref, int* advOut, int* endsOut) {
    memset(g_chsTestTab, 0, sizeof g_chsTestTab);
    ChsFake f; memset(&f, 0, sizeof f); f.lcg = 20260921u;
    void* self = (void*)(uintptr_t)0x04A51230u;
    int adv = 0, ends = 0;
    bool ok = true;
    for (unsigned frame = CHS_TEST_FIRST; frame < CHS_TEST_FIRST + CHS_TEST_FRAMES; ++frame) {
        float out[FL_CHS_OUTS] = {0, 0, 0, 0};
        FlChassisEnt* e = NULL; int did = -1;
        const char r = fl_chs_gate(g_chsTestTab, self, out, frame, fps, 30, FL_CHS_MODE_LERP,
                                   (unsigned)(2 * fps), chs_fake_orig, &f, &e, &did);
        if (!r || !e) { ok = false; break; }
        if (did == FL_CHS_DID_ADVANCE) ++adv;
        unsigned num = (unsigned)(((unsigned long long)frame * 30u) % (unsigned)fps) + 30u;
        const bool atEnd = num >= (unsigned)fps;
        for (int k = 0; k < FL_CHS_OUTS; ++k) {
            const float lo = e->prev[k] < e->out[k] ? e->prev[k] : e->out[k];
            const float hi = e->prev[k] < e->out[k] ? e->out[k] : e->prev[k];
            if (atEnd) { if (memcmp(&out[k], &e->out[k], sizeof(float)) != 0) ok = false; }
            else if (out[k] < lo || out[k] > hi) ok = false;
            if (memcmp(&out[k], &e->shown[k], sizeof(float)) != 0) ok = false;   // shown 必须就是交出去的值
        }
        if (atEnd) {
            if (ends < CHS_TEST_FRAMES && memcmp(&out[0], &ref[ends], sizeof(float)) != 0) ok = false;
            ++ends;
        }
    }
    *advOut = adv; *endsOut = ends;
    return ok;
}

extern "C" __declspec(dllexport) int __stdcall FrameLabSelfTestChassis() {
    static float ref[CHS_TEST_FRAMES], got[CHS_TEST_FRAMES];
    long holds = 0; int maxRun = 0, dbl = 0, mism = 0;
    int mask = 0;

    // 0x01 直通
    const int adv30 = chs_selftest_run(30, 1, ref, &holds, &maxRun, &dbl, &mism);
    const bool ok30 = (adv30 == CHS_TEST_FRAMES) && holds == 0;
    if (ok30) mask |= 0x01;
    FL_INFO("车身门自检 目标30:放行 %d 回放 %ld(应 %d / 0)⇒ %s", adv30, holds, CHS_TEST_FRAMES, ok30 ? "通过" : "★失败");

    // 0x02 / 0x04 整数倍
    const int fpsInt[2] = {60, 90};
    for (int t = 0; t < 2; ++t) {
        const int fps = fpsInt[t];
        const int adv = chs_selftest_run(fps, 1, got, &holds, &maxRun, &dbl, &mism);
        const int wantAdv = CHS_TEST_FRAMES * 30 / fps;
        const bool same = adv <= CHS_TEST_FRAMES && memcmp(ref, got, sizeof(float) * (size_t)adv) == 0;
        const bool ok = adv == wantAdv && holds == CHS_TEST_FRAMES - wantAdv && same && !dbl && !mism
                        && maxRun == fps / 30 - 1;
        if (ok) mask |= (t == 0 ? 0x02 : 0x04);
        FL_INFO("车身门自检 目标%d:放行 %d 回放 %ld(应 %d / %d),位姿序列与 30 帧%s,最长连续回放 %d,"
                "同虚拟帧重复放行=%d 回放值不符=%d ⇒ %s",
                fps, adv, holds, wantAdv, CHS_TEST_FRAMES - wantAdv, same ? "逐位相同" : "**不同**",
                maxRun, dbl, mism, ok ? "通过" : "★失败");
    }

    // 0x08 非整数倍
    bool okFrac = true;
    const int fpsFrac[2] = {45, 75};
    for (int t = 0; t < 2; ++t) {
        const int fps = fpsFrac[t];
        const int adv = chs_selftest_run(fps, 1, got, &holds, &maxRun, &dbl, &mism);
        // ★ 期望值**不许**调 fl_chs_vframe 来算:故障注入实测(2026-09-21)把 fl_chs_vframe 改坏成
        //   「恒等于显示帧号」之后,这一位照样通过 —— 因为期望值和被测值出自同一个函数,一起错。
        //   判据必须有独立来源:这里用自己的 64 位整数算术重算「区间内有几个不同的虚拟帧」。
        const unsigned long long vLast  = (unsigned long long)(CHS_TEST_FIRST + CHS_TEST_FRAMES - 1) * 30u / (unsigned)fps;
        const unsigned long long vFirst = (unsigned long long)CHS_TEST_FIRST * 30u / (unsigned)fps;
        const int wantAdv = (int)(vLast - vFirst) + 1;
        const bool same = adv <= CHS_TEST_FRAMES && memcmp(ref, got, sizeof(float) * (size_t)adv) == 0;
        const bool ok = adv == wantAdv && same && !dbl && !mism && maxRun <= (fps + 29) / 30;
        if (!ok) okFrac = false;
        FL_INFO("车身门自检 目标%d(非整数倍):放行 %d(应 %d)回放 %ld,位姿序列%s,最长连续回放 %d ⇒ %s",
                fps, adv, wantAdv, holds, same ? "逐位相同" : "**不同**", maxRun, ok ? "通过" : "★失败");
    }
    if (okFrac) mask |= 0x08;

    // 0x10 只开探针(门关):每帧放行;且按「90 帧应放行 100 次」这条判据它必须不通过
    const int advOff = chs_selftest_run(90, 0, got, &holds, &maxRun, &dbl, &mism);
    const bool okOff = advOff == CHS_TEST_FRAMES && holds == 0 && advOff != CHS_TEST_FRAMES * 30 / 90;
    if (okOff) mask |= 0x10;
    FL_INFO("车身门自检 门关@90:放行 %d 回放 %ld(应 %d / 0;按修好的判据应为 %d ⇒ 闸门%s)⇒ %s",
            advOff, holds, CHS_TEST_FRAMES, CHS_TEST_FRAMES * 30 / 90,
            advOff != CHS_TEST_FRAMES * 30 / 90 ? "会红,合格" : "★不会红", okOff ? "通过" : "★失败");

    // 0x20 原函数返回 0
    {
        memset(g_chsTestTab, 0, sizeof g_chsTestTab);
        ChsFake f; memset(&f, 0, sizeof f); f.lcg = 7u; f.returnZero = 1;
        void* self = (void*)(uintptr_t)0x04B00040u;
        float out[FL_CHS_OUTS] = {9, 9, 9, 9};
        FlChassisEnt* e = NULL; int did = -1;
        const char r0 = fl_chs_gate(g_chsTestTab, self, out, 3000, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        const bool a = (r0 == 0) && did == FL_CHS_DID_ZERO && e && !e->hasOut && e->advances == 0;
        const char r1 = fl_chs_gate(g_chsTestTab, self, out, 3001, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        const bool b = (r1 == 0) && did == FL_CHS_DID_ZERO;            // 同一虚拟帧也不许「回放」出一个不存在的姿态
        f.returnZero = 0;
        const char r2 = fl_chs_gate(g_chsTestTab, self, out, 3002, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        const bool c = (r2 == 1) && did == FL_CHS_DID_ADVANCE && e && e->hasOut;
        if (a && b && c) mask |= 0x20;
        FL_INFO("车身门自检 原函数返回0:%s/%s/%s ⇒ %s", a ? "不缓存" : "★", b ? "不回放" : "★", c ? "恢复放行" : "★",
                (a && b && c) ? "通过" : "★失败");
    }

    // 0x40 表满直通 + 陈旧回收
    {
        memset(g_chsTestTab, 0, sizeof g_chsTestTab);
        ChsFake f; memset(&f, 0, sizeof f); f.lcg = 11u;
        int noSlot = 0, bad = 0;
        for (unsigned k = 0; k < 5000; ++k) {
            float out[FL_CHS_OUTS] = {0, 0, 0, 0};
            FlChassisEnt* e = NULL; int did = -1;
            void* self = (void*)(uintptr_t)(0x05000000u + k * 0x140u);
            const char r = fl_chs_gate(g_chsTestTab, self, out, 5000, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
            if (!e) ++noSlot;
            if (r != 1 || did != FL_CHS_DID_ADVANCE) ++bad;               // 无论有没有格子,第一次见都必须放行
        }
        int reclaimFail = 0;
        for (unsigned k = 0; k < 100; ++k) {                              // 181 帧之后全部陈旧 ⇒ 新车必能占到格子
            float out[FL_CHS_OUTS] = {0, 0, 0, 0};
            FlChassisEnt* e = NULL; int did = -1;
            void* self = (void*)(uintptr_t)(0x06000000u + k * 0x140u);
            fl_chs_gate(g_chsTestTab, self, out, 5000 + 181, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
            if (!e) ++reclaimFail;
        }
        const bool ok = noSlot > 0 && bad == 0 && reclaimFail == 0;
        if (ok) mask |= 0x40;
        FL_INFO("车身门自检 表满:5000 辆里 %d 辆没格子(应 >0,全部直通放行,异常 %d);陈旧后 100 辆新车占格失败 %d ⇒ %s",
                noSlot, bad, reclaimFail, ok ? "通过" : "★失败");
    }

    // 0x80 陈旧重置
    {
        memset(g_chsTestTab, 0, sizeof g_chsTestTab);
        ChsFake f; memset(&f, 0, sizeof f); f.lcg = 13u;
        void* self = (void*)(uintptr_t)0x04C00080u;
        float out[FL_CHS_OUTS] = {0, 0, 0, 0};
        FlChassisEnt* e = NULL; int did = -1;
        fl_chs_gate(g_chsTestTab, self, out, 9000, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        fl_chs_gate(g_chsTestTab, self, out, 9001, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        const bool held = (did == FL_CHS_DID_HOLD) && e && e->calls == 2;
        fl_chs_gate(g_chsTestTab, self, out, 9001 + 181, 90, 30, 1, 180, chs_fake_orig, &f, &e, &did);
        const bool reset = (did == FL_CHS_DID_ADVANCE) && e && e->calls == 1 && e->holds == 0;
        if (held && reset) mask |= 0x80;
        FL_INFO("车身门自检 陈旧重置:%s / %s ⇒ %s", held ? "先回放过" : "★", reset ? "回来时被重置并放行" : "★",
                (held && reset) ? "通过" : "★失败");
    }

    // 0x100 插值模式
    {
        bool okAll = true;
        const int fpsL[4] = {60, 90, 45, 75};
        for (int t = 0; t < 4; ++t) {
            const int fps = fpsL[t];
            int adv = 0, ends = 0;
            const bool ok = chs_selftest_lerp(fps, ref, &adv, &ends);
            const unsigned long long vLast  = (unsigned long long)(CHS_TEST_FIRST + CHS_TEST_FRAMES - 1) * 30u / (unsigned)fps;
            const unsigned long long vFirst = (unsigned long long)CHS_TEST_FIRST * 30u / (unsigned)fps;
            const int wantAdv = (int)(vLast - vFirst) + 1;
            const bool good = ok && adv == wantAdv && ends >= wantAdv - 1;
            if (!good) okAll = false;
            FL_INFO("车身门自检 插值@%d:放行 %d(应 %d),相位=1 的显示帧 %d 个,端点逐位相同且中间值不越界=%d ⇒ %s",
                    fps, adv, wantAdv, ends, ok ? 1 : 0, good ? "通过" : "★失败");
        }
        if (okAll) mask |= 0x100;
    }

    // 0x200 插值模式的两个边界
    {
        // ① 目标 30:直通,逐位
        memset(g_chsTestTab, 0, sizeof g_chsTestTab);
        ChsFake f; memset(&f, 0, sizeof f); f.lcg = 20260921u;
        void* self = (void*)(uintptr_t)0x04A51230u;
        bool pass30 = true;
        for (unsigned frame = CHS_TEST_FIRST; frame < CHS_TEST_FIRST + 60; ++frame) {
            float out[FL_CHS_OUTS] = {0, 0, 0, 0};
            int did = -1;
            fl_chs_gate(g_chsTestTab, self, out, frame, 30, 30, FL_CHS_MODE_LERP, 60, chs_fake_orig, &f, NULL, &did);
            if (did != FL_CHS_DID_ADVANCE || memcmp(&out[0], &ref[frame - CHS_TEST_FIRST], sizeof(float)) != 0) pass30 = false;
        }
        // ② 位姿不相邻:90 帧下先正常跑 6 帧,再空 30 帧(车不在屏幕里),回来的第一帧相位是 1/3,
        //    但上一次位姿已经过期 ⇒ 输出必须**等于**这一次的引擎位姿,不许从过期位姿插过来。
        memset(g_chsTestTab, 0, sizeof g_chsTestTab);
        ChsFake g; memset(&g, 0, sizeof g); g.lcg = 5u;
        float out[FL_CHS_OUTS] = {0, 0, 0, 0};
        FlChassisEnt* e = NULL; int did = -1;
        for (unsigned frame = 1200; frame < 1206; ++frame)
            fl_chs_gate(g_chsTestTab, self, out, frame, 90, 30, FL_CHS_MODE_LERP, 180, chs_fake_orig, &g, &e, &did);
        fl_chs_gate(g_chsTestTab, self, out, 1236, 90, 30, FL_CHS_MODE_LERP, 180, chs_fake_orig, &g, &e, &did);
        const bool noStale = did == FL_CHS_DID_ADVANCE && e
                             && memcmp(out, e->out, sizeof out) == 0 && memcmp(e->prev, e->out, sizeof out) == 0;
        if (pass30 && noStale) mask |= 0x200;
        FL_INFO("车身门自检 插值边界:目标30直通=%d 过期位姿不当起点=%d ⇒ %s", pass30 ? 1 : 0, noStale ? 1 : 0,
                (pass30 && noStale) ? "通过" : "★失败");
    }

    FL_INFO("车身门自检结果掩码 = 0x%03X(全通过应为 0x3FF)", mask);
    return mask;
}

// ── ★★ 世界坐标跟踪(第十一批·续)──────────────────────────────────────────────
// 把 memscan 找到的地址填进黑匣子的**坐标通道 0**(见 Rec.trk 的注释),这样它就会
// 逐渲染帧记下那个地址上的三个 float。**这是「移动时上下抖动」的唯一直接读数**:
// 时间轴量具看不见位置,动画分数也看不见位置。
//
// 用法(两段式,因为地址每次运行都会变):
//   ① `flctl memscan <pid>` —— 扫出「正在移动的三个相邻 float」的地址
//      (实测拿到过 `(2449.5085,-0.7984,0.6022)` 这种明显是地图坐标、且 x 在动的三元组)
//   ② `flctl track <addr> <pid>` —— 把挑中的地址交给黑匣子
//   ③ `flctl blackbox 1` → 等 → `flctl blackboxdump` → `ring_analyze.py` 看第 8 节
//
// addr = 0 表示清空跟踪。返回 0 = 成功,-1 = 地址不像用户态地址(拒绝,免得热路径裸读崩)。
extern "C" __declspec(dllexport) int __stdcall FrameLabTrack(unsigned addr) {
    if (addr && (addr <= 0x10000u || addr >= 0x7FFF0000u)) {
        FL_ERR("坐标跟踪:地址 %08X 不像用户态地址,拒绝", addr);
        return -1;
    }
    // 真正的实现和日志在 bb::set_track_addr 里 —— g_trkAddr 是 bb 的 static,
    // 而本函数在全局作用域、位置又早于 bb 的完整定义,所以必须走 setter。
    return bb::set_track_addr(addr);
}

// ── ★★ 世界坐标扫描(只读诊断;2026-09-18 第十一批·续)──────────────────────────
// 为什么需要:用户报「载具移动时上下抖动」,并且明确回答了两个分流问题 ——
//   「原版轻微、装补丁后明显加剧」+「**移动时抖,静止时不抖**」。
//   ⇒ 问题在**位置**这条路径上,不在动画混合上(动画在静止时也在播)。
//   可本项目**没有任何能读「单位世界坐标」的量具**:黑匣子读的是时间轴(呈现时刻)和
//   动画分数,两样都看不见位置。而实测这三样在 30 帧下与原版**逐位一致**
//   (`frac` 0.5×256+1.0×256、逻辑增量 +0/+1 各半、时钟恒 +33)⇒ 差异不在这三样里。
//
//   ⇒ 载具的世界坐标就在进程内存里,但没人知道它挂在哪个对象上。**与其逆向整个对象图,
//     不如直接扫。** 这一条命令就能把「移动中的单位」从几百 MB 里筛出来。
//
// 判据(把「移动中的单位」从海量内存里筛出来):
//   ① 三个**相邻** float(x,y,z):x/y 在 ±20000 内、z 在 0..2000 内
//   ② 三个值都**不是整数**(坐标是连续量;整数多半是计数器/ID/句柄)
//   ③ ★**在 1 秒内变化过**,且至少两个分量在变 —— 这就是「移动中」的定义
//   ④ 每分量变化 < 50/秒 —— 排除随机噪声、指针、以及别的进程活动
//
// 返回:找到的「移动中的坐标」个数(0 = 这一局没有单位在动,或判据太严)。
//
// ★ 单参数 + 固定容量,理由同 FrameLabAnimSlots(flctl 用 CreateRemoteThread,只能传一个值)。
#define FL_MEMSCAN_CAP   16384    // ★ 4096 会静默截断:实测一次扫描的「持续移动」候选有 18 个,
                              //   加上「静态地图尺度候选」16 行之后,4096 字节的缓冲被写满,
                              //   anim_slots_append 直接静默丢弃 ⇒ 报告里只剩「RESULT=18」
                              //   而一行候选都没有,看起来像「没找到」。加大到 16384。
#define FL_MEMSCAN_MAX   60000     // 候选上限(60000×16 = 960KB,可接受;20000 会撞满)
#define FL_MEMSCAN_SHOW  40        // 最多打印多少个
extern "C" __declspec(dllexport) int __stdcall FrameLabMemScan(char* buf) {
    if (!buf) return -1;
    const int cap = FL_MEMSCAN_CAP;
    int used = 0;
    anim_slots_append(buf, cap, &used,
                      "世界坐标扫描:找「移动中的单位」(三个相邻 float,1 秒内变化)\r\n");

    // 候选数组 + 第一轮的值。**用 VirtualAlloc 一次性分配,不做动态增长** ——
    // 这是注入进游戏进程的 DLL,越简单越好,而且这两个数组要在两次扫描之间存活。
    static unsigned* candAddr = NULL;    // 每个候选的地址
    static float*    candVal  = NULL;    // 每个候选第一轮读到的 (x,y,z)
    if (!candAddr) {
        candAddr = (unsigned*)VirtualAlloc(NULL, (SIZE_T)FL_MEMSCAN_MAX * sizeof(unsigned),
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        candVal  = (float*)VirtualAlloc(NULL, (SIZE_T)FL_MEMSCAN_MAX * 3 * sizeof(float),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!candAddr || !candVal) {
        anim_slots_append(buf, cap, &used, "  候选数组分配失败(错误 %lu)\r\n", GetLastError());
        return -1;
    }

    // ── 第一轮:枚举可写内存,收集候选 ────────────────────────────────────────
    unsigned nCand = 0;
    unsigned regions = 0;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char* p = (unsigned char*)0x00010000;
    const unsigned char* pEnd = (unsigned char*)0x7FFF0000;
    while (p < pEnd && VirtualQuery(p, &mbi, sizeof mbi)) {
        const unsigned char* base = (const unsigned char*)mbi.BaseAddress;
        const SIZE_T size = mbi.RegionSize;
        const bool writable = (mbi.State == MEM_COMMIT) &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
             mbi.Protect == PAGE_EXECUTE_READWRITE);
        if (writable && size >= 12) {
            ++regions;
            for (const unsigned char* q = base; q + 12 <= base + size; q += 4) {
                const float x = *(const float*)(q + 0);
                const float y = *(const float*)(q + 4);
                const float z = *(const float*)(q + 8);
                if (!(x > -3000.0f && x < 3000.0f)) continue;   // 红警3 地图坐标的量级
                if (!(y > -3000.0f && y < 3000.0f)) continue;
                if (!(z > 0.5f && z < 500.0f)) continue;        // 单位高度:贴地但不会贴到 0
                // ★★ 判据必须**严**,否则候选会撞上限 —— 第一版就是这样:20000 个全满,
                //   而里面绝大多数根本不是坐标。三条收紧(第一版的漏洞都记在这里):
                //   ① 离整数足够远。第一版写的是 `x == (float)(int)x`,以为够了 ——
                //      **不够**:次正规数(如 1e-40,打印出来是 0.0000)与 0.0f **不相等**,
                //      于是海量「其实是 0」的位置全部通过了。改用 1e-3 容差。
                //   ② 离 0 足够远(排除空槽/未初始化/刚被清零)。
                //   ③ z 不接近 0(载具贴地但不会贴到 0)。
                if (x - (float)(int)x < 0.001f && (float)(int)x - x < 0.001f) continue;
                if (y - (float)(int)y < 0.001f && (float)(int)y - y < 0.001f) continue;
                if (z - (float)(int)z < 0.001f && (float)(int)z - z < 0.001f) continue;
                if (x < 0.001f && x > -0.001f) continue;
                if (y < 0.001f && y > -0.001f) continue;
                if (nCand < FL_MEMSCAN_MAX) {
                    candAddr[nCand] = (unsigned)(uintptr_t)q;
                    candVal[nCand * 3 + 0] = x;
                    candVal[nCand * 3 + 1] = y;
                    candVal[nCand * 3 + 2] = z;
                    ++nCand;
                }
            }
        }
        p = (unsigned char*)base + size;
    }
    anim_slots_append(buf, cap, &used,
                      "  扫了 %u 个可写区域,第一轮候选 %u 个(上限 %u)\r\n",
                      regions, nCand, (unsigned)FL_MEMSCAN_MAX);

    // ── ★ 诊断补充:把「静态但像坐标」的也倒出来 ─────────────────────────────
    //   为什么需要它:实测连续 8 次扫描都找到了一堆「移动中」的候选,但**全部**是
    //   「一个标量 + 一个单位向量」(y/z 跨度正好 ±1、模长 ≈ 1),**没有一个**同时满足
    //   「两个分量是地图尺度」。这留下一个必须回答的问题:
    //     是这一局没有单位在动,还是**红警3 的单位坐标根本不是「三个相邻 float」**
    //     (比如带 padding、或者是 Coord3D 的别的布局)?
    //   ⇒ 把「静态但至少两个分量是地图尺度」的倒出来就能回答:
    //     如果**连静态的都没有**,那布局假设就是错的 —— 该去逆向对象,而不是继续扫。
    //   ★必须放在**第二轮压紧之前** —— 第二轮会把数组前面覆盖掉。
    // ★ 2026-09-21 订正(悬挂路径会话):下面这三个「静态兜底」用的变量原本声明在**更靠后**的位置
    //   (Sleep(4000) 之前),而本块里已经在用它们 ⇒ C2065 未声明的标识符,整个 DLL 编不过。
    //   那是 2026-09-20 11:18 的最后一次源码改动,之后没有再构建过(build\Ra3FrameLab.dll 停在 11:17),
    //   所以一直没暴露。把声明挪到首次使用之前;并且每次调用先清零 —— 它们是 static,
    //   不清的话上一次扫描留下的(可能早已释放的)地址会混进这一次的兜底候选里。
    static unsigned staticAddr[64];
    static float    staticVal[64 * 3];
    static unsigned nStatic = 0;
    nStatic = 0;
    {
        int shown = 0;
        anim_slots_append(buf, cap, &used,
                          "  静态地图尺度候选(至少两个分量 |v|>10):\r\n");
        for (unsigned i = 0; i < nCand && shown < 16; ++i) {
            const float* f = (const float*)(uintptr_t)candAddr[i];
            const float x = f[0], y = f[1], z = f[2];
            int big = 0;
            if (x > 10.0f || x < -10.0f) ++big;
            if (y > 10.0f || y < -10.0f) ++big;
            if (z > 10.0f || z < -10.0f) ++big;
            if (big < 2) continue;
            anim_slots_append(buf, cap, &used,
                              "  %08X  (%10.4f,%10.4f,%10.4f)\r\n",
                              candAddr[i], x, y, z);
            // ★ 2026-09-20:顺手存下静态候选(见第三轮后的「静态兜底」)。
            if (nStatic < 64) {
                staticAddr[nStatic] = candAddr[i];
                staticVal[nStatic * 3 + 0] = x;
                staticVal[nStatic * 3 + 1] = y;
                staticVal[nStatic * 3 + 2] = z;
                ++nStatic;
            }
            ++shown;
        }
        anim_slots_append(buf, cap, &used, "  (只列前 16 个)\r\n");
    }

    // ── 第二轮:等 4 秒后重读,**只筛不输出** ─────────────────────────────────
    //   ★★ 间隔从 1 秒改成 4 秒 —— 这是本批最重要的一个参数修正。
    //   实测:间隔 1 秒时能筛出 18~30 个「持续移动」的候选,但把它们交给黑匣子跟踪之后,
    //   **三个分量全读成 0**(地址已被释放),或者在记录窗口里出现「从 0 跳到 1755」。
    //   ⇒ 那些候选是**每帧重算的临时缓冲区**(路径点/插值 scratch),**寿命只有几秒**。
    //   1 秒的窗口根本筛不掉它们 —— 它们那时还活着。
    //   黑匣子要留 512 帧(约 17 秒),所以**地址必须至少活这么久**;
    //   把间隔拉到 4 秒,短命的临时缓冲区就会在第二轮变成 0 而被排除。
    //   (再长会更准,但 memscan 每次调用都会变慢,而探针最多重试 8 次。)
    // ★★★ 2026-09-20:静态兜底。
    //   为什么:本会话多次遇到「memscan 一个候选都没找到」⇒ 探针 exit 6 ⇒
    //   **连动画轴也一起拿不到**(因为黑匣子还没开)。而「位置」这一半一旦缺失,
    //   「同时录动画轴 + 位置」就永远做不成。
    //   ⇒ 第一轮里那些「静态地图尺度候选」(至少两个分量 |v|>10)已经是很强的坐标形状证据,
    //     把它们存下来;第三轮若一个「移动中」的都没有,就退而用静态里 **X 最大**的那个。
    //   注意:这**不是**「找到了移动中的单位」,只是「给黑匣子一个像坐标的地址去记」——
    //   所以日志里会明确标注 `FALLBACK=static`,不许当成正常结果。
    //   (staticAddr / staticVal / nStatic 的声明已挪到上面「静态地图尺度候选」那一块之前,见那里的订正。)
    Sleep(4000);
    //   ★为什么要「两段确认」(第三轮):实测第一版挑中的第一个候选,在 memscan 的
    //     1 秒窗口里确实动过,但真正 dump 时它的 **X 分量恒定不变**(-17.4896)——
    //     那是一次性写入(对象刚被初始化/被拷贝),不是「持续移动」。
    //     ⇒ 只有在**两个窗口里都在动**的,才算「移动中的单位」。
    unsigned nPass = 0;
    for (unsigned i = 0; i < nCand; ++i) {
        const float* f = (const float*)(uintptr_t)candAddr[i];
        const float x2 = f[0], y2 = f[1], z2 = f[2];
        const float dx = x2 - candVal[i * 3 + 0];
        const float dy = y2 - candVal[i * 3 + 1];
        const float dz = z2 - candVal[i * 3 + 2];
        const float ax = dx < 0 ? -dx : dx;
        const float ay = dy < 0 ? -dy : dy;
        const float az = dz < 0 ? -dz : dz;
        // 判据(与第三轮共用同一条式子):
        //   ① 任何分量跳变 > 5 ⇒ 不是移动(是内存被重新写入 / 对象被换掉)
        //   ② 「至少两个分量在动」。★不能要求「三个都动」—— 载具在**平地**行驶时
        //      高度几乎不变,z 分量不动就会被整条丢掉(第二版就是这么错的:实测 0 个)。
        //      排除「空槽被写入」那类假阳性的责任交给**第一轮**(离 0 / 离整数足够远)。
        //   ③ 每个动起来的分量变化都要 ≥ 0.01。
        // ★★★ 「跳变」的阈值必须按**地图尺度**定,不能按直觉定。
        //   我一开始写的是 5.0(以为「单位每秒走几个单位」),结果把**所有真单位**都扔了:
        //   红警3 的单位速度是**几十到几百单位/秒**,1 秒的采样间隔里位移轻松超过 5
        //   ⇒ 真坐标被判成「跳变」排除,反而留下「标量 + 单位向量」那种变化慢的假阳性。
        //   实测证据:把「静态地图尺度候选」打出来后,里面**明明有** `(2032.8, 522.0, 1.5708)`
        //   和 `(-28.0, 1018.7, 280.4)` 这样标准的坐标三元组,却一个都没进「移动中」的列表。
        //   ⇒ 放宽到 500。真正的跳变(内存被重新写入、对象被换掉)会远大于这个数。
        if (ax > 500.0f || ay > 500.0f || az > 500.0f) continue;
        int moved = 0;
        if (ax > 0.01f) ++moved;
        if (ay > 0.01f) ++moved;
        if (az > 0.01f) ++moved;
        if (moved < 2) continue;
        // 压紧到数组前面(nPass <= i,所以原地覆盖安全),并把**这一轮**的值
        // 存成第三轮的基准。
        candAddr[nPass] = candAddr[i];
        candVal[nPass * 3 + 0] = x2;
        candVal[nPass * 3 + 1] = y2;
        candVal[nPass * 3 + 2] = z2;
        ++nPass;
    }
    anim_slots_append(buf, cap, &used,
                      "  第二轮(第一个 1 秒窗口内动过) %u 个\r\n", nPass);

    // ── 第三轮:再等 4 秒,只留「还在动」的 ⇒ 这才叫「持续移动」 ──────────────
    Sleep(4000);
    int found = 0;
    for (unsigned i = 0; i < nPass && found < FL_MEMSCAN_SHOW; ++i) {
        const float* f = (const float*)(uintptr_t)candAddr[i];
        const float x3 = f[0], y3 = f[1], z3 = f[2];
        const float dx = x3 - candVal[i * 3 + 0];
        const float dy = y3 - candVal[i * 3 + 1];
        const float dz = z3 - candVal[i * 3 + 2];
        const float ax = dx < 0 ? -dx : dx;
        const float ay = dy < 0 ? -dy : dy;
        const float az = dz < 0 ? -dz : dz;
        // ★★★ 「跳变」的阈值必须按**地图尺度**定,不能按直觉定。
        //   我一开始写的是 5.0(以为「单位每秒走几个单位」),结果把**所有真单位**都扔了:
        //   红警3 的单位速度是**几十到几百单位/秒**,1 秒的采样间隔里位移轻松超过 5
        //   ⇒ 真坐标被判成「跳变」排除,反而留下「标量 + 单位向量」那种变化慢的假阳性。
        //   实测证据:把「静态地图尺度候选」打出来后,里面**明明有** `(2032.8, 522.0, 1.5708)`
        //   和 `(-28.0, 1018.7, 280.4)` 这样标准的坐标三元组,却一个都没进「移动中」的列表。
        //   ⇒ 放宽到 500。真正的跳变(内存被重新写入、对象被换掉)会远大于这个数。
        if (ax > 500.0f || ay > 500.0f || az > 500.0f) continue;
        int moved = 0;
        if (ax > 0.01f) ++moved;
        if (ay > 0.01f) ++moved;
        if (az > 0.01f) ++moved;
        if (moved < 2) continue;
        anim_slots_append(buf, cap, &used,
                          "  %08X  pass2(%9.4f,%9.4f,%9.4f) -> pass3(%9.4f,%9.4f,%9.4f)"
                          "  d2=(%.4f,%.4f,%.4f)\r\n",
                          candAddr[i],
                          candVal[i * 3 + 0], candVal[i * 3 + 1], candVal[i * 3 + 2],
                          x3, y3, z3, dx, dy, dz);
        ++found;
    }
    anim_slots_append(buf, cap, &used,
                      "  ★持续移动的坐标 %d 个(两个 1 秒窗口都在动;只列前 %d 个)\r\n",
                      found, (int)FL_MEMSCAN_SHOW);

    // ★★★ 2026-09-20:静态兜底(见函数开头 staticAddr 的注释)。
    //   若一个「移动中」的都没有,就退而用静态候选里 X 最大的那个,并**明确标注**。
    //   这样「同时录动画轴 + 位置」不会再因为 memscan 空手而归而整个失败。
    if (found == 0 && nStatic > 0) {
        unsigned bi = 0;
        float bx = -1.0f;
        for (unsigned i = 0; i < nStatic; ++i) {
            const float v = staticVal[i * 3 + 0];
            const float a = v < 0 ? -v : v;
            if (a > bx) { bx = a; bi = i; }
        }
        anim_slots_append(buf, cap, &used,
                          "  FALLBACK=static  0x%08X  (%10.4f,%10.4f,%10.4f)"
                          "  <-- 没找到移动中的,退而用静态候选(X 最大)\r\n",
                          staticAddr[bi], staticVal[bi * 3 + 0],
                          staticVal[bi * 3 + 1], staticVal[bi * 3 + 2]);
        return 1;                       // 1 个,但**不是**「移动中」——上层看 FALLBACK 标记
    }
    return found;
}

// ★动画标尺的核心读数。盯 logicFrames 个**逻辑帧**,量每个记录到的动画对象的「当前帧」推进了多少。
// 返回:所有对象里推进最多的那个的「每逻辑帧推进帧数 ×1000」;采不到东西返回 -1。
//
// 判据(必须先说清楚,免得又拿容易测的替换要判的):
//   同一局录像、同样长度的逻辑帧区间内,配置 A 与配置 B 的这个读数**应当相等**。
//   若 B 是 A 的 3 倍 ⇒ 该配置下动画确实快 3 倍。这是「动画快不快」的客观代理,
//   但它只是代理:最终仍以人眼看到的步频/建造动画为准。
// 为什么按逻辑帧归一:逻辑帧率在两种配置下都是 15(已验证),所以「每逻辑帧推进多少帧」
//   就是不受渲染帧率影响的**不变量** —— 原版与正确的高帧率配置下它必须一样。
extern "C" __declspec(dllexport) int __stdcall FrameLabAnimRate(int logicFrames) {
    if (logicFrames <= 0 || logicFrames > 1800) logicFrames = 150;   // 150 逻辑帧 = 10 秒
    const int f0 = FrameLabLogicFrame();
    if (f0 < 0) { FL_ERR("动画标尺:取不到逻辑帧(还没进对局?)"); return -1; }

    float  prev[FL_ANIM_OWNERS];
    double acc[FL_ANIM_OWNERS];
    long   cnt[FL_ANIM_OWNERS];
    int    live = 0;
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
        prev[i] = fl::g_animFrame[i] ? *fl::g_animFrame[i] : 0.0f;
        acc[i] = 0.0; cnt[i] = 0;
        if (fl::g_animOwner[i]) ++live;
    }
    FL_INFO("动画标尺:开始,盯 %d 个对象(探针累计走过 %ld 次),量 %d 个逻辑帧",
            live, fl::g_animCalls, logicFrames);

    const DWORD t0 = GetTickCount();
    const long  r0 = fl::g_frameCount;
    int cur = f0;
    while (cur - f0 < logicFrames && (GetTickCount() - t0) < 120000) {
        Sleep(2);                                   // 约 500 Hz 采样:足够密,不会漏掉整圈动画
        for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
            float* fp = fl::g_animFrame[i];
            if (!fp || !mem::read_ok(fp, 4)) continue;
            const float v = *fp;
            const float d = v - prev[i];
            // 只累加「向前推进」的样本。动画循环到头会回绕(给出负值),掉帧会给出巨大跳变;
            // 这两种整帧丢掉 —— 宁可少算一点,也不要把一次回绕算成一次巨大的推进。
            if (d >= 0.0f && d < 500.0f) { acc[i] += (double)d; ++cnt[i]; }
            prev[i] = v;
        }
        cur = FrameLabLogicFrame();
        if (cur < 0) break;
    }
    const long done = cur - f0;
    const long rend = fl::g_frameCount - r0;
    const DWORD elapsed = GetTickCount() - t0;
    FL_INFO("动画标尺:区间结束 —— %ld 个逻辑帧 / %ld 个渲染帧(逻辑 %.2f Hz,渲染 %.2f Hz)",
            done, rend,
            elapsed ? (double)done * 1000.0 / (double)elapsed : 0.0,
            elapsed ? (double)rend * 1000.0 / (double)elapsed : 0.0);

    int best = -1;
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
        if (!fl::g_animOwner[i]) continue;
        FL_INFO("  对象 %08X 被见 %ld 次,推进 %.3f 帧 / %ld 个采样 ⇒ 每逻辑帧 %.4f 帧",
                (unsigned)(uintptr_t)fl::g_animOwner[i], fl::g_animHit[i], acc[i], cnt[i],
                done > 0 ? acc[i] / (double)done : -1.0);
        if (acc[i] > 0.0 && (best < 0 || acc[i] > acc[best])) best = i;
    }
    if (best < 0 || done <= 0) {
        FL_ERR("动画标尺:这段区间里没有一个对象在推进(采到 0)—— 读数无效,别拿它当结论");
        return -1;
    }
    const int out = (int)(acc[best] * 1000.0 / (double)done);
    FL_INFO("★动画标尺读数:最快对象 %08X 每逻辑帧推进 %.4f 帧(×1000 = %d)",
            (unsigned)(uintptr_t)fl::g_animOwner[best], acc[best] / (double)done, out);
    return out;
}

// ★★ 2026-09-17 动画定位会话:「每逻辑帧推进多少毫秒游戏时钟」—— 本轮的**主判据**。
//
// 为什么是这把尺子(而不是动画帧数探针):
//   动画的推进量直接来自游戏时钟的增量(证据链见 FL_G_CLOCK):sub_90ECF0 里
//   `dt = 时钟 − 上次时钟`,再 `当前帧 += dt秒 × 动画帧率`。所以「时钟相对 sim 走多快」
//   就是「动画相对 sim 走多快」,而且是**与渲染帧率无关的不变量**:
//     原版(r=2,每帧 33 毫秒):每逻辑帧 = 2 × 33.33 = 66.67 毫秒
//     正确的 90 帧(r=6,每帧 11 毫秒):每逻辑帧 = 6 × 11.11 = 66.67 毫秒  ⇒ 一样
//     坏掉的 90 帧(r=6,每帧仍 33 毫秒):每逻辑帧 = 6 × 33.33 = 200 毫秒 ⇒ **3 倍**
//   它不依赖任何对象被采样到、不依赖录像播到哪一段,所以不会被场景差异污染
//   (动画帧探针在两次运行里采到的是不同对象,已被证明会误导,见 RE-动画推进点 第 12 节)。
//
// 返回:毫秒 × 1000(整数,免浮点);取不到读数返回 -1。
extern "C" __declspec(dllexport) int __stdcall FrameLabClockPerLogicFrame(int logicFrames) {
    if (logicFrames <= 0 || logicFrames > 1800) logicFrames = 150;
    // ★ 2026-09-17:入口先记一行。这个函数原来**只在结尾**写日志,于是 2026-09-17 那次实测里
    //   「日志里找不到它」被我误读成「它没被进入」—— 其实它完全可能进去了、然后崩在半路,
    //   两种情况在日志里长得一模一样。入口日志把这两种情况彻底分开。
    FL_INFO("时钟判据:进入(量 %d 个逻辑帧)", logicFrames);
    // kEngineClockVa 定义在 namespace fl 里(补丁本体那边),导出函数在外,所以要写 fl::
    const int* clock = (const int*)fl::kEngineClockVa;
    if (!mem::read_ok(clock, 4)) { FL_ERR("时钟判据:读不到时钟 %08X", (unsigned)fl::kEngineClockVa); return -1; }
    const int f0 = FrameLabLogicFrame();
    if (f0 < 0) { FL_ERR("时钟判据:取不到逻辑帧(还没进对局?)"); return -1; }
    const int c0 = *clock;

    const DWORD t0 = GetTickCount();
    int cur = f0;
    while (cur - f0 < logicFrames && (GetTickCount() - t0) < 180000) {
        Sleep(2);
        cur = FrameLabLogicFrame();
        if (cur < 0) break;
    }
    const int c1 = *clock;
    const long done = cur - f0;
    const DWORD elapsed = GetTickCount() - t0;

    if (done <= 0) { FL_ERR("时钟判据:一个逻辑帧都没走完 —— 读数无效"); return -1; }
    const int out = (int)((long long)(c1 - c0) * 1000 / done);
    FL_INFO("时钟判据:区间 %ld 个逻辑帧 / %u 毫秒墙钟;时钟走了 %d 毫秒",
            done, (unsigned)elapsed, c1 - c0);
    FL_INFO("  ⇒ 每逻辑帧推进 %d.%03d 毫秒游戏时钟(原版与正确的高帧率配置都应是 66.667;"
            "若约 200 ⇒ 时钟快 3 倍,动画就会快 3 倍)",
            out / 1000, out % 1000);
    return out;
}

// ★★★ 2026-09-17(第六批):「每逻辑帧推进多少毫秒**混合斜坡动画时间**」—— P6b 的客观判据。
//
// 为什么还需要这把尺子(而不是用 clockrate):见 FL_G_STEPPROBE 上方的完整推导。一句话 ——
//   clockrate / animrate 量的是**游戏时钟**那条路径(sub_90ECF0),而建筑建造/揭幕动画走的是
//   **另一条**路径(sub_903910 → sub_8EA220),它的时间步是 `帧号增量 ÷ 客户端帧率`。
//   除数从 30 改成 60,clockrate 与 animrate 一个数都不会变 ⇒ 看不见 P6b。
//
// 判据(与 clockrate 同源,与渲染帧率无关的不变量):
//   每逻辑帧推进的混合斜坡动画时间必须恒为 1/15 秒 = **66.67 毫秒**。
//     原版 30 帧(r=2):2 次 × dt(1/30) = 66.67 ✓
//     修好后 60 帧(r=4):4 次 × dt(1/60) = 66.67 ✓
//     没修 60 帧(r=4):4 次 × dt(1/30) = 133.33 ✗(动画时间跑一倍 ⇒ 建筑动画前一半正常后一半坏)
//
// 另外两个读数用来**验证前提假设**,避免「读错了却当成结论」:
//   · 每次调用平均 dt 应 = 1 ÷ 实际渲染帧率(60 帧 ⇒ 16667 微秒)。
//     这个数同时证明了「vtbl[116] 确实按渲染帧率递增」—— 前提错了它会露馅。
//   · 每逻辑帧调用次数应 = r(60 帧 ⇒ 4 次)。
//
// 返回:毫秒 × 1000(整数,免浮点;66667 = 正确);读数无效返回 -1。
extern "C" __declspec(dllexport) int __stdcall FrameLabBlendStepPerLogicFrame(int logicFrames) {
    if (logicFrames <= 0 || logicFrames > 1800) logicFrames = 150;   // 150 逻辑帧 = 10 秒
    // 入口先记一行(与 FrameLabClockPerLogicFrame 同一纪律):「没进这个函数」与「进去了但崩在半路」
    // 在日志里必须能分开,否则排障时会误读。
    FL_INFO("混合斜坡量尺:进入(量 %d 个逻辑帧)", logicFrames);
    if (!fl::g_stepSite) {
        FL_ERR("混合斜坡量尺:探针没装(需要改动分组含 FL_G_STEPPROBE 0x%04X)—— 读数无效",
               (unsigned)FL_G_STEPPROBE);
        return -1;
    }
    const int f0 = FrameLabLogicFrame();
    if (f0 < 0) { FL_ERR("混合斜坡量尺:取不到逻辑帧(还没进对局?)"); return -1; }

    // 归零必须发生在取 f0 **之后**:否则归零与取 f0 之间那点调用会被算进本区间却不在时间窗内。
    fl::g_stepCalls = 0;
    fl::g_stepDtSum = 0.0;
    fl::g_stepDtCnt = 0;
    fl::g_stepMaxDt = 0;

    const DWORD t0 = GetTickCount();
    const long  r0 = fl::g_frameCount;
    int cur = f0;
    while (cur - f0 < logicFrames && (GetTickCount() - t0) < 180000) {
        Sleep(2);
        cur = FrameLabLogicFrame();
        if (cur < 0) break;
    }
    const long done = cur - f0;
    const long rend = fl::g_frameCount - r0;
    const DWORD elapsed = GetTickCount() - t0;
    const long calls = fl::g_stepCalls;
    const double dtSum = fl::g_stepDtSum;

    if (done <= 0) { FL_ERR("混合斜坡量尺:一个逻辑帧都没走完 —— 读数无效"); return -1; }
    if (calls <= 0) {
        FL_ERR("混合斜坡量尺:区间内一次都没被调用(这段录像里没有建筑在建造/揭幕?)—— 读数无效");
        return -1;
    }
    // 每逻辑帧的动画时间(毫秒)。×1000 输出成整数,与 clockrate 同一套格式。
    const int out = (int)(dtSum * 1000.0 * 1000.0 / (double)done);
    // 每次调用平均 dt(微秒)。正确值 = 1e6 ÷ 实际渲染帧率。
    const int avgDtMicro = (int)(dtSum * 1000000.0 / (double)calls);
    const double callsPerLogic = (double)calls / (double)done;

    FL_INFO("混合斜坡量尺:区间 %ld 逻辑帧 / %ld 渲染帧(渲染 %.2f Hz)｜墙钟 %u 毫秒",
            done, rend, elapsed ? (double)rend * 1000.0 / (double)elapsed : 0.0, (unsigned)elapsed);
    FL_INFO("  调用 %ld 次 ⇒ 每逻辑帧 %.2f 次(应 = r)｜每次平均 dt %d 微秒(应 = 1e6÷渲染帧率)"
            "｜最大 dt %ld 微秒",
            calls, callsPerLogic, avgDtMicro, fl::g_stepMaxDt);
    FL_INFO("★混合斜坡量尺读数:每逻辑帧动画时间 %d.%03d 毫秒"
            "(正确 66.667;÷30 的老算法在 60 帧下会给 133.333)",
            out / 1000, out % 1000);
    return out;
}


// ═══════════════════════════════════════════════════════════════════════════════════════════
// ★★★ 2026-09-17(第七批):显示器帧计数器速率尺子 —— 建筑动画修复的**前提数字**。
//
//   只读,一处都不改。回答一个问题:
//     「vtbl[116](dword_CDB750) 这个帧计数器,每秒 tick 多少次?」
//     (★ 按本项目约定,`vtbl[N]` 里的 N 是 **vtable 字节偏移**,不是槽序号;116 字节 = 槽 29。)
//   · 若 ≈ 60(渲染帧率)⇒ 引擎的三个「帧率派生量」(0xCDBC50/54/5C)必须整体从 30 基准
//     改成 60 基准(开 FL_G_DERIVED2 = 0xE00),否则一切「毫秒 ↔ 帧号」换算差一倍
//     ⇒ 建筑解包动画(StructureUnpackUpdate)快一倍跑完,正是用户报的症状。
//   · 若 ≈ 30(客户端 tick 率没变)⇒ 派生量不用动,第七批的推论不成立,回头重查。
//   这两条路只能靠**实测**分开,所以先量、再改。
//
//   顺带把三个派生量与两个整数帧率全局的**当前值**打出来,并做一次自洽性检查。
//   返回:实测帧/秒(取整);读不到就返回负的错误码。
extern "C" __declspec(dllexport) int __stdcall FrameLabDisplayClock(int millis) {
    if (millis <= 0 || millis > 5000) millis = 600;      // 默认 0.6 秒,够稳又不太久
    FL_INFO("显示器帧计数器量尺:进入(量 %d 毫秒)", millis);
    if (!fl::g_base && !fl::resolve_module()) return FL_ERR_NO_MODULE;

    // ── ① 静态证据:三个派生量 + 两个整数帧率全局的当前值(只读)─────────────────────────
    const float* pFpsFloat  = (const float*)fl::kEngineFpsFloatVa;         // 帧/秒
    const float* pMsPerFrm  = (const float*)fl::kEngineMsPerFrameFloatVa;  // 毫秒/帧
    const float* pFramesPms = (const float*)fl::kEngineFramesPerMsVa;      // 帧/毫秒
    float vFps = -1.0f, vMsPerFrm = -1.0f, vFramesPms = -1.0f;
    if (mem::read_ok(pFpsFloat, 4))  vFps = *pFpsFloat;
    if (mem::read_ok(pMsPerFrm, 4))  vMsPerFrm = *pMsPerFrm;
    if (mem::read_ok(pFramesPms, 4)) vFramesPms = *pFramesPms;
    const int iClient = (fl::g_fpsClient && mem::read_ok(fl::g_fpsClient, 4)) ? *fl::g_fpsClient : -1;
    const int iLogic  = (fl::g_fpsLogic  && mem::read_ok(fl::g_fpsLogic,  4)) ? *fl::g_fpsLogic  : -1;
    FL_INFO("  静态读数:客户端帧率(整数)=%d  逻辑帧率(整数)=%d", iClient, iLogic);
    FL_INFO("            0x%08X (float)帧率 = %.4f", (unsigned)fl::kEngineFpsFloatVa, vFps);
    FL_INFO("            0x%08X 毫秒/帧     = %.4f", (unsigned)fl::kEngineMsPerFrameFloatVa, vMsPerFrm);
    FL_INFO("            0x%08X 帧/毫秒     = %.6f", (unsigned)fl::kEngineFramesPerMsVa, vFramesPms);
    // 自洽性:帧/毫秒 应当恰好等于 (float)帧率 ÷ 1000。不等 ⇒ 这两个不是同一基准。
    if (vFps > 0.0f && vFramesPms >= 0.0f) {
        const float want = vFps * 0.001f;
        FL_INFO("  自洽性:帧/毫秒 应为 (float)帧率÷1000 = %.6f,实际 %.6f ⇒ %s",
                want, vFramesPms,
                (vFramesPms > want * 0.95f && vFramesPms < want * 1.05f) ? "一致" : "★不一致");
    }

    // ── ② 动态测量:帧计数器速率 ───────────────────────────────────────────────────────
    //    ★ 三重保护(缺任何一条就只报错、绝不 call 野指针 —— 那是崩溃而不是读数):
    //      a) dword_CDB750 这个槽本身可读且非空;
    //      b) 它指向的对象可读、vtable 指针非空;
    //      c) vtable 目标槽的落点必须在**主模块映像内**。
    //    调用形式与引擎自己完全一致:
    //      (*(int (__thiscall **)(int))(*(_DWORD *)dword_CDB750 + 116))(dword_CDB750)
    //
    //    ⚠⚠ 踩过的坑(2026-09-17 第七批,第一次测量就撞上):IDA 那个 `+ 116` 是 **vtable 的
    //      字节偏移**,不是「第 116 个槽」! 我第一次写成 `vt[116]`(int 下标 ⇒ 字节偏移 464),
    //      结果取到一个野函数指针,displayclock 返回垃圾值 -17085162 —— 而游戏没崩,
    //      看起来像「量到了怪数」而不是「量错了地方」,极容易误判。
    //      本项目的既有约定也是按字节偏移记槽号的:sub_90BDA0 在 `0x00C61E10`,起点 `0x00C61DD8`,
    //      差 0x38 = 56 字节 = **槽 14**。所以「槽 14」= 字节偏移 56。⇒ 这里必须用
    //      `*(int*)((unsigned char*)vt + 116)`,绝不能写 `vt[116]`。
    const int kFrameNoSlotBytes = 116;                 // vtable 字节偏移(== 槽 29)
    int* slot = (int*)fl::kDisplayClockVa;
    if (!mem::read_ok(slot, 4) || !*slot) {
        FL_ERR("显示器帧计数器量尺:0x%08X 为空 —— 读数无效", (unsigned)fl::kDisplayClockVa);
        return -1;
    }
    void* disp = (void*)(uintptr_t)(unsigned)*slot;
    if (!mem::read_ok(disp, 4)) {
        FL_ERR("显示器帧计数器量尺:对象 %08X 读不到 —— 读数无效", (unsigned)(uintptr_t)disp);
        return -1;
    }
    unsigned char* vt = (unsigned char*)(uintptr_t)(unsigned)*(int**)disp;
    if (!vt || !mem::read_ok(vt + kFrameNoSlotBytes, 4)) {
        FL_ERR("显示器帧计数器量尺:vtable %08X 的字节偏移 %d 读不到 —— 读数无效",
               (unsigned)(uintptr_t)vt, kFrameNoSlotBytes);
        return -1;
    }
    void* fn = (void*)(uintptr_t)(unsigned)*(int*)(vt + kFrameNoSlotBytes);
    const uintptr_t imgLo = (uintptr_t)fl::g_base;
    const uintptr_t imgHi = imgLo + fl::g_imageSize;
    if ((uintptr_t)fn < imgLo || (uintptr_t)fn >= imgHi) {
        FL_ERR("显示器帧计数器量尺:vtbl+%d = %08X 不在主模块映像 [%08X,%08X) 内 —— 读数无效",
               kFrameNoSlotBytes, (unsigned)(uintptr_t)fn, (unsigned)imgLo, (unsigned)imgHi);
        return -1;
    }
    typedef int (__thiscall *FnFrameNo)(void*);
    FnFrameNo frameNo = (FnFrameNo)fn;
    FL_INFO("  帧计数器取到:对象 %08X  vtable %08X  vtbl+%d = %08X(在主模块内,已放行)",
            (unsigned)(uintptr_t)disp, (unsigned)(uintptr_t)vt,
            kFrameNoSlotBytes, (unsigned)(uintptr_t)fn);

    const int a0 = frameNo(disp);
    const DWORD t0 = GetTickCount();
    Sleep((DWORD)millis);
    const int a1 = frameNo(disp);
    const DWORD dt = GetTickCount() - t0;
    if (dt < 50) { FL_ERR("显示器帧计数器量尺:时间窗太短(%u 毫秒)—— 读数无效", (unsigned)dt); return -1; }
    const int delta  = a1 - a0;
    const int perSec = (int)((double)delta * 1000.0 / (double)dt);
    FL_INFO("  帧计数器:%d → %d(Δ%d 帧 / %u 毫秒)", a0, a1, delta, (unsigned)dt);
    FL_INFO("★显示器帧计数器读数:约 %d 帧/秒", perSec);

    // ── ③ 判据:实测速率 vs **目标帧率** ────────────────────────────────────────────────
    //    ★★ 对照物必须是 g_fpsRender(我们要求引擎跑的帧率),**不能**是 dword_CAF9D4!
    //    本补丁**故意从不写** dword_CAF9D4(它有 120 处读者),所以它永远是原版的 30。
    //    拿它当分母会得出**完全相反**的结论 —— 2026-09-17 第七批第一次实现就踩了这个坑:
    //    实测 60 ÷ 全局 30 = 2.0,而判据文字写成「计数器不跟着客户端帧率走 ⇒ 派生量不用动」,
    //    差点把唯一正确的结论读成反的。**误导性的判据比没有判据更坏。**
    const int wantFps = fl::g_fpsRender;
    // ★ 顺带把**当前分组掩码**打出来。没有它,下面 0.5 那一支会让人误以为
    //   「引擎 tick 没跟上」,而真实原因常常是「这一局压根没启用改动」(掩码里没有 R1 等)。
    //   实测教训(2026-09-17 第七批):拿 `-Groups 0x10000` 跑(只开混合斜坡量尺、其余全关),
    //   帧计数器读到 30 帧/秒,而目标写的是 60 —— 那不是引擎的错,是本局没打补丁。
    FL_INFO("  当前改动分组掩码 = 0x%04X(默认集合 FL_G_ALL = 0x%04X)",
            (unsigned)fl::g_groups, (unsigned)FL_G_ALL);
    if (wantFps > 0) {
        const double ratio = (double)perSec / (double)wantFps;
        FL_INFO("  判据:实测 %d 帧/秒 ÷ **目标帧率** %d = %.3f"
                "(整数客户端帧率全局仍是 %d —— 本补丁故意不写它,拿它当分母会读反)",
                perSec, wantFps, ratio, iClient);
        if (ratio > 0.9 && ratio < 1.1) {
            FL_INFO("      ⇒ 帧计数器跟着**实际渲染帧率**走");
            FL_INFO("      ⇒ 三个帧率派生量必须整体按帧率重算(FL_G_DERIVED2 0xE00):");
            FL_INFO("        60 帧正确值 0x00CDBC50=60.0  0x00CDBC54=16.667  0x00CDBC5C=0.06");
            FL_INFO("        否则建筑解包动画(sub_6F6CB0)的进度会按 30/60 的比例跑偏");
        } else if (ratio > 0.4 && ratio < 0.6) {
            FL_WARN("      ⚠计数器只跑到目标帧率的一半。两种可能,先看上面那行掩码:");
            FL_WARN("        ① 掩码里没有 R1(算 r)/时钟修正等 ⇒ **本局根本没把帧率提上去**,"
                    "此时保持原版 30 基准是对的;");
            FL_WARN("        ② 掩码齐全却仍只有一半 ⇒ 引擎 tick 真没跟上,那才需要回头重查。");
        } else {
            FL_WARN("      ⚠实测速率既不是目标帧率也不是它的一半(%.3f)× —— 无法判读,"
                    "可能是窗口没在呈现帧(自动化录像窗口就会这样),请在前台可见的窗口里重测",
                    ratio);
        }
        // 三个派生量与**实测**速率是否自洽(而不是与目标帧率自洽)。
        // ★ 这一条比「与目标比」更硬:它不依赖「目标有没有达到」这个前提,
        //   在「没打补丁」和「打了补丁」两种局里都成立 —— 是本尺子最可靠的判据。
        const float wantFramesPms = (float)perSec * 0.001f;
        FL_INFO("  自洽性(按**实测**速率):0x00CDBC5C 应为 %.6f,实际 %.6f ⇒ %s",
                wantFramesPms, vFramesPms,
                (vFramesPms > wantFramesPms * 0.9f && vFramesPms < wantFramesPms * 1.1f)
                    ? "一致" : "★不一致");
    }
    return perSec;
}


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

// ★★ 2026-09-17 验收口径会话(用户明确「本机 60 Hz,只需实现 90 帧」之后新增):
//   进程内直接量**引擎逻辑帧率**。这一条就是「模拟没被改」的定义 ——
//   用户的要求「90 帧且不影响动画」拆成两句可测的话就是:
//     ① 渲染帧率 = 目标(90)                       → FrameLabFpsWindow
//     ② 逻辑帧率恒为 15(任何渲染帧率下)          → 本函数
//   两者都成立时,动画相对模拟的速度必然与原版一致(动画的 dt 直接取自游戏时钟,
//   而时钟的推进量已由 clock_accumulator_step 精确摊平,见 FrameLabClockPerLogicFrame)。
//
// 为什么必须有它,而不是拿现有的东西凑:
//   1. `FrameLabEngineFps` 读的是 `g_frameObj + 0x70` —— 那是**引擎自己的平滑显示值**,
//      指数平滑 ⇒ 它滞后于真实值。实测:渲染 59.87 fps 时它报 7.81,而 59.87/6 = 9.98。
//      拿它当判据会把「平滑滞后」误判成「模拟跑慢了」。
//   2. 跨进程量(两次 `flctl logicframe` + 外部墙钟)会被 flctl 自身的进程启动开销污染:
//      每次调用约 0.2 秒,8 秒窗口下就是约 5% 的系统性低估 —— 正好落在要判的量级上。
//   ⇒ 进程内量,两端都在被测进程里取,和 FrameLabFpsWindow 同一套做法,没有这些误差。
//
// 返回:逻辑帧率 ×100(整数,免浮点跨边界);取不到逻辑帧返回 -1。
// 判据:任何帧率配置(30/60/90)下都必须 ≈ 1500。原版 = 15.00。
extern "C" __declspec(dllexport) int __stdcall FrameLabLogicFps(int seconds) {
    if (seconds <= 0 || seconds > 120) seconds = 5;
    const int f0 = FrameLabLogicFrame();
    if (f0 < 0) { FL_ERR("逻辑帧率:取不到逻辑帧(还没进对局?)"); return -1; }
    const DWORD t0 = GetTickCount();
    Sleep((DWORD)seconds * 1000);
    const int f1 = FrameLabLogicFrame();
    const DWORD dt = GetTickCount() - t0;
    if (f1 < 0 || !dt) { FL_ERR("逻辑帧率:读数无效(f1=%d dt=%u)", f1, (unsigned)dt); return -1; }
    const int fps100 = (int)((double)(f1 - f0) * 100000.0 / (double)dt);
    FL_INFO("逻辑帧率:%d 帧 / %lu 毫秒 = %d.%02d 帧每秒(任何渲染帧率下都应是 15.00)",
            f1 - f0, dt, fps100 / 100, fps100 % 100);
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

// ═════════════ 2026-09-18(第十一批):黑匣子 —— 崩溃 / 不同步的事后取证 ═════════════
//
// 为什么要有它:用户报「偶尔会不同步」「游戏本体崩溃」,而现有日志只记**安装期**的事
// (特征命中、改动字节、回滚)。出事那一刻什么都没留下 ⇒ 没有现场,就只能猜。
// 项目纪律「先怀疑量具」在这里的版本是:**先让失败留下证据。**
//
// 三件东西:
//   ① 飞行记录仪:环形缓冲,每渲染帧记一行状态。崩溃/不同步时倒出来,就能看到
//      「出事前最后 ~8.5 秒里 逻辑帧 / 渲染帧 / 时钟 / 阶段 / 插值系数 是怎么走的」。
//   ② 异常记录器(VEH):记异常码、地址、它属于哪个模块+偏移、寄存器、栈回溯、模块表。
//      **永远返回 EXCEPTION_CONTINUE_SEARCH** —— 我们只旁观,绝不改变游戏自己的异常处理。
//   ③ 手动快照 + 自检:FrameLabBlackBoxDump() 立刻倒出记录仪(不同步现场用);
//      FrameLabBlackBoxSelfTest() 走一遍**真实的 VEH 路径**来证明它真的会写文件。
//
// ⚠ 纪律:本模块**不改游戏一个字节**,只读状态 + 写我们自己的日志目录。
// ⚠ 崩溃路径上不能用会加锁 / 分配内存的东西 —— 它可能就死在锁里。所以这里**不复用 log::line**
//   (那个要进临界区),而是自己 CreateFile / WriteFile 后立刻关,并且带防递归标志。
namespace bb {

static const int kRingRows = 512;    // ≈ 8.5 秒 @60fps
static const int kMaxCrash = 40;     // 单进程最多记 40 次异常,防「热异常」刷爆磁盘
static const int kMaxStack = 48;     // 栈回溯最多记 48 层
static const int kMaxCodes = 16;     // 去重:记住见过的异常码,每个码最多记 5 份
#define FL_BB_CHS_CHANNELS 4         // 姿态轴同时钉几辆车(2026-09-21,见 Rec.chs 的注释)

// 一行记录。字段都是「出事后要拿来对账」的量,不是随便挑的:
//   us          墙钟**微秒**(QPC)—— 判断「是不是卡住了」,也是帧间隔的唯一可靠来源
//   logicFrame  引擎逻辑帧号 —— 模拟推进到哪(**它归零 = 对局对象被换掉了**)
//   renderFrame 我们的渲染帧计数 —— 渲染是否还在跑
//   clock       游戏时钟 dword_CE1388 —— 动画的时间源
//   msPerFrame  本帧该推进多少毫秒 —— 我们的累加器输出(原版恒 33)
//   ratio/groups  当时的配置 —— 同一份日志里可能换过配置,不带配置的日志没法对账
//   phase/frac    阶段号与插值系数 —— **载具抖动这类问题的第一现场**
//   engineFps   引擎自报 FPS(obj+0x70,指数平滑)
//
// ★★ 2026-09-18(第十一批)订正:时间戳原来用 GetTickCount(),**这是个坏量具**。
//   实测证据(第一次抖动探针,measure30 原版基线臂):帧间隔直方图只落在 31.25 和
//   46.875 两个值上,而这两个数正是 15.625 的整数倍 —— GetTickCount 的默认分辨率就是
//   15.625 毫秒。真实帧间隔约 33.33 毫秒(均匀),被量化成「8 帧 31 毫秒 + 1 帧 47 毫秒」的
//   锯齿,于是在「prog 对墙钟拟合残差」里造出一个 ±7 毫秒、周期约 9 帧的**假抖动**。
//   这个假抖动的量级和我们要找的真抖动是同一个数量级 ⇒ 量具不改,后面的实验全是噪声。
//   改用 QueryPerformanceCounter(实机频率 10 MHz 级 ⇒ 0.1 微秒分辨率)。
//   教训与项目既有纪律一致:**先怀疑量具**;一个分辨率比被测现象还粗的尺子,量出来的
//   只是尺子自己的刻度。
struct Rec {
    unsigned long long us;
    int   logicFrame;
    int   renderFrame;
    int   clock;
    int   msPerFrame;
    int   ratio;
    int   groups;
    int   phase;
    float frac;
    int   engineFps;
    // ★★★ 2026-09-18(第十一批·续):**动画轴** —— 「载具上下抖动」的第一现场。
    //   为什么必须加:上面那 10 个字段**全是时间轴**(呈现时刻、帧号、游戏时钟)。
    //   而用户报的「载具移动时上下抖动」是**动画轴**的现象 —— 时间轴量具在结构上就看不见它。
    //   更糟的是,时间轴读数在**多会话共用的这台机器**上被外部负载主导:同一配置
    //   `30@0x28FFF` 在第 1 臂测到高频抖动 3.844 ms、在第 3 臂测到 0.411 ms(差 9 倍)。
    //   而下面这两个量是**引擎算出来的确定性输出**,不随机器负载漂移 ⇒ 可信。
    //
    //   animCalls 本渲染帧「切动画」钩子(sub_90C1B0 → sub_8EDED0)被调用了几次。
    //             ★这是量化 FL_G_ANIMPROBE 开销的唯一手段:该钩子每次调用都要跑
    //              **2 次 mem::read_ok()**,而后者用的是 **VirtualQuery(系统调用)**。
    //              本项目在别处早已避开这个坑(见 tick 里读逻辑帧号那段注释),但
    //              anim_probe_record 没有避开。若这个数每帧是几百以上,每帧就要打进
    //              几百次系统调用 ⇒ 帧时间被拉长并抖动 ⇒ 作用在**固定步长的动画**上
    //              就表现为跳帧(上下跳)。
    //              -1 = 动画标尺没装(FL_G_ANIMPROBE 关)或探针一次都没被走到。
    //   anim[4]   前 4 个动画对象的**当前动画分数**(owner+0xB0,float)。
    //              载具移动时的上下起伏(悬挂 bob)就是动画混合的输出 ⇒ 这四列
    //              **直接就是「载具上下」的读数**。-1 = 该槽没采到。
    int   animCalls;
    float anim[FL_ANIM_OWNERS];
    // ★★ 2026-09-18(第十一批·续)第二批:**世界坐标跟踪** —— 「移动时上下抖动」的正主。
    //   用户答「原版轻微、装补丁后加剧」+「移动时抖,静止时不抖」⇒ 问题在**位置**路径上。
    //   而位置不在上面任何一个字段里(那 12 个全是时间轴 + 动画轴)。
    //   `flctl memscan` 能扫出「正在移动的三个相邻 float」的地址(实测拿到过
    //   `(2449.5085,-0.7984,0.6022)` 这样明显是地图坐标、且 x 在移动的三元组),
    //   但那些地址**每次运行都会变**,而且单次快照看不出「抖不抖」。
    //   ⇒ 用 `flctl track <addr>` 把地址填进来,这里逐渲染帧记下它的三个 float。
    //   trk[i] 的三个分量依次是 addr+0 / addr+4 / addr+8。-1 = 该通道没配。
    float trk[4][3];
    // ★★★ 2026-09-21(悬挂路径会话):**姿态轴** —— 车身外观(悬挂/俯仰/侧倾)的直接读数。
    //   上面三条轴(时间 / 动画分数 / 世界坐标)都看不见「车身在颠」:它由 calcPhysicsXform
    //   算出来、只写进**渲染矩阵**,逻辑坐标一个字节不动(所以位置轴量到 Z 恒定并不矛盾)。
    //   chsCalls  本渲染帧车身外观包装被进入的次数(全体可见载具);-1 = 包装没装。
    //   chsAdv    本渲染帧原函数被放行且返回非 0 的次数(= 全体载具的递推步数)。
    //             没修时 chsAdv == 有效调用数;节拍门生效后每 N 帧才出现一次非 0。
    //   chs[c]    通道 c 钉住的那辆车最近一次输出的 俯仰 / 侧倾 / 视觉 Z;-9 = 该通道没钉车。
    //             通道按「车身真的在动(moved ≥ 8)且调用最多」自动钉住,闲置约 4 秒后释放。
    int   chsCalls;
    int   chsAdv;
    float chs[FL_BB_CHS_CHANNELS][3];
};

static Rec           g_ring[kRingRows];
static volatile long g_ringPos  = 0;      // 累计写入行数(取模才是槽位)
// 上一渲染帧读到的「切动画」累计调用数。用它做差得到**每帧调用次数**(见 Rec.animCalls)。
static volatile long g_lastAnimCalls = 0;
// ★ 世界坐标跟踪的配置(见 Rec.trk 的注释)。由 flctl `track <addr>` 填进来。
//   0 = 该通道没配。**只在 tick 里做一次地址范围判断,不做 read_ok**(热路径)。
static unsigned      g_trkAddr[4] = {0, 0, 0, 0};
// ★ 姿态轴(见 Rec.chs 的注释)。通道钉的是 fl::g_chsTab 里的**格子下标**,同时记下当时的 key:
//   格子被别的车回收(key 变了)或这辆车约 4 秒没再被调用(不在屏幕里了)就释放通道。
//   闲置用「calls 有没有涨」判,不去问引擎要显示帧号 —— tick 在手工 hook 的调用路径上,
//   能不调引擎就不调。
static volatile long g_lastChsCalls = 0, g_lastChsAdv = 0;
static int           g_chsChanIdx[FL_BB_CHS_CHANNELS]   = {-1, -1, -1, -1};
static void*         g_chsChanKey[FL_BB_CHS_CHANNELS]   = {NULL, NULL, NULL, NULL};
static long          g_chsChanCalls[FL_BB_CHS_CHANNELS] = {0, 0, 0, 0};
static int           g_chsChanIdle[FL_BB_CHS_CHANNELS]  = {0, 0, 0, 0};
static int           g_chsPinTick = 0;
static volatile long g_vehHits  = 0;      // VEH 被调用次数
static volatile long g_vehSaved = 0;      // 实际落盘的报告数
static unsigned      g_codeSeen[kMaxCodes]  = {0};   // 见过的异常码(去重用)
static int           g_codeCount[kMaxCodes] = {0};   // 每个码已经记了几份
static PVOID         g_vehHandle = NULL;
static bool          g_on        = false;
static bool          g_inHandler = false; // 防递归:处理过程中再抛异常就直接放过
static char          g_dir[MAX_PATH * 2] = {0};      // 与主日志同目录
static char          g_last[MAX_PATH * 2] = {0};     // 最近一次报告路径(给上层读)

// ★高分辨率时间源(2026-09-18 第十一批)。见 Rec 上方的订正:GetTickCount 的 15.625 毫秒
//   量化会在「帧间隔」上造出比真现象还大的假抖动,必须换成 QPC。
//   g_qpcBase 把计数减到一个接近 0 的基准,避免「开机很久之后的计数 × 1000000」溢出 64 位。
//   (QPC 频率一般 10 MHz:开机 1 天的计数约 8.6e11,×1e6 = 8.6e17,离 9.2e18 还远;
//    减基准只是为了给长时间挂机的对局留余量。)
static LARGE_INTEGER g_qpcFreq = {0};
static LARGE_INTEGER g_qpcBase = {0};

static unsigned long long now_us() {
    if (g_qpcFreq.QuadPart <= 0) return 0;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const long long d = c.QuadPart - g_qpcBase.QuadPart;
    return (unsigned long long)((d * 1000000LL) / g_qpcFreq.QuadPart);
}

// 崩溃路径专用写文件:不加锁、不留句柄、写完就关。
// ★2026-09-18(第十一批):改成返回 bool。原来返回 void,于是 write_report 无法知道
//   「文件到底有没有写出去」,g_last 也就无条件被置成路径 —— 自检只看 g_vehHits 增加,
//   在磁盘满 / 目录不可写 / 路径解析失败时会给出**假 PASS**。假 PASS 比没有自检更坏:
//   它让人相信黑匣子在守着,而出事那一刻才发现什么都没有。
static bool raw_write(const char* path, const char* text, int len) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        // ★ 2026-09-18(第十一批·续)加这条诊断:实测自检**偶发**返回 FL_ERR_WRITE,
        //   而 g_vehSaved 却增加了 —— 说明 write_report 被调用、但 raw_write 失败。
        //   失败只有两个可能:CreateFile 打不开,或 WriteFile 没写全。这条日志把
        //   是哪一种、错误码是多少直接打出来,免得下次又靠猜。
        FL_ERR("黑匣子:raw_write 打开失败 path=%s err=%lu", path, GetLastError());
        return false;
    }
    DWORD w = 0;
    LARGE_INTEGER sz; sz.QuadPart = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
        // 与主日志同一条纪律:没有 BOM 时记事本 / 部分工具会按 GBK 解码成乱码。
        static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, kBom, 3, &w, NULL);
    }
    const BOOL wroteBody = WriteFile(h, text, (DWORD)len, &w, NULL);
    const DWORD writeErr = wroteBody ? 0 : GetLastError();
    CloseHandle(h);
    if (!wroteBody || w != (DWORD)len) {
        FL_ERR("黑匣子:raw_write 写入不全 path=%s 想要 %d 实际 %lu err=%lu",
               path, len, (unsigned long)w, (unsigned long)writeErr);
        return false;
    }
    return true;
}

static void fmt_append(char* buf, size_t cap, size_t* used, const char* fmt, ...) {
    if (*used + 1 >= cap) return;
    va_list a; va_start(a, fmt);
    int n = _vsnprintf_s(buf + *used, cap - *used, _TRUNCATE, fmt, a);
    va_end(a);
    if (n < 0) { *used = cap - 1; buf[*used] = 0; return; }   // _TRUNCATE 截断时返回 -1
    *used += (size_t)n;
}

// 地址 → 「模块名 + 偏移」。崩溃现场最要紧的一步:只有裸地址没法判断是我们的补丁、
// 游戏本体、还是某个系统 DLL 出的问题。
static void describe_addr(void* addr, char* out, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof mbi)) {
        _snprintf_s(out, n, _TRUNCATE, "%08X  (VirtualQuery 失败:地址不可读)",
                    (unsigned)(uintptr_t)addr);
        return;
    }
    HMODULE mod = (HMODULE)mbi.AllocationBase;
    char full[MAX_PATH] = {0};
    if (mod) GetModuleFileNameA(mod, full, MAX_PATH);
    const char* leaf = strrchr(full, '\\');
    leaf = leaf ? leaf + 1 : full;
    _snprintf_s(out, n, _TRUNCATE, "%08X  %s + 0x%X   [%s]",
                (unsigned)(uintptr_t)addr, leaf[0] ? leaf : "?",
                (unsigned)((uintptr_t)addr - (uintptr_t)mod), full);
}

// 32 位 + /O2 默认省掉帧指针 ⇒ EBP 链基本不可用,所以主手段是**扫栈**:
// 从 Esp 起按 4 字节扫,凡是「落在一个可执行区里」的值就当作返回地址记下来。
// 不是精确回溯,但对「崩溃在哪条调用链上」够用,而且**不依赖任何调试库**(崩溃路径上少一个依赖少一分风险)。
static int scan_stack(const CONTEXT* c, char* buf, size_t cap, size_t* used) {
    const unsigned* p   = (const unsigned*)(uintptr_t)c->Esp;
    const unsigned* end = p + 4096;      // 32 位栈够深了,再往下都是噪声
    int found = 0;
    for (; p < end && found < kMaxStack; ++p) {
        const unsigned v = *p;
        if (v < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)(uintptr_t)v, &mbi, sizeof mbi)) continue;
        if (mbi.State != MEM_COMMIT) continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        if (!mbi.AllocationBase) continue;
        char line[MAX_PATH + 96];
        describe_addr((void*)(uintptr_t)v, line, sizeof line);
        fmt_append(buf, cap, used, "    #%02d  %s\r\n", found, line);
        ++found;
    }
    return found;
}

// 已加载模块表(地址 → 模块+偏移 的换算基准)。走 VirtualQuery 扫地址空间,
// **不走 PEB 的 Ldr 链、也不依赖 psapi/dbghelp** —— 那两样都要多一个结构体偏移假设。
static void dump_modules(char* buf, size_t cap, size_t* used) {
    fmt_append(buf, cap, used, "──────── 已加载模块(把上面的地址换算成 模块+偏移 用) ────────\r\n");
    unsigned char* p = (unsigned char*)0x10000;
    const unsigned char* limit = (const unsigned char*)0x7FFF0000;
    HMODULE last = NULL;
    int n = 0;
    while (p < limit && n < 200) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        HMODULE mod = (HMODULE)mbi.AllocationBase;
        if (mod && mod != last) {
            last = mod;
            char full[MAX_PATH] = {0};
            if (GetModuleFileNameA(mod, full, MAX_PATH)) {
                const char* leaf = strrchr(full, '\\');
                leaf = leaf ? leaf + 1 : full;
                fmt_append(buf, cap, used, "    %08X  %-26s %s\r\n",
                           (unsigned)(uintptr_t)mod, leaf, full);
                ++n;
            }
        }
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) break;                       // 防死循环
        p = next;
    }
}

// 飞行记录仪倒带。**时间正序**输出(不是从槽位 0 开始)—— 人读日志是按时间读的。
static void dump_ring(char* buf, size_t cap, size_t* used) {
    const long total = g_ringPos;
    const long count = total < kRingRows ? total : kRingRows;
    const long start = total - count;
    fmt_append(buf, cap, used, "──────── 飞行记录仪(最近 %ld 帧,时间正序) ────────\r\n", count);
    fmt_append(buf, cap, used,
        "     序号   墙钟us    逻辑帧    渲染帧   时钟ms  每帧ms   r    分组     阶段   插值      引擎FPS"
        "  动画调用    动画0     动画1     动画2     动画3"
        "      跟踪X       跟踪Y       跟踪Z"
        "  车身调用 车身放行  (俯仰 侧倾 视觉Z)×4\r\n");
    for (long i = start; i < total; ++i) {
        const Rec& r = g_ring[i % kRingRows];
        // ★★ 动画轴四列 + 跟踪三列(第十一批·续)是「载具上下抖动」的唯一直接读数,见 Rec 的注释。
        //    放在**行尾**是刻意的:ring_analyze.py 的正则按分组序号取值,行尾追加不影响
        //    既有 11 个分组;而它那边的新分组写成**可选**,所以旧日志一样能解析。
        //    跟踪三列只打 trk[0] —— 一次只盯一个单位,4 个通道全打会把行撑到 200 字符。
        fmt_append(buf, cap, used,
            "  %6ld  %10llu  %7d  %8d  %7d  %6d  %2d  0x%05X  %5d  %.4f  %8d"
            "  %6d",
            i, r.us, r.logicFrame, r.renderFrame, r.clock, r.msPerFrame,
            r.ratio, (unsigned)r.groups, r.phase, r.frac, r.engineFps,
            r.animCalls);
        // ★★ 2026-09-20:动画分数从 4 个扩到 FL_ANIM_OWNERS(12) 个。
        //   为什么:两臂的「分数范围」不同,说明「前 4 个槽」两次运行采到的**不是同一批对象**
        //   (槽分配是「先到先得 + 淘汰最不活跃」,运行时序不同结果就不同),于是**绝对值不可比**,
        //   只能比「形态」(连续 vs 阶梯)。一次倒出 12 个槽,就有机会用「分数范围重叠」
        //   把两臂配到**同一个对象**上,从而量出「推进量到底是 1.0 还是 1.0559」。
        //   ⚠ 逐个追加、空格分隔:ring_analyze.py 把这一段当「一组 float」解析(`{N}` 通配),
        //     所以加宽这里**不必**同时改它的列定义。
        for (int k = 0; k < FL_ANIM_OWNERS; ++k)
            fmt_append(buf, cap, used, "  %9.5f", r.anim[k]);
        fmt_append(buf, cap, used,
            "  %10.4f  %10.4f  %10.4f",
            r.trk[0][0], r.trk[0][1], r.trk[0][2]);
        // ★★★ 2026-09-21:姿态轴(车身调用 / 车身放行 + 4 个通道 × 俯仰/侧倾/视觉Z),同样追加在**行尾**,
        //   ring_analyze.py 那边是又一个**可选组** ⇒ 本批之前的旧日志照常解析。
        //   ⚠ 每行因此多约 150 字节 × 512 行 ⇒ write_report 的缓冲已同步从 160 KB 扩到 384 KB
        //     (fmt_append 满了是**静默截断**,见 19.9.5 那次「找到了但没打出来」的教训)。
        fmt_append(buf, cap, used, "  %5d  %5d", r.chsCalls, r.chsAdv);
        for (int c = 0; c < FL_BB_CHS_CHANNELS; ++c)
            fmt_append(buf, cap, used, "  %9.6f %9.6f %9.6f", r.chs[c][0], r.chs[c][1], r.chs[c][2]);
        fmt_append(buf, cap, used, "\r\n");
    }
}

static void dump_state(char* buf, size_t cap, size_t* used) {
    // 累加器到底有没有在跑,是「30 帧抖动」这个问题的第一现场 —— 所以不只打累加器的值,
    // 直接把判据也算出来打在这里(和 install 里的判据是同一条式子,不许各写一份)。
    const bool clockAtRetail = (fl::g_targetFps == fl::g_retailFps);
    const bool accOn = !fl::g_measureOnly && (fl::g_groups & FL_G_CLOCK)
                       && (!clockAtRetail || (fl::g_groups & FL_G_CLOCK_AT_RETAIL));
    fmt_append(buf, cap, used,
        "补丁状态 已安装=%d 目标帧率=%d 原版帧率=%d r=%d 分组=0x%05X 只量不改=%d 每帧毫秒=%d 时钟累加器=%d/%d 累加器生效=%d\r\n",
        fl::g_installed ? 1 : 0, fl::g_targetFps, fl::g_retailFps, fl::g_ratio,
        (unsigned)fl::g_groups, fl::g_measureOnly ? 1 : 0, fl::g_msPerFrame,
        fl::g_clockAcc, fl::g_clockWant, accOn ? 1 : 0);
    // 2026-09-21:车身外观包装的状态单独一行(不往上一行里塞:ring_analyze.py 的 STATE_RE 按那一行的字段顺序取值)。
    fmt_append(buf, cap, used,
        "车身外观 包装已装=%d 模式=%d 调用=%ld 放行=%ld 回放=%ld 返回0=%ld 表满直通=%ld\r\n",
        fl::g_chsSite ? 1 : 0, fl::g_chsMode, (long)fl::g_chsCalls, (long)fl::g_chsAdv,
        (long)fl::g_chsHold, (long)fl::g_chsZero, (long)fl::g_chsNoSlot);
}

// 报告正文 = 头部 + 状态 + 寄存器(可选) + 栈 + 模块表 + 记录仪。写成一份完整的现场。
static void write_report(EXCEPTION_POINTERS* ep, const char* tag, bool isCrash) {
    // ★★★ 2026-09-20 加自愈。为什么:
    //   实测这个函数**偶发**直接返回 —— 自检报 FL_ERR_WRITE,而 `g_vehSaved` 却增加了
    //   (说明 write_report 被调用了),并且 `raw_write` 的两条失败分支都加了 FL_ERR
    //   却一条都没打出来 ⇒ 卡的就是下面这句 `if (!g_dir[0]) return;`。
    //   可 `bb::init` 当时**明确打印了正确的目录**(日志里能看到) ⇒ `g_dir` 是在那之后
    //   被清空的。HANDOFF §19.8.24 推测是「Rec 结构变大后某个写入点越界」,但始终没定论;
    //   而 2026-09-20 又把 Rec.anim 从 4 扩到 12(Rec 更大),风险只会上升。
    //   ⇒ 与其继续追根因,这里加一条**自愈**:g_dir 为空时从 log::path() 重新推导一次。
    //   崩溃报告/飞行记录仪是这个项目**唯一的现场证据** —— 不能因为一个偶发的内存问题
    //   就把整份现场丢掉。自愈是幂等的:目录本来就对时它什么都不做。
    if (!g_dir[0]) {
        const char* lp = log::path();
        if (lp && lp[0]) {
            _snprintf_s(g_dir, sizeof g_dir, _TRUNCATE, "%s", lp);
            char* slash = strrchr(g_dir, '\\');
            if (slash) *slash = 0;
            FL_WARN("黑匣子:g_dir 曾为空,已从日志路径自愈为 %s", g_dir);
        }
    }
    if (!g_dir[0]) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s-%lu-%04u%02u%02u-%02u%02u%02u.log",
                g_dir, isCrash ? "crash" : "snapshot", GetCurrentProcessId(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // 2026-09-21:160 KB → 384 KB。姿态轴让每行多约 150 字节;此前一份快照已经有 147,951 字节
    //   (离 160 KB 只差 10%),不扩就会被 fmt_append **静默截断**,而截掉的正是行尾的新列。
    static char buf[384 * 1024];
    size_t used = 0;
    char line[MAX_PATH + 96];

    if (isCrash) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const CONTEXT* c = ep->ContextRecord;
        fmt_append(buf, sizeof buf, &used, "════════ 异常报告 ════════\r\n");
        fmt_append(buf, sizeof buf, &used,
            "时间 %04u-%02u-%02u %02u:%02u:%02u.%03u   进程 pid=%lu   线程 tid=%lu   标记=%s\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(), tag);
        // ★措辞要准确:VEH 看到的**永远**是第一次机会(first-chance)。游戏自己的 SEH
        //   有可能把这个异常处理掉并继续跑 —— 所以「有这份报告」≠「游戏崩了」。
        //   要判断「是不是真崩」得看进程有没有退出(外部 watcher 负责那一条)。
        fmt_append(buf, sizeof buf, &used,
            "说明 这是**第一次机会**异常。游戏自己的处理程序可能把它吞掉并继续运行 ——\r\n"
            "     所以有这份报告 ≠ 游戏崩了;要判断真崩,看进程是否退出。\r\n");
        fmt_append(buf, sizeof buf, &used, "异常码 0x%08X   标志 0x%08X   参数个数 %u\r\n",
                   (unsigned)er->ExceptionCode, (unsigned)er->ExceptionFlags,
                   (unsigned)er->NumberParameters);
        describe_addr(er->ExceptionAddress, line, sizeof line);
        fmt_append(buf, sizeof buf, &used, "异常地址 %s\r\n", line);
        if (er->ExceptionCode == 0xC0000005u && er->NumberParameters >= 2) {
            const char* kind = "读";
            if (er->ExceptionInformation[0] == 1) kind = "写";
            else if (er->ExceptionInformation[0] == 8) kind = "执行";
            describe_addr((void*)(uintptr_t)er->ExceptionInformation[1], line, sizeof line);
            fmt_append(buf, sizeof buf, &used, "访问类型 %s   目标地址 %s\r\n", kind, line);
        }
        dump_state(buf, sizeof buf, &used);
        fmt_append(buf, sizeof buf, &used,
            "──────── 寄存器 ────────\r\n"
            "  EAX=%08X  EBX=%08X  ECX=%08X  EDX=%08X\r\n"
            "  ESI=%08X  EDI=%08X  EBP=%08X  ESP=%08X  EIP=%08X  EFL=%08X\r\n",
            (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx,
            (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp,
            (unsigned)c->Eip, (unsigned)c->EFlags);
        // ★★★ 2026-09-20:两段式落盘 —— **必须放在 scan_stack 之前**。
        //   实测(20 次里 5 次):自检报 FL_ERR_WRITE(g_last 空),而 4 毫秒后另一次
        //   **非 VEH** 的 snapshot 调用却成功落盘。日志顺序钉死因果:VEH 那次的
        //   write_report **在 raw_write 之前就被中断** —— 连一行都没写出去。
        //   中断点就是下面这句 scan_stack:它要**扫栈**,而 VEH 上下文里栈可能已经损坏。
        //   本函数**不能用 SEH** 兜底(铁律 42:hook 调用路径上的 SEH 会破坏手工栈平衡,
        //   实测让黑匣子自检从 23 掉到 21),所以内部一崩就整个中断。
        //   ⇒ 在扫栈**之前**先把「头部 + 状态 + 寄存器」落盘并立刻置 g_last。
        //     末尾那次落盘是**追加**(OPEN_ALWAYS + FILE_APPEND_DATA),所以文件仍然完整;
        //     只是现在有了「至少写出一半」的保证 —— 崩溃报告是这个项目唯一的现场证据,
        //     不能因为一个偶发的栈损坏就整份丢掉。
        if (raw_write(path, buf, (int)used))
            _snprintf_s(g_last, sizeof g_last, _TRUNCATE, "%s", path);
        fmt_append(buf, sizeof buf, &used, "──────── 栈回溯(扫栈法,非精确) ────────\r\n");
        if (scan_stack(c, buf, sizeof buf, &used) == 0)
            fmt_append(buf, sizeof buf, &used, "    (栈里没找到可执行地址 —— 栈可能已经损坏)\r\n");
    } else {
        fmt_append(buf, sizeof buf, &used, "════════ 手动快照(不是崩溃,是 FrameLabBlackBoxDump 被调用)════════\r\n");
        fmt_append(buf, sizeof buf, &used, "时间 %04u-%02u-%02u %02u:%02u:%02u   进程 pid=%lu   标记=%s\r\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   GetCurrentProcessId(), tag);
        dump_state(buf, sizeof buf, &used);
    }
    dump_modules(buf, sizeof buf, &used);
    dump_ring(buf, sizeof buf, &used);

    // ★只有真的落盘了才记 g_last。g_last 是上层(自检 / FrameLabBlackBoxLast)判断
    //   「黑匣子到底有没有在工作」的唯一依据,所以它必须是「成功」的凭证,不是「尝试过」的凭证。
    // ★★ 2026-09-20:加一条落盘结果日志。实测自检**偶发**(20 次里 5 次)报 FL_ERR_WRITE,
    //   而 g_vehSaved 增加了、g_dir 也非空(自愈没触发)、raw_write 的两条 FL_ERR 分支
    //   又一条都没打出来 —— 三个线索互相矛盾。这条日志把「到底走到哪一步」钉死:
    //   如果它**没出现**,说明函数在它之前就返回了;如果出现且说「失败」,那就是 raw_write。
    const bool wrote = raw_write(path, buf, (int)used);
    FL_INFO("黑匣子:write_report 落盘 %s,%d 字节 -> %s",
            wrote ? "成功" : "**失败**", (int)used, path);
    if (wrote)
        _snprintf_s(g_last, sizeof g_last, _TRUNCATE, "%s", path);
}

// ★必须过滤的「良性」异常码。踩过的坑(2026-09-18 实测):日志本身走 OutputDebugString,
//   而它**会抛 0x40010006(DBG_PRINTEXCEPTION_C)** —— 没有调试器也照样抛给 VEH。
//   于是「每写一行日志 ⇒ 生成一份崩溃报告」,日志目录会被瞬间塞满。
//   这类码是「通知」不是「故障」,直接放过。
static bool is_benign(unsigned code) {
    switch (code) {
        case 0x40010006u:   // DBG_PRINTEXCEPTION_C   —— OutputDebugStringA
        case 0x4001000Au:   // DBG_PRINTEXCEPTION_WIDE_C
        case 0x40010003u:   // DBG_LOADDLL
        case 0x40010004u:   // DBG_UNLOADDLL
        case 0x40010005u:   // DBG_OUTPUT_STRING
        case 0x406D1388u:   // MS_VC_EXCEPTION —— SetThreadName 用
        case 0xE06D7363u:   // MSVC C++ EH:任何 throw 都会走这里,是控制流不是故障
            return true;
        default:
            return false;
    }
}

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS* ep) {
    // 我们只旁观:**任何情况下都返回 CONTINUE_SEARCH**,让游戏自己的异常处理照常走。
    if (g_inHandler) return EXCEPTION_CONTINUE_SEARCH;      // 处理过程中再抛 ⇒ 直接放过,别递归
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned code = (unsigned)ep->ExceptionRecord->ExceptionCode;
    if (is_benign(code)) return EXCEPTION_CONTINUE_SEARCH;  // 见 is_benign:通知类,不是故障

    // 同一个码最多记 5 份。热异常(比如某个循环里一直抛)不该把磁盘写满 ——
    // 有 5 份就足够看出「是哪个码、在哪个地址」,再多是重复信息。
    int slot = -1;
    for (int i = 0; i < kMaxCodes; ++i) {
        if (g_codeSeen[i] == code) { slot = i; break; }
        if (g_codeSeen[i] == 0 && slot < 0) slot = i;
    }
    if (slot >= 0) {
        if (g_codeSeen[slot] != code) { g_codeSeen[slot] = code; g_codeCount[slot] = 0; }
        if (g_codeCount[slot] >= 5) return EXCEPTION_CONTINUE_SEARCH;
        ++g_codeCount[slot];
    }

    g_inHandler = true;
    ++g_vehHits;
    if (g_vehSaved < kMaxCrash) {
        ++g_vehSaved;
        write_report(ep, "veh", true);
    }
    g_inHandler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

static void init(HMODULE self) {
    (void)self;
    const char* lp = log::path();
    if (!lp || !lp[0]) return;
    _snprintf_s(g_dir, sizeof g_dir, _TRUNCATE, "%s", lp);
    char* slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    CreateDirectoryA(g_dir, NULL);
    // ★在 DllMain 里就注册:这样**补丁还没安装**时崩溃也能留下现场。
    //   第一个参数 1 = 插在链首(我们最先看到);第二个是处理函数。
    g_vehHandle = AddVectoredExceptionHandler(1, veh_handler);
    // ★时间源(2026-09-18 第十一批)。必须在任何 tick() 之前取到,否则 now_us() 返回 0,
    //   整份飞行记录仪的时间列就全废 —— 而「全 0」看起来又很像「帧间隔为 0」,
    //   是个很容易被误读成「渲染卡死」的假现场。
    if (!QueryPerformanceFrequency(&g_qpcFreq) || g_qpcFreq.QuadPart <= 0) {
        g_qpcFreq.QuadPart = 0;
        FL_WARN("黑匣子:QueryPerformanceFrequency 失败 ⇒ 飞行记录仪的时间列会全是 0");
    } else {
        QueryPerformanceCounter(&g_qpcBase);
    }
    FL_INFO("黑匣子:VEH %s(crash-*.log 写到 %s),QPC 频率 %lld",
            g_vehHandle ? "已注册" : "★注册失败", g_dir, g_qpcFreq.QuadPart);
}

}  // namespace bb

// 飞行记录仪的一行。定义在 bb 里,声明在文件开头(逐帧钩子要用)。
namespace bb {
void tick(void* frameObj) {
    if (!g_on) return;
    Rec r;
    // ★用 QPC 而不是 GetTickCount:后者的 15.625 毫秒量化会在帧间隔上造出 ±7 毫秒的假抖动,
    //   量级和我们要找的真抖动一样大。见 Rec 上方的订正与 now_us()。
    r.us          = now_us();
    r.renderFrame = (int)fl::g_frameCount;
    r.msPerFrame  = fl::g_msPerFrame;
    r.ratio       = fl::g_ratio;
    r.groups      = fl::g_groups;
    r.clock       = *(volatile int*)(uintptr_t)0x00CE1388;
    // 逻辑帧号 = *(*(0x00CD8CE4)+0x50)。这里**不用 mem::read_ok** —— 它每次都要 VirtualQuery,
    // 而这是每渲染帧都要走的热路径(60 次/秒 × 系统调用)。用范围判断代替:指针要么是 0,
    // 要么是一个用户态地址。
    r.logicFrame = -1;
    int* gl = *(int**)(uintptr_t)0x00CD8CE4;
    if (gl && (uintptr_t)gl > 0x10000 && (uintptr_t)gl < 0x7FFF0000)
        r.logicFrame = *(int*)((unsigned char*)gl + 0x50);
    // 阶段号 / 插值系数 / 引擎 FPS 取自引擎的逐帧对象;指针由调用方保证有效
    // (同一个钩子里我们本来就在读写它,见 fl_hook_perframe)。
    r.phase = -1; r.frac = -1.0f; r.engineFps = -1;
    if (frameObj) {
        r.phase     = *(int*)((unsigned char*)frameObj + 0x58);
        r.frac      = *(float*)((unsigned char*)frameObj + 0x60);
        r.engineFps = *(int*)((unsigned char*)frameObj + 0x70);
    }
    // ── 动画轴(见 Rec.animCalls / Rec.anim[] 的注释)──────────────────────────────
    //   ★这里**不能**用 __try/__except 兜底,尽管看起来很自然。bb::tick 是被**手工构造**
    //     的 hook(fl_hook_perframe)调用的,而 SEH 会在栈上建立异常注册记录、改变栈帧
    //     布局,破坏那个手工栈平衡。实测代价:黑匣子自检从 23 PASS 掉到 21 PASS,
    //     crash 报告再也写不到磁盘(自检返回 FL_ERR_WRITE=30)。这是本批踩得最深的坑。
    //   ⇒ 改用「新鲜度」判据:只读**最近 2 秒内被探针确认过**的槽(见 fl::g_animTick)。
    //     探针每秒被走约 24 次,所以悬垂窗口被压到 2 秒以内,且完全不需要 SEH。
    r.animCalls = -1;
    for (int i = 0; i < FL_ANIM_OWNERS; ++i) r.anim[i] = -1.0f;
    if (fl::g_animSite) {          // 非空 = 动画标尺(FL_G_ANIMPROBE)确实装上了
        const long animNow = fl::g_animCalls;
        r.animCalls = (int)(animNow - g_lastAnimCalls);   // 本渲染帧的调用次数
        g_lastAnimCalls = animNow;
        const long tickNow = (long)GetTickCount();
        for (int i = 0; i < FL_ANIM_OWNERS; ++i) {
            const float* f = fl::g_animFrame[i];
            if (!f) continue;
            // 不新鲜的槽直接跳过:它的对象可能已经被换掉,指针悬垂。
            if (fl::g_animTick[i] == 0 || (tickNow - fl::g_animTick[i]) > 2000) continue;
            // 再挡一道明显垃圾(0、内核地址)。
            if ((uintptr_t)f <= 0x10000 || (uintptr_t)f >= 0x7FFF0000) continue;
            r.anim[i] = *f;
        }
    }
    // ── 世界坐标跟踪(见 Rec.trk 的注释)────────────────────────────────────────
    //   地址由 `flctl track <addr>` 配置。和动画轴一样:**不用 SEH、不用 read_ok**
    //   (bb::tick 在手工 hook 的调用路径上,见本函数上面那段 SEH 的教训),
    //   只做一次地址范围判断。地址是人手工给的、且来自 memscan 的实测输出,所以
    //   正常情况下一定有效;范围判断只用来挡住 0 和明显的垃圾。
    for (int i = 0; i < 4; ++i) {
        r.trk[i][0] = r.trk[i][1] = r.trk[i][2] = -1.0f;
        const unsigned a = g_trkAddr[i];
        if (!a) continue;
        if (a <= 0x10000u || a >= 0x7FFF0000u) continue;
        const float* f = (const float*)(uintptr_t)a;
        r.trk[i][0] = f[0];
        r.trk[i][1] = f[1];
        r.trk[i][2] = f[2];
    }
    // ── 姿态轴(见 Rec.chs 的注释)────────────────────────────────────────────────
    //   全部读我们**自己的**表(fl::g_chsTab,定长静态数组),不碰游戏内存 ⇒ 不需要任何兜底。
    r.chsCalls = -1; r.chsAdv = -1;
    for (int c = 0; c < FL_BB_CHS_CHANNELS; ++c) r.chs[c][0] = r.chs[c][1] = r.chs[c][2] = -9.0f;
    if (fl::g_chsSite) {           // 非空 = 车身外观包装确实装上了
        const long callsNow = fl::g_chsCalls, advNow = fl::g_chsAdv;
        r.chsCalls = (int)(callsNow - g_lastChsCalls);
        r.chsAdv   = (int)(advNow - g_lastChsAdv);
        g_lastChsCalls = callsNow; g_lastChsAdv = advNow;
        // ① 维护已钉的通道:格子换了人 / 约 4 秒没被调用 ⇒ 释放
        for (int c = 0; c < FL_BB_CHS_CHANNELS; ++c) {
            const int idx = g_chsChanIdx[c];
            if (idx < 0) continue;
            const FlChassisEnt& e = fl::g_chsTab[idx];
            if (e.key != g_chsChanKey[c]) { g_chsChanIdx[c] = -1; continue; }
            if (e.calls == g_chsChanCalls[c]) { if (++g_chsChanIdle[c] > 240) g_chsChanIdx[c] = -1; }
            else { g_chsChanCalls[c] = e.calls; g_chsChanIdle[c] = 0; }
        }
        // ② 有空通道就补:每 16 帧扫一次表(1024 格的整数比较,可以忽略),挑「车身真的在动」
        //    且调用最多、还没被钉的那辆。只钉在动的,免得四个通道全被停着的车占掉。
        if ((++g_chsPinTick & 15) == 0) {
            for (int c = 0; c < FL_BB_CHS_CHANNELS; ++c) {
                if (g_chsChanIdx[c] >= 0) continue;
                int best = -1;
                for (int i = 0; i < FL_CHS_SLOTS; ++i) {
                    const FlChassisEnt& e = fl::g_chsTab[i];
                    if (!e.key || e.moved < 8) continue;
                    bool pinned = false;
                    for (int k = 0; k < FL_BB_CHS_CHANNELS; ++k)
                        if (g_chsChanIdx[k] == i) pinned = true;
                    if (pinned) continue;
                    if (best < 0 || e.calls > fl::g_chsTab[best].calls) best = i;
                }
                if (best < 0) break;
                g_chsChanIdx[c]   = best;
                g_chsChanKey[c]   = fl::g_chsTab[best].key;
                g_chsChanCalls[c] = fl::g_chsTab[best].calls;
                g_chsChanIdle[c]  = 0;
            }
        }
        // ③ 记录
        for (int c = 0; c < FL_BB_CHS_CHANNELS; ++c) {
            const int idx = g_chsChanIdx[c];
            if (idx < 0) continue;
            const FlChassisEnt& e = fl::g_chsTab[idx];
            if (!e.hasOut) continue;
            // ★ 只记「这一帧真的被画到」的车(idle == 0,见上面 ①)。首次实机:钉住的那辆车开出屏幕后
            //   位姿停在最后一次的值上,把「每逻辑帧位姿变化次数」稀释成了 0.87(应为 2)。
            if (g_chsChanIdle[c] != 0) continue;
            // 记的是**真正交给引擎的**位姿(shown):回放模式下它 == 递推结果,插值模式下它在相邻两个
            //   递推结果之间 —— 「屏幕上车身怎么动」要看这个,不是看递推的原始结果。
            r.chs[c][0] = e.shown[0];     // 俯仰
            r.chs[c][1] = e.shown[1];     // 侧倾
            r.chs[c][2] = e.shown[3];     // 视觉 Z
        }
    }
    g_ring[g_ringPos % kRingRows] = r;
    ++g_ringPos;
}

// 坐标跟踪通道 0 的设置入口(声明见文件顶部)。g_trkAddr 是本命名空间的 static,
// 而调用它的导出函数在全局作用域、位置更早,所以必须经由这里。
int set_track_addr(unsigned addr) {
    if (addr && (addr <= 0x10000u || addr >= 0x7FFF0000u)) return -1;
    g_trkAddr[0] = addr;
    if (addr) {
        // 打一份当时的读数,方便确认「填对了地址」—— 三个 float 应该像坐标
        // (比如地图尺度的大数,或者高度那种 0..500 的量)。
        const float* f = (const float*)(uintptr_t)addr;
        FL_INFO("坐标跟踪:通道 0 已指向 %08X,当前读数 (%.4f, %.4f, %.4f)",
                addr, f[0], f[1], f[2]);
    } else {
        FL_INFO("坐标跟踪:通道 0 已清空");
    }
    return 0;
}
}  // namespace bb

// 开 / 关飞行记录仪。默认**关**:它每帧多写 44 字节,虽然便宜但没有诊断需求时不该常开。
extern "C" __declspec(dllexport) int __stdcall FrameLabBlackBox(int on) {
    const bool want = (on != 0);
    if (want && !bb::g_on) {
        // 开的时候把缓冲清一遍,否则第一份报告里会混着上一轮的旧行,时间序就假了。
        for (int i = 0; i < bb::kRingRows; ++i) {
            bb::g_ring[i].us = 0;
            bb::g_ring[i].logicFrame = -1;
            bb::g_ring[i].renderFrame = -1;
        }
        bb::g_ringPos = 0;
        // 姿态轴的「本帧增量」基线也要对齐到现在,否则第一行会把记录仪关着期间攒下的调用数一次吐出来。
        bb::g_lastChsCalls = fl::g_chsCalls;
        bb::g_lastChsAdv   = fl::g_chsAdv;
    }
    bb::g_on = want;
    FL_INFO("黑匣子飞行记录仪:%s(缓冲 %d 行 ≈ %.1f 秒 @60fps;VEH %s,已落盘 %ld 份)",
            want ? "开" : "关", bb::kRingRows, bb::kRingRows / 60.0,
            bb::g_vehHandle ? "在" : "不在", bb::g_vehSaved);
    return FL_OK;
}

// 立刻倒一份现场快照。**不同步的时候就是靠这个**:用户看到「不同步」弹窗后马上调用它,
// 我们就能拿到「弹窗之前那 512 帧」的逐帧状态。
extern "C" __declspec(dllexport) int __stdcall FrameLabBlackBoxDump() {
    if (!bb::g_dir[0]) return FL_ERR_NO_MODULE;
    bb::write_report(NULL, "manual", false);
    FL_INFO("黑匣子:快照已写 %s", bb::g_last[0] ? bb::g_last : "(失败)");
    return bb::g_last[0] ? FL_OK : FL_ERR_WRITE;
}

// 最近一次报告的路径(给上层脚本读;返回长度,内容写进调用方缓冲区)。
extern "C" __declspec(dllexport) int __stdcall FrameLabBlackBoxLast(char* out, int cap) {
    if (!out || cap <= 1) return -1;
    const char* p = bb::g_last;
    int n = 0;
    while (p[n] && n < cap - 1) { out[n] = p[n]; ++n; }
    out[n] = 0;
    return n;
}

// ★自检:走**真实的 VEH 路径**,而不是直接调 write_report —— 否则「注册没成功」这种
//   最可能的失败根本测不出来。
//   做法:抛一个自定义异常码,再用 __try/__except 自己吞掉 ⇒ 游戏不会崩,但 VEH 已经真的跑过一遍,
//   而且真的写了一份 crash-*.log。
//   为什么不用访问违例:AV 有可能被游戏自己的 VEH/SEH 特殊对待(比如吞掉并继续执行),
//   自定义码最不容易踩到别人的逻辑。
//   ★2026-09-18(第十一批)加固:判据从「VEH 被命中」升级为「VEH 被命中 **且** 报告真的落在磁盘上」。
//   原来只看 g_vehHits 增加 —— 那是「尝试过」,不是「成功」。日志目录解析失败、磁盘满、
//   权限不足时,VEH 照样命中而文件不存在,自检会给出假 PASS。现在两种情况返回不同的错误码,
//   调用方(以及 tests/test_loader.cpp)能区分「黑匣子没接上」和「接上了但写不出去」。
//
//   mode 0 = 正面判据:抛一个自定义码(0xE0BB0001),它**必须**被记成一份 crash-*.log。
//   mode 1 = 反面判据:抛一个良性码(0x406D1388 MS_VC_EXCEPTION),它**必须不被记录**。
//     为什么要反面判据:实测踩过的坑 —— 日志本身走 OutputDebugString,而它会抛 0x40010006,
//     不过滤的话「每写一行日志 ⇒ 生成一份崩溃报告」,日志目录瞬间被塞满。
//     光有正面判据测不出「过滤被改坏了」,而这恰恰是最容易回归的一种坏法。
//     用 0x406D1388 而不是 0x40010006 来测:前者就是 MSVC SetThreadName 那个码,
//     「抛出来再自己吞掉」是被广泛使用的标准写法,确定不会惊动系统的硬错误处理。
extern "C" __declspec(dllexport) int __stdcall FrameLabBlackBoxSelfTest(int mode) {
    if (!bb::g_vehHandle) return FL_ERR_NOT_INSTALLED;

    if (mode == 1) {
        const long h0 = bb::g_vehHits;
        __try {
            RaiseException(0x406D1388u, 0, 0, NULL);   // MS_VC_EXCEPTION:良性,应被过滤
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const bool leaked = (bb::g_vehHits != h0);
        FL_INFO("黑匣子反面自检:良性码 0x406D1388 命中计数 %ld → %ld,过滤%s",
                h0, bb::g_vehHits, leaked ? "**失效**" : "生效");
        return leaked ? FL_ERR_BB_FILTER : FL_OK;
    }

    const long before = bb::g_vehHits;
    __try {
        RaiseException(0xE0BB0001u, 0, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 预期路径:VEH 记完账之后,SEH 在这里把它吞掉。
    }
    const long after = bb::g_vehHits;
    const bool hit   = (after > before);
    const bool named = (bb::g_last[0] != 0);
    // 第三道:文件真的在磁盘上。g_last 已经只在写成功时才置位,这一道是防「写成功但路径
    // 被人删掉」之类的边角(也防我们自己哪天又改坏了 g_last 的语义)。
    // ★★ 2026-09-18(第十一批·续)加重试:这条判据**偶发失败** —— 实测同一份 DLL 连跑两次
    //   `test_loader`,一次 23 PASS / 0 FAIL、一次 21 PASS / 2 FAIL,失败的两条正是
    //   「报告真的写在磁盘上」和「最近报告路径非空」。原因是 VEH 的写盘与这里的查询之间
    //   存在竞态(NTFS 的目录项更新有延迟)。**判据本身不变,只是不再抢跑**:重试 3 次、每次 50 ms。
    //   ⚠ 这个偶发失败会浪费下一个会话的时间(看起来像「补丁坏了」),所以必须修。
    bool onDisk = false;
    for (int attempt = 0; attempt < 3 && !onDisk; ++attempt) {
        if (named && GetFileAttributesA(bb::g_last) != INVALID_FILE_ATTRIBUTES) onDisk = true;
        else if (attempt < 2) Sleep(50);
    }
    FL_INFO("黑匣子自检:VEH 命中 %ld → %ld,报告 = %s,在磁盘上 = %d",
            before, after, named ? bb::g_last : "(没有路径)", onDisk ? 1 : 0);
    if (!hit)    return FL_ERR_NOT_INSTALLED;   // VEH 一次都没被调用到 ⇒ 没接上
    if (!onDisk) return FL_ERR_WRITE;           // 命中了,但报告没落在磁盘上
    return FL_OK;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        fl::g_self = module;
        log::init(module);
        bb::init(module);          // ★黑匣子:注册 VEH(补丁还没装也要能记崩溃)
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
