// Owned-memory fixtures for the actual resolver, readback, and transaction code.
// No engine functions, wrappers, getters, remote processes, or freezer are run.
#include "../src/framelab.cpp"

namespace fixture {
static int passed = 0, failed = 0;
// This test-only PE data section owns the fake page before process initialization.
// Only a fully bounded interior page is touched; no game code is executed.
#pragma section(".flfix", read, write)
__declspec(allocate(".flfix")) __declspec(align(4096)) unsigned char canonicalOwned[0x400000] = {};
static const DWORD testBaselineProtection = PAGE_READWRITE;
static unsigned char* text = NULL;
static const size_t textSize = 0x1000;
static unsigned char original[textSize];
static unsigned char* const sites[3] = {
    (unsigned char*)0x0062666B, (unsigned char*)0x006266F7, (unsigned char*)0x006266FE
};

static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++passed : ++failed;
}
static bool protect(DWORD value) {
    DWORD old = 0;
    return VirtualProtect(text, textSize, value, &old) != FALSE;
}
static DWORD protection(const void* p) {
    MEMORY_BASIC_INFORMATION m = {};
    return VirtualQuery(p, &m, sizeof m) ? m.Protect : 0;
}
static void put_pattern(unsigned char* p, int index) {
    const Pattern& pattern = fl::kSimGatePatterns[index];
    for (int j = 0; j < pattern.length; ++j)
        p[j] = pattern.bytes[j] <= 0xFF ? (unsigned char)pattern.bytes[j] : 0;
    unsigned char call[5];
    fl::sim_gate_call_bytes(p, fl::sim_gate_target(index, false), call);
    memcpy(p, call, sizeof call);
}
static void reset(int fps = 90, int groups = 0x003ACFFF) {
    if (fl::g_undoCount || fl::g_patchPoisoned) {
        std::printf("FAIL previous fixture left transaction state\n");
        ExitProcess(3);
    }
    if (!protect(PAGE_READWRITE)) ExitProcess(4);
    memset(text, 0xCC, textSize);
    for (int i = 0; i < 3; ++i) put_pattern(sites[i], i);
    memcpy(original, text, textSize);
    fl::g_text = text;
    fl::g_textSize = textSize;
    fl::g_retailFps = 30;
    fl::g_targetFps = fps;
    fl::g_ratio = fps / 15;
    fl::g_groups = groups;
    fl::g_measureOnly = false;
    fl::g_installed = false;
    fl::g_visPhaseOn = false;
    fl::g_simPinSites = fl::g_simGateSites = 0;
    fl::g_simGateOn = false;
    memset(&fl::g_simInstalled, 0, sizeof fl::g_simInstalled);
    mem::g_writeFault = 0;
    fl::g_selfTestNoWrite = false;
}
static fl::Sites resolve(int fps = 90) {
    fl::Sites s = {};
    check(fl::resolve_sim_gates(fps, 15, s) == FL_OK, "actual shared resolver valid canonical sites");
    return s;
}
static bool same_text() { return memcmp(text, original, textSize) == 0; }
static bool patch_call(const fl::Sites& s, int i) {
    unsigned char code[5];
    fl::sim_gate_call_bytes(s.simGate[i], fl::sim_gate_target(i, true), code);
    return fl::patch(s.simGate[i], code, sizeof code, "owned fake SIMGATE call");
}

