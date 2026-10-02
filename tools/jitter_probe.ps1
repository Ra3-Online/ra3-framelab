# jitter_probe.ps1 -- measure in-engine motion jitter at a given target frame rate.
#
# 2026-09-18 / batch 11 (Claude).
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# WHY THIS EXISTS
#   The user reports: "vehicles bounce up and down badly, mainly at 30 fps; vanilla already had
#   a slight bounce". Their evidence was a 25 fps phone video, and 25 fps cannot resolve a 30 or
#   60 Hz bounce -- it aliases. So the measurement has to happen inside the engine.
#
#   The black box (src/framelab.cpp, namespace bb) already records one row per RENDER frame:
#       wall clock ms | logicFrame | renderFrame | clock(CE1388) | msPerFrame | r | groups
#       | phase | frac | engineFps
#   This script drives one "arm" (a target fps + a group mask), lets it reach steady state, asks
#   the black box to dump, and hands the dump to tools/re/ring_analyze.py, which turns
#   prog = logicFrame + frac into a jitter number (see that file for the reasoning).
#
# THE THREE ARMS THAT MATTER
#   measure          -> "measure only": every hook runs, nothing is written, so this is VANILLA
#                       baseline with instrumentation. This is the reference the user compares to.
#   enable 30        -> the patched 30 fps case the user is complaining about.
#   enable 60        -> the patched 60 fps case, which the user says is fine. If the jitter
#                       number is WORSE at 30 than at 60, the complaint is reproduced numerically.
#
# DISCIPLINE (do not relax these)
#   * This script only ever Stop-Process's the game it launched itself. It REFUSES to start if
#     any game instance is already running, because several sessions share this machine and the
#     user may be playing. Never kill a game you did not start.
#   * do NOT call `flctl disable` anywhere in here: restoring code hooks while threads may be
#     inside them is what froze the game (HANDOFF section 9). Killing the process is safe -- the
#     patch only ever lived in memory.
#   * AppActivate is used because a covered/occluded window can throttle its render loop, and
#     this measurement is about render timing. It costs one focus steal per arm.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\jitter_probe.ps1
#   powershell -ExecutionPolicy Bypass -File tools\jitter_probe.ps1 -Arms "measure,30,60"
#   powershell -ExecutionPolicy Bypass -File tools\jitter_probe.ps1 -Arms "30@0x28FFF,measure,30@0x281FF"
#
# Each arm is either:
#   "measure"        -- unpatched, instrumented (the vanilla reference)
#   "<fps>"          -- patch to that frame rate with -Groups
#   "<fps>@<mask>"   -- patch to that frame rate with THIS mask, overriding -Groups
#
# The per-arm mask matters more than it looks. On 2026-09-18 the 30 fps arm came out with 6x the
# jitter of the vanilla arm, but it had always run SECOND -- so "the patch causes it" and "the
# second arm runs on a busier machine" were indistinguishable. An experiment whose arms are
# always in the same order cannot tell those apart. Per-arm masks let one invocation run
# {patched, vanilla, patched-again, patched-with-one-group-off} and separate the three effects.
param(
    [string]$Arms = "measure,30,60",
    [int]$Groups = 0,             # 0 = the build's default set (FL_G_ALL); overridden by @mask
    [int]$StartupWait = 45,
    [int]$Settle = 60,            # seconds to let the replay reach steady state before dumping
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$OutDir = "",
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = "",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$OutDir = Get-FlabOutputPath $OutDir "build\logs" -Directory

# ---------------------------------------------------------------------------------------------
# Sandbox compatibility: restore PATHEXT. The sandbox narrows PATHEXT to ".CPL" only, and
# PowerShell decides "is this an executable?" from PATHEXT -- so every .exe becomes a "document"
# and cannot be run from a pipeline. Symptom: "Cannot run a document in the middle of a pipeline".
# ---------------------------------------------------------------------------------------------
if ($env:PATHEXT -notmatch '\.EXE') {
    $env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"
    Write-Host ("note: PATHEXT was missing .EXE; restored to " + $env:PATHEXT)
}

$lab      = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl    = Join-Path $build "flctl.exe"
$analyzer = Join-Path $lab "tools\re\ring_analyze.py"
$pythonRuntime = Get-FlabPython $Python
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
# A replay copy name of our own: other sessions use _flab_play / _flabre_play / _raco_play /
# _simre_play. Sharing one would let two sessions clobber each other mid-run.
$playPath = Join-Path $replayDir (New-FlabReplayName "jitter_probe")

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

if (-not (Test-Path $flctl))    { Write-Host "missing $flctl -- build first"; exit 2 }
if (-not (Test-Path $analyzer)) { Write-Host "missing $analyzer"; exit 2 }

# Refuse rather than wait: determinism.ps1 waits (it once waited 3h24m), but for a timing
# measurement an old game instance means the machine is busy and the numbers would be junk.
if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Write-Host "REFUSE: a game instance is already running (RA3 is single-instance)."
    Write-Host "        Not touching it -- it may belong to the user or another session."
    exit 3
}

$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
Write-Host ("replay: {0}  ({1:N0} bytes)" -f $src.Name, $src.Length)

