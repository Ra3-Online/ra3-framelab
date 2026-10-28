// Execute compiled visual readers and wrappers on owned data. This is not a
// game, GPU, MCV deployment, tank trajectory or multiplayer acceptance test.
static float owned_extra_alpha(bool structure, void* engine) {
    float value=0;
    if (structure) {
        __asm { mov ecx, engine }
        __asm { call fl_high_structure_alpha_stub }
    } else {
        __asm { mov eax, engine }
        __asm { call fl_high_bone_alpha_stub }
    }
    __asm { movss value, xmm0 }
    return value;
}
static void test_high_visual_readers() {
    unsigned char module[72]={},logic[84]={},engine[100]={},other[100]={};
    unsigned char* lp=logic; unsigned char* ep=engine;
    const auto oldL=g_constructionLogicSlot, oldE=g_constructionEngineSlot;
    g_constructionLogicSlot=&lp; g_constructionEngineSlot=&ep;
    g_constructionOrigFn=(void*)&mock_progress; g_constructionOn=true;
    put(module,56,45); put(module,60,100); module[68]=1; put(logic,80,100);
    for (int fps : {120,165,240}) {
        g_targetFps=fps; g_highOn=true; g_highOwner=NULL;
        put(engine,88,6); put(engine,96,0x3F800000);
        unsigned oldSteps=0,newSteps=0; float oldPrevious=0,newPrevious=0;
        bool consistent=true,isolated=true;
        for (int frame=0;frame<fps*2;++frame) {
            fl_high_enter(engine);
            unsigned char before[100]; memcpy(before,engine,sizeof before);
            const FlHighFpsSchedule scheduleBefore=g_highSchedule;
            const float native=*(float*)(engine+96);
            const float bone=owned_extra_alpha(false,engine), structure=owned_extra_alpha(true,engine);
            float progress=0; fl_wrap_construction_progress(module,NULL,&progress,0);
            consistent=consistent && bone==g_highDrawAlpha && structure==bone &&
                       absf(progress-bone/45)<0.000001f;
            oldSteps += native!=oldPrevious; newSteps += bone!=newPrevious;
            oldPrevious=native; newPrevious=bone;
            isolated=isolated && !memcmp(engine,before,sizeof before) &&
                     !memcmp(&scheduleBefore,&g_highSchedule,sizeof scheduleBefore);
            int& phase=*(int*)(engine+88);
            if (g_highSchedule.begin((std::uint64_t)frame*1000000/fps,phase,fps)) {
                phase=phase>=6 ? 3 : 6; *(float*)(engine+96)=phase/6.0f;
            }
            fl_high_end(engine);
        }
        std::printf("OBSERVED extra-visual readers fps=%d old_steps=%u new_steps=%u\n",fps,oldSteps,newSteps);
        check(consistent && oldSteps>=59 && oldSteps<=62 && newSteps>(unsigned)(fps*1.7),
              "HIGH bone/structure/mode-0 readers use continuous display fraction during native pre-update ordering");
        check(isolated,"HIGH extra visual callbacks leave engine bytes and simulation scheduler unchanged");
    }
    put(other,96,0x3F000000); g_highOwner=engine;
    check(owned_extra_alpha(false,other)==0.5f && owned_extra_alpha(true,other)==0.5f,
          "HIGH extra visual adapters retain native read for a different owner");
    g_highOn=false;
    check(owned_extra_alpha(false,other)==0.5f && owned_extra_alpha(true,other)==0.5f,
          "HIGH inactive extra readers retain native memory load");
    g_highOn=true; g_highDrawAlpha=0.3125f;
    // Test ABI with native live GPRs/flags, all XMM lanes and an x87 canary.
    const unsigned lanes[4]={0x3F800000,0x40000000,0x40400000,0x40800000};
    unsigned after[8][4]={}; unsigned eaxAfter=0,ecxAfter=0,edxAfter=0,beforeFlags=0,afterFlags=0;
    unsigned mxBefore=0,mxAfter=0; float canary=3.25f, fpAfter=0;
    for (int kind=0;kind<2;++kind) {
        void* fn=kind ? (void*)&fl_high_structure_alpha_stub : (void*)&fl_high_bone_alpha_stub;
        __asm {
            movups xmm0, lanes
            movups xmm1, lanes
            movups xmm2, lanes
            movups xmm3, lanes
            movups xmm4, lanes
            movups xmm5, lanes
            movups xmm6, lanes
            movups xmm7, lanes
            fld dword ptr canary
            stmxcsr mxBefore
            lea eax, engine
            lea ecx, engine
            mov edx, 12345678h
            cmp edx, edx
            pushfd
            pop beforeFlags
            call dword ptr fn
            pushfd
            pop afterFlags
            mov eaxAfter, eax
            mov ecxAfter, ecx
            mov edxAfter, edx
            movups after[0], xmm0
            movups after[16], xmm1
            movups after[32], xmm2
            movups after[48], xmm3
            movups after[64], xmm4
            movups after[80], xmm5
            movups after[96], xmm6
            movups after[112], xmm7
            stmxcsr mxAfter
            fstp dword ptr fpAfter
        }
        bool intact=after[0][0]==0x3EA00000 && !after[0][1] && !after[0][2] && !after[0][3];
        for (int i=1;i<8;++i) intact=intact && !memcmp(after[i],lanes,sizeof lanes);
        check(intact && mxBefore==mxAfter && fpAfter==canary,
              "HIGH extra MOVSS adapters retain upper-lane zeroing, other XMMs, x87 stack and MXCSR");
        check(eaxAfter==(unsigned)(uintptr_t)engine && ecxAfter==eaxAfter && edxAfter==0x12345678 && beforeFlags==afterFlags,
              "HIGH extra MOVSS adapters preserve all live GPRs and EFLAGS");
    }
    float progress=0; progressCalls=0;
    const char result=fl_wrap_construction_progress(module,NULL,&progress,1);
    check(result==37 && progress==0.625f && progressCalls==1 && progressMode==1,
          "HIGH mode-1 simulation query still forwards once to original with unchanged result");
    g_constructionOn=false; g_constructionOrigFn=NULL;
    g_constructionLogicSlot=oldL; g_constructionEngineSlot=oldE;
    g_highOn=false; g_highOwner=NULL; g_targetFps=90;
}

