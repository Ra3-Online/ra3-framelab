extern "C" __declspec(naked) void owned_drag_original() {
    __asm { mulss xmm0,xmm3 }
    __asm { mulss xmm1,xmm4 }
    __asm { ret }
}
static void owned_drag_step(const float* input,float* output,bool patched) {
    __asm {
        mov eax,input
        movss xmm0,[eax]
        movss xmm1,[eax+4]
        movss xmm3,[eax+8]
        movss xmm4,[eax+12]
    }
    if(patched) { __asm { call fl_drag_scroll_stub } }
    else { __asm { call owned_drag_original } }
    __asm {
        mov eax,output
        movss [eax],xmm0
        movss [eax+4],xmm1
    }
}
static void test_drag_scroll() {
    // Settings are evaluated by the native caller on every update. Changing
    // input speed/axis coefficients here catches stale-setting copies.
    const float cases[][4]={
        {0.03f,0.06f,-50,20},{0.12f,0.025f,17,-90},
        {1,1,0,0},{0.05f,0.08f,-100,-100},{0.25f,0.125f,40,80}
    };
    for(int fps : {30,60,90,120,165,180,240}) {
        g_dragScrollScale=30.0f/fps;
        g_dragScrollCalls=0;
        for(const auto& input:cases) {
            float unchanged[4]; memcpy(unchanged,input,sizeof input);
            float native[2]={}; owned_drag_step(input,native,false);
            double total[2]={};
            for(int frame=0;frame<fps;++frame) {
                float output[2]={}; owned_drag_step(input,output,fps!=30);
                total[0]+=output[0]; total[1]+=output[1];
            }
            bool equal=true;
            for(int axis=0;axis<2;++axis) {
                const double reference=(double)native[axis]*30;
                const double error=total[axis]-reference;
                equal=equal && error*error < (reference*reference+1)*1e-10;
            }
            check(equal,"DRAG actual SSE per-second displacement matches native 30 at 30/60/90/120/165/180/240");
            check(memcmp(input,unchanged,sizeof input)==0,
                  "DRAG scaling never writes mouse coordinates, anchors or native user/axis settings");
        }
        check(g_dragScrollCalls==(fps==30 ? 0 : fps*5),
              "DRAG keeps every display update responsive; original 30 executes no adapter");
        std::printf("OBSERVED drag fps=%d scale=%.9f update_calls=%ld baseline_updates=30\n",fps,g_dragScrollScale,g_dragScrollCalls);
    }
    // The two replaced MULSS instructions do not alter integer flags/GPRs or
    // other SSE lanes. Verify actual compiled code with nonzero canaries.
    __declspec(align(16)) float input[8][4],output[8][4];
    for(int r=0;r<8;++r) for(int c=0;c<4;++c) input[r][c]=2.0f+r+c*0.125f;
    unsigned before=0,after=0,eaxAfter=0,ecxAfter=0,edxAfter=0,mxBefore=0,mxAfter=0;
    unsigned short cwBefore=0,cwAfter=0;
    float x87=2.125f,remaining=0;
    g_dragScrollScale=0.25f;
    __asm {
        lea eax,input
        movups xmm0,[eax]
        movups xmm1,[eax+10h]
        movups xmm2,[eax+20h]
        movups xmm3,[eax+30h]
        movups xmm4,[eax+40h]
        movups xmm5,[eax+50h]
        movups xmm6,[eax+60h]
        movups xmm7,[eax+70h]
        fld dword ptr x87
        fnstcw cwBefore
        stmxcsr mxBefore
        mov eax,12345678h
        mov ecx,23456789h
        mov edx,3456789Ah
        cmp eax,eax
        pushfd
        pop before
        call fl_drag_scroll_stub
        pushfd
        pop after
        mov eaxAfter,eax
        mov ecxAfter,ecx
        mov edxAfter,edx
        fnstcw cwAfter
        stmxcsr mxAfter
        fstp dword ptr remaining
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
    check(output[0][0]==input[0][0]*input[3][0]*0.25f &&
          output[1][0]==input[1][0]*input[4][0]*0.25f,
          "DRAG actual adapter scales native two-axis products exactly once");
    check(memcmp(input[0]+1,output[0]+1,12)==0 && memcmp(input[1]+1,output[1]+1,12)==0 &&
          memcmp(input+2,output+2,6*16)==0,
          "DRAG all six non-result XMM registers and result upper lanes preserved");
    check(before==after && eaxAfter==0x12345678 && ecxAfter==0x23456789 && edxAfter==0x3456789A,
          "DRAG telemetry and arithmetic preserve integer flags and GPRs");
    check(cwBefore==cwAfter && (mxBefore&0xFFC0)==(mxAfter&0xFFC0) && remaining==x87,
          "DRAG x87 stack/control word and MXCSR rounding/mask controls preserved");
    g_dragScrollScale=1; g_dragScrollCalls=0;
}