static void eligibility() {
    const int masks[] = {0, 0x28FFF, 0x3ACFFF, 0x403ACFFF};
    const int fps[] = {60, 90};
    for (int i = 0; i < 2; ++i) for (int k = 0; k < 4; ++k) {
        reset(fps[i], masks[k]);
        fl::Sites s = resolve(fps[i]);
        check(s.simGateNeeded && s.simGateRequired, "canonical 60/90 require all gates despite legacy or zero mask");
        check(s.simGate[0] == sites[0] && s.simGate[1] == sites[1] && s.simGate[2] == sites[2], "all three exact fake sites resolved");
        check(same_text() && fl::g_undoCount == 0, "resolution is read only");
    }
    const int other[] = {30, 45, 75};
    for (int i = 0; i < 3; ++i) {
        reset(other[i], 0x3ACFFF);
        fl::Sites s = resolve(other[i]);
        check(!s.simGateRequired && !s.simGateNeeded && same_text(), "30/45/75 without explicit gate have no mandatory gate claim");
    }
    const int rejected[] = {45, 75, 120, 240};
    for (int i = 0; i < 4; ++i) {
        reset(rejected[i], FL_G_SIMGATE);
        fl::Sites s = {};
        check(fl::resolve_sim_gates(rejected[i], 15, s) == FL_ERR_RATIO, "unsupported explicit gate ratio rejected");
        check(same_text() && !s.simGateRequired, "rejected ratio writes no fake bytes");
    }
    reset(90, FL_G_SIMGATE);
    fl::Sites s = {};
    fl::g_retailFps = 60;
    check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_FPS_GLOBALS, "noncanonical retail rejected for explicit gate");
    reset(60, 0x3ACFFF); fl::g_retailFps = 60;
    s = {};
    check(fl::resolve_sim_gates(60, 15, s) == FL_ERR_FPS_GLOBALS && same_text(),
          "foreign retail60/logic15 target60 rejected despite old mask");
    reset(); fl::g_ratio = 4;
    s = {};
    check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_FPS_GLOBALS, "mismatched FPS/ratio rejected despite old mask");
    reset(90, FL_G_SIMGATE); fl::g_ratio = 4;
    s = {};
    check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_FPS_GLOBALS, "mismatched ratio explicit request rejected");
    reset(60, 0x3ACFFF);
    s = {};
    check(fl::resolve_sim_gates(60, 30, s) == FL_ERR_FPS_GLOBALS && same_text(),
          "foreign logic30 baseline rejected despite old mask");
}

static void rejection() {
    // Destroy+7 is outside the Xf window, so each missing fixture is independent.
    const int missingByte[3] = {5, 5, 7};
    for (int i = 0; i < 3; ++i) {
        reset(); sites[i][missingByte[i]] ^= 1;
        fl::Sites s = {};
        check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_SIG_MISS, "each absent full signature rejects installation");
        check(fl::g_undoCount == 0, "missing signature has no undo/write record");
        reset(); put_pattern(text + 0x900, i);
        s = {};
        check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_SIG_AMBIGUOUS, "each duplicate full signature rejected");
        reset(); sites[i][1] ^= 1;
        s = {};
        const int want = i == 2 ? FL_ERR_SIG_MISS : FL_ERR_SIG_MISMATCH;
        check(fl::resolve_sim_gates(90, 15, s) == want, "wrong original target rejected (Destroy also invalidates Xf neighborhood)");
        reset(); s = resolve(); sites[i][1] ^= 1;
        check(!fl::verify_sim_gates(s, false), "each changed original operand fails saved-window preflight");
        reset(); s = resolve(); sites[i][missingByte[i]] ^= 1;
        check(!fl::verify_sim_gates(s, false), "each changed neighborhood fails full-window preflight");
    }
    reset(); sites[2][7] ^= 1; put_pattern(text + 0x900, 2);
    fl::Sites s = {};
    check(fl::resolve_sim_gates(90, 15, s) == FL_ERR_SIG_MISMATCH, "unique separate valid Destroy target rejects nonadjacent Xf pair");
    reset(); s = resolve();
    check(protect(PAGE_NOACCESS), "owned test page made unreadable");
    check(!fl::verify_sim_gates(s, false), "unreadable full-window preflight refuses before byte reads");
    check(protect(PAGE_READWRITE), "owned test page access restored");
    check(same_text(), "unreadable fixture changed no bytes");
}

