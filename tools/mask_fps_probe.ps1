# mask_fps_probe.ps1 -- for each group mask: does the frame rate ACTUALLY go up?
#
# 2026-09-18 / developer probe
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# WHY THIS EXISTS:
#   The group masks are NOT independent knobs. Batch 3 measured (HANDOFF 8):
#       g101 (r only)  -> render 30.1     g103 -> 30.2     g183 -> 30.2     g1FF -> 90.3
#   i.e. the ability to RAISE the frame rate lives inside the low 9 bits. So an arm like
#   0x2CE00 ("low 9 bits off") is very likely still rendering at 30 while the patch has already
#   computed all of its numbers for 60 (g_msPerFrame = 1000/60, the three derived constants,
#   the animation time step). That is not a 60 fps arm at all -- it is a SELF-CONTRADICTORY arm,
#   and its desync verdict cannot be used to conclude anything about 60 fps.
#
#   Before drawing any conclusion from a bisect table, this must be measured, not assumed.
#   This script answers exactly one question per mask: render fps and logic fps after enable.
#   It does NOT wait for a desync -- it needs about 2 minutes per mask.
#
# Discipline (same as the other tools here):
#   - waits for the game to be free; NEVER kills an instance it did not start
#   - one process per mask (changing groups under a live install is untested territory)
#   - never calls `disable` while running; cleanup is Stop-Process on our own pid only
param(
    [string]$Masks = "0x2CFFF,0x2CFFE,0x2CE00,0x2CFE0,0x2CFE3",
    [int]$Fps = 60,
    [int]$SettleSeconds = 6,
    [int]$MeasureSeconds = 10,
    [int]$WaitMinutes = 30,
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

# Preserve caller extensions and restore process environment even on early failure.
$probeOriginalPathExt = [Environment]::GetEnvironmentVariable('PATHEXT', 'Process')
try {
if ([string]::IsNullOrWhiteSpace($probeOriginalPathExt)) {
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
} elseif (($probeOriginalPathExt -split ';' | ForEach-Object { $_.Trim() }) -notcontains '.EXE') {
    $env:PATHEXT = $probeOriginalPathExt + ';.EXE'
}

$lab       = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl     = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playPath = Join-Path $replayDir (New-FlabReplayName "mask_fps_probe")

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

function Wait-GameFree([int]$minutes) {
    $deadline = (Get-Date).AddMinutes($minutes)
    $warned = $false
    while ((Get-Date) -lt $deadline) {
        $busy = Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue
        if (-not $busy) { return $true }
        if (-not $warned) {
            Write-Host ("waiting: another RA3 instance is running (pid {0}) - not ours, will not be touched" -f ($busy | Select-Object -First 1).Id)
            $warned = $true
        }
        Start-Sleep -Seconds 10
    }
    return $false
}

$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    throw "replay copy differs from source"
}
Write-Host ("replay: {0}" -f $src.Name)

$maskList = @($Masks -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$results = @()

foreach ($m in $maskList) {
    if (-not (Wait-GameFree $WaitMinutes)) { Write-Host "game never became free; aborting"; break }

    $maskVal = [Convert]::ToInt64($m, 16)
    Write-Host ""
    Write-Host ("================ mask {0} (bit0 RATIO = {1}) ================" -f $m, ($maskVal -band 1))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres 800 -yres 450 -replayGame "{0}" -config "{1}"' -f $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false

    $proc = $null
    try {
        $proc = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Seconds 45
        if ($proc.HasExited) { Write-Host "game exited during startup (another instance?)"; continue }

        [void](Invoke-Flctl "status" "" $proc.Id)
        $frame = -1
        for ($i = 0; $i -lt 40; $i++) {
            $frame = Invoke-Flctl "logicframe" "" $proc.Id
            if ($frame -gt 0 -and $frame -lt 100000000) { break }
            Start-Sleep -Seconds 2
        }
        if ($frame -le 0) { Write-Host "sim never advanced"; continue }

        [void](Invoke-Flctl "groups" "$m" $proc.Id)
        $rc = Invoke-Flctl "enable" "$Fps" $proc.Id
        Write-Host ("install rc {0}, logic frame at install = {1}" -f $rc, $frame)
        if ($rc -ne 0) { continue }

        Start-Sleep -Seconds $SettleSeconds
        $lA = Invoke-Flctl "logicframe" "" $proc.Id
        $tA = Get-Date
        $fps100 = Invoke-Flctl "fpswin" "$MeasureSeconds" $proc.Id
        $lB = Invoke-Flctl "logicframe" "" $proc.Id
        $secs = ((Get-Date) - $tA).TotalSeconds
        $renderFps = [math]::Round($fps100 / 100.0, 2)
        $logicFps = 0.0
        if ($secs -gt 0) { $logicFps = [math]::Round(($lB - $lA) / $secs, 2) }

        # The whole point: did the frame rate actually go up, or did we just tell the patch 60
        # while the engine kept rendering at 30?
        $raised = "NO  (engine still at ~30 -> this arm is self-contradictory)"
        if ($renderFps -ge 0.9 * $Fps) { $raised = "YES" }
        Write-Host ("  render {0} fps | logic {1} fps | frame rate raised: {2}" -f $renderFps, $logicFps, $raised)

        $results += [pscustomobject]@{
            Mask = $m; Bit0 = ($maskVal -band 1); RenderFps = $renderFps
            LogicFps = $logicFps; Raised = $raised
        }
    } finally {
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 4
    }
}

Write-Host ""
Write-Host "================ RESULT ================"
$results | Format-Table -AutoSize
Write-Host "READ IT LIKE THIS: only masks whose render fps actually reached the target can be used"
Write-Host "to say anything about what happens AT that frame rate. The rest are inconsistent configs."
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
} finally {
    [Environment]::SetEnvironmentVariable('PATHEXT', $probeOriginalPathExt, 'Process')
}
