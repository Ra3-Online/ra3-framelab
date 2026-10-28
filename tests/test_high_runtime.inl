// Exercise the actual compiled x86 entry/advance/exit/limiter adapters on
// owned memory. The dispatcher below is a test callback, never game code.
static unsigned char highEngine[0x100];
static void* highDummy = NULL;
static unsigned highDispatches = 0;
static bool highRefuse = false;
static unsigned highBefore, highAfter, highEbx, highEsi, highEdi;

extern "C" void __stdcall owned_high_dispatch(void* engine) {
    ++highDispatches;
    int& phase = *(int*)((char*)engine+0x58);
    if (phase >= 6 && highRefuse) { phase=7; *(float*)((char*)engine+0x60)=1; return; }
    phase = phase >= 6 ? 3 : 6;
    *(float*)((char*)engine+0x60) = (float)phase * (1.0f/6.0f);
}
extern "C" __declspec(naked) void owned_high_exit() {
    __asm {
        call fl_high_exit_stub
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    }
}
extern "C" __declspec(naked) void owned_high_entry_resume() {
    __asm {
        mov ebx, 1
        call fl_high_advance_stub
        push edi
        push esi
        call owned_high_dispatch
        jmp owned_high_exit
    }
}
static void owned_high_call() {
    __asm {
        push ebx
        push esi
        push edi
        mov highBefore, esp
        mov ebx, 12345678h
        mov esi, 23456789h
        mov edi, 3456789Ah
        lea ecx, highEngine
        call fl_high_entry_stub
        mov highAfter, esp
        mov highEbx, ebx
        mov highEsi, esi
        mov highEdi, edi
        pop edi
        pop esi
        pop ebx
    }
    check(highBefore==highAfter && highEbx==0x12345678 && highEsi==0x23456789 && highEdi==0x3456789A,
          "HIGH compiled adapters balance stack and preserve EBX/ESI/EDI on both branches");
}
static void test_high_runtime() {
    memset(highEngine,0,sizeof highEngine);
    put(highEngine,0x58,6); put(highEngine,0x60,0x3F800000);
    g_highOwner=NULL; g_highOn=true; g_targetFps=240; g_groups=0;
    g_measureOnly=false; g_sampleLeft=0;
    g_highEntryResume=(void*)&owned_high_entry_resume;
    g_highExitSite=(void*)&owned_high_exit;
    g_highExitGlobal=&highDummy;
    QueryPerformanceFrequency(&g_highFrequency);
    const long frames = g_frameCount;
    owned_high_call();
    check(highDispatches==1 && *(int*)(highEngine+0x58)==3 && g_highSchedule.newTick,
          "HIGH actual adapter executes one native-shaped first half");
    check(g_highSimulationBits==0x3F000000,"HIGH native post-batch alpha saved separately from draw alpha");
    g_highSchedule.due=~(std::uint64_t)0;
    for(int i=0;i<5;++i) owned_high_call();
    check(highDispatches==1 && *(int*)(highEngine+0x58)==3 && !g_highSchedule.newTick &&
          g_frameCount==frames+6 && g_highSimulationBits==0x3F000000,
          "HIGH actual display-only path skips dispatcher, clears tick pulse, counts each update, preserves simulation alpha");
    g_highSchedule.due=0; owned_high_call();
    check(highDispatches==2 && *(int*)(highEngine+0x58)==6 && g_highSimulationBits==0x3F800000,
          "HIGH actual second-half dispatch preserves native final alpha");
    highRefuse=true; g_highSchedule.due=0; owned_high_call();
    check(g_highSchedule.retries==1 && g_highSchedule.accepted==2 && !g_highSchedule.fault,
          "HIGH actual network-refusal adapter consumes no tick credit");
    highRefuse=false; g_highSchedule.due=0; owned_high_call();
    check(g_highSchedule.accepted==3 && !g_highSchedule.fault && g_highSchedule.newTick,
          "HIGH actual network retry returns through normal exit once");
    // At entry, restore native alpha before any housekeeping callback.
    *(float*)(highEngine+0x60)=0.123f; fl_high_enter(highEngine);
    check(*(unsigned*)(highEngine+0x60)==g_highSimulationBits,
          "HIGH time-dependent draw alpha cannot survive next entry restore");
    for(int fps : {120,165,180,240}) {
        const double budget=1000.0/fps;
        const float canary=1.2345f;
        float after=0;
        unsigned short cw=0, trunc=0;
        __asm { fnstcw cw }
        trunc=cw|0x0C00;
        g_highLimiterRemainder=0;
        int total=0;
        for(int i=0;i<fps;++i) {
            __asm {
                push esi
                fldcw trunc
                fld dword ptr canary
                fld qword ptr budget
                call fl_high_limiter_stub
                fstp dword ptr after
                fldcw cw
                pop esi
            }
            total+=g_highLimiterWhole;
        }
        check(after==canary && total>=999 && total<=1000 && g_highLimiterRemainder>=0 && g_highLimiterRemainder<1,
              "HIGH actual x87 limiter preserves stack and carries fractional-ms budget for one second");
    }
    g_highOn=false; g_highOwner=NULL; g_highEntryResume=g_highExitSite=NULL;
    g_highExitGlobal=NULL; g_targetFps=90; g_groups=FL_G_ALL;
}

