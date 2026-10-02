// chassis_gate.h — 车身外观(悬挂 / 俯仰 / 侧倾)「30 Hz 节拍门」的纯逻辑(DLL 与离线自检共用,零依赖)
//
// 初始实现:2026-09-21。验证边界见 docs/TECHNICAL.md。
// 为什么:引擎的车身外观计算(sub_53F080 Drawable::draw → sub_535DD0 applyPhysicsXform →
//         sub_532CA0 calcPhysicsXform → sub_526BD0 / sub_51E800 / sub_5262D0)是一组
//         **每显示帧递推一次、不含 dt** 的迭代:俯仰/侧倾弹簧阻尼、随机颠簸(BounceKick)、
//         加速点头、摆动相位、偏航衰减、轮位平滑。原版每秒递推 30 次;渲染提到 60 / 90 帧后
//         每秒递推 60 / 90 次 ⇒ 整套车身运动的时间轴被压缩 2 / 3 倍 —— 幅度差不多,
//         但颠簸频率、角速度、随机踢的密度都翻倍,看起来就是「剧烈颠簸」。
//         它只改渲染矩阵、只用**客户端**随机数(种子 0xCB01F0),不读任何帧率常量,
//         所以此前的动画帧号 / 世界坐标 / 帧率读者普查在结构上都看不见它。
//         机制与验证边界见 docs/TECHNICAL.md。
// 做什么:只在「30 Hz 虚拟帧号」变化的那个显示帧放行引擎的原函数(递推一步),
//         其余显示帧**回放上一次算出的四个输出**(俯仰 / 侧倾 / 偏航 / 视觉 Z)。
//         ⚠ 必须回放,不能只做节拍门:原函数返回 0 时,调用方那一帧**不施加任何姿态**,
//           只跳过不回放,车身会在「有姿态 / 无姿态」之间逐帧闪。
//         另有**插值**模式(为 90 帧准备):递推仍 30 Hz,显示时在相邻两个位姿之间按相位取值,见下面 FL_CHS_MODE_LERP。
// 约束:· 目标帧率 <= 原版帧率时虚拟帧号 == 显示帧号 ⇒ 每帧放行 ⇒ 与原版逐位一致(直通)。
//       · 纯整数推导,不读时钟;同样的调用序列永远给出同样的结果(离线可测)。
//       · 表满 / 探查窗口内没有可用格子时**退回直通**(每帧放行 = 没装门时的行为),绝不丢姿态。
#ifndef RA3_FRAMELAB_CHASSIS_GATE_H
#define RA3_FRAMELAB_CHASSIS_GATE_H

#define FL_CHS_BITS    10
#define FL_CHS_SLOTS   (1 << FL_CHS_BITS)   // 表的格子数 1024(同屏载具通常几十到一两百)
#define FL_CHS_PROBES  16          // 线性探查窗口;窗口内既没命中也没有可回收格子 ⇒ 退回直通
#define FL_CHS_OUTS    4           // 引擎输出的四个 float:俯仰、侧倾、偏航、视觉 Z

struct FlChassisEnt {
    void*    key;                  // Drawable 指针;NULL = 空格子
    unsigned vframe;               // 缓存的输出属于哪一个 30 Hz 虚拟帧
    unsigned lastSeen;             // 最后一次被调用时的显示帧号(判陈旧 / 回收用)
    float    out[FL_CHS_OUTS];     // 最近一次原函数算出的输出(回放用)
    float    prev[FL_CHS_OUTS];    // 再上一次的输出(插值模式用;没有相邻的上一帧时 = out)
    float    shown[FL_CHS_OUTS];   // 最近一次**真正交给引擎**的输出(回放 = out;插值 = prev..out 之间)
    int      hasOut;               // out[] 是否有效
    int      appearance;           // 该载具的 locomotor 外观枚举(-1 = 还没读到);只给量具用
    long     calls;                // 包装函数被进入的次数
    long     advances;             // 原函数真的被放行且返回非 0 的次数(= 递推步数)
    long     holds;                // 回放缓存的次数
    long     moved;                // 放行后输出与上一次不同的次数(「这辆车的车身真的在动」)
};

// 被包装的原函数的「测试友好」形状:ctx 由调用方自带(DLL 里是 NULL,自检里是假递推的状态)。
typedef char (*FlChsOrigFn)(void* ctx, void* self, float* out);

