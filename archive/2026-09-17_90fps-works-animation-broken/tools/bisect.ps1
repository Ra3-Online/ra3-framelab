# bisect.ps1 -- run one replay under several patch-group masks and capture matched screenshots.
#
# 2026-09-16 / session 68ee9b9d (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why this exists: the user saw broken animations (barracks build-up, bear movement) at 90 fps
# with every group enabled. Guessing which change did it is not acceptable -- so each group can
# be switched off individually and we compare, at the SAME ABSOLUTE LOGIC FRAME, what the screen
# actually shows. Same replay + same logic frame = the only fair comparison; wall-clock is not.
#
# Discipline:
#   - waits for the game to be free; never kills an instance it did not start
#     (another session's tests use this game too)
#   - only ever drives the pid it launched itself
#   - the replay copy is named _flab_play (other sessions use _simre_play / _racol_play)
param(
    [int]$Fps = 90,
    [string]$Masks = "0x101,0x103,0x183,0x1FF",
    [string]$ShotFrames = "200,300,400,500,600",   # comma list; -File passes args as strings
    [int]$WaitMinutes = 30
)

$ErrorActionPreference = "Stop"
$lab       = Split-Path $PSScriptRoot -Parent
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe   = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playName  = "_flab_play.RA3Replay"
# absolute path: passing the bare file name made the engine fail with "replay read error"
# intermittently (worked once, failed twice). The known-good launcher passes the full path.
$playPath  = Join-Path $replayDir $playName
$outRoot   = Join-Path $build "bisect"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

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

function Capture([IntPtr]$hwnd, [string]$path) {
    $r = New-Object W+RECT
    if (-not [W]::GetWindowRect($hwnd, [ref]$r)) { return $false }
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

function Run-Config([string]$label, [int]$mask, [bool]$patched) {
    Write-Host ""
    Write-Host ("================ {0} ================" -f $label)
    if (-not (Wait-GameFree $WaitMinutes)) { Write-Host "game never became free; aborting"; return $null }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = '-win -xres 800 -yres 450 -replayGame "' + $playPath + '" -config "' + $skudef + '"'
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("launched pid {0}" -f $proc.Id)
    # peer session measured: attaching to a just-started game freezes it (1.2-1.5s always,
    # ~7s usually fine, 45-60s safest). 12s was inside the "usually" band and did bite us once.
    Start-Sleep -Seconds 45
    if ($proc.HasExited) { Write-Host "game exited immediately (another instance grabbed it?)"; return $null }

    [void](Invoke-Flctl "status" "" $proc.Id)
    $frame = -1
    for ($i = 0; $i -lt 40; $i++) {
        $frame = Invoke-Flctl "logicframe" "" $proc.Id
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if ($frame -le 0 -or $frame -ge 100000000) {
        Write-Host "FAIL: sim never advanced"
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
        return $null
    }

    if ($patched) {
        [void](Invoke-Flctl "groups" "$mask" $proc.Id)
        $rc = Invoke-Flctl "enable" "$Fps" $proc.Id
    } else {
        $rc = Invoke-Flctl "measure" "" $proc.Id
    }
    Write-Host ("install rc {0} (0 = ok), logic frame at install = {1}" -f $rc, $frame)
    if ($rc -ne 0) { Stop-Process -Id $proc.Id -Force; return $null }

    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
    # pin the window to a fixed spot AND make it topmost: last round the capture grabbed whatever
    # was covering the window (desktop, chat) because the rect moved and the window was not on top.
    [void][W]::SetWindowPos($hwnd, [IntPtr](-1), 40, 40, 0, 0, 0x0001)   # HWND_TOPMOST, SWP_NOSIZE
    [void][W]::SetForegroundWindow($hwnd)
    $dir = Join-Path $outRoot $label
    New-Item -ItemType Directory -Force $dir | Out-Null

    # fps is measured INSIDE the game (one remote call). Sampling it from outside every 200ms
    # dragged the sim from 15 to 4 logic fps -- the ruler was crushing the thing it measured.
    $lA = Invoke-Flctl "logicframe" "" $proc.Id
    $tA = Get-Date
    $fps100 = Invoke-Flctl "fpswin" "10" $proc.Id
    $lB = Invoke-Flctl "logicframe" "" $proc.Id
    $renderFps = [math]::Round($fps100 / 100.0, 2)
    $logicFps = [math]::Round(($lB - $lA) / ((Get-Date) - $tA).TotalSeconds, 2)
    Write-Host ("{0}: render {1} fps, logic {2} fps (measured inside the process)" -f $label, $renderFps, $logicFps)
    foreach ($target in $shotList) {
        $now = Invoke-Flctl "waitframe" "$target" $proc.Id
        if ($proc.HasExited -or $now -lt 0) { break }
        [void][W]::SetWindowPos($hwnd, [IntPtr](-1), 40, 40, 0, 0, 0x0001)
        [void][W]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 600
        # NOTE: do NOT judge the shot by GetForegroundWindow. SetForegroundWindow is refused to
        # background processes, so that flag says "not foreground" even when HWND_TOPMOST already
        # made the window fully visible and the capture is perfect -- it fired on every good shot.
        # The judge has to BE the conclusion: "is there game content in the image". That check
        # lives in the Python analysis, which can actually look at the pixels.
        $png = Join-Path $dir ("f{0:D4}_at{1:D4}.png" -f $target, $now)
        if (Capture $hwnd $png) { Write-Host ("  shot at logic frame {0} (target {1})" -f $now, $target) }
    }

    if (-not $proc.HasExited) {
        [void](Invoke-Flctl "disable" "" $proc.Id)
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 4
    return [pscustomobject]@{ Label = $label; RenderFps = $renderFps; LogicFps = $logicFps }
}

# ---- replay basis ----
$src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
       Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
       Sort-Object Length -Descending | Select-Object -First 1
if (-not $src) { throw "no usable replay" }
Copy-Item $src.FullName $playPath -Force
$srcHash = (Get-FileHash $src.FullName -Algorithm SHA1).Hash
$cpyHash = (Get-FileHash $playPath -Algorithm SHA1).Hash
if ($srcHash -ne $cpyHash) { throw "replay copy differs from source - refusing to run on a bad basis" }
Write-Host ("replay basis: {0} {1} bytes sha1 {2} (copy verified identical)" -f $src.Name, $src.Length, $cpyHash)
if (Test-Path $outRoot) { Remove-Item $outRoot -Recurse -Force }

$shotList = @($ShotFrames -split "," | ForEach-Object { [int]$_.Trim() })
$results = @()
$r = Run-Config "baseline-30" 0 $false
if ($r) { $results += $r }
foreach ($m in ($Masks -split ",")) {
    $mask = [Convert]::ToInt32(($m.Trim() -replace '^0[xX]', ''), 16)
    $r = Run-Config ("g{0:X3}" -f $mask) $mask $true
    if ($r) { $results += $r }
}

Write-Host ""
Write-Host "================ SUMMARY ================"
$results | Format-Table -AutoSize
Write-Host ("screenshots under {0} - same logic frames across configs, so they are directly comparable" -f $outRoot)
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
