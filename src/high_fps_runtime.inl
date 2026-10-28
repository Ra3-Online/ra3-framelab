// Included inside namespace fl after the ordinary runtime wrappers.
// Addresses/signatures describe only the supported 2009 retail 1.12 image.
// Implementation is local; design comparison: docs/HIGH_FPS_RESEARCH.md.
struct HighPatchSite {
    unsigned char* address;
    unsigned char original[48];
    unsigned char installed[48];
    int contextSize, writeSize;
};
struct HighSites {
    HighPatchSite entry, advance, exit, phaseTest, limiter, window[2], drawableAlpha;
    HighPatchSite turretRead, turretYaw, turretPitch, turretSave;
    HighPatchSite boneAlpha, structureAlpha;
};
static const int kHighSiteCount = 14;
static FlHighFpsSchedule g_highSchedule;
static FlHighTurretClock g_highTurretClock;
static volatile long g_highTurretReads = 0, g_highTurretYawCaptures = 0, g_highTurretPitchCaptures = 0;
static void* g_highTurretYawShift = (void*)0x8FB826;
static void* g_highTurretPitchKeep = (void*)0x8FB944;
static float g_highTurretOne = 1.0f;
static void* g_highOwner = NULL;
static unsigned g_highSimulationBits = 0x3F800000;
// GameClient's drawable preparation runs before advance/end. Retain the
// preceding completed update's draw fraction, as native call order expects,
// separately from the simulation field restored by fl_high_enter.
static float g_highDrawAlpha = 1.0f;
static volatile long g_highDrawableReads = 0;
static volatile long g_highNativeAlphaReads = 0;
static volatile long g_highBoneReads = 0, g_highStructureReads = 0, g_highConstructionReads = 0;
static void** g_highDrawableEngineSlot = (void**)0xCE2F8C;
static void* g_highEntryResume = NULL;
static void* g_highExitSite = NULL;
static void** g_highExitGlobal = NULL;
static LARGE_INTEGER g_highFrequency = {};
static double g_highLimiterRemainder = 0.0;
static int g_highLimiterWhole = 0;

static float fl_high_visual_alpha(const void* engine, float nativeValue) {
    if (!g_highOn || !engine || engine != g_highOwner) return nativeValue;
    ++g_highConstructionReads;
    return g_highDrawAlpha;
}

