// Execute actual visual wrappers/assembly with owned params, objects and stack.
// No game globals, engine calls, injection, GPU drawing or GUI are used.
#include "../src/framelab.cpp"
#include <initializer_list>

using namespace fl;
static int passed = 0, failed = 0;
static float absf(float value) { return value < 0.0f ? -value : value; }
static void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}

static void put(unsigned char* tpl, unsigned off, unsigned value) {
    memcpy(tpl + off, &value, sizeof value);
}
static void retail_template(unsigned char* tpl) {
    memset(tpl, 0, 260);
    put(tpl, 256, kFlSovietPowerGlowId);
    put(tpl, 80, 5);
    for (unsigned off : {100u,156u,168u}) put(tpl, off, 1);
    for (unsigned off : {104u,108u,160u,164u}) put(tpl, off, 0x41700000u);
    for (unsigned off : {172u,176u}) put(tpl, off, 0x3F800000u);
}
static void* mockSelf = NULL;
static void* mockHandle = NULL;
static const FlGpuParticleParams* mockInput = NULL;
static FlGpuParticleParams mockParams;
static int mockCalls = 0;
// A fastcall with unused EDX has the same ABI as the native thiscall: ECX,
// two stack args, callee ret 8, and EAX result. It never calls the game.
static void* __fastcall mock_gpu_ctor(void* self, void*, void* handle, const FlGpuParticleParams* params) {
    ++mockCalls;
    mockSelf = self; mockHandle = handle; mockInput = params;
    if (params) mockParams = *params;
    return self;
}
static void run_glow(void* self, void* handle, const FlGpuParticleParams* params,
                     const FlGpuParticleParams& expected, bool adjusted) {
    mockCalls = 0;
    const FlGpuParticleParams before = *params;
    void* result = fl_wrap_gpu_ctor(self, NULL, handle, params);
    check(mockCalls == 1 && result == self && mockSelf == self && mockHandle == handle,
          "actual GPU wrapper calls original exactly once and preserves ABI arguments/result");
    check(!memcmp(&mockParams, &expected, sizeof expected), "all 48 native parameter bytes forwarded correctly");
    check(!memcmp(params, &before, sizeof before), "caller creation params remain byte-identical");
    check((mockInput != params) == adjusted, "stack clone used only for eligible glow");
}
static void test_glow() {
    unsigned char tpl[260], system[12] = {}, self[4] = {};
    retail_template(tpl);
    void* templatePointer = tpl;
    memcpy(system + 8, &templatePointer, sizeof templatePointer);
    void* ref[2] = {system, NULL};
    void* handle = ref;
    FlGpuParticleParams input;
    for (unsigned i = 0; i < 12; ++i) input.words[i] = 0xAABBCC00u + i;
    input.words[9] = 15;
    FlGpuParticleParams expected = input;
    expected.words[9] = 16;
    g_glowOrigFn = (void*)&mock_gpu_ctor;
    g_glowOn = true; g_psOn = true;
    g_glowSeen = 0; g_glowAdjusted = 0; g_glowSkipped = 0;
    for (int fps : {60,90}) {
        g_targetFps = fps;
        run_glow(self, &handle, &input, expected, true);
    }
    check(g_glowSeen == 2 && g_glowAdjusted == 2 && g_glowSkipped == 0, "eligible glow diagnostics count actual corrections");
    for (int fps : {30,120}) {
        g_targetFps = fps;
        run_glow(self, &handle, &input, input, false);
    }
    g_targetFps = 90;
    g_psOn = false;
    run_glow(self, &handle, &input, input, false);
    g_psOn = true; g_glowOn = false;
    run_glow(self, &handle, &input, input, false);
    g_glowOn = true;
    const unsigned offsets[] = {80,100,104,108,112,156,160,164,168,172,176,256};
    for (unsigned off : offsets) {
        retail_template(tpl);
        put(tpl, off, fl_visual_word(tpl, off) ^ 1u);
        run_glow(self, &handle, &input, input, false);
    }
    retail_template(tpl); tpl[72] = 1;
    run_glow(self, &handle, &input, input, false);
    retail_template(tpl);
    for (unsigned life : {0u,14u,16u,0xffffffffu}) {
        input.words[9] = life;
        run_glow(self, &handle, &input, input, false);
    }
    check(g_glowAdjusted == 2 && g_glowSeen == 18 && g_glowSkipped == 16,
          "different template not counted; changed fields/lifetimes explicitly counted as skipped");
    input.words[9] = 15;
    handle = NULL;
    run_glow(self, &handle, &input, input, false);
    ref[0] = NULL; handle = ref;
    run_glow(self, &handle, &input, input, false);
    templatePointer = NULL;
    memcpy(system + 8, &templatePointer, sizeof templatePointer);
    ref[0] = system;
    run_glow(self, &handle, &input, input, false);
    run_glow(self, NULL, &input, input, false);
    g_glowOn = false; g_psOn = false; g_glowOrigFn = NULL;
}


