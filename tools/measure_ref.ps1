# measure_ref.ps1 -- measure the ACTUAL render rate of a reference (already-patched) game binary.
#
# 2026-09-16 / developer probe
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why: the community "60 FPS patch" claims 60 fps in its README. Nobody here has measured it.
# Our own runs show the client tick pinned at ~30/s no matter which of our 11 redirects are on,
# so before building more machinery we check whether the reference binary actually renders faster.
# "The author says so" is not a measurement.
#
# How it stays inside the isolation rules:
#   - the reference .game is run FROM ITS OWN FOLDER; the game install is never modified,
#     nothing is copied into it, no file of the user's game is touched
#   - WorkingDir is the game root (the engine needs it) and -config points at the real SkuDef
#   - our DLL is attached in measure-only mode: it patches exactly ONE site (the per-frame phase
#     advance) purely to count frames, and its fraction result is bit-identical to retail
#   - never touches a game instance it did not start itself
param(
    [string]$Exe = "",
    [int]$SampleSeconds = 10,
    [int]$WaitMinutes = 15,
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
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playName = New-FlabReplayName "measure_ref"
$playPath  = Join-Path $replayDir $playName

$Exe = Get-FlabExistingFile $Exe "Reference executable (-Exe)"
$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

$deadline = (Get-Date).AddMinutes($WaitMinutes)
while ((Get-Date) -lt $deadline -and (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue)) {
    Write-Host "waiting for the game to be free (an instance we did not start is running)"
    Start-Sleep -Seconds 10
}

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) { throw "Game still running after -WaitMinutes; refusing to launch." }

Write-Host ("reference binary: {0}" -f $Exe)
Write-Host ("  sha1 {0}  {1} bytes" -f (Get-FileHash $Exe -Algorithm SHA1).Hash, (Get-Item $Exe).Length)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = '-win -xres 800 -yres 450 -replayGame "' + $playPath + '" -config "' + $skudef + '"'
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host ("launched pid {0}" -f $proc.Id)
Start-Sleep -Seconds 45           # attaching earlier can freeze the process

[void](Invoke-Flctl "status" "" $proc.Id)
$frame = -1
for ($i = 0; $i -lt 30; $i++) {
    $frame = Invoke-Flctl "logicframe" "" $proc.Id
    if ($frame -gt 0 -and $frame -lt 100000000) { break }
    Start-Sleep -Seconds 2
}
if ($frame -le 0) {
    Write-Host "FAIL: sim never advanced on the reference binary"
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    exit 1
}
$rc = Invoke-Flctl "measure" "" $proc.Id
Write-Host ("measure-only install rc {0} (0 = ok), logic frame {1}" -f $rc, $frame)
if ($rc -ne 0) { Stop-Process -Id $proc.Id -Force; exit 1 }

$lA = Invoke-Flctl "logicframe" "" $proc.Id
$tA = Get-Date
$fps100 = Invoke-Flctl "fpswin" "$SampleSeconds" $proc.Id
$lB = Invoke-Flctl "logicframe" "" $proc.Id
$engine = Invoke-Flctl "enginefps" "" $proc.Id
$renderFps = [math]::Round($fps100 / 100.0, 2)
$logicFps  = [math]::Round(($lB - $lA) / ((Get-Date) - $tA).TotalSeconds, 2)

Write-Host ""
Write-Host "================ REFERENCE BINARY ================"
Write-Host ("render {0} fps (our counter)   engine's own fps field {1}" -f $renderFps, ([math]::Round($engine / 100.0, 2)))
Write-Host ("logic  {0} fps   <- retail is 15; the community patch sets the logic-fps global to 30" -f $logicFps)
Write-Host "compare with our own binary measured the same way: render 30.2 / logic 14.82"

if (-not $proc.HasExited) {
    [void](Invoke-Flctl "disable" "" $proc.Id)
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
