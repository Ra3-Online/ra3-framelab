# track_probe.ps1 -- find a MOVING unit's coordinates, hand the address to the black box,
#                   and record it once per render frame. This is the only instrument in the
#                   project that can see POSITION.
#
# Why: the user reports vehicles jitter up/down while moving (steady when standing still),
# and says vanilla is slight while the patched build is clearly worse. So the problem is on
# the POSITION path. The black box's time-axis columns (frame timing) and animation-axis
# columns (animation scores) both cannot see position at all.
#
# Flow (two-stage, because the address changes every run):
#   1. `flctl memscan`  -> scan writable memory for three-adjacent-float triples that MOVED
#                          within 1 s (that is the definition of "a unit in motion")
#   2. pick the candidate whose |x| is largest (map-scale coordinates; rotation matrices
#      and unit vectors have |x| <= 1, so this filter drops them cleanly)
#   3. `flctl track 0x<addr>` -> black box now records those three floats every render frame
#   4. `flctl blackbox 1` -> settle -> `flctl blackboxdump`
#   5. `ring_analyze.py` -> section 8 compares the three components' high-pass residuals
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\track_probe.ps1
#   powershell -ExecutionPolicy Bypass -File tools\track_probe.ps1 -Fps 30 -Settle 45

param(
    [int]$Fps = 30,
    [int]$Groups = 0x2CFFF,
    [int]$StartupWait = 45,
    [int]$Settle = 45,
    [int]$Width = 1024,
    [int]$Height = 576,
    # * -Mode: "enable" (patched, default) or "measure" (hooks installed but NOTHING is
    #   written -- the vanilla reference). Needed for A/B: run the same unit in both modes
    #   and compare its position residual. Pair the two runs with -PickX, not by address.
    [string]$Mode = "enable",
    # * -PickX <value>: pick the candidate whose X is CLOSEST to this value, instead of the
    #   one with the largest |x|. This is what makes A/B possible: the heap addresses change
    #   every run, so you cannot pair two runs by address -- but the replay is deterministic
    #   (driven by logic frames), so with the same -Settle both runs reach about the same
    #   logic frame and therefore the same unit's X. Pair by UNIT, not by address.
    #   Workflow: run once with -Fps 30, note the picked X from the report, then run again
    #   with -PickX <that value> to track the SAME unit in the other configuration.
    [double]$PickX = [double]::NaN,
    # * -PickY <value>: ALSO require the SECOND component to be close to this.
    #   Needed because X alone cannot identify the same FIELD. Measured: run 1 picked a
    #   triple shaped (x, 280, yaw) -- 280 in slot 2 -- while run 2 picked (x, y, 280) --
    #   280 in slot 3. Those are two different position fields: one updates every render
    #   frame (frame-to-frame |d| median 0.85), the other is a 15 Hz staircase (median 0.00).
    #   Comparing them is meaningless. -PickX together with -PickY pins the shape too.
    [double]$PickY = [double]::NaN,
    # * -PickIndex <n>: skip the first n candidates and take the (n+1)-th that passes the
    #   value tests. Used to survey DIFFERENT candidates across runs, because the winner
    #   changes run to run and we need to find one whose recorded frame-to-frame median is large
    #   (that identifies a RENDER position rather than a 15 Hz staircase logic position).
    [int]$PickIndex = 0,
    # * -SkipTrack: skip memscan + track entirely and only run the black box, then dump.
    #   Purpose: the ANIMATION AXIS (section 7 of the report) needs no address at all -- it
    #   is fed by the anim-probe hook. Measured need: with -Mode measure the memscan found
    #   nothing and the script exited 6 BEFORE reaching the black box, so no animation-axis
    #   data was produced at all, and the baseline comparison could not be made.
    [switch]$SkipTrack,
    # * -StartLogicFrame <N>: wait until the sim reaches logic frame N, THEN turn the
    #   recorder on. This is the correct way to make A/B comparable.
    #   Measured why -PickX alone fails: the X used for pairing comes from a previous run's
    #   report (the value at DUMP time), while memscan samples about 42 s earlier
    #   (settle 25 + memscan 2 + blackbox settle 25). At ~14 units/s that is ~590 units of
    #   travel -- which is exactly the "distance" the pairing reported (617 / 190).
    #   The replay is logic-frame driven, so starting the recorder at the SAME logic frame
    #   in both runs makes the recorded window cover the SAME unit's SAME stretch of path.
    #   0 = do not wait (old behaviour).
    [int]$StartLogicFrame = 0,
    # * -RecordSeconds: how long to record AFTER the black box is switched on (default 20).
    #   This must be separate from -Settle. Measured why: with -Settle 0 (needed so the
    #   logic-frame wait starts early enough to actually land on 412) the recorder was
    #   switched on and dumped almost immediately -- the report had only 3 rows and
    #   ring_analyze.py refused it (it needs at least 8 rows).
    [int]$RecordSeconds = 20,
    # * -Replay <path-or-name>: use this replay instead of auto-picking the largest one.
    #   The user recorded a dedicated test replay ("90 FPS Test.RA3Replay", 47 KB) that
    #   contains three vehicles they specifically identified as jittering -- auto-picking
    #   "largest" would grab the old 1.9 MB match replay instead and never see them.
    #   Accepts a full path or just a file name (resolved against the replay directory).
    [string]$Replay = "",
    [string]$OutDir = "G:\Ra3 FrameLab\build\logs"
)

