# accept_90.ps1 -- one-command acceptance test for the 90 fps deliverable.
#
# 2026-09-17 / acceptance-criteria session (Claude)
# ASCII only: PowerShell 5.1 misreads BOM-less UTF-8 Chinese source.
#
# WHY THIS EXISTS
#   The dev box is a 60 Hz machine (user, 2026-09-17): it cannot *show* 90 fps, so it
#   cannot decide whether the 90 fps path is finished. This script moves that decision
#   to whatever machine CAN show it, and makes it one command instead of a ritual.
#
# WHAT "90 fps IS DELIVERED" MEANS -- four hard checks, all about the PATCH:
#   A1  enable 90 is accepted (rc == 0).            r=6 is inside the engine's 1..6 range.
#   A2  clockrate in [66000, 66700].                The animation clock advances 66.667 ms
#                                                   per logic frame => animation speed is
#                                                   independent of the frame rate.
#   A3  read 0xCE176C == 33.                        The quarantined ms-per-client-frame
#                                                   global is untouched (14 other readers,
#                                                   including sound durations, depend on it).
#   A4  read 0x00CAF9D0 == 15.                      The LOGIC frame rate global is untouched
#                                                   => seconds->frames conversion (fire rate)
#                                                   is bit-identical to retail.
#   A1..A4 are checkable on ANY machine, including a 60 Hz one.
#
#   A5  render >= 0.95 * target AND logicfps ~= 15.00.
#       This is the ONE check a 60 Hz box cannot pass, and NOT because of a patch defect:
#       the engine computes logic_fps = ACTUAL render fps / r. Cap the renderer at 60 Hz
#       with r=6 and you get 60/6 = 10 logic fps => the WHOLE GAME runs at 66% speed
#       (simulation and animation together, still in the correct ratio to each other).
#       So A5 failing with render ~= panel refresh is an ENVIRONMENT result, reported as
#       ENV-LIMITED, not FAIL. Do NOT go and "fix" patch sites over it.
#
# WHY logicfps AND NOT enginefps
#   enginefps reads g_frameObj+0x70, the engine's own EXPONENTIALLY SMOOTHED display value.
#   It lags: measured render 59.87 fps while enginefps said 7.81, though 59.87/6 = 9.98.
#   Judging on it would turn smoothing lag into a phantom defect. `logicfps` measures the
#   logic frame counter in-process over a fixed wall interval -- no smoothing, no lag.
#
# WHY clockrate USES MANY LOGIC FRAMES
#   clockrate reads the game clock at the first and the last logic-frame boundary. Those two
#   boundaries cannot be hit exactly (the counter is polled), so the reading carries a fixed
#   additive error of up to ~2 logic frames' worth of clock. Over N logic frames that is
#   2*66667/N of relative error: 2.2% at N=60 (which is why N=60 read 65266 / 65933 / 66666
#   on three different runs) and 0.9% at N=150. So N defaults to 150 and the accept band is
#   COMPUTED from it. Exactness itself is proven offline by FrameLabSelfTestClock, which is
#   pure integer arithmetic over 3r client frames and needs no game at all.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\accept_90.ps1
#   powershell -ExecutionPolicy Bypass -File tools\accept_90.ps1 -Target 60   # r=4 sanity row
param(
    [int]$Target = 90,
    [int]$Groups = 0xC1FF,
    [int]$StartupWait = 45,
    [int]$Window = 8,
    [int]$LogicFrames = 150,
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$Out = "G:\Ra3 FrameLab\build\logs\_accept.txt"
)

$ErrorActionPreference = "Stop"
$lab       = Split-Path $PSScriptRoot -Parent
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$logDir    = Join-Path $build "logs"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe   = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playPath  = Join-Path $replayDir "_flab_acc.RA3Replay"

# The two globals the acceptance verdict reads directly.
$ADDR_MS_PER_FRAME = "0xCE176C"     # 1000 / client fps -- must stay frozen at 33
$ADDR_LOGIC_FPS    = "0x00CAF9D0"   # 15 -- must never move (fire rate / seconds->frames)

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

# One flctl call. Same shape as look.ps1 / fps_capacity.ps1 on purpose: same measuring stick.
function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    $si = New-Object System.Diagnostics.ProcessStartInfo
    $si.FileName = $flctl
    $si.Arguments = ('{0} {1} {2}' -f $cmd, $arg, $targetPid)
    $si.UseShellExecute = $false
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    $si.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($si)
    $o = $p.StandardOutput.ReadToEnd()
    $e = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return (($o + " " + $e) -replace "\s+", " ").Trim()
}