// 30 Hz 虚拟帧号。目标帧率不高于原版帧率时原样返回显示帧号(直通)。
// 用 64 位乘法:显示帧号在长局里能到几十万,乘 30 不会溢出 32 位,但别赌。
static unsigned fl_chs_vframe(unsigned displayFrame, int targetFps, int retailFps) {
    if (targetFps <= 0 || retailFps <= 0 || targetFps <= retailFps) return displayFrame;
    return (unsigned)(((unsigned long long)displayFrame * (unsigned)retailFps) / (unsigned)targetFps);
}

// Knuth 乘法散列:取 32 位乘积的**高位**。对象按 16 字节对齐分配,低 4 位恒为 0,先移掉。
static unsigned fl_chs_hash(const void* key) {
    const unsigned x = (unsigned)((size_t)key >> 4);
    return (unsigned)(x * 2654435761u) >> (32 - FL_CHS_BITS);
}

static void fl_chs_reset_ent(FlChassisEnt* e, void* key, unsigned displayFrame) {
    int k;
    e->key = key;
    e->vframe = 0;
    e->lastSeen = displayFrame;
    for (k = 0; k < FL_CHS_OUTS; ++k) { e->out[k] = 0.0f; e->prev[k] = 0.0f; e->shown[k] = 0.0f; }
    e->hasOut = 0;
    e->appearance = -1;
    e->calls = 0;
    e->advances = 0;
    e->holds = 0;
    e->moved = 0;
}

// 找到(或新占)key 对应的格子。staleAfter = 多少个显示帧没被调用就算陈旧(可被别人回收;
// 自己再回来时也要重置 —— 同一个地址上可能已经是另一辆车了)。
// ★ 探查时**不在空格子处提前停**:陈旧格子会被别的 key 回收,提前停会让「排在它后面」的
//   活 key 查不到。窗口只有 16 格,全扫一遍的代价可以忽略。
// 返回 NULL = 窗口内没有可用格子(调用方退回直通)。
static FlChassisEnt* fl_chs_find(FlChassisEnt* tab, void* key, unsigned displayFrame, unsigned staleAfter) {
    unsigned h, i;
    FlChassisEnt* reuse = 0;
    if (!key) return 0;
    h = fl_chs_hash(key);
    for (i = 0; i < FL_CHS_PROBES; ++i) {
        FlChassisEnt* e = &tab[(h + i) & (FL_CHS_SLOTS - 1)];
        const int stale = e->key && (unsigned)(displayFrame - e->lastSeen) > staleAfter;
        if (e->key == key) {
            if (stale) fl_chs_reset_ent(e, key, displayFrame);
            return e;
        }
        if (!reuse && (!e->key || stale)) reuse = e;
    }
    if (reuse) fl_chs_reset_ent(reuse, key, displayFrame);
    return reuse;
}

// 这一次调用发生了什么(给调用方记总账用)。
#define FL_CHS_DID_ZERO     0      // 原函数被放行,返回 0(无 locomotor / 外观不处理 / 引擎自己的同帧去重)
#define FL_CHS_DID_ADVANCE  1      // 原函数被放行,返回非 0(递推了一步)
#define FL_CHS_DID_HOLD     2      // 回放 / 插值,原函数没被调用

// 节拍门的三种模式。
#define FL_CHS_MODE_PROBE   0      // 只记账、每帧放行(只读探针,行为逐位不变)
#define FL_CHS_MODE_HOLD    1      // 30 Hz 放行,其余显示帧**回放**上一次的输出(位姿序列与原版逐位相同,但只有 30 Hz 台阶)
#define FL_CHS_MODE_LERP    2      // 30 Hz 放行,每个显示帧在「上一次 → 这一次」之间**线性插值**(见下)

