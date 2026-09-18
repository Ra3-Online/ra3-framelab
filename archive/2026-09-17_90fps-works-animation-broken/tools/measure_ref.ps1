# measure_ref.ps1 -- measure the ACTUAL render rate of a reference (already-patched) game binary.
#
# 2026-09-16 / session 68ee9b9d (Claude)
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
    [string]$Exe = "G:\参考源码\红警3平台增强\red-alert-3-60fps-mod-main\Data\60FPS\ra3_1.12.game",
    [int]$SampleSeconds = 10,
    [int]$WaitMinutes = 15
)

$ErrorActionPreference = "Stop"
$lab       = Split-Path $PSScriptRoot -Parent
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playName  = "_flab_play.RA3Replay"
$playPath  = Join-Path $replayDir $playName

if (-not (Test-Path $Exe)) { throw "missing reference binary: $Exe" }
if (-not (Test-Path $playPath)) {
    $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
           Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
           Sort-Object Length -Descending | Select-Object -First 1
    Copy-Item $src.FullName $playPath -Force
}

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