static int vertexCalls = 0;
static void* vertexStorage;
static const void* vertexParticle;
static float* vertexBuffer;
static float nativeVertices[60];
static unsigned nativeWritten = 50;
static float* __fastcall mock_vertices(void* storage, void*, const void* particle, float* out) {
    ++vertexCalls; vertexStorage = storage; vertexParticle = particle; vertexBuffer = out;
    if (out) memcpy(out, nativeVertices, sizeof nativeVertices);
    return out ? out + nativeWritten : NULL;
}
static void test_vertices() {
    unsigned char tpl[260], system[12] = {}, storage[40] = {}, particle[88] = {};
    retail_template(tpl);
    void* t = tpl; memcpy(system+8, &t, 4);
    void* ref[2] = {system, NULL}; void* handle = ref;
    memcpy(storage+4, &handle, 4);
    put(storage,36,5); put(particle,40,16);
    g_glowVerticesOrigFn = (void*)&mock_vertices;
    g_glowOn = true; g_psOn = true;
    g_glowVertexCalls = g_glowVertexAdjusted = g_glowVertexSkipped = 0;
    g_glowVertexTraceCount = g_glowVertexTraceLogged = 0;
    memset((void*)g_glowVertexRejected,0,sizeof g_glowVertexRejected);
    for (int i=0;i<60;++i) nativeVertices[i] = i * 0.125f;
    for (int i=0;i<5;++i) nativeVertices[i*10+3] = 16.0f;
    auto invoke = [&](bool adjust) {
        float out[60]; memcpy(out,nativeVertices,sizeof out); vertexCalls=0;
        unsigned char before[88]; memcpy(before,particle,sizeof before);
        float* result=fl_wrap_gpu_vertices(storage,NULL,particle,out);
        check(vertexCalls==1 && vertexStorage==storage && vertexParticle==particle &&
              vertexBuffer==out && result==out+nativeWritten, "actual upload preserves native call, args and end pointer");
        float expected[60]; memcpy(expected,nativeVertices,sizeof expected);
        if(adjust) for(int i=0;i<5;++i) expected[i*10+3]=kFlGlowVisibleLifetime;
        check(!memcmp(out,expected,sizeof out), "only five visible-life floats change; centre, birth, velocity, position and tail survive");
        check(!memcmp(particle,before,sizeof before), "upload never changes native particle object");
    };
    for (int fps : {60,90}) { g_targetFps=fps; invoke(true); }
    check(g_glowVertexAdjusted==2, "upload counts actual paired correction");
    for (int fps : {30,120}) {g_targetFps=fps;invoke(false);}
    g_targetFps=90;g_glowOn=false;invoke(false);g_glowOn=true;
    g_psOn=false;invoke(false);g_psOn=true;
    for(unsigned count : {0u,1u,3u,4u,6u,0xffffffffu}) {put(storage,36,count);invoke(false);}
    put(storage,36,5);
    for(unsigned written : {0u,40u,49u,51u,60u}) {nativeWritten=written;invoke(false);}
    nativeWritten=50;
    for(unsigned life : {0u,15u,17u,0xffffffffu}) {put(particle,40,life);invoke(false);}
    put(particle,40,16);
    for(unsigned off : {72u,80u,100u,104u,108u,112u,156u,160u,164u,168u,172u,176u,256u}) {
        retail_template(tpl);put(tpl,off,fl_visual_word(tpl,off)^1u);invoke(false);
    }
    retail_template(tpl);
    nativeVertices[23]=15.0f;invoke(false);nativeVertices[23]=16.0f;
    nativeVertices[43]=15.0f;invoke(false);nativeVertices[43]=16.0f;
    check(g_glowVertexRejected[1]==6 && g_glowVertexRejected[2]==4 &&
          g_glowVertexRejected[3]==5 && g_glowVertexRejected[4]==2,
          "diagnostics distinguish count, native span, pool and fifth-vertex lifetime rejections");
    g_glowOn=false;g_psOn=false;g_glowVerticesOrigFn=NULL;
}