// Bounded diagnostics at the existing model-prepare callback, after native
// animation advance. Observe live owner fields only during that callback;
// status printing never dereferences saved game pointers. No game state writes,
// allocation, scans, timers, RNG, getters or file IO occur on this hot path.
struct HighVisualObservation {
    uintptr_t module, drawable;
    unsigned drawableId, templateId, vtable, frame;
    unsigned samples, rawSteps, visualSteps, moving, held, maxHeld, runHeld, bypass;
    unsigned raw[3], visual[3];
    unsigned mode[3], clipFrame[3], clipSteps, modeMask, stamp, stampSteps;
};
static HighVisualObservation g_highVisual[128];
static unsigned g_highVisualOverflow = 0;
static void fl_high_visual_observe(const void* module) {
    if (!module) return;
    const unsigned char* m = (const unsigned char*)module;
    const unsigned char* d = *(const unsigned char* const*)(m + 8);
    const unsigned char* o = d ? *(const unsigned char* const*)(d + 312) : NULL;
    const unsigned char* tpl = d ? *(const unsigned char* const*)(d + 4) : NULL;
    if (!o || !tpl) return; // exclude placement previews and unbound drawables
    const unsigned vt = fl_visual_word(module, 0);
    unsigned modes[3]; bool selected = vt == 0xC62D30 || vt == 0xC62EB0;
    for (unsigned i=0;i<3;++i) {
        modes[i]=fl_visual_word(module,300+i*28);
        selected |= modes[i]==2 || modes[i]==4 || modes[i]==7;
    }
    // Also catch moving generic/scripted models: vehicles need not use the
    // native TankDraw class. Inspect history only, never change its flags.
    if (!selected) for (unsigned i=0;i<3;++i)
        selected |= fl_visual_word(d,208+i*4) != fl_visual_word(d,220+i*4);
    if (!selected) return;
    const unsigned id = fl_visual_word(d, 608), tid = fl_visual_word(tpl, 256);
    const unsigned first = (id ^ (id >> 7)) & 127;
    HighVisualObservation* found = NULL;
    for (unsigned i = 0; i < 4; ++i) {
        HighVisualObservation& r = g_highVisual[(first + i) & 127];
        if (!r.module || (r.module == (uintptr_t)module && r.drawable == (uintptr_t)d &&
                          r.drawableId == id && r.templateId == tid)) { found = &r; break; }
    }
    if (!found) { ++g_highVisualOverflow; return; }
    HighVisualObservation& r = *found;
    const unsigned frame = (unsigned)g_frameCount;
    if (r.module && r.frame == frame) return; // same module drawn twice in one update
    const bool previous = r.module != 0;
    r.module = (uintptr_t)module; r.drawable = (uintptr_t)d;
    r.drawableId = id; r.templateId = tid; r.vtable = vt; r.frame = frame;
    bool rawStep = false, visualStep = false, moving = false;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned raw = fl_visual_word(o, 56 + i*4);
        const unsigned visual = fl_visual_word(d, 292 + i*4);
        rawStep |= previous && raw != r.raw[i];
        visualStep |= previous && visual != r.visual[i];
        moving |= fl_visual_word(d, 208 + i*4) != fl_visual_word(d, 220 + i*4);
        r.raw[i] = raw; r.visual[i] = visual;
    }
    ++r.samples; r.rawSteps += rawStep; r.visualSteps += visualStep;
    if (moving && previous) {
        ++r.moving;
        if (!visualStep) { ++r.held; ++r.runHeld; }
        else r.runHeld = 0;
        if (r.runHeld > r.maxHeld) r.maxHeld = r.runHeld;
    } else r.runHeld = 0;
    r.bypass += (d[318] & 4) != 0;
    const unsigned stamp = fl_visual_word(m, 200);
    bool clipStep=false;
    for (unsigned i=0;i<3;++i) {
        const unsigned clip = fl_visual_word(m,288+i*28);
        clipStep |= previous && (clip != r.clipFrame[i] || modes[i] != r.mode[i]);
        if (modes[i]<8) r.modeMask |= 1u << modes[i];
        r.mode[i]=modes[i]; r.clipFrame[i]=clip;
    }
    r.clipSteps += clipStep;
    r.stampSteps += previous && stamp != r.stamp;
    r.stamp = stamp;
}

static bool fl_high_boundary(unsigned bit) {
    return g_highSchedule.takeBoundary(bit);
}
static bool fl_high_new_tick(void* engine) {
    return engine && engine == g_highOwner && g_highSchedule.newTick;
}

extern "C" void __stdcall fl_high_enter(void* self) {
    if (self != g_highOwner) {
        g_highOwner = self;
        g_highSchedule = FlHighFpsSchedule();
        g_highTurretClock = FlHighTurretClock();
        memcpy(&g_highSimulationBits, (char*)self + 0x60, 4);
        memcpy(&g_highDrawAlpha, &g_highSimulationBits, 4);
    } else {
        // The time-dependent draw fraction must not reach the next simulation
        // update (including its pre-update MC / transform / destruction work).
        memcpy((char*)self + 0x60, &g_highSimulationBits, 4);
    }
}
extern "C" int __stdcall fl_high_advance(void* self) {
    LARGE_INTEGER ticks = {};
    if (!QueryPerformanceCounter(&ticks) || g_highFrequency.QuadPart <= 0) {
        g_highSchedule.fault = true;
        return 0;
    }
    const std::uint64_t q = (std::uint64_t)ticks.QuadPart;
    const std::uint64_t f = (std::uint64_t)g_highFrequency.QuadPart;
    const std::uint64_t us = q / f * 1000000u + (q % f) * 1000000u / f;
    const int phase = *(int*)((char*)self + 0x58);
    const bool run = g_highSchedule.begin(us, phase, g_targetFps);
    // Keep the existing display-update counter, black box and visual clock.
    // The high path leaves the native +1/clamp block itself untouched.
    fl_hook_perframe(self);
    return run ? 1 : 0;
}
extern "C" void __stdcall fl_high_end(void* self) {
    const int phase = *(int*)((char*)self + 0x58);
    const bool attempted = g_highSchedule.attempted;
    g_highSchedule.end(phase);
    g_highTurretClock.observe(g_highSchedule, phase);
    if (attempted) memcpy(&g_highSimulationBits, (char*)self + 0x60, 4);
    g_highDrawAlpha = g_highSchedule.fraction(phase);
    *(float*)((char*)self + 0x60) = g_highDrawAlpha;
}

