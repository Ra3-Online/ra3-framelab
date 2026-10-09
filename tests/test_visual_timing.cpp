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
static float* __fastcall mock_vertices(void* storage, void*, const void* particle, float* out) {
    ++vertexCalls; vertexStorage = storage; vertexParticle = particle; vertexBuffer = out;
    if (out) memcpy(out, nativeVertices, sizeof nativeVertices);
    return out ? out + 40 : NULL;
}
static void test_vertices() {
    unsigned char tpl[260], system[12] = {}, storage[40] = {}, particle[88] = {};
    retail_template(tpl);
    void* t = tpl; memcpy(system+8, &t, 4);
    void* ref[2] = {system, NULL}; void* handle = ref;
    memcpy(storage+4, &handle, 4);
    put(storage,36,4); put(particle,40,16);
    g_glowVerticesOrigFn = (void*)&mock_vertices;
    g_glowOn = true; g_psOn = true;
    g_glowVertexCalls = g_glowVertexAdjusted = g_glowVertexSkipped = 0;
    for (int i=0;i<60;++i) nativeVertices[i] = i * 0.125f;
    for (int i=0;i<4;++i) nativeVertices[i*10+3] = 16.0f;
    auto invoke = [&](bool adjust) {
        float out[60]; memcpy(out,nativeVertices,sizeof out); vertexCalls=0;
        unsigned char before[88]; memcpy(before,particle,sizeof before);
        float* result=fl_wrap_gpu_vertices(storage,NULL,particle,out);
        check(vertexCalls==1 && vertexStorage==storage && vertexParticle==particle &&
              vertexBuffer==out && result==out+40, "actual upload preserves native call, args and end pointer");
        float expected[60]; memcpy(expected,nativeVertices,sizeof expected);
        if(adjust) for(int i=0;i<4;++i) expected[i*10+3]=kFlGlowVisibleLifetime;
        check(!memcmp(out,expected,sizeof out), "only four visible-life floats change; birth, velocity, position and tail survive");
        check(!memcmp(particle,before,sizeof before), "upload never changes native particle object");
    };
    for (int fps : {60,90}) { g_targetFps=fps; invoke(true); }
    check(g_glowVertexAdjusted==2, "upload counts actual paired correction");
    for (int fps : {30,120}) {g_targetFps=fps;invoke(false);}
    g_targetFps=90;g_glowOn=false;invoke(false);g_glowOn=true;
    g_psOn=false;invoke(false);g_psOn=true;
    for(unsigned count : {0u,1u,3u,5u,0xffffffffu}) {put(storage,36,count);invoke(false);}
    put(storage,36,4);
    for(unsigned life : {0u,15u,17u,0xffffffffu}) {put(particle,40,life);invoke(false);}
    put(particle,40,16);
    for(unsigned off : {72u,80u,100u,104u,108u,112u,156u,160u,164u,168u,172u,176u,256u}) {
        retail_template(tpl);put(tpl,off,fl_visual_word(tpl,off)^1u);invoke(false);
    }
    retail_template(tpl);
    nativeVertices[23]=15.0f;invoke(false);nativeVertices[23]=16.0f;
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

static int modelCalls = 0, animateCalls = 0, modelSequence = 0;
static void* modelSelf = NULL;
static char modelForce = -1, modelLoad = -1;
static std::uint32_t modelFlags[15];
static char __fastcall mock_model(void* self, void*, const void* flags, char force, char load) {
    ++modelCalls; modelSequence = (modelSequence << 8) | 1;
    modelSelf = self; modelForce = force; modelLoad = load;
    memcpy(modelFlags, flags, sizeof modelFlags);
    // Native sub_90CE60 stores its input at (this - 24) + 400.
    memcpy((unsigned char*)self + 376, flags, sizeof modelFlags);
    return 19;
}
static void __fastcall mock_animate(void* self, void*) {
    ++animateCalls; modelSequence = (modelSequence << 8) | 2;
    put((unsigned char*)self, 200, *g_constructionStamp);
}
static void test_construction_prepare() {
    unsigned char module[660] = {}, drawable[816] = {}, object[1300] = {};
    void* d = drawable; memcpy(module + 8, &d, 4);
    void* o = object; memcpy(drawable + 312, &o, 4);
    unsigned stamp = 42;
    auto oldStamp = g_constructionStamp; g_constructionStamp = &stamp;
    g_constructionModelFn = (void*)&mock_model;
    g_constructionAnimateFn = (void*)&mock_animate;
    g_constructionOn = true; g_constructionPrepared = g_constructionPrepareCalls = 0;
    // Pending conditions include construction and unrelated bits in every word.
    // Drawable overrides are distinct and must keep native (pending & ~remove)|add order.
    for (unsigned i = 0; i < 15; ++i) {
        put(object, 160 + i*4, 0x20001000u | (i << 16));
        put(drawable, 672 + i*4, 0x20000000u);
        put(drawable, 732 + i*4, 0x40000000u | i);
    }
    // The world's queue links are sentinels, never drained or rewritten.
    put(object, 1260, 0xAA55AA55); put(object, 1264, 0x55AA55AA);
    put(module, 200, stamp);
    unsigned char beforeDrawable[816], beforeObject[1300];
    memcpy(beforeDrawable, drawable, sizeof drawable); memcpy(beforeObject, object, sizeof object);
    auto resetCalls = [&]() { modelCalls = animateCalls = modelSequence = 0; };
    resetCalls(); fl_wrap_construction_prepare(module, NULL);
    check(modelCalls == 1 && animateCalls == 1 && modelSequence == 0x102 &&
          modelSelf == module + 24 && modelForce == 0 && modelLoad == 0,
          "first same-stamp render chooses construction model before applying its initial pose");
    bool flagsOK = true;
    for (unsigned i = 0; i < 15; ++i)
        flagsOK &= modelFlags[i] == ((fl_visual_word(object,160+i*4) &
                                     ~fl_visual_word(drawable,672+i*4)) | fl_visual_word(drawable,732+i*4));
    check(flagsOK && !memcmp(modelFlags,module+400,sizeof modelFlags),
          "all pending conditions including wall-segment bits and Drawable overrides reach only the draw module");
    check(!memcmp(object,beforeObject,sizeof object) && !memcmp(drawable,beforeDrawable,sizeof drawable),
          "actual preparation leaves world object, queue links and Drawable byte-identical");
    resetCalls(); fl_wrap_construction_prepare(module, NULL);
    check(modelCalls == 0 && animateCalls == 0 && g_constructionPrepared == 1,
          "same pending transition prepares once and preserves the native same-stamp skip afterwards");
    ++stamp; resetCalls(); fl_wrap_construction_prepare(module, NULL);
    check(modelCalls == 0 && animateCalls == 1,
          "later display stamp retains one native animation update without rebuilding the model");
    put(module,400,0); put(drawable,672, kFlStructureUnpacking);
    resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==0, "Drawable remove override prevents false construction transition");
    put(drawable,672,0x20000000u); put(object,160,0);
    resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==0, "non-construction object never triggers visual preparation");
    put(object,160,kFlStructureUnpacking); g_constructionOn=false;
    resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==0, "disabled wrapper retains native same-stamp behavior");
    ++stamp; resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==1, "disabled wrapper retains native different-stamp call count");
    g_constructionOn=true; o=NULL; memcpy(drawable+312,&o,4);
    ++stamp; resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==1, "preview Drawable without a world owner forwards native animation only");
    d=NULL; memcpy(module+8,&d,4);
    resetCalls(); fl_wrap_construction_prepare(module,NULL);
    check(modelCalls==0 && animateCalls==0, "null Drawable retains native same-stamp skip");

    // Native render can carry a live x87 value across its animation call. Execute
    // this compiled wrapper under that ABI and confirm stack/CW/value survive.
    d=drawable; memcpy(module+8,&d,4); o=object; memcpy(drawable+312,&o,4);
    static const double input=1.234567890123;
    static volatile double output=0;
    unsigned beforeStack=0, afterStack=0; unsigned short beforeCW=0, afterCW=0;
    void* p=module;
    __asm {
        mov beforeStack, esp
        fnstcw beforeCW
        fld input
        mov ecx, p
        xor edx, edx
        call fl_wrap_construction_prepare
        fstp output
        fnstcw afterCW
        mov afterStack, esp
    }
    if (beforeStack!=afterStack || beforeCW!=afterCW || input!=output)
        std::printf("render ABI: stack %08X/%08X CW %04X/%04X value %.17g/%.17g\n",
                    beforeStack,afterStack,beforeCW,afterCW,input,output);
    check(beforeStack==afterStack && beforeCW==afterCW && input==output,
          "compiled first-render wrapper preserves stack, x87 control word and live caller value");
    g_constructionOn=false; g_constructionStamp=oldStamp;
    g_constructionModelFn=g_constructionAnimateFn=NULL;
}
int main() {
    test_glow();test_vertices();test_glow_seam();test_construction();test_construction_prepare();
    std::printf("visual timing: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
