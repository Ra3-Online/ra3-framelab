// Execute the actual candidate's signed-elapsed assembly in an owned stack
// fixture. No game globals, engine calls, injection or GUI are used.
#include "../src/framelab.cpp"

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

int main() {
    for (int fps = 30; fps <= 90; fps += 30) {
        const int ratio = fps / 15;
        const unsigned start = 1500, duration = 450;
        check(fl_construction_clock(start, ratio) == static_cast<unsigned>(fps * 100), "clock agrees with 100 seconds at target rate");
        const unsigned elapsedSamples[] = {0u, 1u, 225u, 450u};
        for (unsigned elapsed : elapsedSamples) {
            const unsigned now = fl_construction_clock(start + elapsed, ratio);
            const unsigned birth = fl_construction_clock(start, ratio);
            const unsigned span = fl_construction_clock(duration, ratio);
            check((now - birth) * duration == elapsed * span, "creation/midpoint/completion use one unit at every FPS");
            const unsigned pausedNow = fl_construction_clock(start + elapsed + 100, ratio);
            check((pausedNow - birth) + (now - pausedNow) == now - birth, "pause stamp cancels current time without changing progress");
        }
    }
    check(fl_construction_clock(0xffffffffu, 6) == 0xfffffffau, "clock preserves unsigned counter wrap");
    check(fl_construction_clock(0, 6) == 0, "new session clock restarts at zero");
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
