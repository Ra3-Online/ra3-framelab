// flctl.cpp — Ra3FrameLab 控制台工具:把 DLL 注入正在运行的红警3,并调用它的开关 / 诊断接口。
//
// 日期:2026-09-16  初始研究实现
// 用法:
//   flctl dryrun [帧率]     只检查不改任何字节,把「能不能装 / 卡在哪」写进日志(排障第一步)
//   flctl enable [帧率]     注入并启用(默认 60)
//   flctl disable           还原所有改动(DLL 留在进程里)
//   flctl status            查询当前状态(0 = 未启用,否则为当前目标帧率)
//   flctl simstatus         查询模拟相位修正安装位(1=可见性,2=高度pin,4=边界门,8=原R4);不是联机安全验收
//   flctl diag              把诊断快照写进 DLL 日志
//   flctl unload            先还原再把 DLL 从游戏进程卸掉(换新版 DLL 时用)
//   flctl measure           只量不改:装帧计数器但不改帧率(自动化验证的基线组)
//   flctl frames            取渲染帧计数
//   flctl logicframe        取引擎自己的逻辑帧号(判游戏速度有没有变)
//   flctl desync <绝对帧>   让引擎在该逻辑帧吐出逐字段 sim 状态
//   flctl desyncrestore     还原 desync 两个开关
//   flctl read <0x地址>     读任意全局的 4 字节(只读;量时钟/帧率全局用。地址要带 0x)
//   flctl animrate <逻辑帧> 量这段区间里动画对象「每逻辑帧推进多少帧」×1000(2026-09-17 新增)
//   flctl animreset         清空动画标尺记下的对象(换配置前后各调一次;2026-09-17 新增)
//   flctl logicfps <秒>     进程内量引擎逻辑帧率 ×100(任何渲染帧率下都应 = 1500)。
//                           ★这是「模拟没被改」的定义,别用 enginefps 代替 —— 后者读的是
//                           引擎的指数平滑显示值,滞后于真实值。(2026-09-17 验收口径会话新增)
//   flctl clockrate <逻辑帧> ★主判据:每逻辑帧推进多少毫秒游戏时钟 ×1000。原版与正确的高帧率
//                           配置都应是 66667;若约 200000 ⇒ 时钟快 3 倍,动画就会快 3 倍。
//                           (2026-09-17 新增;见 src/framelab.cpp 的 FrameLabClockPerLogicFrame)
//   flctl blendrate <逻辑帧> ★第二把动画时间尺子:每逻辑帧推进多少毫秒**混合斜坡**动画时间 ×1000。
//                           建筑建造/揭幕走的是这条路径(sub_903910 → sub_8EA220),
//                           它**不经过游戏时钟**,所以 clockrate 看不见它。正确值 66667;
//                           老算法(除数仍是 30)在 60 帧下会给 133333 ⇒ 动画时间跑一倍。
//                           (2026-09-17 P6b 会话新增;见 FrameLabBlendStepPerLogicFrame)
//   flctl displayclock <毫秒> ★第七批(建筑动画真因)的**前提尺子**:实测显示器帧计数器
//                           (vtbl[116](dword_CDB750))每秒 tick 多少次,并打印引擎三个
//                           「帧率派生量」0xCDBC50/54/5C 的当前值。判据:实测 ≈ 客户端帧率
//                           ⇒ 三个派生量必须整体按目标帧率重算(开 FL_G_DERIVED2 = 0xE00),
//                           否则「毫秒↔帧号」换算差一倍,建筑解包动画快一倍跑完。
//                           (2026-09-17 第七批新增;见 FrameLabDisplayClock)
//   flctl groups <掩码>      逐组开关;**十进制或 0x 十六进制都行**(2026-09-21 起按 0x 前缀解析,
//                           此前 atoi("0x…") 静默得到 0 ⇒ 回落默认集合)。必须在 enable / measureex **之前**调。
//   flctl chassisrate <逻辑帧> ★车身(悬挂/俯仰/侧倾)路径的主判据:同一辆车每逻辑帧递推几步 ×1000。
//                           原版与修好的配置 = 2000;没修的 60 / 90 帧 = 4000 / 6000。
//                           需要先开探针位 0x80000(或节拍门位 0x100000)。-2 = 包装没装。
//                           (2026-09-21 悬挂路径会话新增;见 FrameLabChassisRate)
//   flctl chassisslots       倒出被车身外观包装见过的载具全表(外观枚举、调用/放行/回放、最近输出)。
// 所有命令都可以在最后再跟一个**进程号**,指定就只动那个进程(自动化时必须给,免得误伤用户正在玩的那局)。
// 说明:只对**已经在运行**的游戏进程操作;不改游戏文件,不随游戏自动启动。
//       找不到游戏进程就直接退出,不会去动别的程序。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cwchar>