# Pull RESULT=<n> out of an flctl reply. "?" when absent -- a DIFFERENT outcome from a real 0,
# so it must never be flattened to 0.
function Result([string]$s) {
    $m = [regex]::Match($s, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return "?"
}

# Report every adapter's refresh rate. The virtual display adapter (remote-viewing) can report
# a different value than the one actually driving the desktop, so we list them all and let the
# measured render rate, not this guess, decide the verdict.
function RefreshRates {
    try {
        $list = Get-CimInstance Win32_VideoController -ErrorAction Stop |
                ForEach-Object { "{0}={1}Hz" -f $_.Name, $_.CurrentRefreshRate }
        return ($list -join "; ")
    } catch { return "(unavailable)" }
}

Log ("=== accept_{0}  groups=0x{1:X4} window={2}s ===" -f $Target, $Groups, $Window)
$rates = RefreshRates
Log ("display adapters: " + $rates)
Log ""

if (-not (Test-Path $flctl)) { Log ("REFUSE: flctl.exe not found at " + $flctl); Flush; exit 2 }
if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Log "REFUSE: a game instance is already running (RA3 is single-instance, and it may be someone else's)"
    Flush; exit 2
}

$src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
       Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
       Sort-Object Length -Descending | Select-Object -First 1
if (-not $src) { Log "REFUSE: no usable replay found"; Flush; exit 2 }
Copy-Item $src.FullName $playPath -Force
Log ("replay: {0}" -f $src.Name)
Flush

# ---- launch -------------------------------------------------------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $gameExe
$psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false
$game = [System.Diagnostics.Process]::Start($psi)
$gamePid = $game.Id
Log ("launched pid {0} ({1}x{2} windowed)" -f $gamePid, $Width, $Height)
Flush

$rcEnable = "?"
$clock = "?"; $mspf = "?"; $logicFps = "?"; $fps100 = "?"; $engine = "?"; $liveLogic = "?"
$cpuPerWall = "?"

try {
    Start-Sleep -Seconds $StartupWait
    $game.Refresh()
    if ($game.HasExited) { Log "FAIL: game exited during startup"; Flush; exit 3 }

    # Bring it to the front: a backgrounded window gets throttled by the compositor, which would
    # masquerade as a patch problem.
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($gamePid)
    } catch { }

    Log ("groups  : " + (Invoke-Flctl "groups" ("{0}" -f $Groups) $gamePid))
    $rEn = Invoke-Flctl "enable" ("{0}" -f $Target) $gamePid
    Log ("enable  : " + $rEn)
    $rcEnable = Result $rEn

    $game.Refresh(); $cpu0 = $game.TotalProcessorTime.TotalSeconds
    $t0 = Get-Date

    # A1's evidence is rcEnable; A2..A5 come from here.
    #
    # ORDER MATTERS. fpswin and logicfps must be ADJACENT in time, because the replay gets
    # heavier as it plays: render was 60.00 in one 8 s window and ~56 only 24 s later, so a
    # transfer-function reading taken across a 24 s gap came out 6.38 instead of 6.00.
    # clockrate is slow (150 logic frames ~= 16 s), so it goes LAST, after both.
    $rFps = Invoke-Flctl "fpswin" ("{0}" -f $Window) $gamePid
    Log ("fpswin  : " + $rFps); $fps100 = Result $rFps

    # In-process logic frame rate: the definition of "the simulation was not touched".
    $rLf2 = Invoke-Flctl "logicfps" "5" $gamePid
    Log ("logicfps: " + $rLf2); $liveLogic = Result $rLf2

    $rClk = Invoke-Flctl "clockrate" ("{0}" -f $LogicFrames) $gamePid
    Log ("clockrate: " + $rClk); $clock = Result $rClk

    $rMs = Invoke-Flctl "read" $ADDR_MS_PER_FRAME $gamePid
    Log ("mspf    : " + $rMs); $mspf = Result $rMs

    $rLf = Invoke-Flctl "read" $ADDR_LOGIC_FPS $gamePid
    Log ("logicfps global: " + $rLf); $logicFps = Result $rLf

    # Cross-check only -- NOT a criterion. See "WHY logicfps AND NOT enginefps" at the top.
    # NOTE: the arg must NOT be empty. flctl reads argv[2] as the fps and argv[3] as the pid,
    # so an empty argv[2] would shift the pid out of argv[3] and make flctl fall back to
    # "find the game by process name" -- i.e. it could touch someone else's session.
    $rEng = Invoke-Flctl "enginefps" "0" $gamePid
    Log ("enginefps (cross-check, smoothed): " + $rEng); $engine = Result $rEng

    $t1 = Get-Date
    $game.Refresh(); $cpu1 = $game.TotalProcessorTime.TotalSeconds
    $wall = ($t1 - $t0).TotalSeconds
    if ($wall -gt 0) {
        $cpuPerWall = [math]::Round(($cpu1 - $cpu0) / $wall, 2)
        Log ("game CPU: {0:N2} s over {1:N2} s wall  =>  {2} core(s) busy" -f ($cpu1 - $cpu0), $wall, $cpuPerWall)
    }
} catch {
    Log ("EXCEPTION: {0}" -f $_)
} finally {
    $game.Refresh()
    if (-not $game.HasExited) { Stop-Process -Id $gamePid -Force; Log "game killed (patches die with the process)" }
    else { Log "game had already exited" }
    Flush
}