// ── 插值模式(2026-09-21 同日追加):为 90 帧准备的 ─────────────────────────────────────
// 回放模式下车身姿态每秒只变 30 次:60 帧时每个姿态停 2 帧,90 帧时停 3 帧 —— 而车的平移是每帧都在动的,
// 于是「车身相对自己的平移」会有 30 Hz 的台阶。幅度很小(姿态增量通常零点几度),但它正好与
// 「提帧 = 更顺」这个目的相反,帧率越高越显眼。
// 做法:递推仍然严格 30 Hz(动力学与原版逐步相同),只是**显示**时在相邻两个 30 Hz 位姿之间线性插值:
//     显示帧 f 的「虚拟时刻」τ = (f + 1) × 30 ÷ 目标帧率(单位:30 Hz 虚拟帧)
//     相位 φ = τ − 当前虚拟帧号 = ((f × 30) mod 目标帧率 + 30) ÷ 目标帧率,钳到 1
//     输出 = 上一次位姿 + (这一次位姿 − 上一次位姿) × φ;φ = 1 时**直接拷贝**这一次位姿(不做浮点运算,保证逐位)
//   90 帧:φ = 1/3, 2/3, 1;60 帧:φ = 1/2, 1;45 / 75 帧这类非整数倍同一条式子自然成立。
// 代价:显示的位姿比递推晚至多 1 个虚拟帧(33 毫秒),对车身颠簸这种量级不可感知。
// 什么时候**不**插值(prev 直接取 = 这一次):这辆车第一次出现、或它上一次位姿不属于紧邻的上一个虚拟帧
//   (车刚回到屏幕里 / 格子被重置)—— 拿一个过期的位姿当起点会凭空造出一次甩动。
// 四个输出都是小角度 / 小位移,线性插值足够;不涉及绕 ±π 的角。
static void fl_chs_phase(unsigned displayFrame, int targetFps, int retailFps, unsigned* num, unsigned* den) {
    if (targetFps <= 0 || retailFps <= 0 || targetFps <= retailFps) { *num = 1; *den = 1; return; }
    *den = (unsigned)targetFps;
    *num = (unsigned)(((unsigned long long)displayFrame * (unsigned)retailFps) % (unsigned)targetFps) + (unsigned)retailFps;
    if (*num > *den) *num = *den;
}

static void fl_chs_blend(const FlChassisEnt* e, float* out, unsigned num, unsigned den) {
    int k;
    if (num >= den) { for (k = 0; k < FL_CHS_OUTS; ++k) out[k] = e->out[k]; return; }
    {
        const float phi = (float)num / (float)den;
        for (k = 0; k < FL_CHS_OUTS; ++k) out[k] = e->prev[k] + (e->out[k] - e->prev[k]) * phi;
    }
}

// 节拍门本体。entOut / didOut 可为 NULL;entOut 带回这次用到的格子(可能是 NULL = 表满直通)。
static char fl_chs_gate(FlChassisEnt* tab, void* self, float* out, unsigned displayFrame,
                        int targetFps, int retailFps, int mode, unsigned staleAfter,
                        FlChsOrigFn orig, void* ctx, FlChassisEnt** entOut, int* didOut) {
    int k;
    char r;
    unsigned num = 1, den = 1;
    const unsigned vf = fl_chs_vframe(displayFrame, targetFps, retailFps);
    FlChassisEnt* e = fl_chs_find(tab, self, displayFrame, staleAfter);
    if (entOut) *entOut = e;
    if (mode == FL_CHS_MODE_LERP) fl_chs_phase(displayFrame, targetFps, retailFps, &num, &den);
    if (e) {
        ++e->calls;
        e->lastSeen = displayFrame;
        if (mode != FL_CHS_MODE_PROBE && e->hasOut && e->vframe == vf) {
            if (mode == FL_CHS_MODE_LERP) fl_chs_blend(e, out, num, den);
            else for (k = 0; k < FL_CHS_OUTS; ++k) out[k] = e->out[k];
            for (k = 0; k < FL_CHS_OUTS; ++k) e->shown[k] = out[k];
            ++e->holds;
            if (didOut) *didOut = FL_CHS_DID_HOLD;
            return 1;
        }
    }
    r = orig(ctx, self, out);
    if (didOut) *didOut = r ? FL_CHS_DID_ADVANCE : FL_CHS_DID_ZERO;
    if (r && e) {
        int changed = 0;
        // 上一次位姿只有在「紧邻的上一个虚拟帧」时才拿来当插值起点
        const int adjacent = e->hasOut && (unsigned)(vf - e->vframe) == 1u;
        for (k = 0; k < FL_CHS_OUTS; ++k) {
            if (e->out[k] != out[k]) changed = 1;
            e->prev[k] = adjacent ? e->out[k] : out[k];
            e->out[k] = out[k];
        }
        if (changed && e->hasOut) ++e->moved;
        e->vframe = vf;
        e->hasOut = 1;
        ++e->advances;
        if (mode == FL_CHS_MODE_LERP) fl_chs_blend(e, out, num, den);   // 放行的那一帧也按相位显示
        for (k = 0; k < FL_CHS_OUTS; ++k) e->shown[k] = out[k];
    }
    return r;
}

#endif
