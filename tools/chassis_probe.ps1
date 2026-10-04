# chassis_probe.ps1 -- measure the vehicle BODY POSE path (suspension bounce / pitch / roll).
#
# Why: the chassis pose comes from a client-side chain that advances ONE STEP PER DISPLAY
# FRAME with no dt (sub_535DD0 -> sub_532CA0 -> sub_526BD0 ...). At 60/90 fps it therefore
# runs 2x/3x fast: the body bounces violently while the animation axis and the position
# axis both look perfectly clean. Historical chassis notes are archived separately; see docs/TECHNICAL.md.
#
# One run = one ARM. Run the arms one after another (RA3 is single-instance):
#   baseline : -Mode measure                                  expect chassisrate ~ 2000
#   unfixed  : -Mode enable -Fps N -Groups 0x0A8FFF           expect ~ N/15*1000 (4000 @60, 6000 @90)
#   hold     : -Mode enable -Fps N -Groups 0x1A8FFF           expect ~ 2000
#   lerp     : -Mode enable -Fps N -Groups 0x3A8FFF           expect ~ 2000 (pose changes every frame)
#
# The judge is `flctl chassisrate`: recurrence steps per logic frame x1000 for the busiest
# on-screen vehicle. It is an invariant (independent of the render frame rate), same family as
# clockrate / blendrate.  -2 = wrapper not installed (NOT the same as -1 = no data).
#
# Portable on purpose (this script is meant to be taken to the 90 fps machine):
#   -GameRoot   the folder that contains Data\ra3_1.12.game and the *.SkuDef
#   -Python     python.exe used for tools\re\ring_analyze.py (stdlib only); optional
# Safety: refuses to start when a game is already running; only ever stops the process IT
# started; never calls `disable` on a live game (that freezes it); deletes only its own
# replay copy.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\chassis_probe.ps1 -Mode measure -Label base
#   powershell -ExecutionPolicy Bypass -File tools\chassis_probe.ps1 -Fps 60 -Groups 0x1A8FFF -Label hold