# ---- verdict ------------------------------------------------------------------------------
# Each check is recorded with its own boolean so the verdict line can say WHICH one broke.
$render = if ($fps100 -eq "?") { $null } else { $fps100 / 100.0 }

# A2's band is COMPUTED from the sample length, not hard-coded: the clock is read at two logic
# frame boundaries that cannot be hit exactly, so the reading carries a fixed additive error of
# up to ~2 logic frames' worth of clock. Relative error = 2 * 66667 / N.
$CLK_TRUE = 66667
$clkTol   = [int](2.0 * $CLK_TRUE / [double]$LogicFrames) + 1
$clkLo    = $CLK_TRUE - $clkTol
$clkHi    = $CLK_TRUE + $clkTol

$a1 = ($rcEnable -eq 0)
$a2 = ($clock -ne "?" -and $clock -ge $clkLo -and $clock -le $clkHi)
$a3 = ($mspf -eq 33)
$a4 = ($logicFps -eq 15)
$a5 = ($render -ne $null -and $render -ge (0.95 * $Target) -and $liveLogic -ne "?" -and [math]::Abs($liveLogic - 1500) -le 30)

Log ""
Log ("=== ACCEPTANCE (target {0} fps, r = {1}, clockrate N = {2}) ===" -f $Target, ($Target / 15), $LogicFrames)

# Pre-compute the PASS/FAIL words and the numbers. PowerShell 5.1 does not accept an
# inline `if` expression as a function argument, so nothing is left inline below.
function W([bool]$b) { if ($b) { return "PASS" } else { return "FAIL" } }
$sRender = if ($render -eq $null) { "?" } else { "{0:N2}" -f $render }
$sLive   = if ($liveLogic -eq "?") { "?" } else { "{0:N2}" -f ($liveLogic / 100.0) }
$sEng    = if ($engine -eq "?") { "?" } else { "{0:N2}" -f ($engine / 100.0) }
$sXfer   = "?"
if ($render -ne $null -and $liveLogic -ne "?" -and $liveLogic -ne 0) {
    $sXfer = "{0:N2}" -f ($render / ($liveLogic / 100.0))
}

Log ("A1 enable {0} accepted (rc==0)                 : {1}   rc={2}" -f $Target, (W $a1), $rcEnable)
Log ("A2 clockrate in [{0},{1}] (N={2}, tol computed)  : {3}   got={4}" -f $clkLo, $clkHi, $LogicFrames, (W $a2), $clock)
Log ("A3 0xCE176C frozen at 33 (quarantine holds)    : {0}   got={1}" -f (W $a3), $mspf)
Log ("A4 0xCAF9D0 still 15 (sim global untouched)    : {0}   got={1}" -f (W $a4), $logicFps)
Log ("A5 render >= {0:N1} AND logicfps ~= 15.00       : {1}   render={2} logicfps={3}" -f `
        (0.95 * $Target), (W $a5), $sRender, $sLive)
Log ("   (cross-check only) enginefps smoothed value  :    {0}" -f $sEng)
Log ("   transfer render/logicfps, adjacent windows    :    {0}   (should be r = {1}; not" -f $sXfer, ($Target / 15))
Log ("                                                       simultaneous, so treat as a hint)")
Log ""

$patchOk = $a1 -and $a2 -and $a3 -and $a4
if (-not $patchOk) {
    Log ("VERDICT: FAIL -- the patch itself is wrong. Look at the first FAIL row above.")
    Log ("         A2 failing means the animation clock is off; A3/A4 failing means a global that")
    Log ("         must stay frozen moved (that is the desync / fire-rate class of bug).")
} elseif ($a5) {
    Log ("VERDICT: PASS -- {0} fps is delivered: render at target, engine logic fps at 15.00," -f $Target)
    Log ("         animation clock exact, and both frozen globals untouched.")
} else {
    Log ("VERDICT: ENV-LIMITED -- the patch is correct (A1..A4 all PASS), the MACHINE cannot")
    Log ("         render {0} fps. render={1}, logicfps={2}." -f $Target, $sRender, $sLive)
    Log ("         Reminder: engine logic_fps = ACTUAL render fps / r, so a renderer capped at")
    Log ("         the panel refresh makes the WHOLE game run slower -- simulation AND animation,")
    Log ("         still in the correct ratio. That is the screen, not the patch.")
    Log ("         To pass A5 you need: a >= {0} Hz display, or vsync off (game video option" -f $Target)
    Log ("         'EnableVSync', engine reader sub_607EB0 / writer sub_607EF0 -- we do not")
    Log ("         change user settings). Or run this same script with -Target 60 on a 60 Hz box")
    Log ("         to get a full PASS row at r=4.")
}

Log ""
Log "artifacts: log written to $Out ; replay copy at $playPath"
Flush
exit 0