static void transaction() {
    reset(); fl::Sites s = resolve();
    check(protect(PAGE_EXECUTE_READ), "owned calls start on execute/read page");
    check(patch_call(s, 0) && patch_call(s, 1), "first two calls patched using actual transaction");
    check(!fl::verify_sim_gates(s, true), "two of three wrapper calls cannot pass installed full-window readback");
    check(patch_call(s, 2), "third call patched using actual transaction");
    check(fl::g_undoCount == 3 && fl::verify_sim_gates(s, true), "3/3 wrapper targets pass overlapping full-window postimage");
    check(!fl::verify_sim_gates(s, false), "patched image cannot pass original preimage");
    check(protection(text) == PAGE_EXECUTE_READ, "successful patch restores original page protection");
    check(protect(PAGE_READWRITE), "allow owned corruption fixture");
    sites[2][1] ^= 1;
    check(!fl::verify_sim_gates(s, true), "overlapped Destroy operand corruption fails installed readback");
    sites[2][1] ^= 1;
    sites[1][5] ^= 1;
    check(!fl::verify_sim_gates(s, true), "untouched Xf neighborhood corruption fails installed readback");
    sites[1][5] ^= 1;
    check(protect(PAGE_EXECUTE_READ), "restore owned protection before rollback");
    check(fl::rollback() == 0 && fl::g_undoCount == 0 && !fl::g_patchPoisoned, "all three actual patches rolled back and undo cleared");
    check(same_text() && protection(text) == PAGE_EXECUTE_READ, "whole fake text and page protection exactly restored");

    for (int faultCall = 1; faultCall < 3; ++faultCall) for (int fault = 1; fault <= 4; ++fault) {
        reset(); s = resolve();
        check(protect(PAGE_EXECUTE_READ), "fault fixture starts on original execute/read protection");
        bool priorOk = true;
        for (int i = 0; i < faultCall; ++i) priorOk = patch_call(s, i) && priorOk;
        check(priorOk, "prior call patches succeeded before injected fault");
        mem::g_writeFault = fault;
        check(!patch_call(s, faultCall), "second/third call write fault rejected by actual patch");
        check(fl::g_undoCount == faultCall + 1, "failed write retains original bytes and protection undo");
        check(fl::rollback() == 0 && fl::g_undoCount == 0 && !fl::g_patchPoisoned, "faulted three-call transaction rolls back without lost undo");
        check(same_text() && protection(text) == PAGE_EXECUTE_READ, "fault rollback restores all bytes and original page protection");
    }
    reset(); s = resolve(); protect(PAGE_EXECUTE_READ);
    check(patch_call(s, 0) && patch_call(s, 1) && patch_call(s, 2), "poison fixture has three genuine patches");
    fl::g_selfTestNoWrite = true;
    check(fl::rollback() != 0 && fl::g_undoCount == 3 && fl::g_patchPoisoned, "failed rollback retains entire undo and poison state");
    check(FrameLabEnable(60) == FL_ERR_WRITE && FrameLabDryRun(60) == FL_ERR_WRITE &&
          FrameLabMeasureOnly() == FL_ERR_WRITE && FrameLabMeasureOnlyEx(1) == FL_ERR_WRITE,
          "poison refuses all new install/dry-run modes before any resolver/freezer");
    check(FrameLabSimStatus() == -2, "poison reported as incomplete instead of installed success");
    fl::g_selfTestNoWrite = false;
    check(fl::rollback() == 0 && fl::g_undoCount == 0 && !fl::g_patchPoisoned && same_text(), "preserved undo permits later honest retry");
    check(protection(text) == PAGE_EXECUTE_READ, "honest retry restores original protection");
}