$env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$lab       = "G:\Ra3 FrameLab"
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$analyzer  = Join-Path $lab "tools\re\ring_analyze.py"
$python    = "C:\Users\Mithlan\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe   = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playPath  = Join-Path $replayDir "_flabtrk_play.RA3Replay"

if (-not (Test-Path $flctl)) { Write-Host "missing $flctl -- build first"; exit 2 }

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Write-Host "REFUSE: a game instance is already running (RA3 is single-instance)."
    Write-Host "        Not touching it -- it may belong to the user or another session."
    exit 3
}

if ($Replay -ne "") {
    # -Replay given: resolve it (full path, or a bare name inside the replay directory).
    if (Test-Path $Replay) { $src = Get-Item $Replay }
    else {
        $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
               Where-Object { $_.Name -eq $Replay -or $_.BaseName -eq $Replay } |
               Select-Object -First 1
    }
    if (-not $src) { Write-Host ("replay not found: {0}" -f $Replay); exit 2 }
} else {
    $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
           Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
           Sort-Object Length -Descending | Select-Object -First 1
}
if (-not $src) { Write-Host "no usable replay found in $replayDir"; exit 2 }
Copy-Item $src.FullName $playPath -Force
Write-Host ("replay: {0}  ({1:N0} bytes)" -f $src.Name, $src.Length)