static void test_high_glow_clock_model() {
    unsigned longTailErrors=0;
    for(int fps : {120,135,150,165,180,195,210,225,240})
    for(unsigned base : {0u,100001u,3600000u,28800000u}) {
        unsigned gaps=0, overlap=0;
        for(const auto& p : kFlPowerGlows) {
            const float visible=fl_glow_visible_lifetime(p,fps);
            const int end=(int)p.poolLifetime*fps/30; // both pool lifetimes even
            for(int offset=0;offset<fps;++offset) {
                const unsigned begin=base+(unsigned)(offset*1000/fps);
                const float expiry=(float)((double)begin*(double)0.03f+p.poolLifetime);
                const float birth=(float)((double)expiry-p.poolLifetime);
                for(int frame=0;frame<=end;++frame) {
                    const unsigned now=base+(unsigned)((frame+offset)*1000/fps);
                    const float shader=(float)((float)((double)now*0.001)*30.0f);
                    const float age=shader-birth;
                    gaps += frame<end && age>visible;
                    overlap += frame==end && age<=visible;
                }
            }
        }
        if(base<=3600000)
            check(!gaps && !overlap,"HIGH glow CPU clock model: no sampled seam at 0/100s/one-hour timestamps");
        else {
            longTailErrors+=gaps+overlap;
            if(gaps || overlap) std::printf("OBSERVED HIGH long-uptime float limit fps=%d base_ms=%u gaps=%u overlaps=%u (not GPU acceptance)\n",fps,base,gaps,overlap);
        }
    }
    check(longTailErrors>0,"HIGH eight-hour float-precision counterexample remains visible, not claimed fixed");
}