static void durable_guard() {
    reset();
    fl::g_installed = true;
    fl::g_visPhaseOn = true;
    fl::g_simPinSites = fl::g_simGateSites = 3;
    fl::g_simGateOn = true;
    fl::g_simInstalled.targetFps = 90;
    fl::g_simInstalled.ratio = 6;
    fl::g_simInstalled.retailFps = 30;
    fl::g_simInstalled.requestedGroups = 0x3ACFFF;
    fl::g_simInstalled.pinNeeded = fl::g_simInstalled.pinRequired = true;
    fl::g_simInstalled.gateNeeded = fl::g_simInstalled.gateRequired = true;
    const fl::SimInstallContract before = fl::g_simInstalled;
    check(FrameLabSimStatus() == 15, "durable required contract reports all installed bits");
    const int installedGroups = fl::g_groups;
    check(FrameLabSetGroups(0x403ACFFF) == installedGroups && fl::g_groups == installedGroups,
          "public SetGroups cannot mutate groups of installed runtime");
    fl::g_groups = 0; fl::g_targetFps = 30; fl::g_ratio = 2;
    check(FrameLabSimStatus() == 15, "mutable groups and target cannot erase installed requirement");
    const int oldGroups = fl::g_groups, oldTarget = fl::g_targetFps, oldRatio = fl::g_ratio;
    const bool oldMeasure = fl::g_measureOnly;
    check(FrameLabDryRun(60) == FL_ERR_ALREADY && FrameLabEnable(60) == FL_ERR_ALREADY &&
          FrameLabMeasureOnly() == FL_ERR_ALREADY && FrameLabMeasureOnlyEx(123) == FL_ERR_ALREADY,
          "installed entries refuse mode/ratio changes before resolver/freezer");
    check(fl::g_groups == oldGroups && fl::g_targetFps == oldTarget && fl::g_ratio == oldRatio &&
          fl::g_measureOnly == oldMeasure && memcmp(&before, &fl::g_simInstalled, sizeof before) == 0,
          "rejected installed requests preserve all mode and durable contract fields");
    fl::g_simGateSites = 2;
    check(FrameLabSimStatus() == -2, "durable gate requirement detects incomplete 2/3 despite cleared mask");
    fl::g_simGateSites = 3; fl::g_simGateOn = false;
    check(FrameLabSimStatus() == -2, "durable gate requirement detects disabled gate with three counters");
    fl::g_simGateOn = true; fl::g_simPinSites = 2;
    check(FrameLabSimStatus() == -2, "durable height requirement detects incomplete pin independently");
    fl::g_installed = false;
    check(FrameLabSimStatus() == 0, "uninstalled status is zero");
    reset();
    fl::Sites s; memset(&s, 0x7F, sizeof s);
    const int rc = fl::resolve_perframe_only(s);
    check(rc == FL_ERR_SIG_MISS || rc == FL_ERR_SIG_AMBIGUOUS, "measure parser refuses actual nongame executable naturally");
    check(!s.simGateNeeded && !s.simGateRequired && !s.simPinNeeded && !s.simPinRequired &&
          !s.simGate[0] && !s.simGate[1] && !s.simGate[2], "measure parser resets all gate/pin requirements before its resolution");
    check(same_text() && fl::g_undoCount == 0, "measure failure writes no fake code/data");
    reset();
    check(fl::remember(sites[0], 5), "owned pending undo recorded without executing any code");
    const int dirtyGroups = fl::g_groups;
    check(FrameLabSetGroups(123) == dirtyGroups && fl::g_groups == dirtyGroups,
          "public SetGroups cannot change groups with pending undo");
    check(FrameLabEnable(60) == FL_ERR_WRITE && FrameLabDryRun(60) == FL_ERR_WRITE &&
          FrameLabMeasureOnly() == FL_ERR_WRITE && FrameLabMeasureOnlyEx(123) == FL_ERR_WRITE,
          "nonpoisoned pending undo refuses all install/dry-run modes before resolver/freezer");
    check(!fl::g_measureOnly && fl::g_groups == dirtyGroups && fl::g_targetFps == 90 && fl::g_ratio == 6,
          "dirty guard rejection preserves existing mode and ratio");
    check(fl::rollback() == 0 && same_text(), "owned pending undo closes honestly");
}

