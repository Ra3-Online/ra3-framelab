# watch.ps1 -- launch the replay with the patch on and LEAVE IT RUNNING for a human to watch.
#
# 2026-09-17 / developer probe
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why this is separate from bisect.ps1: that one measures and then kills the game. This one hands
# a live, patched game to the user so they can judge the animations with their eyes -- the one
# ruler that actually works here, after the still-frame pixel diff failed to separate the configs.
#
# It prints the toggle commands so the user can flip patched/unpatched ON THE SAME REPLAY while
# watching. Same content, same moment, one variable -- far better than comparing two separate runs.
param(
    [int]$Fps = 90,
    [int]$Width = 1024,
    [int]$Height = 576,
    [int]$Groups = 0, # 0 = the default set (everything except the three derived globals)
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

$lab       = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl     = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playName = New-FlabReplayName "watch"
$playPath  = Join-Path $replayDir $playName

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    throw "a game instance is already running - not ours, refusing to start a second one (RA3 is single-instance)"
}

$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    throw "replay copy differs from source - refusing to run on a bad basis"
}
Write-Host ("replay: {0} ({1} bytes)" -f $src.Name, $src.Length)

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $gameExe
$psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host ("launched pid {0} ({1}x{2} windowed)" -f $proc.Id, $Width, $Height)

# attaching to a just-started game freezes it (measured: 1.2-1.5s always, ~7s usually, 45-60s safest)
Start-Sleep -Seconds 45
if ($proc.HasExited) { throw "game exited during startup" }

[void](Invoke-Flctl "status" "" $proc.Id)
$frame = -1
for ($i = 0; $i -lt 40; $i++) {
    $frame = Invoke-Flctl "logicframe" "" $proc.Id
    if ($frame -gt 0 -and $frame -lt 100000000) { break }
    Start-Sleep -Seconds 2
}
if ($frame -le 0) { Stop-Process -Id $proc.Id -Force; throw "sim never advanced" }

if ($Groups -ne 0) { [void](Invoke-Flctl "groups" "$Groups" $proc.Id) }
$rc = Invoke-Flctl "enable" "$Fps" $proc.Id
if ($rc -ne 0) { Write-Host ("enable failed rc={0} - see the log" -f $rc) }

$fps100 = Invoke-Flctl "fpswin" "6" $proc.Id
$engine = Invoke-Flctl "enginefps" "" $proc.Id
Write-Host ""
Write-Host "================================================================"
Write-Host ("  patched to {0} fps, rc={1}, logic frame at install = {2}" -f $Fps, $rc, $frame)
Write-Host ("  render {0} fps (our counter) / {1} fps (engine's own field)" -f `
            [math]::Round($fps100/100.0,2), [math]::Round($engine/100.0,2))
Write-Host ("  GAME PID = {0}   <-- the game is LEFT RUNNING for you to watch" -f $proc.Id)
Write-Host "================================================================"
Write-Host "toggle while watching (same replay, one variable):"
Write-Host ('  "{0}" disable 0 {1}      <- back to stock 30 fps' -f $flctl, $proc.Id)
Write-Host ('  "{0}" enable {1} {2}     <- patched again' -f $flctl, $Fps, $proc.Id)
