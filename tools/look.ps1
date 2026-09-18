# look.ps1 -- launch a replay, apply a patch group mask, and HOLD the game open so a HUMAN
#             can eyeball the animations. It measures nothing and judges nothing.
#
# 2026-09-17 / animation-locating session (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why it exists: in this environment a process launched by a tool command dies when that
# command ends, so the game can only be handed to a human WHILE the command is still alive.
# Run this one in the background and the game stays up for -Hold seconds.
#
# What to look at (this is the only judge that matters for the animation fix):
#   1. Infantry walk speed: do the legs keep pace with the body? In the broken config the
#      legs cycle ~3x too fast (feet slide, the unit looks like it is skating).
#   2. Building construction: does the build-up animation finish exactly when the building
#      actually completes? In the broken config it snaps to finished while the sim is still
#      building, then the building sits there "done" but not yet built.
#   Both are ANIMATION-vs-SIM relationships, so they stay valid even when the machine is too
#   loaded to reach 90 fps: everything slows down together, the relationship does not change.
#
# Group masks (see src/framelab.cpp):
#   0xC1FF = 0x1FF (patch set) | 0x4000 (animation ruler) | 0x8000 (clock fix)
#            -> the FIXED config (what we want to ship). NOTE it also carries the ruler probe;
#               that is deliberate -- every historical measurement was taken with it and the
#               probe was proven harmless (CHANGES.md W.2). Do not "clean it up" without re-measuring.
#   0x41FF = the same patch set + ruler but WITHOUT the clock fix -> the BROKEN config (comparison)
#   0x4000 = ruler only                      -> stock (30 fps, r=2): the reference
#   0x1C1FF / 0x1C17F (2026-09-17 batch 6) = 0xC1FF plus FL_G_STEPPROBE (0x10000, the blend-ramp
#            ruler). Use 0x1C1FF vs 0x1C17F to A/B the P6b animation-time-step fix with `flctl
#            blendrate`. See RE-animation-doc section 21.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\look.ps1 -Groups 0xC1FF -Fps 60 -Hold 900
#   ALWAYS pass -Fps explicitly: the default is 90 and this dev box has a 60 Hz panel, where
#   90 would make the whole game run at 66% speed (see HANDOFF section 11).
param(
    [int]$Fps = 90,
    [int]$Groups = 0xC1FF,
    [int]$StartupWait = 45,
    [int]$Hold = 900,            # seconds to keep the game open for the human
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$Out = "G:\Ra3 FrameLab\build\logs\_look.txt"
)

$ErrorActionPreference = "Stop"
$lab      = Split-Path $PSScriptRoot -Parent
$build    = Join-Path $lab "build"
$flctl    = Join-Path $build "flctl.exe"
$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe  = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef   = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playPath  = Join-Path $replayDir "_flab_play.RA3Replay"

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

Log ("=== look  fps={0} groups=0x{1:X4} hold={2}s ===" -f $Fps, $Groups, $Hold)

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Log "REFUSE: a game instance is already running (RA3 is single-instance)"
    Flush; exit 2
}

$src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
       Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
       Sort-Object Length -Descending | Select-Object -First 1
if (-not $src) { Log "no usable replay found"; Flush; exit 2 }
Copy-Item $src.FullName $playPath -Force
Log ("replay: {0}" -f $src.Name)

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

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $gameExe
$psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)
Log ("launched pid {0} ({1}x{2} windowed)" -f $proc.Id, $Width, $Height)

$rc = 0
try {
    Log ("waiting {0}s for boot" -f $StartupWait)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Log "FAIL: game exited during startup"; throw "game exited" }

    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { }

    Log ("groups : " + (Invoke-Flctl "groups" ("{0}" -f $Groups) $proc.Id))
    Log ("enable : " + (Invoke-Flctl "enable" ("{0}" -f $Fps) $proc.Id))
    Log ("clockrate: " + (Invoke-Flctl "clockrate" "60" $proc.Id))
    Log ("fpswin : " + (Invoke-Flctl "fpswin" "5" $proc.Id))

    Log ""
    Log ("################ GAME IS UP -- pid {0} -- HOLDING FOR {1} SECONDS ################" -f $proc.Id, $Hold)
    Log "LOOK AT: infantry leg speed vs movement (skating?) and building construction vs completion."
    Log "In the broken config (0x41FF) legs cycle ~3x too fast and the build-up animation finishes early."
    Log ("################ closing at {0} ################" -f (Get-Date).AddSeconds($Hold).ToString("HH:mm:ss"))
    Flush
    Start-Sleep -Seconds $Hold
} catch {
    Log ("EXCEPTION: {0}" -f $_)
    $rc = 1
} finally {
    $proc.Refresh()
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Log "game killed (patches die with the process)" }
    else { Log "game had already exited" }
    Flush
}

exit $rc