namespace {

DWORD find_game(const char* wanted) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_strnicmp(pe.szExeFile, wanted, strlen(wanted)) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// 在目标进程里找已加载的模块基址
HMODULE remote_module(DWORD pid, const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    HMODULE found = NULL;
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, name) == 0) { found = me.hModule; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// 在目标进程里跑一个函数,返回它的返回值(32 位进程:线程退出码就是 eax)
bool call_remote(HANDLE proc, void* addr, void* arg, DWORD* result) {
    HANDLE th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)addr, arg, 0, NULL);
    if (!th) return false;
    WaitForSingleObject(th, 200000);   // waitframe / fpswin block inside the game; 15s is far too short
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    if (result) *result = code;
    return true;
}

bool inject(HANDLE proc, const wchar_t* dllPath) {
    const size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, dllPath, bytes, &written) || written != bytes) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    void* loadLib = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    DWORD result = 0;
    const bool ok = call_remote(proc, loadLib, remote, &result) && result != 0;
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const char* cmd = argc > 1 ? argv[1] : "status";
    const int fps = argc > 2 ? atoi(argv[2]) : 60;
    // 2026-09-16:第三个参数 = 目标进程号。自动化验证时必须指定,
    // 否则按进程名找会**误伤用户自己正在玩的那一局**(平台起的游戏同名)。
    const DWORD wantPid = argc > 3 ? (DWORD)strtoul(argv[3], NULL, 10) : 0;
    const char* gameName = "ra3_1.12";      // 进程名前缀
    const char* dllName = "Ra3FrameLab.dll";

    wchar_t dllPath[MAX_PATH];
    const DWORD pathLen = GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    if (!pathLen || pathLen >= MAX_PATH) { std::printf("Executable path is too long.\n"); return 2; }
    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (slash) *(slash + 1) = 0;
    if (wcscat_s(dllPath, MAX_PATH, L"Ra3FrameLab.dll") != 0) return 2;
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        std::printf("找不到 %ls(应与 flctl.exe 同目录)\n", dllPath);
        return 2;
    }

    const DWORD pid = wantPid ? wantPid : find_game(gameName);
    if (!pid) {
        std::printf("没找到游戏进程(进程名以 %s 开头)。先把游戏开起来再运行本工具。\n", gameName);
        return 3;
    }
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        std::printf("打不开进程 %lu(错误 %lu)。试试用管理员身份运行。\n", pid, GetLastError());
        return 4;
    }
    std::printf("游戏进程 pid = %lu\n", pid);

    // unload:把 DLL 从游戏里卸掉(DllMain 的 PROCESS_DETACH 会先整体回滚补丁,再卸载)。
    // 开发期换新版 DLL 必须先走这一步 —— LoadLibrary 同一路径只会返回已加载的那份,不会重新读盘。
    if (_stricmp(cmd, "unload") == 0) {
        HMODULE mod = remote_module(pid, dllName);
        if (!mod) { std::printf("游戏里本来就没有 %s,无需卸载\n", dllName); CloseHandle(proc); return 0; }
        void* freeLib = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FreeLibrary");
        DWORD r = 0;
        const bool ok = call_remote(proc, freeLib, (void*)mod, &r);
        const bool gone = remote_module(pid, dllName) == NULL;
        std::printf("卸载%s(FreeLibrary 返回 %lu,模块%s)\n",
                    ok && gone ? "成功" : "失败", r, gone ? "已消失" : "★仍在");
        CloseHandle(proc);
        return (ok && gone) ? 0 : 9;
    }

    if (!remote_module(pid, dllName)) {
        std::printf("注入 %ls ...\n", dllPath);
        if (!inject(proc, dllPath)) {
            std::printf("注入失败(错误 %lu)\n", GetLastError());
            CloseHandle(proc);
            return 5;
        }
    }
    HMODULE remoteBase = remote_module(pid, dllName);
    HMODULE localBase = LoadLibraryW(dllPath);   // 本地加载一份只为算导出偏移
    if (!remoteBase || !localBase) {
        std::printf("拿不到模块基址(远端 %p 本地 %p)\n", (void*)remoteBase, (void*)localBase);
        CloseHandle(proc);
        return 6;
    }

    const char* exportName = "FrameLabStatus";
    void* arg = NULL;
    if (_stricmp(cmd, "enable") == 0) { exportName = "FrameLabEnable"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "simstatus") == 0) exportName = "FrameLabSimStatus";
    else if (_stricmp(cmd, "dryrun") == 0) { exportName = "FrameLabDryRun"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "measure") == 0) exportName = "FrameLabMeasureOnly";
    // * measureex <groups>: "measure only" WITH an explicit groups mask. Needed because
    //   `measure` alone calls install() and thereby sets g_installed, after which a later
    //   `groups`+`enable` is rejected with FL_ERR_ALREADY -- which silently left the
    //   animation probe uninstalled and the vanilla baseline's animation axis empty.
    else if (_stricmp(cmd, "measureex") == 0) {
        exportName = "FrameLabMeasureOnlyEx";
        arg = (void*)(uintptr_t)strtoul(argc > 2 ? argv[2] : "0", NULL, 0);
    }
    // ★ 2026-09-21 订正:`groups` 必须按 0x 前缀解析(strtoul base 0,十进制照旧可用)。
    //   原来走的是上面那个 `fps = atoi(argv[2])` —— atoi("0x2CFFE") = 0,而 FrameLabSetGroups(0)
    //   的含义是「取默认集合」⇒ **静默回落到 0x28FFF,不报错**。tools\mask_fps_probe.ps1 传的正是
    //   十六进制字符串,所以它那一轮五条臂实际全是默认掩码(DLL 日志里每一臂都记着 0x28FFF),
    //   已归档的研究 §18.6b「五条臂全部真的跑在 60 帧」那张表因此无效。与下面 `read` / `track` 同一个坑。
    else if (_stricmp(cmd, "groups") == 0) {
        exportName = "FrameLabSetGroups";
        arg = (void*)(uintptr_t)strtoul(argc > 2 ? argv[2] : "0", NULL, 0);
    }
    else if (_stricmp(cmd, "frames") == 0) exportName = "FrameLabFrameCount";
    else if (_stricmp(cmd, "logicframe") == 0) exportName = "FrameLabLogicFrame";
    else if (_stricmp(cmd, "waitframe") == 0) { exportName = "FrameLabWaitFrame"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "fpswin") == 0) { exportName = "FrameLabFpsWindow"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "enginefps") == 0) exportName = "FrameLabEngineFps";
    // 2026-09-17 验收口径会话:进程内量引擎逻辑帧率(×100)。
    // ★别用 enginefps 代替它:enginefps 读的是引擎自己的**指数平滑显示值**,滞后于真实值
    //   (实测渲染 59.87 时它报 7.81,而 59.87/6 = 9.98)。判「模拟有没有被改」要用这个。
    else if (_stricmp(cmd, "logicfps") == 0) { exportName = "FrameLabLogicFps"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "gatestats") == 0) exportName = "FrameLabGateStats";
    else if (_stricmp(cmd, "sampleanim") == 0) { exportName = "FrameLabSampleAnim"; arg = (void*)(uintptr_t)fps; }
    // 2026-09-17 动画定位会话新增三个只读命令(见 framelab.cpp 里同名导出的注释):
    //   animrate  <逻辑帧数>  量这段区间里动画对象每逻辑帧推进多少帧(核心读数,×1000)
    //   animreset             清空标尺记下的对象(换配置前后各来一次)
    //   read      <0x地址>    读任意全局的 4 字节(量时钟/帧率全局用)
    else if (_stricmp(cmd, "animrate") == 0) { exportName = "FrameLabAnimRate"; arg = (void*)(uintptr_t)fps; }
    // 2026-09-17:本轮的主判据 —— 每逻辑帧推进多少毫秒游戏时钟(×1000)。见 framelab.cpp 同名导出。
    else if (_stricmp(cmd, "clockrate") == 0) { exportName = "FrameLabClockPerLogicFrame"; arg = (void*)(uintptr_t)fps; }
    // 2026-09-17 P6b 会话:第二把动画时间尺子 —— 量**混合斜坡**那条路径
    //   (sub_903910 → sub_8EA220,dt = 帧号增量 ÷ 客户端帧率)每逻辑帧推进多少毫秒(×1000)。
    // ★为什么不能用 clockrate 代替:clockrate 量的是**游戏时钟**(sub_90ECF0)那条路径,
    //   建筑建造/揭幕动画走的是这条;除数 30 改 60,clockrate 一个数都不会变。
    //   正确值 66667;老算法(÷30)在 60 帧下会给 133333。
    else if (_stricmp(cmd, "blendrate") == 0) { exportName = "FrameLabBlendStepPerLogicFrame"; arg = (void*)(uintptr_t)fps; }
    // 2026-09-17 第七批:显示器帧计数器速率 + 三个帧率派生量的当前值(只读,不改任何东西)。
    // 参数是**采样毫秒数**(不是逻辑帧数),默认 600。
    else if (_stricmp(cmd, "displayclock") == 0) { exportName = "FrameLabDisplayClock"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "animreset") == 0) exportName = "FrameLabAnimReset";
    else if (_stricmp(cmd, "read") == 0) {
        // ★这里必须按 0x 前缀解析:atoi("0xCE1388") 会得到 0,静默读错地址比读不到更糟。
        exportName = "FrameLabReadDword";
        arg = (void*)(uintptr_t)strtoul(argc > 2 ? argv[2] : "0", NULL, 0);
    }
    else if (_stricmp(cmd, "desync") == 0) { exportName = "FrameLabDesyncDump"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "desyncrestore") == 0) exportName = "FrameLabDesyncRestore";
    // 2026-09-18(第十一批)黑匣子。用户的诉求是「记录游戏本体崩溃/不同步的原因」——
    //   现有的日志只记安装期的事,出事那一刻什么都没留下。
    //   blackbox on|off   开/关飞行记录仪(每渲染帧一行环形缓冲)
    //   blackboxdump      立刻倒一份现场快照(snapshot-*.log);**看到「不同步」弹窗就马上敲它**
    //   blackboxtest      自检:走真实的 VEH 路径(自定义异常码 + 自己吞掉),证明它真的会写文件
    else if (_stricmp(cmd, "blackbox") == 0) { exportName = "FrameLabBlackBox"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "blackboxdump") == 0) exportName = "FrameLabBlackBoxDump";
    else if (_stricmp(cmd, "blackboxtest") == 0) exportName = "FrameLabBlackBoxSelfTest";
    // 2026-09-18(第十一批·续)动画槽全表。★**连续敲两次、间隔约 1 秒**,对比「当前」列:
    //   变的那个槽才是「正在动的对象」。单次只能看到「谁登记过」,看不到「谁在动」。
    //   为什么需要它:黑匣子只记前 4 个槽,而实测那 4 个两次采到的都是**静态**对象
    //   (分数整段不变),可「切动画」钩子每秒被走 24 次 ⇒ 在动的对象在后面的槽里。
    else if (_stricmp(cmd, "animslots") == 0) exportName = "FrameLabAnimSlots";
    // 2026-09-18(第十一批·续)世界坐标扫描。用户答「移动时抖,静止时不抖」⇒ 问题在**位置**
    //   路径上,而项目里没有任何能读「单位世界坐标」的量具。这条命令扫可写内存,筛出
    //   「1 秒内在移动」的三个相邻 float。和 animslots 一样是「带输出缓冲」的命令。
    //   ⚠ 它内部会 Sleep(1000),所以远程调用要等约 1 秒。
    else if (_stricmp(cmd, "memscan") == 0) exportName = "FrameLabMemScan";
    // 2026-09-18(第十一批·续)坐标跟踪。把 memscan 找到的地址交给黑匣子,让它逐渲染帧
    //   记下那个地址上的三个 float。**这是「移动时上下抖动」的唯一直接读数。**
    //   和 `read` 一样按 0x 前缀解析地址(atoi("0x...") 会静默得到 0,比读不到更糟)。
    //   `track 0` 表示清空。
    else if (_stricmp(cmd, "track") == 0) {
        exportName = "FrameLabTrack";
        arg = (void*)(uintptr_t)strtoul(argc > 2 ? argv[2] : "0", NULL, 0);
    }
    // 2026-09-21(悬挂路径会话)车身外观的两把尺子(先 `groups` 开 0x80000 探针位,再 enable / measureex):
    //   chassisrate  <逻辑帧>  ★车身路径的主判据:同一辆车每逻辑帧递推几步 ×1000。
    //                          原版 / 修好的配置 = 2000;没修的 60 / 90 帧 = 4000 / 6000。
    //                          -1 = 没有可用读数;-2 = 包装没装(两者不是一回事)。
    //   chassisslots           「带输出缓冲」的命令:倒出被包装函数见过的载具全表
    //                          (Drawable 地址、locomotor 外观枚举、调用/放行/回放次数、最近一次四个输出)。
    else if (_stricmp(cmd, "chassisrate") == 0) { exportName = "FrameLabChassisRate"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "chassisslots") == 0) exportName = "FrameLabChassisSlots";
    else if (_stricmp(cmd, "disable") == 0) exportName = "FrameLabDisable";
    else if (_stricmp(cmd, "diag") == 0) exportName = "FrameLabDumpDiagnostics";

    void* localProc = (void*)GetProcAddress(localBase, exportName);
    if (!localProc) {
        std::printf("DLL 里没有导出 %s\n", exportName);
        CloseHandle(proc);
        return 7;
    }
    void* remoteProc = (void*)((unsigned char*)remoteBase + ((unsigned char*)localProc - (unsigned char*)localBase));
    DWORD result = 0;

    // ★ animslots 是个特例:它要往「目标进程里的输出缓冲」写字符串,而通用路径的 arg
    //   只能是一个整数(CreateRemoteThread 只传一个 lpParameter)。所以这里自己分配缓冲、
    //   自己调用、自己读回来打印,不走下面那条通用路径。
    // ★ 这两条命令都需要「目标进程里的输出缓冲」,走同一条特例路径。
    //   (通用路径的 arg 只能是一个整数,因为 CreateRemoteThread 只传一个 lpParameter。)
    if (_stricmp(cmd, "animslots") == 0 || _stricmp(cmd, "memscan") == 0 || _stricmp(cmd, "chassisslots") == 0) {
        const SIZE_T kCap = 16384;  // 必须与 DLL 里的 FL_ANIM_SLOTS_CAP / FL_MEMSCAN_CAP 一致
                                    // (4096 会让 memscan 的报告被静默截断,见 framelab.cpp 的注释)
        void* rbuf = VirtualAllocEx(proc, NULL, kCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!rbuf) {
            std::printf("分配远端缓冲失败(错误 %lu)\n", GetLastError());
            CloseHandle(proc);
            return 9;
        }
        if (!call_remote(proc, remoteProc, rbuf, &result)) {
            std::printf("远程调用 %s 失败(错误 %lu)\n", exportName, GetLastError());
            VirtualFreeEx(proc, rbuf, 0, MEM_RELEASE);
            CloseHandle(proc);
            return 8;
        }
        // ★ 本地接收缓冲必须和 kCap 一样大。踩过的坑:远端缓冲改成 16384 之后,
        //   这里忘了同步改 —— 仍是 char local[4096],而 ReadProcessMemory 按 kCap-1
        //   读了 16383 字节 ⇒ **栈越界**,flctl 直接崩掉、什么都不打印。
        //   表现是「memscan 的输出完全为空」,看起来像「没找到候选」,其实是我们自己写坏了。
        //   用 static 而不是栈上,免得 16 KB 压栈(虽然默认 1 MB 栈也够)。
        static char local[16384] = {0};
        SIZE_T got = 0;
        if (ReadProcessMemory(proc, rbuf, local, kCap - 1, &got)) {
            local[kCap - 1] = 0;
            std::printf("%s", local);
        } else {
            std::printf("读回缓冲失败(错误 %lu)\n", GetLastError());
        }
        // 纯 ASCII 的机器可读行,和别的命令保持一致,便于脚本解析。
        std::printf("RESULT=%ld\n", (long)(int)result);
        VirtualFreeEx(proc, rbuf, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 0;
    }

    if (!call_remote(proc, remoteProc, arg, &result)) {
        std::printf("远程调用 %s 失败(错误 %lu)\n", exportName, GetLastError());
        CloseHandle(proc);
        return 8;
    }
    std::printf("%s 返回 %lu\n", exportName, result);
    // 纯 ASCII 的机器可读行:自动化脚本靠它解析,不依赖中文(脚本文件必须是 ASCII,
    // 而且控制台代码页会把中文输出搞乱,拿中文当判据是给自己挖坑)。
    std::printf("RESULT=%ld\n", (long)(int)result);
    if (_stricmp(cmd, "enable") == 0) {
        if (result == 0) {
            if (fps == 60 || fps == 90) {
                void* localStatus = (void*)GetProcAddress(localBase, "FrameLabSimStatus");
                void* remoteStatus = localStatus ? (void*)((unsigned char*)remoteBase +
                    ((unsigned char*)localStatus - (unsigned char*)localBase)) : NULL;
                DWORD sim = 0;
                if (!remoteStatus || !call_remote(proc, remoteStatus, NULL, &sim) || (int)sim <= 0 || !(sim & 4)) {
                    std::printf("必需的相位边界门未确认安装。请检查 DLL 版本和日志。\n");
                    CloseHandle(proc);
                    return 24;
                }
                std::printf("SIMSTATUS=%ld\n", (long)(int)sim);
                std::printf("60/90 必需相位边界门已安装3/3；联机验证范围见技术报告。\n");
            }
            std::printf("已启用。日志在 DLL 旁边的 logs\\ 目录里。\n");
        }
        else std::printf("未启用(错误码 %lu)。把 logs\\ 里的日志发回来就能定位。\n", result);
    } else if (_stricmp(cmd, "dryrun") == 0) {
        if (result == 0) std::printf("干扫通过:可以安装,游戏一个字节都没改。\n");
        else std::printf("干扫未通过(错误码 %lu)。日志里有错误码对照表。\n", result);
    }
    CloseHandle(proc);
    return (int)result;
}