// Actual integer millisecond progression and float birth reconstruction, rather
// than assuming ideal exact ages. This is a CPU model, never a GPU visual test.
static void test_glow_seam() {
    for(int fps : {60,90}) for(unsigned base : {0u,100001u,3600000u,28800000u}) {
        const int q=fps/30;
        int gaps15=0, overlaps16=0, gapsPair=0, overlapsPair=0;
        for(int phase=0;phase<q;++phase) {
            const unsigned begin=base+(unsigned)(phase*1000/fps);
            const float expiry=(float)((double)begin*(double)0.03f+16.0);
            const float birth=(float)((double)expiry-16.0);
            for(int frame=0;frame<=16*q;++frame) {
                const unsigned now=base+(unsigned)((frame+phase)*1000/fps);
                const float shader=(float)((float)((double)now*0.001)*30.0f);
                const float age=shader-birth;
                const bool replaced=frame==16*q;
                gaps15+=!replaced && age>15.0f;
                overlaps16+=replaced && age<=16.0f;
                gapsPair+=!replaced && age>kFlGlowVisibleLifetime;
                overlapsPair+=replaced && age<=kFlGlowVisibleLifetime;
            }
        }
        check(gaps15>0, "native 15 has sampled intermediate-display gaps in clock model");
        check(overlaps16>0, "visual2 16 overlaps on replacement in clock model");
        check(gapsPair==0 && overlapsPair==0, "paired retention/visible-life avoids both gaps and overlap in clock model up to eight hours");
    }
}
static int progressCalls=0, progressMode=0;
static void* progressModule=NULL;
static float* progressOutput=NULL;
static char __fastcall mock_progress(void* module,void*,float* out,int mode) {
    ++progressCalls;progressModule=module;progressOutput=out;progressMode=mode;
    if(out)*out=0.625f;
    return 37;
}
static void test_construction() {
    unsigned char module[72]={},logic[84]={},engine[100]={};
    unsigned char* lp=logic;unsigned char* ep=engine;
    auto oldL=g_constructionLogicSlot;auto oldE=g_constructionEngineSlot;
    g_constructionLogicSlot=&lp;g_constructionEngineSlot=&ep;
    g_constructionOrigFn=(void*)&mock_progress;g_constructionOn=true;
    g_constructionTraceCount=g_constructionTraceLogged=0;
    memset(g_constructionRecent,0,sizeof g_constructionRecent);
    module[68]=1;put(module,56,45);put(module,60,100);put(logic,80,100);
    for(int fps : {60,90}) {
        float previous=-1.0f;
        for(int phase=0;phase<fps/15;++phase) {
            const float fraction=(float)phase/(fps/15);memcpy(engine+96,&fraction,4);
            unsigned char before[72];memcpy(before,module,sizeof before);
            float out=-99;progressCalls=0;
            const char result=fl_wrap_construction_progress(module,NULL,&out,0);
            check(result==1 && progressCalls==0 && out>=0 && out<1 && out>previous,
                  "display progresses on every 60/90 frame using native interpolation");
            check(absf(out-fraction/45.0f)<0.000001f && !memcmp(before,module,sizeof before),
                  "display progress matches same-domain ratio and leaves module byte-identical");
            previous=out;
        }
    }
    put(module,64,110);module[69]=1;put(logic,80,120);
    for(float fraction : {0.0f,0.5f,1.0f}) {
        memcpy(engine+96,&fraction,4);float out;
        fl_wrap_construction_progress(module,NULL,&out,0);
        check(absf(out-10.0f/45.0f)<0.000001f,"held construction ignores changing fraction/current time");
    }
    module[69]=0;put(module,60,121);float f=0.5f;memcpy(engine+96,&f,4);
    float out;fl_wrap_construction_progress(module,NULL,&out,0);
    check(out==0.0f,"future start is zero progress, never unsigned completion");
    module[68]=0;const char inactive=fl_wrap_construction_progress(module,NULL,&out,0);
    check(inactive==0 && out==0,"inactive module retains native false/zero display result");module[68]=1;
    auto forwarded=[&](int mode){
        progressCalls=0;out=-99;
        const char r=fl_wrap_construction_progress(module,NULL,&out,mode);
        check(progressCalls==1 && r==37 && out==0.625f && progressMode==mode &&
              progressModule==module && progressOutput==&out,
              "fallback/other mode calls original once with unchanged ABI/result");
    };
    forwarded(1);forwarded(2);g_constructionOn=false;forwarded(0);g_constructionOn=true;
    lp=NULL;forwarded(0);lp=logic;ep=NULL;forwarded(0);ep=engine;
    check(fl_construction_progress(0,0xffffffffu,0,10,0.5f,false)==0.15f,"unsigned clock rollover keeps local signed delta");
    check(fl_construction_progress(10,10,0,0,0.0f,false)==1.0f,"zero duration finishes after start");
    check(fl_construction_progress(9,10,0,0,0.0f,false)==0.0f,"zero duration does not finish before start");
    check(fl_construction_progress(25,10,0,10,0.5f,false)==1.0f,"completed progress capped at one");
    check(fl_construction_progress(10,10,0,10,-1.0f,false)==0.0f &&
          fl_construction_progress(10,10,0,10,2.0f,false)==0.0f,"invalid interpolation does not create completion");
    check(g_constructionTraceCount>0 && g_constructionTraceCount<=64,"diagnostics are bounded and observe inactive/active transitions");
    g_constructionOn=false;g_constructionLogicSlot=oldL;g_constructionEngineSlot=oldE;
    g_constructionOrigFn=NULL;
}

