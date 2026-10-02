# flctl_probe.ps1 -- one game launch, a whole BATTERY of flctl calls, every result recorded.
#
# 2026-09-17 / animation-locating session (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why this exists:
#   In the 16:18 and 16:21 runs, `flctl clockrate` produced NO output at all -- not even
#   flctl's own "pid = ..." line -- while every other command worked. flctl's stdout is a
#   pipe, and a pipe is block-buffered, so a process that dies before exit loses everything
#   it printed. That makes "crashed" indistinguishable from "never ran".
#   This script records, for every call: exit code, wall time, whether the game was still
#   alive, and the raw stdout/stderr. It also runs clockrate EARLY, MID and LATE so an
#   order-dependent failure shows up as a pattern instead of a single mystery.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\flctl_probe.ps1 -Out ...\_probe.txt
#
# 2026-09-17 P6b session -- the blendrate A/B (the ONLY way to see whether P6b took effect):
#   powershell -ExecutionPolicy Bypass -File tools\flctl_probe.ps1 -Fps 60 -Groups 0x1C1FF -Out ...\_A.txt
#   powershell -ExecutionPolicy Bypass -File tools\flctl_probe.ps1 -Fps 60 -Groups 0x1C17F -Out ...\_B.txt
#   0x1C1FF = default set + clock fix + animation ruler + blend-step ruler + P6b   -> expect blendrate 66667
#   0x1C17F = the same with FL_G_VISUAL (0x80) OFF, i.e. P6 + P6b both off        -> expect blendrate 133333
#   The two differ by exactly 2x if the diagnosis is right, and by 1x if it is wrong.
#
# Every line of the report is ASCII. No judgement is made here -- it only records.
param(
    [int]$Fps = 90,
    [int]$Groups = 0x4000,       # 0x4000 = probe only (stock baseline: nothing else patched)
    [int]$StartupWait = 45,      # seconds to let the game boot before attaching (see HANDOFF)
    [int]$Small = 20,            # logic frames for the quick calls
    [int]$Big = 150,             # logic frames for the full-length call
    [string]$Out = "",
    [switch]$KeepOpen,
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$Out = Get-FlabOutputPath $Out "build\logs\_probe.txt"

$lab      = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl    = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playPath = Join-Path $replayDir (New-FlabReplayName "flctl_probe")

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

Log ("=== flctl_probe  fps={0} groups=0x{1:X4} small={2} big={3} ===" -f $Fps, $Groups, $Small, $Big)

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Log "REFUSE: a game instance is already running (RA3 is single-instance)"
    Flush; exit 2
}

# Explicit -Replay selects the test basis; no personal replay is chosen implicitly.
$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    Log "REFUSE: replay copy differs from source"; Flush; exit 2
}
Log ("replay: {0} ({1} bytes, sha1 ok)" -f $src.Name, $src.Length)

# ---- launch ----
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $gameExe
$psi.Arguments = ('-win -xres 1024 -yres 576 -replayGame "{0}" -config "{1}"' -f $playPath, $skudef)
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)
Log ("launched pid {0} (1024x576 windowed)" -f $proc.Id)