// Replaces exactly mov edx,[TheGameEngine]; fld dword [edx+60h].
// Keep EDX, EFLAGS, all other registers and one x87 push identical to native;
// only the normal Drawable::update caller gets cached display alpha. The
// binding/initialization caller and unknown contexts retain the native read.
// At this point the original function saved 3 registers + 16 local bytes;
// our call then pushfd put its outer return address at [esp+24h].
extern "C" __declspec(naked) void fl_high_drawable_alpha_stub() {
    __asm {
        mov edx, g_highDrawableEngineSlot
        mov edx, [edx]
        pushfd
        cmp dword ptr [esp+24h], 53A797h
        jne native_read
        cmp byte ptr [g_highOn], 0
        je native_read
        cmp edx, g_highOwner
        jne native_read
        inc dword ptr [g_highDrawableReads]
        popfd
        fld dword ptr [g_highDrawAlpha]
        ret
    native_read:
        inc dword ptr [g_highNativeAlphaReads]
        popfd
        fld dword ptr [edx+60h]
        ret
    }
}

// This MOVSS sits between TEST EBP and JE. Preserve flags, all GPRs,
// non-result XMM registers, x87 and MXCSR; MOVSS retains native upper-lane zeroing.
extern "C" __declspec(naked) void fl_high_turret_read_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_read
        inc dword ptr [g_highTurretReads]
        popfd
        movss xmm0, dword ptr [g_highTurretClock.alpha]
        ret
    native_read:
        popfd
        movss xmm0, dword ptr [eax+60h]
        ret
    }
}

// These replace only a verified MOVSS and keep its native memory-load upper
// lane clearing, GPRs, EFLAGS, other XMMs, x87 stack and MXCSR. Neither writes
// engine+60h. Distinct register contracts are intentional (EAX vs ECX).
extern "C" __declspec(naked) void fl_high_bone_alpha_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_read
        cmp eax, g_highOwner
        jne native_read
        inc dword ptr [g_highBoneReads]
        popfd
        movss xmm0, dword ptr [g_highDrawAlpha]
        ret
    native_read:
        popfd
        movss xmm0, dword ptr [eax+60h]
        ret
    }
}
extern "C" __declspec(naked) void fl_high_structure_alpha_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_read
        cmp ecx, g_highOwner
        jne native_read
        inc dword ptr [g_highStructureReads]
        popfd
        movss xmm0, dword ptr [g_highDrawAlpha]
        ret
    native_read:
        popfd
        movss xmm0, dword ptr [ecx+60h]
        ret
    }
}

// Replace the two native "alpha==1 || alpha==lastAlpha" cache predicates.
// Native yaw/pitch capture instructions and shortest-arc math remain in place.
// The original cache's last-alpha word stores our raw epoch, avoiding pointer
// tables and duplicate captures on repeated draws, clamped alpha or network waits.
extern "C" __declspec(naked) void fl_high_turret_yaw_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_test
        push eax
        mov eax, g_highTurretClock.serial
        cmp [ebx+10h], eax
        pop eax
        je keep
        inc dword ptr [g_highTurretYawCaptures]
        popfd
        lea esp, [esp+4]
        jmp dword ptr [g_highTurretYawShift]
    keep:
        popfd
        ret
    native_test:
        popfd
        ucomiss xmm4, xmm3
        lahf
        test ah, 44h
        jnp shift_native
        ucomiss xmm4, dword ptr [ebx+10h]
        lahf
        test ah, 44h
        jnp shift_native
        ret
    shift_native:
        lea esp, [esp+4]
        jmp dword ptr [g_highTurretYawShift]
    }
}
extern "C" __declspec(naked) void fl_high_turret_pitch_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_test
        push eax
        mov eax, g_highTurretClock.serial
        cmp [ebx+10h], eax
        pop eax
        je keep
        inc dword ptr [g_highTurretPitchCaptures]
        popfd
        ret
    keep:
        popfd
        lea esp, [esp+4]
        jmp dword ptr [g_highTurretPitchKeep]
    native_test:
        popfd
        ucomiss xmm0, dword ptr [g_highTurretOne]
        lahf
        test ah, 44h
        jnp capture_native
        ucomiss xmm0, dword ptr [ebx+10h]
        lahf
        test ah, 44h
        jp keep_native
    capture_native:
        ret
    keep_native:
        lea esp, [esp+4]
        jmp dword ptr [g_highTurretPitchKeep]
    }
}
extern "C" __declspec(naked) void fl_high_turret_save_stub() {
    __asm {
        pushfd
        cmp byte ptr [g_highOn], 0
        je native_save
        push eax
        mov eax, g_highTurretClock.serial
        mov [ebx+10h], eax
        pop eax
        popfd
        ret
    native_save:
        popfd
        movss dword ptr [ebx+10h], xmm0
        ret
    }
}