static int animateCalls = 0, objectProgressCalls = 0;
static const void* queriedObject = NULL;
static char objectActive = 1;
static float objectProgress = 0.002222222f;
static char __fastcall mock_object_progress(const void* object, void*, float* out) {
    ++objectProgressCalls; queriedObject = object; *out = objectProgress;
    return objectActive;
}
static void __fastcall mock_animate(void* self, void*) {
    ++animateCalls;
    put((unsigned char*)self, 200, *g_constructionStamp);
    ((unsigned char*)self)[592] = 0;
    for (unsigned i=0;i<3;++i) {
        const unsigned off=284+i*28;
        if (fl_visual_word(self,off) && fl_visual_word(self,off+16)==7) {
            const float current=*(float*)((unsigned char*)self+off+4);
            memcpy((unsigned char*)self+off+8,&current,4);
            const float next=objectProgress*29.0f;
            memcpy((unsigned char*)self+off+4,&next,4);
        }
    }
}
static void test_construction_prepare() {
    unsigned char module[660]={},drawable[816]={},object[1300]={};
    void* d=drawable;memcpy(module+8,&d,4);
    void* o=object;memcpy(drawable+312,&o,4);
    put(module,12,0x12345678);put(module,64,0x87654321);
    unsigned stamp=42;
    auto oldStamp=g_constructionStamp;g_constructionStamp=&stamp;
    g_constructionObjectProgressFn=(void*)&mock_object_progress;
    g_constructionAnimateFn=(void*)&mock_animate;
    g_constructionOn=true;
    g_constructionPrepared=g_constructionPrepareCalls=0;
    g_constructionMatched=g_constructionSameStamp=g_constructionPrimeRefused=0;
    g_constructionModelTraceCount=g_constructionModelTraceLogged=0;
    memset(g_constructionModelRecent,0,sizeof g_constructionModelRecent);
    memset(g_constructionLastDraw,0,sizeof g_constructionLastDraw);
    put(object,160,kFlStructureUnpacking);put(module,400,kFlStructureUnpacking);
    put(object,1260,0xAA55AA55);put(object,1264,0x55AA55AA);
    put(module,200,stamp);
    unsigned char beforeDrawable[816],beforeObject[1300];
    memcpy(beforeDrawable,drawable,sizeof drawable);memcpy(beforeObject,object,sizeof object);
    auto resetCalls=[]() {animateCalls=objectProgressCalls=0;queriedObject=NULL;};
    auto initialTrack=[&](unsigned i) {
        put(module,284+i*28,0x11111111);put(module,300+i*28,7);
        const float current=0.0f,previous=-0.00001f;
        memcpy(module+288+i*28,&current,4);memcpy(module+292+i*28,&previous,4);
    };
    put(object,160,0);put(module,400,0);
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0,
          "default model is only observed, never switched or hidden");
    put(object,160,kFlStructureUnpacking);put(module,400,kFlStructureUnpacking);
    initialTrack(0);resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==1 && objectProgressCalls==1 && queriedObject==object &&
          g_constructionPrepared==1 && *(float*)(module+288)>0.0f,
          "selected construction model with same stamp consumes native initial-pose sentinel before draw");
    check(!memcmp(object,beforeObject,sizeof object) && !memcmp(drawable,beforeDrawable,sizeof drawable),
          "priming leaves Object, world queue links and Drawable byte-identical without a model setter");
    check(g_constructionModelTraceCount==1 && g_constructionModelTrace[0].initial==1 &&
          g_constructionModelTrace[0].primed==1 && g_constructionModelTrace[0].current[0]==0.0f &&
          g_constructionModelTrace[0].after[0]>0.0f,
          "bounded diagnostics publish initial frames and post-update frames");
    check(g_constructionModelTrace[0].priorFound==1 &&
          g_constructionModelTrace[0].prior.displayedFlags==0 &&
          g_constructionModelTrace[0].prior.objectFlags==0,
          "diagnostics link a preceding default-model draw to the same owner and module");
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0 && g_constructionPrepared==1,
          "consumed initial sentinel retains native subsequent same-stamp skip");
    ++stamp;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==1 && objectProgressCalls==0,
          "different stamp still invokes native animation once without extra progress query");
    for(unsigned track : {1u,2u}) {
        initialTrack(track);resetCalls();fl_wrap_construction_prepare(module,NULL);
        check(animateCalls==1 && objectProgressCalls==1,
              "initial construction sentinel in a secondary track is also consumed once");
    }
    module[592]=1;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==1 && module[592]==0,
          "native pending model restart is consumed on same-stamp construction render");
    initialTrack(0);objectActive=0;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==1,
          "inactive unpacking does not force native animation");
    objectActive=1;objectProgress=1.0f;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==1,
          "completed unpacking does not trigger an extra state transition");
    objectProgress=-0.5f;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0, "invalid visual progress is not eligible for priming");
    objectProgress=0.002222222f;
    module[168]=1;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0,
          "frozen native animation is respected");
    module[168]=0;
    put(module,400,0);resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0 && fl_visual_word(module,400)==0,
          "pending Object condition never causes premature model selection");
    put(module,400,kFlStructureUnpacking);
    for(unsigned off : {12u,64u}) {
        const unsigned saved=fl_visual_word(module,off);put(module,off,0);
        resetCalls();fl_wrap_construction_prepare(module,NULL);
        check(animateCalls==0 && objectProgressCalls==0,
              "missing geometry or state retains native same-stamp behavior");
        put(module,off,saved);
    }
    for(unsigned i=0;i<3;++i)put(module,300+i*28,1);
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0,
          "looping animations never trigger construction priming");
    initialTrack(0);g_constructionOn=false;
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0, "disabled wrapper preserves same-stamp skip");
    ++stamp;resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==1 && objectProgressCalls==0, "disabled wrapper preserves different-stamp native call");
    g_constructionOn=true;initialTrack(0);
    o=NULL;memcpy(drawable+312,&o,4);
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0,
          "preview without a world owner does not prime a construction pose");
    d=NULL;memcpy(module+8,&d,4);
    resetCalls();fl_wrap_construction_prepare(module,NULL);
    check(animateCalls==0 && objectProgressCalls==0, "null Drawable preserves native same-stamp skip");

    d=drawable;memcpy(module+8,&d,4);o=object;memcpy(drawable+312,&o,4);
    initialTrack(0);resetCalls();
    static const double input=1.234567890123;
    static volatile double output=0;
    unsigned beforeStack=0,afterStack=0;unsigned short beforeCW=0,afterCW=0;
    void* p=module;
    __asm {
        mov beforeStack,esp
        fnstcw beforeCW
        fld input
        mov ecx,p
        xor edx,edx
        call fl_wrap_construction_prepare
        fstp output
        fnstcw afterCW
        mov afterStack,esp
    }
    check(beforeStack==afterStack && beforeCW==afterCW && input==output && animateCalls==1,
          "compiled priming path preserves stack, x87 control word and live caller value");
    check(!memcmp(object,beforeObject,sizeof object) && !memcmp(drawable,beforeDrawable,sizeof drawable),
          "all exercised paths preserve world and Drawable owned fixture bytes");
    g_constructionOn=false;g_constructionStamp=oldStamp;
    g_constructionObjectProgressFn=g_constructionAnimateFn=NULL;
}