static void test_high_visual_observations() {
    unsigned char module[604]={},drawable[816]={},object[1300]={},tpl[260]={};
    put(module,0,0xC62D30); put(module,8,(unsigned)(uintptr_t)drawable);
    put(drawable,4,(unsigned)(uintptr_t)tpl); put(drawable,312,(unsigned)(uintptr_t)object);
    put(drawable,608,7); put(tpl,256,0x1234);
    put(drawable,208,0x3F800000); put(drawable,220,0x40000000);
    auto reset=[&]() { memset(g_highVisual,0,sizeof g_highVisual); g_highVisualOverflow=0; };
    auto run=[&](bool smooth) {
        reset();
        for (unsigned i=0;i<22;++i) {
            g_frameCount=(long)i;
            put(object,56,i/11+1); put(drawable,292,smooth ? i+1 : i/11+1);
            put(module,288,i+1); put(module,200,i+1); put(module,300,2);
            unsigned char beforeM[sizeof module],beforeD[sizeof drawable],beforeO[sizeof object];
            memcpy(beforeM,module,sizeof module); memcpy(beforeD,drawable,sizeof drawable); memcpy(beforeO,object,sizeof object);
            fl_high_visual_observe(module); fl_high_visual_observe(module);
            check(!memcmp(beforeM,module,sizeof module) && !memcmp(beforeD,drawable,sizeof drawable) &&
                  !memcmp(beforeO,object,sizeof object),"HIGH model observation never changes module, drawable or object bytes");
        }
        const HighVisualObservation& r=g_highVisual[7];
        check(r.samples==22 && r.rawSteps==1 && r.clipSteps==21 && r.stampSteps==21 && r.moving==21,
              "HIGH bounded observation separates simulation steps, clip changes and duplicate draws");
        check(smooth ? r.visualSteps==21 && !r.held : r.visualSteps==1 && r.held==20 && r.maxHeld==10,
              "HIGH observer distinguishes smooth display movement from stepwise whole-object movement");
    };
    run(false); run(true);
    drawable[318]=4; ++g_frameCount; fl_high_visual_observe(module);
    check(g_highVisual[7].bypass==1,"HIGH diagnostic exposes native interpolation-bypass flag without clearing it");
    put(drawable,608,134); ++g_frameCount; fl_high_visual_observe(module);
    check(g_highVisual[8].drawableId==134 && g_highVisual[8].samples==1,
          "HIGH reused pointer with a new native drawable ID starts a separate observation");
    for (int i=0;i<128;++i) g_highVisual[i].module=1;
    fl_high_visual_observe(module);
    check(g_highVisualOverflow==1,"HIGH full observation table drops a sample with explicit overflow and no game writes");
    reset(); put(drawable,608,7); put(module,0,0x12345678); put(module,300,0); put(module,328,7);
    fl_high_visual_observe(module);
    check(g_highVisual[7].samples==1 && g_highVisual[7].mode[1]==7 && (g_highVisual[7].modeMask&128),
          "HIGH secondary unpack animation selects a generic model even when primary track is inactive");
    reset(); put(module,328,0); fl_high_visual_observe(module);
    check(g_highVisual[7].samples==1,"HIGH moving generic/scripted model is observed independently of its draw type");
    reset(); put(drawable,220,fl_visual_word(drawable,208)); fl_high_visual_observe(module);
    check(!g_highVisual[7].module,"HIGH unrelated model with no selected animation remains outside observation table");
    reset(); g_frameCount=0;
}