param(
    [string]$Mode = "enable",
    [int]$Fps = 60,
    [int]$Groups = 0x0A8FFF,
    [string]$Label = "",
    [int]$StartupWait = 45,
    [int]$StartLogicFrame = 0,
    [int]$RateFrames = 150,
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$Replay = "",
    [string]$GameRoot = "",
    [string]$Python = "",
    [string]$OutDir = "",
    # * -Fullscreen: do not pass -win/-xres/-yres. Measured 2026-09-21: with a remote-desktop session
    #   active (virtual display adapter), a WINDOWED game is composited at ~31 fps no matter what the
    #   patch does (the control arm with the plain default mask was capped the same way), so target 60
    #   ran the whole game at half speed. The chassis ruler is a ratio and survives the cap, but
    #   render/logic fps do not.
    #   CAVEAT, also measured: under that same remote session, full screen counted 60.00 fps and
    #   15.00 Hz logic -- but the chassis wrapper was called ZERO times, i.e. the device was lost
    #   and the engine was spinning without drawing the scene. A frame COUNTER reaching 60 is not
    #   a picture reaching 60. Use this switch only at the physical console of the machine.
    [switch]$Fullscreen,
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$OutDir = Get-FlabOutputPath $OutDir "build\logs" -Directory

# Preserve caller extensions and restore process environment even on early failure.
$probeOriginalPathExt = [Environment]::GetEnvironmentVariable('PATHEXT', 'Process')
$probeOriginalPythonEncoding = [Environment]::GetEnvironmentVariable('PYTHONIOENCODING', 'Process')
try {
if ([string]::IsNullOrWhiteSpace($probeOriginalPathExt)) {
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
} elseif (($probeOriginalPathExt -split ';' | ForEach-Object { $_.Trim() }) -notcontains '.EXE') {
    $env:PATHEXT = $probeOriginalPathExt + ';.EXE'
}
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$lab      = Split-Path -Parent $PSScriptRoot
$build = Get-FlabBuildDirectory
$flctl    = Join-Path $build "flctl.exe"
$analyzer = Join-Path $lab "tools\re\ring_analyze.py"
$logDir = Join-Path $build "logs"
if ($OutDir -eq "") { $OutDir = Join-Path $build "logs" }
if ($Label -eq "") { $Label = ("{0}{1}_g{2:X}" -f $Mode, $Fps, $Groups) }

$pythonRuntime = $null
try { $pythonRuntime = Get-FlabPython $Python } catch { Write-Host $_.Exception.Message }

$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playPath = Join-Path $replayDir (New-FlabReplayName "chassis_probe")

if (-not (Test-Path $flctl))   { Write-Host "missing $flctl -- build first"; exit 2 }
if (-not (Test-Path $gameExe)) { Write-Host "missing $gameExe -- pass -GameRoot"; exit 2 }
if (-not $skudef)              { Write-Host "no *1.12.SkuDef in $GameRoot"; exit 2 }

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Write-Host "REFUSE: a game instance is already running (RA3 is single-instance)."
    Write-Host "        Not touching it -- it may belong to the user or another session."
    exit 3
}

$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
Write-Host ("replay: {0}  ({1:N0} bytes)" -f $src.Name, $src.Length)

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    return (& $flctl $cmd $arg $targetPid 2>&1 | Out-String)
}
function Get-Result([string]$text) {
    $m = [regex]::Match($text, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return $null
}

$script:exitCode = 1
$proc = $null
$report = Join-Path $OutDir ("_chassis_{0}_{1}.txt" -f $Label, (Get-Date -Format "HHmmss"))
$lines = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { Write-Host $s; $lines.Add($s) }

try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    if ($Fullscreen) {
        $psi.Arguments = ('-replayGame "{0}" -config "{1}"' -f $playPath, $skudef)
    } else {
        $psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
    }
    $psi.WorkingDirectory = $GameRoot
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    Say ("ARM={0} mode={1} fps={2} groups=0x{3:X} pid={4}" -f $Label, $Mode, $Fps, $Groups, $proc.Id)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Say "game exited during startup"; exit 4 }

    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { }

    if ($Mode -eq "measure") {
        # measure-only: hooks installed, nothing written -> the vanilla reference. The full
        # mask must be passed (it is assigned outright); 0x100 keeps the per-frame recorder.
        $g = ($Groups -bor 0x80000 -bor 0x100)
        $rc = Get-Result (Invoke-Flctl "measureex" ("0x{0:X}" -f $g) $proc.Id)
        Say ("INSTALL measureex 0x{0:X} rc={1}" -f $g, $rc)
    } else {
        $rg = Get-Result (Invoke-Flctl "groups" ("{0}" -f $Groups) $proc.Id)
        Say ("GROUPS set -> 0x{0:X}" -f $rg)
        if ($rg -ne $Groups) { Say "groups mask did not stick -- abort"; exit 5 }
        $rc = Get-Result (Invoke-Flctl "enable" ("{0}" -f $Fps) $proc.Id)
        Say ("INSTALL enable {0} rc={1}" -f $Fps, $rc)
    }
    if ($rc -ne 0) { Say "install failed -- see build\logs\framelab-*.log"; exit 5 }

    $frame = -1
    for ($i = 0; $i -lt 40; $i++) {
        $frame = Get-Result (Invoke-Flctl "logicframe" "" $proc.Id)
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if (-not ($frame -gt 0)) { Say "sim never advanced -- giving up"; exit 6 }
    Say ("sim running at logic frame {0}" -f $frame)

    if ($StartLogicFrame -gt 0) {
        for ($i = 0; $i -lt 9000; $i++) {
            $lf = Get-Result (Invoke-Flctl "logicframe" "" $proc.Id)
            if ($lf -ge $StartLogicFrame) { break }
            Start-Sleep -Milliseconds 20
        }
        Say ("START_LOGIC_FRAME={0}" -f $lf)
    }

    [void](Invoke-Flctl "blackbox" "1" $proc.Id)
    # chassisrate blocks inside the game for $RateFrames logic frames (~10 s at 150); the black
    # box ring holds the last 512 render frames, so dump right after it returns.
    $rate = Get-Result (Invoke-Flctl "chassisrate" ("{0}" -f $RateFrames) $proc.Id)
    [void](Invoke-Flctl "blackboxdump" "" $proc.Id)
    $slots = Invoke-Flctl "chassisslots" "" $proc.Id
    # adjacent on purpose: the two numbers that get divided must be measured back to back
    $fpswin   = Get-Result (Invoke-Flctl "fpswin" "4" $proc.Id)
    $logicfps = Get-Result (Invoke-Flctl "logicfps" "4" $proc.Id)
    $clock    = Get-Result (Invoke-Flctl "clockrate" "60" $proc.Id)
    $alive = -not $proc.HasExited

    Say ("RESULT_CHASSISRATE={0}   (2000 = vanilla / fixed; 4000 / 6000 = unfixed 60 / 90; -2 = not installed)" -f $rate)
    Say ("RESULT_RENDERFPS_X100={0}" -f $fpswin)
    Say ("RESULT_LOGICFPS_X100={0}   (must stay 1500)" -f $logicfps)
    Say ("RESULT_CLOCKRATE={0}   (must stay ~66667; 66000 is normal for the measure arm)" -f $clock)
    Say ("GAME_ALIVE_AT_END={0}" -f $alive)
    Say "---- chassisslots ----"
    foreach ($l in ($slots -split "`r?`n")) { if ($l.Trim() -ne "") { $lines.Add($l) } }
    $top = ($slots -split "`r?`n") | Where-Object { $_ -match 'drawable=' } | Select-Object -First 6
    foreach ($l in $top) { Write-Host $l }

    $snap = Get-ChildItem $logDir -Filter ("snapshot-{0}-*.log" -f $proc.Id) -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($snap -and $null -ne $pythonRuntime -and (Test-Path $analyzer)) {
        $env:PYTHONIOENCODING = "utf-8"
        $ana = (Invoke-FlabPython $pythonRuntime $analyzer $snap.FullName 2>&1 | Out-String)
        $lines.Add("---- ring_analyze ($($snap.Name)) ----")
        $keep = $false
        foreach ($l in ($ana -split "`r?`n")) {
            if ($l -match '^.. 9\.') { $keep = $true }
            if ($keep) { $lines.Add($l) }
        }
        Say ("SNAPSHOT={0}" -f $snap.FullName)
    } elseif (-not $snap) {
        Say "no snapshot produced (black box dump failed?)"
    } else {
        Say ("SNAPSHOT={0}   (no python found -- run tools\re\ring_analyze.py on it by hand)" -f $snap.FullName)
    }
    $crash = Get-ChildItem $logDir -Filter ("crash-{0}-*.log" -f $proc.Id) -ErrorAction SilentlyContinue
    Say ("CRASH_REPORTS={0}" -f @($crash).Count)
    $script:exitCode = 0
}
finally {
    [System.IO.File]::WriteAllLines($report, $lines, (New-Object System.Text.UTF8Encoding($true)))
    Write-Host ("REPORT={0}" -f $report)
    if ($proc -and -not $proc.HasExited) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch { }
        # wait for the handle to go away, otherwise the replay copy below is still locked
        # (measured: one run left _flabchs_play.RA3Replay behind)
        try { [void]$proc.WaitForExit(8000) } catch { }
    }
    if (Test-Path $playPath) { Remove-Item $playPath -Force -ErrorAction SilentlyContinue }
}
# flctl returns the export's value as ITS exit code, so $LASTEXITCODE is garbage here.
exit $script:exitCode
} finally {
    [Environment]::SetEnvironmentVariable('PATHEXT', $probeOriginalPathExt, 'Process')
    [Environment]::SetEnvironmentVariable('PYTHONIOENCODING', $probeOriginalPythonEncoding, 'Process')
}