struct Interleave {
    int kind;
    HANDLE started;
    int result;
};
static DWORD WINAPI guarded_selftest(void* opaque) {
    Interleave* c = (Interleave*)opaque;
    SetEvent(c->started);
    c->result = c->kind == 0 ? FrameLabSelfTestRollback() : FrameLabSelfTestClock();
    return 0;
}
static void api_interleave() {
    for (int kind = 0; kind < 2; ++kind) for (int installed = 0; installed < 2; ++installed) {
        reset();
        Interleave c = { kind, CreateEventW(NULL, TRUE, FALSE, NULL), -999 };
        if (!c.started) { check(false, "owned selftest handshake event available"); ExitProcess(6); }
        HANDLE thread = NULL;
        fl::Undo undoBefore[fl::kUndoMax];
        {
            api::PatchGuard transaction;
            thread = CreateThread(NULL, 0, &guarded_selftest, &c, 0, NULL);
            if (!thread) { check(false, "owned selftest thread available"); ExitProcess(6); }
            check(WaitForSingleObject(c.started, 5000) == WAIT_OBJECT_0, "second owned thread reaches selftest call while transaction lock held");
            check(WaitForSingleObject(thread, 100) == WAIT_TIMEOUT && c.result == -999,
                  "exported selftest cannot finish while original transaction lock held");
            fl::g_installed = installed != 0;
            check(fl::remember(sites[0], 5), "main records owned pending undo before releasing API lock");
            fl::g_clockAcc = 117; fl::g_clockWant = 228; fl::g_msPerFrame = 339;
            memcpy(undoBefore, fl::g_undo, sizeof undoBefore);
        }
        const DWORD waited = WaitForSingleObject(thread, 5000);
        check(waited == WAIT_OBJECT_0, "original owned selftest thread joined after lock release");
        if (waited != WAIT_OBJECT_0) ExitProcess(6);
        check(c.result == 0 && fl::g_undoCount == 1 && !fl::g_patchPoisoned &&
              memcmp(undoBefore, fl::g_undo, sizeof undoBefore) == 0 && same_text(),
              "selftest rechecks installed or dirty state after lock and preserves exact undo/text");
        check(fl::g_installed == (installed != 0) && fl::g_clockAcc == 117 &&
              fl::g_clockWant == 228 && fl::g_msPerFrame == 339 && fl::g_ratio == 6,
              "blocked rollback/clock selftest leaves installed state and clock fields untouched");
        check(CloseHandle(thread) != FALSE && CloseHandle(c.started) != FALSE,
              "all owned thread and event handles closed after original thread joined");
        fl::g_installed = false;
        check(fl::rollback() == 0 && same_text(), "interleave fixture pending owned undo cleaned honestly");
    }
}
} // namespace fixture

int main() {
    static_assert(sizeof(void*) == 4, "fixtures require x86");
    // The exact Xf window binds the adjacent Destroy call to the canonical address.
    // Use only the dedicated static array owned by this test image.
    const uintptr_t begin = (uintptr_t)fixture::canonicalOwned;
    const uintptr_t end = begin + sizeof fixture::canonicalOwned;
    const uintptr_t page = 0x00626000;
    MEMORY_BASIC_INFORMATION m = {};
    const uintptr_t mainBase = (uintptr_t)GetModuleHandleW(NULL);
    if (mainBase != 0x00400000 || begin > page || page > end || end - page < fixture::textSize ||
        !VirtualQuery((void*)page, &m, sizeof m) || m.Type != MEM_IMAGE || m.State != MEM_COMMIT ||
        (uintptr_t)m.AllocationBase != mainBase || (m.Protect != PAGE_READWRITE && m.Protect != PAGE_WRITECOPY)) {
        std::printf("FAIL owned IMAGE fixture unavailable; main=%p buffer=%p end=%p page=%p allocation=%p type=%08lX state=%08lX protect=%08lX\n",
                    (void*)mainBase, (void*)begin, (void*)end, (void*)page, m.AllocationBase, m.Type, m.State, m.Protect);
        return 2;
    }
    std::printf("OWNED_IMAGE main=%p buffer=%p end=%p page=%p allocation=%p type=%08lX state=%08lX protect=%08lX; fixture_bytes=%lu\n",
                (void*)mainBase, (void*)begin, (void*)end, (void*)page, m.AllocationBase, m.Type, m.State, m.Protect,
                (unsigned long)sizeof fixture::canonicalOwned);
    fixture::text = (unsigned char*)page;
    // Copy-on-write image pages cannot be promised their initial loader state back.
    // Establish an explicit owned test baseline and restore that baseline at exit.
    if (!fixture::protect(fixture::testBaselineProtection)) {
        std::printf("FAIL could not establish owned IMAGE fixture read/write baseline (%lu)\n", GetLastError());
        return 2;
    }
    fixture::eligibility();
    fixture::rejection();
    fixture::transaction();
    fixture::durable_guard();
    fixture::api_interleave();
    fixture::check(fixture::protect(fixture::testBaselineProtection) &&
                   fixture::protection(fixture::text) == fixture::testBaselineProtection,
                   "owned IMAGE data page test baseline protection restored; storage remains process lifetime");
    std::printf("RESULT %d PASS / %d FAIL; engine_calls=0 wrappers_executed=0 remote_calls=0 freezer_calls=0\n",
                fixture::passed, fixture::failed);
    return fixture::failed ? 1 : 0;
}
