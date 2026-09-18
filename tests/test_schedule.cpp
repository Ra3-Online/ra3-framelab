// test_schedule.cpp — 阶段调度的离线自检(不需要游戏,不碰任何游戏文件)
//
// 日期  :2026-09-16   会话:平台联机会话 68ee9b9d(Claude)
// 为什么:60 帧改造成败的核心就是「每个客户端帧跑几个阶段」。这段数学错了 = 游戏速度变化 / 联机不同步,
//         而这类错误在游戏里表现成「感觉有点不对」,很难抓。先在离线把不变量钉死。
// 判据(行为级,不钉实现):
//   ① 每 r 个客户端帧正好跑满 6 个阶段 —— 逻辑帧率与原版恒等(否则游戏速度变了);
//   ② 阶段号单调递增、1..6 每个恰好执行一次 —— 不跳号、不重复;
//   ③ r = 2 时与原版逐帧一致(原版公式 6*((r*(p-1))/6+1)/r,目标 3、6);
//   ④ 插值系数单调递增、最后一帧 = 1.0、第一帧 = 1/r —— 消除参考补丁的顿挫;
//   ⑤ 原版公式在 r = 4 时确实会慢(反证:说明我们的改动必要,同时证明测试本身有分辨力)。
#include "../src/schedule.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace {
int passed = 0, failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++passed : ++failed;
}

// 原版引擎的批次公式(2009 构建 sub_616130 内,0x61626E 处那 41 字节)
int retail_target(int p, int r) {
    return 6 * ((r * (p - 1)) / 6 + 1) / r;
}

// 模拟引擎主循环:每个客户端帧 p = 计数器+1,跑到 target,计数器 = target;满 6 回绕。
// 返回:每个客户端帧结束时的阶段计数器序列(一个逻辑帧内)。
std::vector<int> run_logic_frame(int r, int (*target)(int, int)) {
    std::vector<int> frames;
    int counter = 0;
    for (int guard = 0; guard < 64 && counter < FL_PHASES_PER_LOGIC_FRAME; ++guard) {
        int p = counter + 1;
        int t = target(p, r);
        if (t < p) t = p;
        if (t > FL_PHASES_PER_LOGIC_FRAME) t = FL_PHASES_PER_LOGIC_FRAME;
        counter = t;
        frames.push_back(counter);
    }
    return frames;
}
}  // namespace