$rc = 0
try {
    Log ("waiting {0}s for boot" -f $StartupWait)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Log "FAIL: game exited during startup"; throw "game exited" }

    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { Log ("AppActivate failed: {0}" -f $_) }

    # ---- one flctl call, fully instrumented ----
    function Run-Flctl([string]$cmd, [string]$arg) {
        $si = New-Object System.Diagnostics.ProcessStartInfo
        $si.FileName = $flctl
        $si.Arguments = ('{0} {1} {2}' -f $cmd, $arg, $proc.Id)
        $si.UseShellExecute = $false
        $si.RedirectStandardOutput = $true
        $si.RedirectStandardError = $true
        $si.CreateNoWindow = $true
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $p = [System.Diagnostics.Process]::Start($si)
        $o = $p.StandardOutput.ReadToEnd()
        $e = $p.StandardError.ReadToEnd()
        $p.WaitForExit()
        $sw.Stop()
        # ExitCode can come back $null (or throw) depending on how the process died; keep the
        # three cases apart instead of collapsing them into one misleading number.
        $code = "?"
        try {
            $c = $p.ExitCode
            if ($null -eq $c) { $code = "<null>" } else { $code = [string]$c }
        } catch { $code = "<threw:" + $_.Exception.GetType().Name + ">" }
        $proc.Refresh()
        $alive = -not $proc.HasExited
        Log ("--- flctl {0} {1}" -f $cmd, $arg)
        Log ("    exit={0}  ms={1}  gameAlive={2}" -f $code, $sw.ElapsedMilliseconds, $alive)
        Log ("    out='{0}'" -f (($o -replace "\s+", " ").Trim()))
        if ($e.Trim().Length -gt 0) { Log ("    err='{0}'" -f (($e -replace "\s+", " ").Trim())) }
        if (-not $alive) { Log "    !! the game process is GONE after this call" }
        return $o
    }

    Log ""
    Log "### attach: groups then enable (groups MUST be set before enable) ###"
    [void](Run-Flctl "status" "0")
    [void](Run-Flctl "groups" ("{0}" -f $Groups))
    [void](Run-Flctl "enable" ("{0}" -f $Fps))

    Log ""
    Log "### batch-7: display-clock rate + the three derived constants ###"
    # This is the number the whole derived-constant fix hinges on: does the DISPLAY frame
    # counter (vtbl[116](dword_CDB750)) tick at the RENDER frame rate (== Fps) or at a fixed
    # 30? It is measured, not assumed. Read-only; no group flag needed.
    #   rate ~= Fps  -> the counter tracks the render rate, so 0xCDBC50/54/5C MUST be re-based
    #                   onto the target fps (FL_G_DERIVED2 = 0xE00). Leaving them at 30 makes
    #                   every "ms <-> frame-number" conversion 2x wrong at 60 fps, which is
    #                   exactly the building unpack animation (sub_6F6CB0) running 2x fast.
    #   rate ~= 30   -> the counter did NOT change; the derived constants must stay untouched.
    [void](Run-Flctl "displayclock" "800")

    Log ""
    Log "### battery ###"
    # clockrate EARLY -- before any long blocking call has ever run
    [void](Run-Flctl "clockrate" ("{0}" -f $Small))
    [void](Run-Flctl "logicframe" "0")
    [void](Run-Flctl "read" "0xCE1388")
    [void](Run-Flctl "clockrate" ("{0}" -f $Small))     # clockrate MID
    [void](Run-Flctl "animrate" ("{0}" -f $Small))      # a known-good 10s-style blocker at 20 frames
    [void](Run-Flctl "read" "0xCE176C")                 # ms-per-frame global: must stay 33 (we never write it)
    [void](Run-Flctl "fpswin" "6")                      # render fps x100 (per-frame counter must be installed)
    # 2026-09-17 P6b session: blendrate is the ONLY reading that sees the SECOND animation time
    # path (sub_903910 -> sub_8EA220, dt = frame-delta / client-fps). clockrate and animrate
    # measure the game-clock path instead, so they cannot show whether P6b took effect.
    # Needs FL_G_STEPPROBE (0x10000) in -Groups. 66667 = correct; 133333 = P6b not applied.
    [void](Run-Flctl "blendrate" ("{0}" -f $Big))
    [void](Run-Flctl "clockrate" ("{0}" -f $Big))       # clockrate LATE, full length
    [void](Run-Flctl "frames" "0")
    [void](Run-Flctl "enginefps" "0")
    [void](Run-Flctl "displayclock" "800")              # again, after the battery (stability check)

    Log ""
    Log "### after battery ###"
    $proc.Refresh()
    Log ("gameAlive={0}" -f (-not $proc.HasExited))
    if ($proc.HasExited) {
        try { Log ("game exit code = {0}" -f $proc.ExitCode) } catch { Log "game exit code = <unreadable>" }
    }
} catch {
    Log ("EXCEPTION: {0}" -f $_)
    $rc = 1
} finally {
    if (-not $KeepOpen) {
        $proc.Refresh()
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Log "game killed (patches die with the process)" }
        else { Log "game had already exited -- nothing to kill" }
        # 2026-09-17 batch-7: Stop-Process once reported success while the process was still
        # alive. A leftover game keeps Ra3FrameLab.dll mapped, and the NEXT build then dies with
        # LNK1104 ("cannot open file Ra3FrameLab.dll") -- a confusing failure a long way from its
        # cause. So verify, and fall back to taskkill. Only ever the pid WE launched.
        $proc.Refresh()
        if (-not $proc.HasExited) {
            Log "game still alive after Stop-Process -- falling back to taskkill"
            Start-Sleep -Milliseconds 500
            & taskkill.exe /PID $proc.Id /F 2>&1 | ForEach-Object { Log ("  taskkill: {0}" -f $_) }
        }
        $proc.Refresh()
        Log ("final: gameAlive={0}" -f (-not $proc.HasExited))
    } else {
        Log ("LEAVING GAME RUNNING pid={0}" -f $proc.Id)
    }
    Flush
}

exit $rc