// Native reader used as the regression control. This reproduces only the
// verified nine-byte load on test-owned memory, never a game function.
extern "C" __declspec(naked) void owned_high_original_alpha() {
    __asm {
        mov edx, g_highDrawableEngineSlot
        mov edx, [edx]
        fld dword ptr [edx+60h]
        ret
    }
}
static float owned_high_draw_alpha(bool patched, unsigned caller=0x53A797) {
    float value=0;
    if(patched) {
        __asm {
            push caller
            sub esp, 1Ch
            call fl_high_drawable_alpha_stub
            add esp, 20h
        }
    } else {
        __asm { call owned_high_original_alpha }
    }
    __asm { fstp dword ptr value }
    return value;
}
static void test_high_unit_alpha() {
    void** savedSlot=g_highDrawableEngineSlot;
    void* engine=highEngine;
    g_highDrawableEngineSlot=&engine;
    // Test the full native ordering: restore for simulation, client prepares
    // cached drawable transforms, then the next batch/exit computes draw alpha.
    for(int fps : {120,165,240}) {
        memset(highEngine,0,sizeof highEngine);
        put(highEngine,0x58,6); put(highEngine,0x60,0x3F800000);
        g_highOwner=NULL; g_highOn=true; g_highDrawableReads=0;
        unsigned oldChanges=0,newChanges=0;
        float oldPrevious=1,newPrevious=1;
        bool isolated=true,bounded=true;
        for(int frame=0;frame<fps*2;++frame) {
            fl_high_enter(highEngine);
            unsigned char before[sizeof highEngine]; memcpy(before,highEngine,sizeof before);
            const float old=owned_high_draw_alpha(false);
            const float draw=owned_high_draw_alpha(true);
            oldChanges += old!=oldPrevious; newChanges += draw!=newPrevious;
            oldPrevious=old; newPrevious=draw;
            isolated=isolated && memcmp(before,highEngine,sizeof before)==0 &&
                     (old==0.5f || old==1.0f);
            bounded=bounded && draw>=0 && draw<=1;
            int& phase=*(int*)(highEngine+0x58);
            if(g_highSchedule.begin((std::uint64_t)frame*1000000/fps,phase,fps)) {
                phase=phase>=6 ? 3 : 6;
                *(float*)(highEngine+0x60)=(float)phase*(1.0f/6.0f);
            }
            fl_high_end(highEngine);
        }
        std::printf("OBSERVED unit-alpha ordering fps=%d old_changes=%u new_changes=%u reads=%ld\n",
                    fps,oldChanges,newChanges,g_highDrawableReads);
        check(oldChanges>=59 && oldChanges<=62,"HIGH regression control reproduces about 30 Hz unit-transform alpha");
        check(newChanges>(unsigned)(fps*1.7) && bounded && !g_highSchedule.fault,
              "HIGH actual new x87 reader exposes continuous drawable alpha at 120/165/240 before advance");
        check(isolated && g_highDrawableReads==fps*2,
              "HIGH visual reader never modifies engine simulation fields or consumes simulation boundaries");
    }
    // Preserve the replaced instruction's register/flags/x87 contract.
    float upper=0,lower=0;
    const float canary=2.125f;
    unsigned flagsBefore=0,flagsAfter=0,edxAfter=0,eaxAfter=0,ecxAfter=0;
    unsigned short cwBefore=0,cwAfter=0;
    g_highDrawAlpha=0.3125f;
    __asm {
        push 53A797h
        sub esp, 1Ch
        fnstcw cwBefore
        fld dword ptr canary
        mov eax, 12345678h
        mov ecx, 23456789h
        cmp eax, eax
        pushfd
        pop flagsBefore
        call fl_high_drawable_alpha_stub
        pushfd
        pop flagsAfter
        mov edxAfter, edx
        mov eaxAfter, eax
        mov ecxAfter, ecx
        fstp dword ptr upper
        fstp dword ptr lower
        fnstcw cwAfter
        add esp, 20h
    }
    check(upper==0.3125f && lower==canary && cwBefore==cwAfter,
          "HIGH drawable reader pushes one x87 value and preserves lower stack/control word");
    check(edxAfter==(unsigned)(uintptr_t)engine && eaxAfter==0x12345678 && ecxAfter==0x23456789 && flagsBefore==flagsAfter,
          "HIGH drawable reader retains native EDX result and preserves EAX/ECX/EFLAGS");
    const unsigned nativeBits=*(unsigned*)(highEngine+0x60);
    const float nativeValue=*(float*)(highEngine+0x60);
    check(owned_high_draw_alpha(true,0x532DD0)==nativeValue && *(unsigned*)(highEngine+0x60)==nativeBits,
          "HIGH binding/unknown callers keep native alpha and engine bits");
    g_highOn=false;
    check(owned_high_draw_alpha(true)==nativeValue,"HIGH inactive hook fallback keeps native alpha");
    g_highOn=true; g_highOwner=NULL;
    check(owned_high_draw_alpha(true)==nativeValue,"HIGH unknown engine owner keeps native alpha");
    g_highDrawableEngineSlot=savedSlot; g_highOwner=NULL; g_highOn=false;
    g_highDrawAlpha=1; g_highDrawableReads=0; g_targetFps=90;
}