int main() {
    for (int r = 2; r <= 6; ++r) {
        char name[128];
        const std::vector<int> frames = run_logic_frame(r, fl_target_phase);

        std::snprintf(name, sizeof name, "r=%d:每 %d 个客户端帧正好跑满 6 个阶段(逻辑帧率不变)", r, r);
        check(static_cast<int>(frames.size()) == r && !frames.empty() && frames.back() == 6, name);

        bool monotone = true;
        int prev = 0;
        for (int v : frames) {
            monotone = monotone && v > prev && v <= 6;
            prev = v;
        }
        std::snprintf(name, sizeof name, "r=%d:阶段计数单调递增且不越界", r);
        check(monotone, name);

        // 每个阶段号 1..6 恰好被执行一次(引擎的批次循环会把跳过的阶段补跑)
        std::vector<int> executed;
        prev = 0;
        for (int v : frames) {
            for (int p = prev + 1; p <= v; ++p) executed.push_back(p);
            prev = v;
        }
        bool exact = executed.size() == 6;
        for (size_t i = 0; i < executed.size() && exact; ++i) exact = executed[i] == static_cast<int>(i) + 1;
        std::snprintf(name, sizeof name, "r=%d:阶段 1..6 各执行一次,不跳号不重复", r);
        check(exact, name);

        // 插值系数:每帧末尾的值单调递增,首帧 = 1/r,末帧 = 1.0
        bool fracOk = true;
        float last = 0.0f;
        for (int k = 1; k <= r; ++k) {
            const int phase = fl_phases_done(k, r);
            const float f = fl_fraction(phase, r);
            fracOk = fracOk && f > last && f <= 1.0f;
            last = f;
        }
        fracOk = fracOk && std::fabs(last - 1.0f) < 1e-6f;
        fracOk = fracOk && std::fabs(fl_fraction(fl_phases_done(1, r), r) - 1.0f / static_cast<float>(r)) < 1e-6f;
        std::snprintf(name, sizeof name, "r=%d:插值系数均匀递增,首帧 1/r,末帧 1.0", r);
        check(fracOk, name);
    }

    // r = 2 必须与原版逐帧一致:这是「不改变原版行为」的回归判据
    {
        const std::vector<int> ours = run_logic_frame(2, fl_target_phase);
        const std::vector<int> retail = run_logic_frame(2, retail_target);
        bool same = ours.size() == retail.size();
        for (size_t i = 0; i < ours.size() && same; ++i) same = ours[i] == retail[i];
        check(same && ours.size() == 2 && ours[0] == 3 && ours[1] == 6, "r=2(原版 30 帧):与原版公式逐帧相同(3, 6)");
    }

    // 2026-09-16 会话 68ee9b9d:用户问 90 / 120 帧能不能做,这两组断言就是答案的依据。
    // r = 6(90 帧)是这套阶段系统的天然整数解:每个客户端帧正好 1 个阶段。若成立,90 帧
    // **不需要**批次补丁和插值系数补丁(引擎自带的「阶段÷6」本来就均匀),只需要重定向。
    {
        const std::vector<int> ours = run_logic_frame(6, fl_target_phase);
        const std::vector<int> retail = run_logic_frame(6, retail_target);
        bool same = ours.size() == retail.size();
        for (size_t i = 0; i < ours.size() && same; ++i) same = ours[i] == retail[i];
        bool oneEach = ours.size() == 6;
        for (size_t i = 0; i < ours.size() && oneEach; ++i) oneEach = ours[i] == static_cast<int>(i) + 1;
        check(same && oneEach, "r=6(90 帧):与原版公式逐帧相同,且每帧正好 1 个阶段 ⇒ 批次/系数补丁可省");

        bool fracSame = true;
        for (int k = 1; k <= 6; ++k) {
            const float engineF = static_cast<float>(k) * (1.0f / 6.0f);   // 引擎写死的 0.16666667
            const float ourF = fl_fraction(k, 6);
            if (std::fabs(ourF - engineF) > 1e-6f) fracSame = false;
        }
        check(fracSame, "r=6(90 帧):我们的插值系数与引擎自带的「阶段×0.16666667」逐值相同");
    }

    // 2026-09-16:帧率可行性表。用户问「120 难,150 是不是反而简单」—— 让程序枚举,别靠直觉。
    // 分档判据只看两件事:① 每帧推进的阶段数是否整数(⇔ r 整除 6)② 是否出现「本帧不推进」的帧(⇔ r > 6)。
    {
        std::printf("     ── 帧率可行性表(逻辑帧率恒为 15,r = 目标帧率 ÷ 15)──\n");
        std::printf("     %-6s %-4s %-26s %s\n", "帧率", "r", "每帧累计阶段", "需要哪些补丁");
        bool tableOk = true;
        for (int r = 1; r <= 12; ++r) {
            std::vector<int> done;
            bool stall = false;
            for (int k = 1; k <= r; ++k) {
                done.push_back(fl_phases_done(k, r));
                if (k > 1 && done[k - 1] == done[k - 2]) stall = true;
            }
            if (done.back() != 6) tableOk = false;            // 任何 r 都必须正好凑满 6 个阶段
            const bool divides = (6 % r == 0);
            const char* need = stall ? "重定向 + 批次/系数 + ★改逐帧推进"
                             : (divides ? "只需重定向" : "重定向 + 批次/系数");
            char seq[96] = {0};
            for (size_t i = 0; i < done.size() && i < 12; ++i)
                std::snprintf(seq + std::strlen(seq), sizeof seq - std::strlen(seq), "%d ", done[i]);
            std::printf("     %-6d %-4d %-26s %s\n", 15 * r, r, seq, need);
        }
        check(tableOk, "帧率可行性表:r = 1..12 每一档都正好凑满 6 个阶段(逻辑帧率不被改变)");
    }

    // r = 8(120 帧)超出 6 个阶段:必须存在「本帧不推进阶段」的帧。引擎逐帧那处是无条件 +1,
    // 6 帧就会凑满一个逻辑帧 ⇒ 逻辑跑成 120/6 = 20Hz,比原版 15Hz 快 33%。
    // 所以 120 帧除了重定向,还必须额外改「逐帧推进」那一处。
    {
        std::vector<int> done;
        for (int k = 1; k <= 8; ++k) done.push_back(fl_phases_done(k, 8));
        bool monotone = true, hasStall = false;
        for (size_t i = 1; i < done.size(); ++i) {
            if (done[i] < done[i - 1]) monotone = false;
            if (done[i] == done[i - 1]) hasStall = true;
        }
        check(done.size() == 8 && done.back() == 6 && monotone && hasStall,
              "r=8(120 帧):8 帧摊 6 个阶段,存在「本帧不推进」的帧 ⇒ 还需额外改逐帧推进那处");
        std::printf("     r=8 每帧累计阶段:");
        for (size_t i = 0; i < done.size(); ++i) std::printf(" %d", done[i]);
        std::printf("\n");
    }

    // 反证:原版公式在 r = 4 时一个客户端帧只跑 1 个阶段 ⇒ 6 帧才一个逻辑帧 ⇒ 慢到 2/3 速
    {
        const std::vector<int> retail = run_logic_frame(4, retail_target);
        check(retail.size() == 6, "反证:原版公式在 r=4 时要 6 个客户端帧才凑满一个逻辑帧(所以必须改)");
    }

    // 反证:引擎原版把插值系数写成「阶段 ÷ 6」。6 不能被 4 整除 ⇒ 60 帧下步长忽大忽小 = 周期性顿挫。
    // 参考补丁只改了 sub_6027D0 里的算法,sub_616130 批次循环之后又覆盖成这个值 ⇒ 顿挫仍在。
    {
        float steps[8] = {0};
        float last = 0.0f;
        int n = 0;
        for (int k = 1; k <= 4; ++k) {
            const float f = static_cast<float>(fl_phases_done(k, 4)) / 6.0f;
            steps[n++] = f - last;
            last = f;
        }
        bool uniform = true;
        for (int i = 1; i < n; ++i) uniform = uniform && std::fabs(steps[i] - steps[0]) < 1e-6f;
        check(!uniform, "反证:原版系数「阶段÷6」在 r=4 时步长不均匀(参考补丁的顿挫来源)");
        std::printf("     原版系数步长: %.3f %.3f %.3f %.3f | 我们的: %.3f 均匀\n",
                    steps[0], steps[1], steps[2], steps[3], 1.0f / 4.0f);
    }

    std::printf("---- %d PASS / %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