static bool high_drawable_caller_valid() {
    const unsigned char* prefix[] = {(const unsigned char*)0x8E8236,(const unsigned char*)0x8F3D85};
    const short* signature[] = {kSigHighBoneAlpha,kSigHighStructureAlpha};
    for (unsigned k=0;k<2;++k) {
        const unsigned n = k==0 ? 5 : 6;
        if (!mem::read_ok(prefix[k],n)) return false;
        for (unsigned j=0;j<n;++j) if (prefix[k][j]!=(unsigned char)signature[k][j]) return false;
    }
    const unsigned char* p=(const unsigned char*)0x53A790;
    if(!mem::read_ok(p,sizeof kSigHighDrawableCaller/sizeof(short))) return false;
    for(unsigned i=0;i<sizeof kSigHighDrawableCaller/sizeof(short);++i)
        if(p[i]!=(unsigned char)kSigHighDrawableCaller[i]) return false;
    p=(const unsigned char*)0x8FB6C5;
    if(!mem::read_ok(p,sizeof kSigHighTurretPrefix/sizeof(short))) return false;
    for(unsigned i=0;i<sizeof kSigHighTurretPrefix/sizeof(short);++i)
        if(p[i]!=(unsigned char)kSigHighTurretPrefix[i]) return false;
    return true;
}

// Stubs keep the original stack, callee-saved registers, and EFLAGS contracts.
// No original relative instruction is copied into a relocated trampoline.
extern "C" __declspec(naked) void fl_high_entry_stub() {
    __asm {
        pushfd
        pushad
        push ecx
        call fl_high_enter
        popad
        popfd
        push ecx
        push ebx
        push esi
        mov esi, ecx
        jmp dword ptr [g_highEntryResume]
    }
}
extern "C" __declspec(naked) void fl_high_advance_stub() {
    __asm {
        pushfd
        pushad
        push esi
        call fl_high_advance
        mov [esp+28], eax
        popad
        popfd
        test eax, eax
        jz display_only
        mov ecx, [esi+58h]
        cmp ecx, 6
        ret
    display_only:
        add esp, 4 // discard only this stub's call return address
        push edi   // native exit pops EDI; skipped advance has not pushed it
        jmp dword ptr [g_highExitSite]
    }
}
extern "C" __declspec(naked) void fl_high_exit_stub() {
    __asm {
        pushfd
        pushad
        push esi
        call fl_high_end
        popad
        popfd
        mov ecx, g_highExitGlobal
        mov ecx, [ecx]
        ret
    }
}
extern "C" __declspec(naked) void fl_high_limiter_stub() {
    __asm {
        // Native has already selected truncation rounding. Preserve its net
        // speed scaling and x87 depth, carrying only the discarded fraction.
        fadd qword ptr [g_highLimiterRemainder]
        fld st(0)
        fistp dword ptr [g_highLimiterWhole]
        fisub dword ptr [g_highLimiterWhole]
        fstp qword ptr [g_highLimiterRemainder]
        mov esi, g_highLimiterWhole
        ret
    }
}
extern "C" bool __fastcall fl_high_phase_test(void* self, void*) {
    return fl_high_new_tick(self);
}

