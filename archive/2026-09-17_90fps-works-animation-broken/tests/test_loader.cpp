// test_loader.cpp — 安全性自检:把 Ra3FrameLab.dll 加载到一个**不是红警3**的进程里。
//
// 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
// 判据:① DLL 能加载、导出名字正确;② 在错误的进程里必须**拒绝安装**并返回明确错误码(而不是乱改内存或崩);
//       ③ 诊断接口能把快照写进日志;④ 没装过时关闭请求返回「未安装」。
// 这条自检不需要游戏,也不碰游戏任何文件。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* what, int got = 0) {
    std::printf("%s %s (返回 %d)\n", ok ? "PASS" : "FAIL", what, got);
    ok ? ++passed : ++failed;
}
}  // namespace

typedef int(__stdcall* FnInt)(int);
typedef int(__stdcall* FnVoid)();
typedef const char*(__stdcall* FnStr)();

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "Ra3FrameLab.dll";
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        std::printf("FAIL 加载不了 %s(错误 %lu)\n", dll, GetLastError());
        return 2;
    }
    check(true, "DLL 加载成功");

    FnStr version = (FnStr)GetProcAddress(h, "FrameLabVersion");
    FnInt enable = (FnInt)GetProcAddress(h, "FrameLabEnable");
    FnVoid disable = (FnVoid)GetProcAddress(h, "FrameLabDisable");
    FnVoid status = (FnVoid)GetProcAddress(h, "FrameLabStatus");
    FnVoid diag = (FnVoid)GetProcAddress(h, "FrameLabDumpDiagnostics");
    check(version && enable && disable && status && diag, "五个导出都取得到");
    if (!version || !enable || !disable || !status || !diag) return 2;

    std::printf("     版本 = %s\n", version());
    check(status() == 0, "初始状态 = 未安装", status());

    const int rc = enable(60);
    // 20 = 特征没找到(正确行为:这个进程不是红警3);11 = 不是 32 位 PE。绝不能返回 0(装上了)。
    check(rc == 20 || rc == 11, "在非红警3 进程里安装被拒绝(错误码 20 / 11)", rc);
    check(status() == 0, "被拒绝后仍是未安装状态", status());

    // 干扫必须和安装走同一套判据:在同一个非红警3 进程里,它也必须给出同样的拒绝码,
    // 且绝不能把状态改成「已安装」。判据不一致的话,用户日志里的「干扫通过」就没有意义了。
    // 日期:2026-09-16  会话:平台联机会话 68ee9b9d(Claude)
    FnInt dryrun = (FnInt)GetProcAddress(h, "FrameLabDryRun");
    check(dryrun != NULL, "干扫导出取得到");
    if (dryrun) {
        const int rcDry = dryrun(60);
        check(rcDry == rc, "干扫与安装给出同一个拒绝码", rcDry);
        check(status() == 0, "干扫之后仍是未安装状态", status());
    }

    // 还原核验闸门的**故障注入**:证明它真的会红,而不只是"跑完没报错"。
    // 2026-09-16 会话 68ee9b9d —— 判据从「mem::write 返回成功」改成「逐字节 memcmp 回原样」之后,
    // 新判据必须自己先红一次才有资格。全程只动 DLL 自己进程里的一小块内存,不需要游戏。
    FnVoid selftest = (FnVoid)GetProcAddress(h, "FrameLabSelfTestRollback");
    check(selftest != NULL, "还原核验自检导出取得到");
    if (selftest) {
        const int rcSelf = selftest();
        check((rcSelf & 1) != 0, "还原核验·诚实路径:改完再还原,逐字节回到原样", rcSelf);
        check((rcSelf & 2) != 0, "还原核验·故障注入:写入谎称成功时闸门当场报红", rcSelf);
    }

    const int rcDisable = disable();
    check(rcDisable == 1, "没装过时关闭返回「未安装」(1)", rcDisable);

    const int rcDiag = diag();
    check(rcDiag == 0, "诊断快照接口可调用", rcDiag);

    FreeLibrary(h);
    std::printf("---- %d PASS / %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
