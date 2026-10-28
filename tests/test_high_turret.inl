// Execute the compiled turret adapters with owned cache/engine memory and
// test-only branch continuations. No game functions or addresses are executed.
extern "C" __declspec(naked) void owned_turret_yaw_shift() {
    __asm { mov eax, 1 }
    __asm { ret }
}
extern "C" __declspec(naked) void owned_turret_pitch_keep() {
    __asm { xor eax, eax }
    __asm { ret }
}
extern "C" __declspec(naked) void owned_turret_yaw_branch() {
    __asm { call fl_high_turret_yaw_stub }
    __asm { xor eax, eax }
    __asm { ret }
}
extern "C" __declspec(naked) void owned_turret_pitch_branch() {
    __asm { call fl_high_turret_pitch_stub }
    __asm { mov eax, 1 }
    __asm { ret }
}
static bool owned_turret_capture(float* row, float alpha, bool pitch=false) {
    unsigned result=0;
    const float one=1;
    __asm {
        push ebx
        mov ebx, row
        movss xmm0, alpha
        movss xmm4, alpha
        movss xmm3, one
    }
    if(pitch) { __asm { call owned_turret_pitch_branch } }
    else { __asm { call owned_turret_yaw_branch } }
    __asm { mov result, eax }
    __asm { pop ebx }
    return result!=0;
}
static void owned_turret_save(float* row, float alpha) {
    __asm {
        push ebx
        mov ebx, row
        movss xmm0, alpha
        call fl_high_turret_save_stub
        pop ebx
    }
}
static float owned_turret_read() {
    float result=0;
    __asm {
        lea eax, highEngine
        call fl_high_turret_read_stub
        movss result, xmm0
    }
    return result;
}
static float owned_turret_blend(float previous,float next,float alpha) {
    // Native shortest-arc branch, independently checked against +/-pi examples.
    const float pi=3.1415927f, tau=6.2831855f;
    float difference=next-previous;
    if(difference>pi) difference-=tau;
    else if(difference<-pi) difference+=tau;
    return previous+difference*alpha;
}
static void test_high_turret() {
    void* savedYaw=g_highTurretYawShift;
    void* savedPitch=g_highTurretPitchKeep;
    g_highTurretYawShift=(void*)&owned_turret_yaw_shift;
    g_highTurretPitchKeep=(void*)&owned_turret_pitch_keep;
    g_highOn=true;
    for(int fps : {120,165,240}) {
        memset(highEngine,0,sizeof highEngine);
        put(highEngine,0x58,6); put(highEngine,0x60,0x3F800000);
        g_highOwner=NULL;
        float row[5]={},oldRow[5]={};
        unsigned oldCaptures=0,newCaptures=0,expectedCaptures=0,previousSerial=0;
        unsigned oldChanges=0,newChanges=0;
        float oldPrevious=0,newPrevious=0;
        bool isolated=true,duplicates=true,pitchMatches=true,bounded=true;
        for(int frame=0;frame<fps*2;++frame) {
            fl_high_enter(highEngine);
            unsigned char before[sizeof highEngine]; memcpy(before,highEngine,sizeof before);
            const FlHighFpsSchedule scheduleBefore=g_highSchedule;
            const float old=*(float*)(highEngine+0x60);
            const float alpha=owned_turret_read();
            const unsigned serial=g_highTurretClock.serial;
            expectedCaptures+=serial!=previousSerial; previousSerial=serial;
            const float desired=(g_highTurretClock.completions+1)*0.02f;
            float oldValue=0,newValue=0,first=0;
            for(int repeated=0;repeated<3;++repeated) {
                if(old==1 || old==oldRow[4]) {
                    ++oldCaptures; oldRow[0]=oldRow[2]; oldRow[2]=desired;
                    oldValue=oldRow[0];
                } else oldValue=owned_turret_blend(oldRow[0],oldRow[2],old);
                oldRow[4]=old;
                const bool capture=owned_turret_capture(row,alpha);
                pitchMatches=pitchMatches && capture==owned_turret_capture(row,alpha,true);
                if(capture) {
                    ++newCaptures; row[0]=row[2]; row[2]=desired;
                    newValue=row[0];
                } else newValue=owned_turret_blend(row[0],row[2],alpha);
                owned_turret_save(row,alpha);
                if(repeated==0) first=newValue;
                else duplicates=duplicates && first==newValue;
            }
            oldChanges+=oldValue!=oldPrevious; newChanges+=newValue!=newPrevious;
            oldPrevious=oldValue; newPrevious=newValue;
            bounded=bounded && alpha>=0 && alpha<1;
            isolated=isolated && memcmp(before,highEngine,sizeof before)==0 &&
                     memcmp(&scheduleBefore,&g_highSchedule,sizeof scheduleBefore)==0;
            int& phase=*(int*)(highEngine+0x58);
            if(g_highSchedule.begin((std::uint64_t)frame*1000000/fps,phase,fps)) {
                phase=phase>=6 ? 3 : 6;
                *(float*)(highEngine+0x60)=(float)phase*(1.0f/6.0f);
            }
            fl_high_end(highEngine);
        }
        std::printf("OBSERVED turret fps=%d old_changes=%u new_changes=%u old_captures=%u new_captures=%u expected=%u\n",
                    fps,oldChanges,newChanges,oldCaptures,newCaptures,expectedCaptures);
        check(oldChanges<=62 && newChanges>(unsigned)(fps*1.7),
              "HIGH native turret cache regression: discrete original reader vs continuous visual yaw");
        check(newCaptures==expectedCaptures && oldCaptures>newCaptures*2 && pitchMatches,
              "HIGH compiled yaw/pitch guards capture once per cache epoch, including triple draws");
        check(isolated && duplicates && bounded,
              "HIGH turret repeats are visually stable and adapters never modify simulation memory");
        // Hold a saturated visual alpha through a long network wait: identical
        // fractions must not be mistaken for a fresh simulation endpoint.
        const unsigned stableSerial=g_highTurretClock.serial;
        owned_turret_save(row,owned_turret_read());
        bool held=true;
        for(int i=0;i<500;++i) {
            g_highSchedule.now+=10000;
            g_highTurretClock.observe(g_highSchedule,7);
            const float alpha=owned_turret_read();
            held=held && !owned_turret_capture(row,alpha) && !owned_turret_capture(row,alpha,true);
            owned_turret_save(row,alpha);
        }
        check(held && g_highTurretClock.serial==stableSerial && owned_turret_read()==0.9999f,
              "HIGH plateau/network wait cannot recapture endpoints or extrapolate beyond newest angle");
    }
    check(owned_turret_blend(3.1f,-3.1f,0.5f)>3.14f &&
          owned_turret_blend(-3.1f,3.1f,0.5f)<-3.14f,
          "HIGH yaw retains shortest arc across both +/-pi wrap directions");

    FlHighFpsSchedule schedule;
    FlHighTurretClock clock;
    schedule.accepted=1; schedule.now=100; clock.observe(schedule,3);
    const unsigned first=clock.serial;
    schedule.accepted=2; schedule.now=33100; clock.observe(schedule,6);
    check(clock.serial==first+1 && clock.alpha==0 && clock.completions==1,
          "HIGH only successful phase-six completion creates a turret epoch");
    schedule.retry=true; schedule.now=100000; clock.observe(schedule,7);
    check(clock.serial==first+1 && clock.alpha==0.9999f,
          "HIGH rejected native tick changes neither cache epoch nor endpoint count");
    schedule.now=0; clock.observe(schedule,7);
    check(clock.serial==first+1 && clock.alpha==0,
          "HIGH backward clock is bounded and cannot create a cache commit");
    clock.serial=~0u; schedule.accepted=3; schedule.now=1000; clock.observe(schedule,6);
    check(clock.serial==1,"HIGH turret epoch rollover reserves native fresh-cache zero");

    // Inactive adapters reproduce the original equality/one predicates and save.
    g_highOn=false;
    float nativeRow[5]={0,0,0,0,0.5f};
    check(owned_turret_capture(nativeRow,0.5f) && owned_turret_capture(nativeRow,1) &&
          !owned_turret_capture(nativeRow,0.25f) && owned_turret_capture(nativeRow,0.5f,true) &&
          owned_turret_capture(nativeRow,1,true) && !owned_turret_capture(nativeRow,0.25f,true),
          "HIGH inactive yaw/pitch guards retain native equality and one-value decisions");
    owned_turret_save(nativeRow,0.375f);
    *(float*)(highEngine+0x60)=0.625f;
    check(nativeRow[4]==0.375f && owned_turret_read()==0.625f,
          "HIGH inactive SSE read/save adapters retain original values");
    // Missing native endpoints use the original sentinel initialization branch;
    // saving the marker must touch only the final word, never those endpoints.
    g_highOn=true;
    const float sentinel=-1001;
    float fresh[5]={sentinel,sentinel,sentinel,sentinel,0};
    unsigned beforeSave[5]; memcpy(beforeSave,fresh,sizeof fresh);
    owned_turret_save(fresh,0.625f);
    check(memcmp(beforeSave,fresh,16)==0 && *(unsigned*)(fresh+4)==g_highTurretClock.serial,
          "HIGH marker save preserves fresh-cache yaw/pitch sentinels and all endpoint fields");

    // TEST EBP flags cross the replaced MOVSS, while only XMM0 changes.
    g_highOn=true;
    g_highTurretClock.alpha=0.3125f;
    __declspec(align(16)) unsigned input[8][4],output[8][4];
    for(int r=0;r<8;++r) for(int c=0;c<4;++c) input[r][c]=0x3f000000u+r*16+c;
    unsigned flagsBefore=0,flagsAfter=0,eaxAfter=0,ecxAfter=0,edxAfter=0,mxBefore=0,mxAfter=0;
    unsigned short cwBefore=0,cwAfter=0;
    float canary=1.2345f,remaining=0;
    for(unsigned zf : {0u,1u}) {
        __asm {
            lea eax, input
            movups xmm0,[eax]
            movups xmm1,[eax+10h]
            movups xmm2,[eax+20h]
            movups xmm3,[eax+30h]
            movups xmm4,[eax+40h]
            movups xmm5,[eax+50h]
            movups xmm6,[eax+60h]
            movups xmm7,[eax+70h]
            fnstcw cwBefore
            stmxcsr mxBefore
            fld dword ptr canary
            mov ecx,12345678h
            mov edx,23456789h
            lea eax,highEngine
            cmp zf,1
            pushfd
            pop flagsBefore
            call fl_high_turret_read_stub
            pushfd
            pop flagsAfter
            mov eaxAfter,eax
            mov ecxAfter,ecx
            mov edxAfter,edx
            fstp dword ptr remaining
            fnstcw cwAfter
            stmxcsr mxAfter
            lea eax,output
            movups [eax],xmm0
            movups [eax+10h],xmm1
            movups [eax+20h],xmm2
            movups [eax+30h],xmm3
            movups [eax+40h],xmm4
            movups [eax+50h],xmm5
            movups [eax+60h],xmm6
            movups [eax+70h],xmm7
        }
        check(flagsBefore==flagsAfter && eaxAfter==(unsigned)(uintptr_t)highEngine &&
              ecxAfter==0x12345678 && edxAfter==0x23456789,
              "HIGH turret MOVSS adapter preserves live TEST flags and GPRs for both JE outcomes");
        check(output[0][0]==0x3EA00000 && !output[0][1] && !output[0][2] && !output[0][3] &&
              memcmp(input+1,output+1,7*16)==0 && cwBefore==cwAfter && mxBefore==mxAfter && remaining==canary,
              "HIGH turret MOVSS preserves seven XMM registers, native upper lanes, x87 and MXCSR");
    }
    g_highTurretYawShift=savedYaw; g_highTurretPitchKeep=savedPitch;
    g_highOn=false; g_highOwner=NULL; g_targetFps=90;
}
