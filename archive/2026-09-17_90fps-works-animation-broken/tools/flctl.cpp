// flctl.cpp — Ra3FrameLab 控制台工具:把 DLL 注入正在运行的红警3,并调用它的开关 / 诊断接口。
//
// 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
// 用法:
//   flctl dryrun [帧率]     只检查不改任何字节,把「能不能装 / 卡在哪」写进日志(排障第一步)
//   flctl enable [帧率]     注入并启用(默认 60)
//   flctl disable           还原所有改动(DLL 留在进程里)
//   flctl status            查询当前状态(0 = 未启用,否则为当前目标帧率)
//   flctl diag              把诊断快照写进 DLL 日志
//   flctl unload            先还原再把 DLL 从游戏进程卸掉(换新版 DLL 时用)
//   flctl measure           只量不改:装帧计数器但不改帧率(自动化验证的基线组)
//   flctl frames            取渲染帧计数
//   flctl logicframe        取引擎自己的逻辑帧号(判游戏速度有没有变)
//   flctl desync <绝对帧>   让引擎在该逻辑帧吐出逐字段 sim 状态
//   flctl desyncrestore     还原 desync 两个开关
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

bool inject(HANDLE proc, const char* dllPath) {
    const size_t bytes = strlen(dllPath) + 1;
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, dllPath, bytes, &written) || written != bytes) return false;
    void* loadLib = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
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

    char dllPath[MAX_PATH];
    GetModuleFileNameA(NULL, dllPath, MAX_PATH);
    char* slash = strrchr(dllPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(dllPath, MAX_PATH, dllName);
    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        std::printf("找不到 %s(应与 flctl.exe 同目录)\n", dllPath);
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
        std::printf("注入 %s ...\n", dllPath);
        if (!inject(proc, dllPath)) {
            std::printf("注入失败(错误 %lu)\n", GetLastError());
            CloseHandle(proc);
            return 5;
        }
    }
    HMODULE remoteBase = remote_module(pid, dllName);
    HMODULE localBase = LoadLibraryA(dllPath);   // 本地加载一份只为算导出偏移
    if (!remoteBase || !localBase) {
        std::printf("拿不到模块基址(远端 %p 本地 %p)\n", (void*)remoteBase, (void*)localBase);
        CloseHandle(proc);
        return 6;
    }

    const char* exportName = "FrameLabStatus";
    void* arg = NULL;
    if (_stricmp(cmd, "enable") == 0) { exportName = "FrameLabEnable"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "dryrun") == 0) { exportName = "FrameLabDryRun"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "measure") == 0) exportName = "FrameLabMeasureOnly";
    else if (_stricmp(cmd, "groups") == 0) { exportName = "FrameLabSetGroups"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "frames") == 0) exportName = "FrameLabFrameCount";
    else if (_stricmp(cmd, "logicframe") == 0) exportName = "FrameLabLogicFrame";
    else if (_stricmp(cmd, "waitframe") == 0) { exportName = "FrameLabWaitFrame"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "fpswin") == 0) { exportName = "FrameLabFpsWindow"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "enginefps") == 0) exportName = "FrameLabEngineFps";
    else if (_stricmp(cmd, "desync") == 0) { exportName = "FrameLabDesyncDump"; arg = (void*)(uintptr_t)fps; }
    else if (_stricmp(cmd, "desyncrestore") == 0) exportName = "FrameLabDesyncRestore";
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
        if (result == 0) std::printf("已启用。日志在 DLL 旁边的 logs\\ 目录里。\n");
        else std::printf("未启用(错误码 %lu)。把 logs\\ 里的日志发回来就能定位。\n", result);
    } else if (_stricmp(cmd, "dryrun") == 0) {
        if (result == 0) std::printf("干扫通过:可以安装,游戏一个字节都没改。\n");
        else std::printf("干扫未通过(错误码 %lu)。日志里有错误码对照表。\n", result);
    }
    CloseHandle(proc);
    return (int)result;
}