function Invoke-FlctlRaw([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    return (& $flctl $cmd $arg $targetPid 2>&1 | Out-String)
}

# Read three adjacent floats starting at a hex address.
# `flctl read` returns a DWORD, so the float's bit pattern has to be reinterpreted.
# Returns $null on any read failure.
function Read-Triple([int]$targetPid, [string]$hexAddr) {
    $out = @()
    $base = [Convert]::ToInt64($hexAddr, 16)
    for ($k = 0; $k -lt 3; $k++) {
        $a = $base + ($k * 4)
        $o = Invoke-FlctlRaw "read" ("0x{0:X}" -f $a) $targetPid
        $m = [regex]::Match($o, 'RESULT=(-?\d+)')
        if (-not $m.Success) { return $null }
        $dw = [uint32](([int64]$m.Groups[1].Value) -band 0xFFFFFFFFL)
        $out += [BitConverter]::ToSingle([BitConverter]::GetBytes($dw), 0)
    }
    return $out
}

$proc = $null
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false

    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("launched pid {0}, waiting {1}s for boot" -f $proc.Id, $StartupWait)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Write-Host "game exited during startup"; exit 4 }

    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { }

    if ($Mode -eq "measure") {
        # "measure only": every hook is installed but NOTHING is written -- the vanilla
        # reference. g_measureOnly makes the interpolation return the retail value
        # bit-for-bit, so this arm's game behaviour is vanilla.
        Write-Host "mode: measure only (vanilla reference)"
        # * ONE call does everything: sets measureOnly, sets the groups mask, and installs.
        #   It must be one call because install() refuses to run twice (FL_ERR_ALREADY), so
        #   "measure" followed by "groups"+"enable" leaves the animation probe uninstalled
        #   (measured: three baseline runs all reported "0 valid frames").
        #   ** Pass the FULL default groups (0x2CFFF), not just 0x4000: the mask is assigned
        #     outright, so passing only FL_G_ANIMPROBE dropped every other hook -- including
        #     the black box's own per-frame recorder, and the dump came back with
        #     "0 rows". measureOnly makes all of them no-ops for game behaviour, so the
        #     arm stays vanilla while the rulers are still installed.
        [void](Invoke-FlctlRaw "measureex" ("0x{0:X}" -f $Groups) $proc.Id)
    } else {
        Write-Host ("mode: enable fps={0} groups=0x{1:X}" -f $Fps, $Groups)
        [void](Invoke-FlctlRaw "groups" ("{0}" -f $Groups) $proc.Id)
        [void](Invoke-FlctlRaw "enable" ("{0}" -f $Fps) $proc.Id)
    }

    $frame = -1
    for ($i = 0; $i -lt 30; $i++) {
        $o = Invoke-FlctlRaw "logicframe" "" $proc.Id
        $m = [regex]::Match($o, 'RESULT=(-?\d+)')
        if ($m.Success) { $frame = [int64]$m.Groups[1].Value }
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if ($frame -le 0) { Write-Host "sim never advanced -- giving up"; exit 5 }
    Write-Host ("sim running at logic frame {0}" -f $frame)
    Start-Sleep -Seconds $Settle

    # ---- wait for a FIXED logic frame BEFORE scanning ----------------------------------
    # * This MUST happen before memscan, not after track. Measured why: with the wait placed
    #   after track, run 1's memscan saw x ~ 1580 while run 2's saw x ~ 2178 for what should
    #   be the same unit -- because the two runs' memscan moments were still seconds apart
    #   (the fixed settle does not fix the moment). -PickX then picked the wrong unit
    #   (distance 583). Waiting for the same logic frame first makes memscan see the SAME
    #   positions in both runs, so -PickX actually means something.
    if ($StartLogicFrame -gt 0) {
        Write-Host ("waiting for logic frame {0} before scanning ..." -f $StartLogicFrame)
        $lf = -1
        for ($i = 0; $i -lt 9000; $i++) {
            $o = Invoke-FlctlRaw "logicframe" "" $proc.Id
            $m = [regex]::Match($o, 'RESULT=(-?\d+)')
            if ($m.Success) { $lf = [int64]$m.Groups[1].Value }
            if ($lf -ge $StartLogicFrame) { break }
            # * 20 ms, not 500 ms. Measured why: at 15 logic frames/s a 500 ms poll overshoots
            #   by 7~8 frames. Asking for 412 landed on 442 -- 2 s late -- and this replay's
            #   "units are moving" window is narrow enough that 442 already missed it (the
            #   scan found only a memory-write artifact: X jumped -0.0000 -> 1758.9).
            #   A fine poll lands within ~1 frame.
            Start-Sleep -Milliseconds 20
        }
        Write-Host ("reached logic frame {0} (asked for {1})" -f $lf, $StartLogicFrame)
    }

    # ---- stage 1: find a moving triple -------------------------------------------------
    # * Print the logic frame BEFORE scanning, as a machine-readable line. THIS is the value
    #   to pin with -StartLogicFrame for a paired run -- not the value read after a pick.
    #   Measured why: pinning to 571 (read after a successful pick, i.e. a few seconds after
    #   memscan) made both runs reach logic frame 577, where they found 100~116 candidates
    #   that moved in the first 1 s window but NONE still moving in the second -- the units
    #   had already stopped. The usable window is EARLIER, so the frame must be sampled at
    #   the same moment the scan happens.
    $lfPre = -1
    $lfPreOut = Invoke-FlctlRaw "logicframe" "" $proc.Id
    $lfPreM = [regex]::Match($lfPreOut, 'RESULT=(-?\d+)')
    if ($lfPreM.Success) { $lfPre = [int64]$lfPreM.Groups[1].Value }
    Write-Host ("SCAN_LOGIC_FRAME={0}   <-- logic frame at scan time" -f $lfPre)
    # Retry: a replay has long stretches where nothing is moving, and memscan legitimately
    # returns 0 (or returns only rotation-matrix / unit-vector triples) there. Observed:
    # one run found 4 candidates with x ~ 1205 (real map coordinates), the next found 17
    # candidates all with |x| < 1.1 (rotation matrices). So retry generously.
    #
    # * Three tests, all needed. Each was added because a real measurement slipped past the
    #   previous set -- the failures are recorded so the next session does not re-add them:
    #     (1) |x| > 100      -- map coordinates are hundreds to thousands; rotation matrices,
    #         unit vectors and normalized directions have |x| <= ~1.1.
    #         Rejected: run4's 17 candidates, all |x| < 1.1.
    #     (2) |d2.x| > 0.5   -- X must actually be MOVING.
    #         Rejected: x = 284.99 constant (net displacement -0.0016).
    #     (3) *at least TWO components* must be map-scale (|v| > 10).
    #         Rejected: run6/run7's x = 1865 moving nicely (net -75.4) but y/z spanned
    #         exactly -0.9986..0.9986 -- i.e. "a scalar plus a UNIT VECTOR" (0.7010 and
    #         -0.7131 have norm 0.9997), NOT a coordinate triple.
    #         NOTE: an earlier version of test (3) checked the INSTANTANEOUS y/z for
    #         |v| in [0.98,1.02]. That cannot work: a unit vector's instantaneous values
    #         are e.g. (0.35, 0.93) -- both well inside the range -- while its SPAN is +-1.
    #         Magnitude is the only usable discriminator here, so require two big ones.
    #         Cost: a unit sitting near y ~ 0 (map edge) with z ~ 0 would be missed.
    #         Acceptable -- we need one moving unit, not all of them.
    $minAbsX = 100.0
    $minDx = 0.5
    $mapScale = 10.0
    $usePickX = -not [double]::IsNaN($PickX)
    $usePickY = -not [double]::IsNaN($PickY)
    $skipLeft = $PickIndex
    $best = $null; $bestAbs = $minAbsX; $bestDist = [double]::MaxValue
    $bestX = 0.0; $bestY = 0.0; $bestZ = 0.0
    # * Keep ALL passing candidates, not just the winner. Needed because the winner is
    #   frequently a TEMPORARY buffer (measured: after tracking, all three components read
    #   back as 0 -- the address had been freed/cleared). So we must verify before adopting,
    #   and to do that we need the runners-up to fall back on.
    $cands = @()
    if ($SkipTrack) { Write-Host "SKIPTRACK: not scanning memory; black box only (animation axis)" }
    for ($try = 1; $try -le 8 -and -not $SkipTrack; $try++) {
        Write-Host ""
        Write-Host ("---- memscan try {0}/8 (looking for a unit in motion) ----" -f $try)
        $scan = Invoke-FlctlRaw "memscan" "" $proc.Id
        Write-Host $scan

        # Parse: "  04225394  pass2(1208.2626, 0.8387, 0.5446) -> pass3(...)  d2=(-2.7620,...)"
        # NOTE the DLL emits ASCII labels on purpose ("pass2"/"pass3", not Chinese):
        # this script must stay pure ASCII, so a regex cannot depend on Chinese text.
        # * FALLBACK line (2026-09-20): the DLL emits this when it found NO "moving" candidate
        #   and fell back to the best STATIC map-scale triple. Without handling it here the
        #   probe would exit 6 and lose the animation axis too.
        $fallbackAddr = $null
        foreach ($line in ($scan -split "`n")) {
            $mf = [regex]::Match($line, 'FALLBACK=static\s+(0x[0-9A-Fa-f]{8})\s+\(\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)\)')
            if ($mf.Success) {
                $fa = [Convert]::ToInt64($mf.Groups[1].Value.Substring(2), 16)
                if ($fa -ge 0x01000000) { $fallbackAddr = $mf.Groups[1].Value }
                continue
            }
            $m = [regex]::Match($line, '^\s+([0-9A-Fa-f]{8})\s+\S+\(\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)\)\s*->.*d2=\(\s*(-?[\d.]+),')
            if (-not $m.Success) { continue }
            # * Address filter: only accept addresses >= 0x01000000.
            #   Measured, and it is a clean separator: every FALSE POSITIVE we hit was in the
            #   low region (0x00D07C28, 0x00D07D48, 0x001Axxxx) -- those are reused config /
            #   static slots whose contents get rewritten (X jumped -0.0000 -> 1758.9), while
            #   every genuine unit coordinate sat in the high heap (0x047BC6C4, 0x044EB4D4,
            #   0x0496511C). Rejecting the low region removes the whole false-positive class
            #   before any value-based test runs.
            if ([Convert]::ToInt64($m.Groups[1].Value, 16) -lt 0x01000000) { continue }
            $x  = [double]$m.Groups[2].Value
            $y  = [double]$m.Groups[3].Value
            $z  = [double]$m.Groups[4].Value
            $dx = [double]$m.Groups[5].Value
            $ax = [Math]::Abs($x)
            if ($ax -le $minAbsX) { continue }                       # test (1)
            if ([Math]::Abs($dx) -le $minDx) { continue }            # test (2)
            $big = 0
            if ($ax -gt $mapScale) { $big++ }
            if ([Math]::Abs($y) -gt $mapScale) { $big++ }
            if ([Math]::Abs($z) -gt $mapScale) { $big++ }
            if ($big -lt 2) { continue }                             # test (3)
            if ($usePickX) {
                # A/B mode: pair by UNIT and by FIELD SHAPE, not by address.
                # -PickY pins which slot holds the constant height (280), so run 1 and run 2
                # are guaranteed to look at the same field rather than two different ones.
                if ($usePickY -and [Math]::Abs($y - $PickY) -gt 20.0) { continue }
                $d = [Math]::Abs($x - $PickX)
                if ($d -lt $bestDist) {
                    $bestDist = $d; $best = $m.Groups[1].Value
                    $bestX = $x; $bestY = $y; $bestZ = $z
                }
            }
            $cands += ,@($m.Groups[1].Value, $x, $y, $z)
        }
        # * Loop exit: we need at least ONE candidate, not "a winner".
        #   ** 2026-09-20 bug fix: this used to be `if ($best) { break }`, but $best is only
        #   assigned inside the -PickX branch. Once the "pick by index" logic moved OUT of the
        #   loop (so that -PickIndex could survey candidates), a run WITHOUT -PickX left $best
        #   permanently null => the loop ran all 8 retries and exited 6 even though 40 perfectly
        #   good candidates had been collected ("no candidate passes all three tests" was a lie:
        #   the tests DID pass, the exit condition was wrong).
        if ($cands.Count -gt 0) { break }
        Write-Host "no candidate passes all three tests right now -- retrying"
        Start-Sleep -Seconds 3
    }
    if ($cands.Count -eq 0 -and -not $SkipTrack -and -not $fallbackAddr) {
        Write-Host "no usable moving triple found after 8 tries -- nothing to track"; exit 6
    }

    # * -PickIndex: take the (n+1)-th passing candidate instead of "largest |x|".
    #   Purpose: SURVEY candidates across runs. The winner changes run to run, and we need
    #   one whose recorded frame-to-frame median is LARGE -- that identifies a RENDER position
    #   (updates every render frame) rather than a 15 Hz staircase logic position.
    #   Both kinds pass "moved in two windows", so the survey is the only way to tell them
    #   apart without changing the DLL.
    if (-not $usePickX) {
        if ($cands.Count -eq 0) {
            if ($fallbackAddr) {
                # * DLL fell back to a STATIC candidate. Use it so the black box still gets
                #   a position channel -- but say so loudly, because this is NOT "a unit in
                #   motion". The animation axis is unaffected either way.
                $cands += ,@($fallbackAddr, 0.0, 0.0, 0.0)
                Write-Host "FALLBACK: using static candidate $fallbackAddr (not a moving unit)"
            } elseif ($SkipTrack) { $best = $null }
            else { Write-Host "no candidates collected"; exit 6 }
        } else {
        $idx = [Math]::Min($PickIndex, $cands.Count - 1)
        $pick = $cands[$idx]
        $best = $pick[0]; $bestX = $pick[1]; $bestY = $pick[2]; $bestZ = $pick[3]
        $bestAbs = [Math]::Abs($bestX)
        Write-Host ("using candidate index {0} of {1}" -f $idx, $cands.Count)
        }
    }
    if (-not $SkipTrack) {
    if ($usePickX) {
        Write-Host ("picked 0x{0}  (closest to X = {1}, distance {2})" -f $best, $PickX, $bestDist)
    } else {
        Write-Host ("picked 0x{0}  (|x| = {1})" -f $best, $bestAbs)
    }
    # * Print the picked candidate's value AS SEEN AT SCAN TIME. Needed because the report's
    #   the "range" line comes from the recording window (which starts seconds later), so it
    #   tell you what -PickX should have been for a paired run. Measured: a successful run
    #   picked a candidate whose recorded X range was 1624..1767, i.e. the scan-time X was
    #   around 1624, not the 1457 that was fed in.
    Write-Host ("PICKED_VALUE=({0},{1},{2})" -f $bestX, $bestY, $bestZ)
    Write-Host ("CANDIDATES={0}" -f $cands.Count)

    # ---- stage 1b: VERIFY before adopting ----------------------------------------------
    # * Why this step exists (measured): memscan's winner is frequently a TEMPORARY buffer --
    #   after tracking it, all three components read back as 0 because the address had been
    #   freed or cleared. A whole A/B run was wasted on one such address.
    #   So: take the top candidates, point the tracker at each in turn, read the triple back
    #   twice a second apart, and adopt the first one that is BOTH non-zero AND still there.
    #   Reading a float via `flctl read` (a dword) needs the bit pattern reinterpreted.
    $verified = $null
    $verifyMax = [Math]::Min(3, $cands.Count)
    for ($ci = 0; $ci -lt $verifyMax; $ci++) {
        $candAddr = $cands[$ci][0]
        [void](Invoke-FlctlRaw "track" ("0x{0}" -f $candAddr) $proc.Id)
        Start-Sleep -Milliseconds 1500
        $v1 = Read-Triple $proc.Id $candAddr
        # * 6 s apart, not 1 s. Measured why: with a 1 s gap a candidate passed verification
        #   and was adopted, yet during the recording window it went 0 -> 1755 (X), i.e. the
        #   address was freed a few seconds later. memscan's winners are mostly PER-FRAME
        #   TEMPORARIES (path/interpolation scratch), whose lifetime is only seconds.
        #   The black box keeps the last 512 frames (~17 s), so an address must survive at
        #   least that long to produce a clean record. A 6 s gap rejects the short-lived ones.
        Start-Sleep -Milliseconds 6000
        $v2 = Read-Triple $proc.Id $candAddr
        if ($null -eq $v1 -or $null -eq $v2) {
            Write-Host ("  0x{0}: read failed" -f $candAddr); continue
        }
        $z1 = ([Math]::Abs($v1[0]) -lt 0.001) -and ([Math]::Abs($v1[1]) -lt 0.001) -and ([Math]::Abs($v1[2]) -lt 0.001)
        $z2 = ([Math]::Abs($v2[0]) -lt 0.001) -and ([Math]::Abs($v2[1]) -lt 0.001) -and ([Math]::Abs($v2[2]) -lt 0.001)
        Write-Host ("  0x{0}: ({1:F3},{2:F3},{3:F3}) -> ({4:F3},{5:F3},{6:F3}){7}" -f `
            $candAddr, $v1[0], $v1[1], $v1[2], $v2[0], $v2[1], $v2[2], `
            $(if ($z1 -or $z2) { "   REJECT (zeroed = temporary buffer)" } else { "   ACCEPT" }))
        if (-not $z1 -and -not $z2) { $verified = $candAddr; break }
    }
    if (-not $verified) {
        Write-Host "no candidate survived verification (all were temporary buffers)"
        exit 6
    }
    Write-Host ("VERIFIED=0x{0}  <-- adopting this one" -f $verified)
    $best = $verified
    }   # end if (-not $SkipTrack)

    # * Print the logic frame at the moment the candidate was picked, as a machine-readable
    #   line. Feed it back as -StartLogicFrame for the paired A/B run.
    #   Why this matters (measured): this test replay is short, and with -StartLogicFrame
    #   800 / 250 both runs found 90~186 candidates that moved in the first 1 s window but
    #   NONE that were still moving in the second -- i.e. the units had already stopped.
    #   Without -StartLogicFrame the probe retries over ~24 s and does hit a moving window.
    #   So: run once unfixed to discover WHEN a moving candidate exists, read this line,
    #   then run both arms with -StartLogicFrame pinned to it.
    $lfNow = -1
    $lfOut = Invoke-FlctlRaw "logicframe" "" $proc.Id
    $lfm = [regex]::Match($lfOut, 'RESULT=(-?\d+)')
    if ($lfm.Success) { $lfNow = [int64]$lfm.Groups[1].Value }
    Write-Host ("PICK_LOGIC_FRAME={0}   <-- use this as -StartLogicFrame for the paired run" -f $lfNow)

    # ---- stage 2: hand it to the black box ---------------------------------------------
    Write-Host ""
    Write-Host "---- track + record ----"
    $trk = ""
    if (-not $SkipTrack) {
        $trk = Invoke-FlctlRaw "track" ("0x{0}" -f $best) $proc.Id
        Write-Host $trk
    }

    # (The logic-frame wait has already happened, before memscan -- see above.)
    [void](Invoke-FlctlRaw "blackbox" "1" $proc.Id)
    Write-Host ("recorder on; recording {0}s (separate from -Settle {1})" -f $RecordSeconds, $Settle)
    Start-Sleep -Seconds $RecordSeconds

    if ($proc.HasExited) { Write-Host "game exited before the dump"; exit 7 }

    $before = @(Get-ChildItem (Join-Path $build "logs") -Filter ("snapshot-" + $proc.Id + "-*.log") -ErrorAction SilentlyContinue)
    [void](Invoke-FlctlRaw "blackboxdump" "" $proc.Id)
    Start-Sleep -Seconds 3
    $after = @(Get-ChildItem (Join-Path $build "logs") -Filter ("snapshot-" + $proc.Id + "-*.log") -ErrorAction SilentlyContinue)
    $new = $after | Where-Object { $before.FullName -notcontains $_.FullName } | Sort-Object LastWriteTime
    if (-not $new) { Write-Host "dump produced no new snapshot file"; exit 8 }
    $report = $new[-1].FullName
    Write-Host ("dump -> {0}" -f $report)

    $stamp = Get-Date -Format "HHmmss"
    $txt = Join-Path $OutDir ("_track_{0}_{1}.txt" -f $best, $stamp)
    & $python $analyzer $report --out $txt 2>&1 | Out-String | Write-Host
    Write-Host ("analysis -> {0}" -f $txt)
}
finally {
    # Only ever kill the process we started ourselves. Never call `flctl disable`.
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 3
    Remove-Item $playPath -Force -ErrorAction SilentlyContinue
}
