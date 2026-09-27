// schedule.h — 60 帧改造的「阶段调度」纯数学(DLL 与离线自检共用,零依赖)
//
// 日期  :2026-09-16   会话:平台联机会话 68ee9b9d(Claude)   目录:G:\Ra3 FrameLab(与其它成果隔离)
// 为什么:引擎在两个逻辑帧之间用 **6 个阶段** 插值,每个客户端帧应推进 `6 ÷ r` 个阶段(r = 客户端帧率 ÷ 逻辑帧率)。
//         原版 r = 2 ⇒ 每帧 3 个阶段,刚好整除;r = 4(60 帧)⇒ 6 ÷ 4 在整数运算里等于 1 ⇒ 6 个客户端帧才凑满一个
//         逻辑帧 ⇒ 游戏慢到 2/3 速。所以必须自己算「这一帧该跑到第几个阶段」。
// 实现什么:
//   fl_phases_done(k, r)  第 k 个客户端帧结束时,本逻辑帧累计应完成的阶段数(四舍五入,k = r 时恒为 6)
//   fl_frame_index(p, r)  阶段号 p 属于本逻辑帧的第几个客户端帧
//   fl_target_phase(p, r) 引擎批次循环的目标阶段(= 本帧结束时的阶段号)
//   fl_fraction(p, r)     插值系数 = k ÷ r(均匀步进;引擎原版写的是 阶段 ÷ 6,在 r 不整除 6 时会忽快忽慢)
// 约束:纯整数推导 + 一次除法得出的浮点,**不读时钟、不依赖调用顺序**,各客户端算出的值逐位相同。
#ifndef RA3_FRAMELAB_SCHEDULE_H
#define RA3_FRAMELAB_SCHEDULE_H

#define FL_PHASES_PER_LOGIC_FRAME 6

// 第 k 个客户端帧(1..r)结束时应完成的累计阶段数。四舍五入保证:单调不减、k=r 时正好 6。
static int fl_phases_done(int k, int r) {
    if (r <= 0) return FL_PHASES_PER_LOGIC_FRAME;
    if (k <= 0) return 0;
    if (k >= r) return FL_PHASES_PER_LOGIC_FRAME;
    return (FL_PHASES_PER_LOGIC_FRAME * k + r / 2) / r;
}

// 阶段号 p(1..6)落在第几个客户端帧:最小的 k 使 fl_phases_done(k, r) >= p。
static int fl_frame_index(int p, int r) {
    int k;
    if (r <= 0) return 1;
    if (p < 1) p = 1;
    for (k = 1; k <= r; ++k) {
        if (fl_phases_done(k, r) >= p) return k;
    }
    return r;
}

// 本客户端帧要跑到的目标阶段号(引擎批次循环的上界)。永远 >= p,且 <= 6。
static int fl_target_phase(int p, int r) {
    int target;
    if (r <= 1) return FL_PHASES_PER_LOGIC_FRAME;
    if (p < 1) p = 1;
    if (p > FL_PHASES_PER_LOGIC_FRAME) return p;
    target = fl_phases_done(fl_frame_index(p, r), r);
    return target < p ? p : target;
}

// 插值系数 = 本逻辑帧内已过的客户端帧数 ÷ r,范围 (0, 1]。
static float fl_fraction(int p, int r) {
    int k;
    if (r <= 0) return 1.0f;
    if (p < 1) p = 1;
    if (p >= FL_PHASES_PER_LOGIC_FRAME) return 1.0f;
    k = fl_frame_index(p, r);
    if (k >= r) return 1.0f;
    return (float)k / (float)r;
}

#endif
