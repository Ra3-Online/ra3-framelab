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

    // 时钟推进量累加器的离线自检(2026-09-17 动画定位会话,Claude)。
    // 这段逻辑是纯整数算术,不需要游戏 —— 它保证「每逻辑帧精确推进 66.667 毫秒」。
    // 动机:引擎是每**客户端帧**推进一次,一个逻辑帧含 r 个客户端帧,于是旧的 1000÷帧率
    // 整数截断被 r 放大 —— r=4(60 帧)时 4×16=64,比 66.667 慢 4%,动画肉眼可见地偏慢。
    // 细节见 src/framelab.cpp 的 clock_accumulator_step()。
    FnVoid selftestClock = (FnVoid)GetProcAddress(h, "FrameLabSelfTestClock");
    check(selftestClock != NULL, "时钟累加器自检导出取得到");
    if (selftestClock) {
        const int rcClock = selftestClock();
        check((rcClock & 0x1F) == 0x1F,
              "时钟累加器:r=2..6 每个逻辑帧都推进 66 或 67 毫秒,3 帧合计恰好 200", rcClock);
        check((rcClock & 0x20) != 0,
              "时钟累加器·自检资格:同一条判据量旧公式会报不通过(闸门真的会红)", rcClock);
    }

    // 黑匣子自检(2026-09-18 第十一批,Claude)。
    // 动机:用户报「游戏本体崩溃 / 偶尔不同步」,而原有日志只记安装期的事(特征命中、改动字节)——
    //   出事那一刻什么都没留下,只能猜。
    // 这一段走**真实的 VEH 路径**:导出内部抛一个自定义异常码(0xE0BB0001),注册的向量化异常
    //   处理器应当记一笔并写一份 crash-*.log,然后它自己的 __try/__except 把它吞掉。
    // ⇒ 在**非红警3进程**里也能证明「异常真的会被记下来」,不需要起游戏。
    //   ★这正是「闸门必须能被证明会绿、也必须能被证明会红」那条纪律的用法:
    //     如果 VEH 注册失败,SelfTest 返回非 0,这里当场变红。
    FnInt  bbOn   = (FnInt)GetProcAddress(h, "FrameLabBlackBox");
    FnVoid bbDump = (FnVoid)GetProcAddress(h, "FrameLabBlackBoxDump");
    FnInt  bbTest = (FnInt)GetProcAddress(h, "FrameLabBlackBoxSelfTest");
    // FrameLabBlackBoxLast(char* out, int cap) —— 返回最近一次报告路径的长度。
    //   用它来验「报告路径是空的」这种失败:只看 VEH 命中计数的话,日志目录解析失败时
    //   自检会给出假 PASS(命中了、文件却不存在)。见 framelab.cpp 里 SelfTest 的注释。
    typedef int(__stdcall* FnLast)(char*, int);
    FnLast bbLast = (FnLast)GetProcAddress(h, "FrameLabBlackBoxLast");
    check(bbOn != NULL && bbDump != NULL && bbTest != NULL && bbLast != NULL,
          "黑匣子四个导出都取得到");
    if (bbOn && bbDump && bbTest && bbLast) {
        check(bbOn(1) == 0, "黑匣子:飞行记录仪可开启", bbOn(1));
        // 正面判据:自定义异常码必须被记下来,而且报告必须真的落在磁盘上。
        const int rcTest = bbTest(0);
        check(rcTest == 0, "黑匣子:VEH 被调用到,且报告真的写在磁盘上(走真实异常路径)", rcTest);
        char bbPath[600] = {0};
        const int nPath = bbLast(bbPath, (int)sizeof bbPath);
        check(nPath > 0 && bbPath[0] != 0, "黑匣子:最近报告路径非空", nPath);
        // 反面判据:良性码必须被过滤。这是实测踩过的坑 —— 日志走 OutputDebugString,
        //   而它抛 0x40010006;不过滤就是「每写一行日志生成一份崩溃报告」。
        //   光有正面判据测不出「过滤被改坏了」,而那恰恰是最容易回归的坏法。
        const int rcNeg = bbTest(1);
        check(rcNeg == 0, "黑匣子·反面判据:良性码(0x406D1388)被过滤掉,没被记成崩溃", rcNeg);
        const int rcDump = bbDump();
        check(rcDump == 0, "黑匣子:手动快照可写出", rcDump);
        check(bbOn(0) == 0, "黑匣子:飞行记录仪可关闭", bbOn(0));
    }

    // 车身外观 30 Hz 节拍门的离线自检(2026-09-21 悬挂路径会话,Claude)。
    // 动机:用户在真 90 帧的机器上录到「海啸坦克上下颠簸剧烈」。车身的悬挂/俯仰/侧倾由引擎的
    //   calcPhysicsXform 每**显示帧**递推一步、不含 dt ⇒ 90 帧下每秒递推 90 步(原版 30)。
    //   节拍门只在「30 Hz 虚拟帧」变化的显示帧放行原函数,其余帧回放缓存(src/chassis_gate.h)。
    // 这段逻辑是纯算术 + 一张定长表,不需要游戏:自检用一个与引擎同形的假弹簧递推来驱动它,
    //   判据是「任何目标帧率下,放行得到的位姿序列与 30 帧逐位相同」。
    // ★同一条纪律:闸门必须能被证明会红 —— 0x10 那一位同时断言「门关着的 90 帧按同一判据不通过」。
    FnVoid selftestChassis = (FnVoid)GetProcAddress(h, "FrameLabSelfTestChassis");
    FnInt  chassisRate     = (FnInt)GetProcAddress(h, "FrameLabChassisRate");
    typedef int(__stdcall* FnBuf)(char*);
    FnBuf  chassisSlots    = (FnBuf)GetProcAddress(h, "FrameLabChassisSlots");
    check(selftestChassis != NULL && chassisRate != NULL && chassisSlots != NULL, "车身外观三个导出都取得到");
    if (selftestChassis) {
        const int rcCh = selftestChassis();
        check((rcCh & 0x01) != 0, "车身门·目标 30:每帧放行、零回放(直通,与原版逐位一致)", rcCh);
        check((rcCh & 0x06) == 0x06, "车身门·目标 60 / 90:放行次数钉回每秒 30 步,位姿序列与 30 帧逐位相同", rcCh);
        check((rcCh & 0x08) != 0, "车身门·非整数倍(45 / 75):同一虚拟帧不重复放行,序列仍逐位相同", rcCh);
        check((rcCh & 0x10) != 0, "车身门·自检资格:门关着的 90 帧每帧放行,按同一判据不通过(闸门真的会红)", rcCh);
        check((rcCh & 0x20) != 0, "车身门·原函数返回 0:原样返回、不缓存、不回放", rcCh);
        check((rcCh & 0xC0) == 0xC0, "车身门·表满退回直通(不丢姿态)+ 陈旧格子可回收 / 重置", rcCh);
        // 2026-09-21 同日追加:插值模式(为 90 帧准备 —— 递推仍 30 Hz,显示时在相邻两个位姿之间按相位插值)。
        check((rcCh & 0x100) != 0, "车身门·插值(60/90/45/75):虚拟帧末的输出与引擎位姿逐位相同,中间帧不越界", rcCh);
        check((rcCh & 0x200) != 0, "车身门·插值边界:目标 30 直通;过期位姿不当插值起点", rcCh);
    }
    if (chassisRate) {
        // 没装包装时必须返回 -2(「没装」),而不是 -1(「没数据」)—— 两种情况绝不能长得一样。
        const int rcRate = chassisRate(1);
        check(rcRate == -2, "车身尺子:包装没装时返回 -2(「没装」≠「没数据」)", rcRate);
    }
    if (chassisSlots) {
        static char slotBuf[16384];
        const int rcSlots = chassisSlots(slotBuf);
        check(rcSlots == -2 && slotBuf[0] != 0, "车身全表:包装没装时返回 -2 且说明原因", rcSlots);
    }

    // 2026-09-23:粒子系统节拍门(电厂 / 矿场的光)。GUI 靠这个导出报「到底装没装上」——
    //   没装时必须是 0(关),不能是 -2(「开了但没装上」只在装过之后才有意义)。
    typedef int(__stdcall* FnStatus)();
    FnStatus psysStatus = (FnStatus)GetProcAddress(h, "FrameLabPsysStatus");
    check(psysStatus != NULL, "粒子系统节拍门的状态导出取得到");
    if (psysStatus) {
        const int rcPs = psysStatus();
        check(rcPs == 0, "粒子系统节拍门:没安装时状态 = 0(关),不是 -2", rcPs);
    }

    // 2026-09-26:过场运镜计时。没安装时必须是 0(关),不能是 -2。
    FnStatus camStatus = (FnStatus)GetProcAddress(h, "FrameLabCameraStatus");
    check(camStatus != NULL, "过场运镜计时的状态导出取得到");
    if (camStatus) {
        const int rcCam = camStatus();
        check(rcCam == 0, "过场运镜计时:没安装时状态 = 0(关),不是 -2", rcCam);
    }

    // 2026-09-28:单位闪烁 / 染色计时。没安装时必须是 0(关),不能是 -2。
    FnStatus tintStatus = (FnStatus)GetProcAddress(h, "FrameLabTintStatus");
    check(tintStatus != NULL, "单位闪烁 / 染色计时的状态导出取得到");
    if (tintStatus) {
        const int rcTint = tintStatus();
        check(rcTint == 0, "单位闪烁 / 染色计时:没安装时状态 = 0(关),不是 -2", rcTint);
    }

    const int rcDisable = disable();
    check(rcDisable == 1, "没装过时关闭返回「未安装」(1)", rcDisable);

    const int rcDiag = diag();
    check(rcDiag == 0, "诊断快照接口可调用", rcDiag);

    FreeLibrary(h);
    std::printf("---- %d PASS / %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