function Run-Arm([string]$label, [string]$mode, [int]$fps, [int]$mask) {
    Write-Host ""
    Write-Host ("================ arm {0}  (mode={1} fps={2} groups={3}) ================" -f $label, $mode, $fps, $mask)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false

    $proc = $null
    $report = $null
    try {
        $proc = [System.Diagnostics.Process]::Start($psi)
        Write-Host ("launched pid {0}, waiting {1}s for boot" -f $proc.Id, $StartupWait)
        Start-Sleep -Seconds $StartupWait
        if ($proc.HasExited) { Write-Host "game exited during startup"; return $null }

        try {
            Add-Type -AssemblyName Microsoft.VisualBasic
            [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
        } catch { }

        if ($mask -ne 0) { [void](Invoke-Flctl "groups" "$mask" $proc.Id) }
        if ($mode -eq "measure") {
            [void](Invoke-Flctl "measure" "" $proc.Id)
        } else {
            $rc = Invoke-Flctl "enable" "$fps" $proc.Id
            Write-Host ("enable rc {0}" -f $rc)
            if ($rc -ne 0) { Write-Host "install failed -- skipping arm"; return $null }
        }

        # Wait for the sim to actually be running before we trust anything (the replay takes a
        # while to load; a ring filled during loading has no motion in it and would read as
        # "perfectly smooth" for the wrong reason).
        $frame = -1
        for ($i = 0; $i -lt 30; $i++) {
            $frame = Invoke-Flctl "logicframe" "" $proc.Id
            if ($frame -gt 0 -and $frame -lt 100000000) { break }
            Start-Sleep -Seconds 2
        }
        if ($frame -le 0) { Write-Host "sim never advanced -- skipping arm"; return $null }
        Write-Host ("sim running at logic frame {0}" -f $frame)

        # Turn the flight recorder on, then give it time to fill and settle. 512 rows at 30 fps
        # is ~17 s; at 60 fps ~8.5 s. Settle=60 s also lets the opening camera moves finish, so
        # we measure a steady scene rather than a cut.
        [void](Invoke-Flctl "blackbox" "1" $proc.Id)
        Write-Host ("recorder on; settling {0}s" -f $Settle)
        Start-Sleep -Seconds $Settle

        if ($proc.HasExited) { Write-Host "game exited before the dump -- no data"; return $null }

        $before = @(Get-ChildItem (Join-Path $build "logs") -Filter ("snapshot-" + $proc.Id + "-*.log") -ErrorAction SilentlyContinue)
        [void](Invoke-Flctl "blackboxdump" "" $proc.Id)
        Start-Sleep -Seconds 3
        $after = @(Get-ChildItem (Join-Path $build "logs") -Filter ("snapshot-" + $proc.Id + "-*.log") -ErrorAction SilentlyContinue)
        $new = $after | Where-Object { $before.FullName -notcontains $_.FullName } | Sort-Object LastWriteTime
        if (-not $new) { Write-Host "dump produced no new snapshot file"; return $null }
        $report = $new[-1].FullName
        Write-Host ("dump -> {0}" -f $report)

        $txt = Join-Path $OutDir ("_jitter_{0}.txt" -f $label)
        # Cross-run collisions: $labelSeq only de-duplicates WITHIN one run, so a second
        # invocation silently overwrote the previous run's file (observed: this batch's
        # `_jitter_30_g28FFF.txt` destroyed the previous batch's result). Never overwrite --
        # if the name is taken, append a timestamp.
        if (Test-Path $txt) {
            $stamp = Get-Date -Format "HHmmss"
            $txt = Join-Path $OutDir ("_jitter_{0}_{1}.txt" -f $label, $stamp)
        }
        Invoke-FlabPython $pythonRuntime $analyzer $report --out $txt 2>&1 | Out-String | Write-Host
        Write-Host ("analysis -> {0}" -f $txt)
        return [pscustomobject]@{ Arm = $label; Mode = $mode; Fps = $fps; Report = $report; Analysis = $txt }
    } finally {
        # Only ever kill the process we started ourselves.
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 4
    }
}

$results = @()
# A label can legitimately be used twice (that is how "is it the patch, or the order?" gets
# answered). Give repeats a suffix -- otherwise the second result SILENTLY OVERWRITES the first,
# and the one that gets overwritten is the control arm.
$labelSeq = @{}
foreach ($arm in ($Arms -split ',')) {
    $a = $arm.Trim()
    if ($a -eq "") { continue }
    $mask  = $Groups
    $label = $a
    if ($a -match '@') {
        $parts = $a -split '@'
        $a     = $parts[0].Trim()
        $mask  = [Convert]::ToInt32(($parts[1].Trim() -replace '^0[xX]', ''), 16)
        $label = ("{0}_g{1:X}" -f $a, $mask)
    }
    if ($labelSeq.ContainsKey($label)) { $labelSeq[$label] = $labelSeq[$label] + 1 } else { $labelSeq[$label] = 1 }
    if ($labelSeq[$label] -gt 1) { $label = ("{0}_run{1}" -f $label, $labelSeq[$label]) }

    if ($a -eq "measure") {
        $r = Run-Arm $label "measure" 0 $mask
    } else {
        $fps = [int]$a
        $r = Run-Arm $label "enable" $fps $mask
    }
    if ($r) { $results += $r }
}

Write-Host ""
Write-Host "================ RESULT ================"
$results | Format-Table -AutoSize
Write-Host "Compare the 'residual std (ms)' line in each _jitter_*.txt. Same replay, same scene, so"
Write-Host "the ratio between arms is the evidence -- an absolute number alone is not."
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
