// sim_gate.h — 2026-09-28:「逐客户端帧执行、却会碰到模拟」的工作按原版相位边界放行(FL_G_SIMGATE),
// 以及迷雾可见性 / 模型状态 / 残影更新的原版相位测试。纯逻辑,DLL 与 tests/test_schedule.cpp 共用。
//
// 背景(HANDOFF §27、audit-2026-09-28/):原版每个逻辑帧固定 2 个客户端帧,6 个阶段分两批 [1,2,3] [4,5,6];
// 引擎在**每个客户端帧开头**(sub_6027D0 → vt+148 = sub_626620)做几件会改模拟状态的延迟工作,
// 所以原版里它们永远落在「阶段 3 之后」或「阶段 6 之后」(外加不推进任何东西的空转帧)。
// 补丁把 r 提到 4 / 6 后,客户端帧边界变成 {2,3,5,6} / {1..6},这些工作就挪到了别的阶段之间 ⇒ 与原版不同步。
//
// 读的是引擎 +0x58 = sub_6027D0 的 this[22] = 上一个客户端帧**结束时**已完成的阶段号(1..6;空转帧 >= 7)。
#ifndef FL_SIM_GATE_H
#define FL_SIM_GATE_H

#include "schedule.h"

#define FL_SIM_PHASE_OFFSET 0x58   // 引擎(dword_CE2F8C)里的「已完成阶段」字段

// 变换冲刷 / ModelCondition 同步:原版只在阶段 3、6 之后(以及空转帧)跑。
// lastPhase <= 0 视为开局初值,放行(原版开局第一帧也会跑,且那一刻还没有任何阶段推进过)。
static int fl_sim_boundary_ok(int lastPhase) {
    return lastPhase <= 0 || lastPhase == 3 || lastPhase >= FL_PHASES_PER_LOGIC_FRAME;
}

// 销毁表:请求在阶段 5 发出,原版只在阶段 6 之后的帧开头消费。
static int fl_sim_destroy_ok(int lastPhase) {
    return lastPhase <= 0 || lastPhase >= FL_PHASES_PER_LOGIC_FRAME;
}

// 本 r 下「第一个 >= 3 的客户端帧边界」:原版(r=2)是 3;r=4 / 6 也是 3;r=3 / 5(45 / 75 帧)是 4。
// 迷雾可见性那条相位测试用它 ⇒ 任何 r 下都是「每逻辑帧恰好一次、且尽量贴近原版的阶段 3」。
static int fl_vis_phase(int r) {
    int k;
    if (r <= 1) return FL_PHASES_PER_LOGIC_FRAME;
    for (k = 1; k <= r; ++k) {
        const int p = fl_phases_done(k, r);
        if (p >= 3) return p;
    }
    return FL_PHASES_PER_LOGIC_FRAME;
}

// 边界门能否做到与原版逐位等价:要求本 r 的客户端帧边界里**有**阶段 3(r = 2 / 4 / 6,即 30 / 60 / 90 帧)。
static int fl_sim_gate_exact(int r) {
    return fl_vis_phase(r) == 3;
}

#endif
