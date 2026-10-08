// Execute actual visual wrappers/assembly with owned params, objects and stack.
// No game globals, engine calls, injection, GPU drawing or GUI are used.
#include "../src/framelab.cpp"
#include <initializer_list>

using namespace fl;
static int passed = 0, failed = 0;
static void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}

// Input args above an owned 40-byte local area mimic the native hook's stack.
// Return results: elapsed, original store, flags, EBX, EDI, EBP, ESP delta.
__declspec(naked) static void __cdecl elapsed_fixture(int, int, unsigned*) {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        sub esp, 28h
        mov esi, [esp+3ch]
        mov eax, [esp+40h]
        mov [esp+24h], eax
        mov ebx, 12345678h
        mov edi, 23456789h
        mov ebp, 3456789ah
        lea eax, continued
        mov g_constructionResume, eax
        mov [esp+20h], esp
        jmp fl_construction_signed_elapsed
    continued:
        pushfd
        pop eax
        mov edx, [esp+44h]
        mov [edx], esi
        mov ecx, [esp+24h]
        mov [edx+4], ecx
        mov [edx+8], eax
        mov [edx+0ch], ebx
        mov [edx+10h], edi
        mov [edx+14h], ebp
        mov eax, esp
        sub eax, [esp+20h]
        mov [edx+18h], eax
        add esp, 28h
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
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
    // Offline shader/emitter model, not a GPU capture: emission interval 16,
    // shader kills age > lifetime. The 15/16 boundary is visible at 60/90 Hz.
    for (int fps : {60,90}) {
        const int rendersPerStep = fps / 30;
        int gaps15 = 0, gaps16 = 0;
        for (int frame = 0; frame < 16 * rendersPerStep; ++frame) {
            gaps15 += frame > 15 * rendersPerStep;
            gaps16 += frame > 16 * rendersPerStep;
        }
        check(gaps15 == rendersPerStep - 1 && gaps16 == 0,
              "shader/emitter model covers intermediate-frame lifetime seam at 60/90");
    }
    g_glowOn = false; g_psOn = false; g_glowOrigFn = NULL;
}

int main() {
    test_glow();
    const int samples[] = {-2147483647, -1, 0, 1, 2147483647};
    const int modes[] = {0, 1, 2};
    for (int mode : modes) for (int input : samples) {
        unsigned out[7] = {};
        const int expected = mode == 0 && input < 0 ? 0 : input;
        const long before = g_constructionClamps;
        elapsed_fixture(input, mode, out);
        check(static_cast<int>(out[0]) == expected && static_cast<int>(out[1]) == expected, "actual native hook clamps visual negatives only; stores correct elapsed");
        const unsigned flags = out[2];
        check(((flags >> 6) & 1) == static_cast<unsigned>(expected == 0) && ((flags >> 7) & 1) == static_cast<unsigned>(expected < 0) && !(flags & 0x801), "native TEST sign/zero/carry/overflow flags preserved");
        check(out[3] == 0x12345678 && out[4] == 0x23456789 && out[5] == 0x3456789a && out[6] == 0, "registers and stack survive actual hook");
        check(g_constructionClamps - before == (mode == 0 && input < 0 ? 1 : 0), "clamp diagnostic counts only actual visual corrections");
    }
    g_constructionResume = NULL;
    std::printf("visual timing: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