static bool high_record(HighPatchSite& s, unsigned char* p, int context, int write) {
    if (!p || p == (unsigned char*)-1 || context > (int)sizeof s.original ||
        write > context || !mem::read_ok(p, context)) return false;
    s.address = p; s.contextSize = context; s.writeSize = write;
    memcpy(s.original, p, context);
    return true;
}
static HighPatchSite* high_site(HighSites& h, int i) {
    HighPatchSite* sites[] = {&h.entry, &h.advance, &h.exit, &h.phaseTest,
                             &h.limiter, &h.window[0], &h.window[1], &h.drawableAlpha,
                             &h.turretRead, &h.turretYaw, &h.turretPitch, &h.turretSave,
                             &h.boneAlpha, &h.structureAlpha};
    return sites[i];
}
static int resolve_high_sites(HighSites& h) {
    struct Spec { const char* name; const short* bytes; int size; unsigned va; int write; };
    const Spec spec[] = {
        {"high entry", kSigHighEntry, sizeof kSigHighEntry / sizeof(short), 0x6027D0, 5},
        {"high advance", kSigHighAdvance, sizeof kSigHighAdvance / sizeof(short), 0x60282F, 6},
        {"high exit", kSigHighExit, sizeof kSigHighExit / sizeof(short), 0x60294F, 6},
        {"high new tick", kSigFpsGlobals, sizeof kSigFpsGlobals / sizeof(short), 0x5FFAD0, 5},
        {"high limiter", kSigHighLimiter, sizeof kSigHighLimiter / sizeof(short), 0x629829, 8}
    };
    for (int i = 0; i < 5; ++i) {
        const Spec& v = spec[i];
        Pattern p = {v.name, v.bytes, v.size, v.va};
        unsigned char* hit = scan(p);
        if (!hit) return FL_ERR_SIG_MISS;
        if (hit == (unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
        if ((uintptr_t)hit != v.va || !high_record(*high_site(h,i), hit, v.size, v.write))
            return FL_ERR_SIG_MISMATCH;
    }
    Pattern p = {"high interpolation windows", kSigHighWindow,
                 sizeof kSigHighWindow / sizeof(short), 0};
    unsigned char* hits[4] = {};
    const int n = scan_all(p, 2, (void*)0x130, hits, 4);
    if (n != 2) return n > 2 ? FL_ERR_SIG_AMBIGUOUS : FL_ERR_SIG_MISS;
    const uintptr_t expected[] = {0x5296CA, 0x52F527};
    for (int i = 0; i < 2; ++i) {
        if ((uintptr_t)hits[i] != expected[i] || !high_record(h.window[i], hits[i], p.length, 9))
            return FL_ERR_SIG_MISMATCH;
    }
    Pattern draw = {"high drawable alpha", kSigHighDrawableAlpha,
                    sizeof kSigHighDrawableAlpha / sizeof(short), 0x52F5FF};
    unsigned char* hit = scan(draw);
    if (!hit) return FL_ERR_SIG_MISS;
    if (hit == (unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
    if ((uintptr_t)hit != draw.expectedVa || !high_record(h.drawableAlpha, hit, draw.length, 9))
        return FL_ERR_SIG_MISMATCH;
    const Spec turret[] = {
        {"high turret read", kSigHighTurretRead, sizeof kSigHighTurretRead/sizeof(short), 0x8FB6D2, 5},
        {"high turret yaw", kSigHighTurretYaw, sizeof kSigHighTurretYaw/sizeof(short), 0x8FB7BD, 19},
        {"high turret pitch", kSigHighTurretPitch, sizeof kSigHighTurretPitch/sizeof(short), 0x8FB91C, 23},
        {"high turret save", kSigHighTurretSave, sizeof kSigHighTurretSave/sizeof(short), 0x8FBCAA, 5}
    };
    for(int i=0;i<4;++i) {
        const Spec& s=turret[i];
        Pattern turretPattern={s.name,s.bytes,s.size,s.va};
        unsigned char* address=scan(turretPattern);
        if(!address) return FL_ERR_SIG_MISS;
        if(address==(unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
        if((uintptr_t)address!=s.va || !high_record(*high_site(h,i+8),address,s.size,s.write))
            return FL_ERR_SIG_MISMATCH;
    }
    const Spec visual[] = {
        {"high attached bone alpha", kSigHighBoneAlpha, sizeof kSigHighBoneAlpha/sizeof(short), 0x8E8236, 5},
        {"high structure alpha", kSigHighStructureAlpha, sizeof kSigHighStructureAlpha/sizeof(short), 0x8F3D85, 5}
    };
    for (int i=0;i<2;++i) {
        const Spec& s=visual[i];
        Pattern vp={s.name,s.bytes,s.size,s.va};
        unsigned char* address=scan(vp);
        if (!address) return FL_ERR_SIG_MISS;
        if (address==(unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
        const int prefix = i==0 ? 5 : 6;
        if ((uintptr_t)address!=s.va ||
            !high_record(*high_site(h,i+12),address+prefix,s.size-prefix,s.write)) return FL_ERR_SIG_MISMATCH;
    }
    if (!high_drawable_caller_valid()) return FL_ERR_SIG_MISMATCH;
    if (h.advance.address + 0x120 != h.exit.address ||
        *(void***)(h.exit.address + 2) != (void**)0xCD6C9C)
        return FL_ERR_SIG_MISMATCH;
    if (!QueryPerformanceFrequency(&g_highFrequency) || g_highFrequency.QuadPart <= 0)
        return FL_ERR_SIG_MISMATCH;
    return FL_OK;
}
static bool verify_high_sites(HighSites& h, bool installed) {
    if (!high_drawable_caller_valid()) return false;
    for (int i = 0; i < kHighSiteCount; ++i) {
        const HighPatchSite& s = *high_site(h, i);
        const int n = installed ? s.writeSize : s.contextSize;
        if (!mem::read_ok(s.address,n) || memcmp(s.address,
            installed ? s.installed : s.original, n) != 0) return false;
    }
    return true;
}
static bool install_high_sites(HighSites& h) {
    g_highSchedule = FlHighFpsSchedule();
    g_highTurretClock = FlHighTurretClock();
    g_highTurretReads = g_highTurretYawCaptures = g_highTurretPitchCaptures = 0;
    g_highOwner = NULL;
    g_highLimiterRemainder = 0.0;
    g_highDrawAlpha = 1.0f;
    g_highDrawableReads = 0;
    g_highNativeAlphaReads = 0;
    g_highBoneReads = g_highStructureReads = g_highConstructionReads = 0;
    memset(g_highVisual,0,sizeof g_highVisual); g_highVisualOverflow = 0;
    g_highEntryResume = h.entry.address + 5;
    g_highExitSite = h.exit.address;
    g_highExitGlobal = *(void***)(h.exit.address + 2);
    void* targets[] = {(void*)&fl_high_entry_stub, (void*)&fl_high_advance_stub,
                      (void*)&fl_high_exit_stub, (void*)&fl_high_phase_test,
                      (void*)&fl_high_limiter_stub, (void*)&fl_high_drawable_alpha_stub,
                      (void*)&fl_high_turret_read_stub, (void*)&fl_high_turret_yaw_stub,
                      (void*)&fl_high_turret_pitch_stub, (void*)&fl_high_turret_save_stub,
                      (void*)&fl_high_bone_alpha_stub, (void*)&fl_high_structure_alpha_stub};
    for (int i = 0; i < kHighSiteCount; ++i) {
        HighPatchSite& s = *high_site(h,i);
        memset(s.installed, 0x90, s.writeSize);
        if (i < 5 || i >= 7) {
            s.installed[0] = i == 0 || i == 3 ? 0xE9 : 0xE8;
            const int rel = (int)((unsigned char*)targets[i >= 7 ? i-2 : i] - (s.address + 5));
            memcpy(s.installed + 1, &rel, 4);
        } else {
            memcpy(s.installed, s.original, s.writeSize);
            s.installed[8] = (unsigned char)(g_targetFps / 15 + 2);
        }
        if (!patch(s.address, s.installed, s.writeSize, "HIGH clock/batch/draw contract")) return false;
    }
    g_highOn = true;
    return verify_high_sites(h, true);
}