static unsigned char* wallMockGeometry = NULL;
static void* wallMockScene = NULL;
static void* wallMockModel = NULL;
static void* wallMockState = NULL;
static void* wallMockSelf = NULL;
static const void* wallMockFlags = NULL;
static unsigned wallModelCalls = 0, wallSceneCalls = 0, wallAlphaCalls = 0, wallMcCalls = 0;
static char wallMockForce = 0, wallMockExtra = 0;
static bool wallDeliverMc = true;
static bool wallEarlyAlphaHidden = false;
static unsigned char* wallMcNewGeometry = NULL;
static void* __fastcall mock_wall_scene(void* scene, void*, void** geometry) {
    ++wallSceneCalls;
    wallEarlyAlphaHidden = *(float*)((unsigned char*)*geometry + 96) == 0.0f;
    return scene;
}
static char __fastcall mock_wall_model(void* self, void*, void* model, char force, void* state) {
    ++wallModelCalls; wallMockSelf = self; wallMockModel = model;
    wallMockForce = force; wallMockState = state;
    put((unsigned char*)self, 12, (unsigned)(uintptr_t)wallMockGeometry);
    put((unsigned char*)self, 60, (unsigned)(uintptr_t)model);
    fl_wrap_wall_scene(wallMockScene, NULL, (void**)((unsigned char*)self + 12));
    return 0x61;
}
static double __fastcall mock_wall_alpha(void*, void*) {
    ++wallAlphaCalls; return 0.375;
}
static char __fastcall mock_wall_mc(void* receiver, void*, const void* flags, char force, char extra) {
    ++wallMcCalls; wallMockSelf = receiver; wallMockFlags = flags;
    wallMockForce = force; wallMockExtra = extra;
    if (wallDeliverMc) {
        unsigned char* module = (unsigned char*)receiver - 24;
        if (wallMcNewGeometry) {
            put(module, 12, (unsigned)(uintptr_t)wallMcNewGeometry);
            fl_wrap_wall_scene(wallMockScene, NULL, (void**)(module + 12));
        }
        memcpy(module + 400, flags, 60);
        put(module, 64, (unsigned)(uintptr_t)wallMockState);
        const bool unpacking = (*(const unsigned*)flags & kFlStructureUnpacking) != 0;
        put(module, 284, unpacking ? (unsigned)(uintptr_t)wallMockState : 0);
        put(module, 300, unpacking ? kFlMatchUnpacking : 0);
        *(float*)(module + 288) = 0.0f;
    }
    return 0x63;
}
static uintptr_t wallResumeThis, wallResumeFlags;
static unsigned wallResumeForce, wallResumeExtra;
static uintptr_t wallAbiEspBefore, wallAbiEspAfter, wallAbiEbpBefore, wallAbiEbpAfter;
static unsigned wallAbiEax, wallAbiEbx, wallAbiEsi, wallAbiEdi;
static void* wallAbiReceiver;
static void* wallAbiFlags;
// Owned continuation of the verified five-byte native MC prologue. The game
// is never called. Argument offsets include 124 locals and two saved registers.
static __declspec(naked) void mock_wall_mc_resume() {
    __asm {
        mov wallResumeThis, ecx
        mov eax, [esp + 88h]
        mov wallResumeFlags, eax
        mov eax, [esp + 8Ch]
        mov wallResumeForce, eax
        mov eax, [esp + 90h]
        mov wallResumeExtra, eax
        mov eax, 65h
        pop ebp
        pop ebx
        add esp, 7Ch
        ret 0Ch
    }
}
// All observations use static addresses. In particular, changing EBX here
// cannot corrupt MSVC's dynamically aligned frame pointer in the C++ fixture.
static __declspec(naked) void run_wall_mc_trampoline_abi() {
    __asm {
        pushad
        mov wallAbiEbpBefore, ebp
        mov wallAbiEspBefore, esp
        mov ebx, 12345678h
        mov esi, 23456789h
        mov edi, 3456789Ah
        push 91h
        push 27h
        push dword ptr [wallAbiFlags]
        mov ecx, wallAbiReceiver
        call fl_wall_mc_trampoline
        mov wallAbiEax, eax
        mov wallAbiEbx, ebx
        mov wallAbiEsi, esi
        mov wallAbiEdi, edi
        mov wallAbiEspAfter, esp
        mov wallAbiEbpAfter, ebp
        popad
        ret
    }
}
static void test_wall_first_display() {
    unsigned char module[660], drawable[850], tpl[16], object[260], geometry[260];
    unsigned char logic[100] = {}, engine[100] = {};
    unsigned char* lp = logic; unsigned char* ep = engine;
    unsigned stamp = 1000, model = 7, state = 9, scene = 11;
    unsigned flags[15] = { kFlStructureUnpacking };
    auto oldL = g_constructionLogicSlot; auto oldE = g_constructionEngineSlot;
    auto oldStamp = g_constructionStamp;
    g_constructionLogicSlot = &lp; g_constructionEngineSlot = &ep; g_constructionStamp = &stamp;
    g_wallModelFn = (void*)&mock_wall_model; g_wallSceneFn = (void*)&mock_wall_scene;
    g_wallAlphaFn = (void*)&mock_wall_alpha; g_wallMcFn = (void*)&mock_wall_mc;
    wallMockGeometry = geometry; wallMockScene = &scene; wallMockState = &state;
    auto reset = [&](unsigned instance, unsigned drawFlags = 0) {
        memset(module, 0, sizeof module); memset(drawable, 0, sizeof drawable);
        memset(tpl, 0, sizeof tpl); memset(object, 0xA5, sizeof object);
        memset(geometry, 0, sizeof geometry); memset(g_wallFirstDisplay, 0, sizeof g_wallFirstDisplay);
        g_wallBorn = g_wallSceneHidden = g_wallRenderHeld = g_wallReleased = 0;
        g_wallOverflow = g_wallPreview = g_wallStale = 0; g_wallTraceCount = g_wallTraceLogged = 0;
        g_wallHolding = 0;
        put(module, 8, (unsigned)(uintptr_t)drawable);
        put(drawable, 4, (unsigned)(uintptr_t)tpl); put(drawable, 608, 42);
        put(drawable, 340, drawFlags);
        put(tpl, 4, 0x942FFF2Du); put(tpl, 8, instance);
        *(float*)(geometry + 96) = 0.875f;
        g_targetFps = 90; g_constructionOn = true; wallDeliverMc = true;
        wallMcNewGeometry = NULL;
        wallModelCalls = wallSceneCalls = wallAlphaCalls = wallMcCalls = 0;
        wallEarlyAlphaHidden = false;
    };
    for (unsigned id : {0x296799CFu,0x09435832u,0xF8C50039u,0xBF93CE00u,0xA82CF003u,0x0895CAE6u}) {
        for (int fps : {60,90}) {
            reset(id); g_targetFps = fps;
            // This is intentionally before owner binding and before any
            // unpacking flags exist: the preceding candidate missed that gap.
            const char result = fl_wrap_wall_default(module, NULL, &model, 7, &state);
            check(result == 0x61 && wallModelCalls == 1 && wallMockSelf == module &&
                  wallMockModel == &model && wallMockState == &state && wallMockForce == 7,
                  "wall default wrapper preserves native model arguments, one call and AL");
            check(g_wallBorn == 1 && g_wallSceneHidden == 1 && wallSceneCalls == 1 && wallEarlyAlphaHidden,
                  "wall default geometry is already transparent when its native scene-add runs");
            put(drawable, 312, (unsigned)(uintptr_t)object);
            unsigned char beforeObject[sizeof object], beforeDrawable[sizeof drawable];
            unsigned char beforeModule[sizeof module], beforeGeometry[sizeof geometry];
            memcpy(beforeObject, object, sizeof object); memcpy(beforeDrawable, drawable, sizeof drawable);
            memcpy(beforeModule, module, sizeof module); memcpy(beforeGeometry, geometry, sizeof geometry);
            for (unsigned phase = 1; phase <= 6; ++phase) {
                put(engine, 88, phase);
                check(fl_wrap_wall_alpha(module, NULL) == 0.0,
                      "extra renders hold the default wall without a timer or early MC delivery");
            }
            check(!memcmp(beforeObject, object, sizeof object) && !memcmp(beforeDrawable, drawable, sizeof drawable) &&
                  !memcmp(beforeModule, module, sizeof module) && !memcmp(beforeGeometry, geometry, sizeof geometry),
                  "render guard never mutates object, drawable, native model/animation or geometry pose");
            wallDeliverMc = false;
            const char early = fl_wrap_wall_mc(module + 24, NULL, flags, 5, 9);
            check(early == 0x63 && wallMcCalls == 1 && wallMockSelf == module + 24 &&
                  wallMockFlags == flags && wallMockForce == 5 && wallMockExtra == 9 &&
                  g_wallReleased == 0 && fl_wrap_wall_alpha(module, NULL) == 0.0,
                  "native early return preserves receiver ABI and cannot release an uninitialized wall");
            wallDeliverMc = true;
            const char ready = fl_wrap_wall_mc(module + 24, NULL, flags, 3, 4);
            check(ready == 0x63 && wallMcCalls == 2 && g_wallReleased == 1 &&
                  fl_wrap_wall_alpha(module, NULL) == 0.375 && wallAlphaCalls == 8,
                  "native model and animation initialization release the guard and restore native alpha result");
            check(g_wallTraceCount >= 4 && g_wallDisplayTrace[g_wallTraceCount - 1].event == 4 &&
                  g_wallDisplayTrace[g_wallTraceCount - 1].mode == kFlMatchUnpacking &&
                  g_wallDisplayTrace[g_wallTraceCount - 1].displayedFlags == kFlStructureUnpacking,
                  "bounded lifecycle trace identifies the wall template and the completed native construction state");
            check(!memcmp(beforeObject, object, sizeof object) && !memcmp(beforeDrawable, drawable, sizeof drawable),
                  "native MC callback is the only source of model changes; world fixtures stay byte-identical");
        }
    }
    reset(0xA82CF003u, 8);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallBorn == 0 && g_wallPreview == 1 && !wallEarlyAlphaHidden &&
          fl_wrap_wall_alpha(module, NULL) == 0.375, "native wall placement preview remains visible");
    reset(0xBD8CD4C6u);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallBorn == 0 && !wallEarlyAlphaHidden && fl_wrap_wall_alpha(module, NULL) == 0.375,
          "other templates including power-glow ID are ineligible for wall visibility changes");
    reset(0xA82CF003u); put(tpl, 4, 0);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallBorn == 0 && !wallEarlyAlphaHidden, "wall instance ID alone cannot match a different asset type");
    for (int fps : {30,120}) {
        reset(0xA82CF003u); g_targetFps = fps;
        fl_wrap_wall_default(module, NULL, &model, 0, &state);
        check(g_wallBorn == 0 && !wallEarlyAlphaHidden && fl_wrap_wall_alpha(module, NULL) == 0.375,
              "wall guard is limited to the supported 60 and 90 render rates");
    }
    reset(0xA82CF003u); g_constructionOn = false;
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallBorn == 0 && wallSceneCalls == 1 && fl_wrap_wall_alpha(module, NULL) == 0.375,
          "disabled wall wrappers forward native behavior");
    reset(0xA82CF003u); put(module, 60, 1);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallBorn == 0 && !wallEarlyAlphaHidden, "reinitializing an existing model does not acquire a first-display guard");
    reset(0xA82CF003u);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    unsigned idleFlags[15] = {};
    wallDeliverMc = false;
    fl_wrap_wall_mc(module + 24, NULL, idleFlags, 0, 0);
    check(g_wallReleased == 0 && fl_wrap_wall_alpha(module, NULL) == 0.0,
          "equal zero flags alone cannot release a wall without initialized native animation state");
    wallDeliverMc = true;
    fl_wrap_wall_mc(module + 24, NULL, idleFlags, 0, 0);
    check(g_wallReleased == 1 && g_wallHolding == 0 && fl_wrap_wall_alpha(module, NULL) == 0.375,
          "native idle-state initialization releases completed and map-initial wall models");
    reset(0xA82CF003u);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    unsigned char replacementGeometry[260] = {};
    *(float*)(replacementGeometry + 96) = 1.0f;
    wallMcNewGeometry = replacementGeometry;
    fl_wrap_wall_mc(module + 24, NULL, flags, 0, 0);
    check(wallSceneCalls == 2 && wallEarlyAlphaHidden && g_wallSceneHidden == 2 &&
          g_wallReleased == 1 && g_wallHolding == 0 && fl_wrap_wall_alpha(module, NULL) == 0.375,
          "native replacement geometry remains hidden until its entire model/animation callback returns");
    reset(0xA82CF003u);
    const unsigned start = fl_wall_hash((uintptr_t)module);
    for (unsigned i = 0; i < 16; ++i)
        g_wallFirstDisplay[(start + i) & 511u].module = (uintptr_t)module + i + 1;
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    check(g_wallOverflow == 1 && wallModelCalls == 1 && !wallEarlyAlphaHidden &&
          fl_wrap_wall_alpha(module, NULL) == 0.375, "a full cache probe fails open with a diagnostic and preserves native calls");
    reset(0xA82CF003u);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    put(drawable, 608, 43);
    check(fl_wrap_wall_alpha(module, NULL) == 0.375, "reused drawable pointer with a different native ID cannot match stale guard");
    put(drawable, 608, 42); put(module, 12, (unsigned)(uintptr_t)object);
    check(fl_wrap_wall_alpha(module, NULL) == 0.375 && g_wallStale == 1,
          "unobserved geometry replacement fails open without dereferencing a retained pointer");
    reset(0xA82CF003u);
    fl_wrap_wall_default(module, NULL, &model, 0, &state);
    // Check the actual compiler-generated floating-point ABI with a preexisting
    // x87 value below the returned ST(0), both while held and after native MC.
    double alphaResult = -1.0; float canaryResult = 0.0f, canary = 17.25f;
    void* moduleArg = module;
    __asm {
        fld canary
        mov ecx, moduleArg
        xor edx, edx
        call fl_wrap_wall_alpha
        fstp qword ptr alphaResult
        fstp dword ptr canaryResult
    }
    check(alphaResult == 0.0 && canaryResult == canary, "held wall alpha preserves the native x87 result ABI and lower stack value");
    fl_wrap_wall_mc(module + 24, NULL, flags, 0, 0);
    __asm {
        fld canary
        mov ecx, moduleArg
        xor edx, edx
        call fl_wrap_wall_alpha
        fstp qword ptr alphaResult
        fstp dword ptr canaryResult
    }
    check(alphaResult == 0.375 && canaryResult == canary, "released wall alpha preserves x87 stack and exact native multiplier");
    wallAbiFlags = flags; wallAbiReceiver = module + 24;
    g_wallMcResume = (void*)&mock_wall_mc_resume;
    run_wall_mc_trampoline_abi();
    check(wallAbiEspBefore == wallAbiEspAfter && wallAbiEbpBefore == wallAbiEbpAfter && wallAbiEax == 0x65 &&
          wallAbiEbx == 0x12345678 && wallAbiEsi == 0x23456789 && wallAbiEdi == 0x3456789A &&
          wallResumeThis == (uintptr_t)wallAbiReceiver && wallResumeFlags == (uintptr_t)wallAbiFlags &&
          wallResumeForce == 0x27 && wallResumeExtra == 0x91,
          "actual MC trampoline preserves arguments, AL, callee stack cleanup and nonvolatile registers");
    g_constructionOn = false; g_constructionLogicSlot = oldL; g_constructionEngineSlot = oldE;
    g_constructionStamp = oldStamp;
    g_wallModelFn = g_wallSceneFn = g_wallAlphaFn = g_wallMcFn = g_wallMcResume = NULL;
}

int main() {
    test_glow();test_vertices();test_glow_seam();test_construction();test_construction_prepare();test_wall_first_display();
    std::printf("visual timing: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
