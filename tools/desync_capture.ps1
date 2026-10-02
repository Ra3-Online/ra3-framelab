# desync_capture.ps1 -- when the sim "stalls", LOOK at the screen instead of guessing.
#
# 2026-09-18 / developer probe
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# WHY THIS EXISTS (the ruler is suspect -- suspect it first):
#   determinism.ps1 decides "desync" from ONE number: FrameLabLogicFrame(), which reads
#   *(int*)(*(0x00CD8CE4) + 0x50) -- the frame counter of the CURRENT match object.
#   When its verdict fires, every arm's log line reads "logic frame stalled at 0".
#   Note what that means: the counter did not FREEZE at N, it RESET to 0.
#   Two very different situations produce exactly that reading:
#     (a) the engine popped its own "out of sync" dialog / dropped back to the front end;
#     (b) the match ended or aborted for an unrelated reason and the game sits in a menu.
#   A single integer cannot separate them. A screenshot can.
#
#   So this script re-runs ONE arm with the same replay and, the instant the stall is seen,
#   captures the game window (twice, 6 s apart, plus one early reference shot).
#   It does NOT change determinism.ps1's verdict logic: it is a second, independent ruler.
#
# Discipline (same as the other tools in this directory):
#   - waits for the game to be free; NEVER kills an instance it did not start
#     (other sessions on this machine use the same game)
#   - only ever drives the pid it launched itself
#   - never calls `disable` while running (see HANDOFF 9: that freezes the game);
#     cleanup is Stop-Process on our own pid only
param(
    [int]$Fps = 60,
    [int]$Minutes = 20,
    [int]$Groups = 0x2CFFF,
    [int]$StallAfter = 2,        # consecutive stalled reads before we call it and shoot
    [int]$EarlyFrame = 900,      # also take one reference shot once the sim passes this frame
    [int]$WaitMinutes = 30,
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$OutDir = Get-FlabRunDirectory $OutDir "build\desync_capture"

# ------ sandbox compatibility: restore PATHEXT (same reason as determinism.ps1) ------
# The sandbox narrows PATHEXT to ".CPL", which makes PowerShell treat every .exe as a
# *document* => "Cannot run a document in the middle of a pipeline".
if ($env:PATHEXT -notmatch '\.EXE') {
    $env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"
    Write-Host ("note: PATHEXT was missing .EXE; restored to " + $env:PATHEXT)
}

$lab       = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl     = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
# distinct name: other sessions use _simre_play / _racol_play, bisect uses _flab_play
$playPath = Join-Path $replayDir (New-FlabReplayName "desync_capture")
$outRoot = $OutDir

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
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

function Get-Title([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][W]::GetWindowTextW($h, $sb, 512)
    return $sb.ToString()
}

# Foreground window title + its owning pid. If the game dropped to a menu we still see the
# game's own window; if something else (an error box owned by another process, the desktop)
# took over, this says so.
function Describe-Foreground() {
    $fg = [W]::GetForegroundWindow()
    $pid2 = 0
    [void][W]::GetWindowThreadProcessId($fg, [ref]$pid2)
    return ("fg hwnd=0x{0:X} pid={1} title='{2}'" -f [int64]$fg, $pid2, (Get-Title $fg))
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

# Pin the window topmost + foreground so CopyFromScreen grabs the GAME and not whatever
# happens to be on top (bisect.ps1 learned this the hard way).
function Show-Game([IntPtr]$hwnd) {
    [void][W]::SetWindowPos($hwnd, [IntPtr](-1), 40, 40, 0, 0, 0x0001)   # HWND_TOPMOST, SWP_NOSIZE
    [void][W]::SetForegroundWindow($hwnd)
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

# Explicit -Replay selects the test basis; no personal replay is chosen implicitly.
$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    throw "replay copy differs from source"
}

$dir = Join-Path $outRoot ("g{0:X}_fps{1}" -f $Groups, $Fps)
New-Item -ItemType Directory -Force $dir | Out-Null

Write-Host ("replay: {0}" -f $src.Name)
Write-Host ("arm:    -Fps {0} -Groups 0x{1:X}  ({2} min, stall threshold {3})" -f $Fps, $Groups, $Minutes, $StallAfter)
Write-Host ("shots:  {0}" -f $dir)

if (-not (Wait-GameFree $WaitMinutes)) { Write-Host "game never became free; aborting"; exit 2 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $gameExe
$psi.Arguments = ('-win -xres 800 -yres 450 -replayGame "{0}" -config "{1}"' -f $playPath, $skudef)
$psi.WorkingDirectory = $gameRoot
$psi.UseShellExecute = $false

$proc = $null
try {
    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("launched pid {0}" -f $proc.Id)
    Start-Sleep -Seconds 45
    if ($proc.HasExited) { Write-Host "game exited during startup"; exit 3 }

    [void](Invoke-Flctl "status" "" $proc.Id)
    $frame = -1
    for ($i = 0; $i -lt 40; $i++) {
        $frame = Invoke-Flctl "logicframe" "" $proc.Id
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if ($frame -le 0) { Write-Host "sim never advanced"; exit 4 }

    [void](Invoke-Flctl "groups" "$Groups" $proc.Id)
    $rc = Invoke-Flctl "enable" "$Fps" $proc.Id
    Write-Host ("install rc {0}, starting at logic frame {1}" -f $rc, $frame)
    if ($rc -ne 0) { exit 5 }

    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
    Show-Game $hwnd
    Write-Host ("game hwnd=0x{0:X} title='{1}'" -f [int64]$hwnd, (Get-Title $hwnd))

    # ---- render/logic fps readout, measured INSIDE the process -------------------------------
    # Needed because the group masks are not independent: turning FL_G_DIVFPS|UNSURE off leaves
    # the frame rate at 30, which changes WHICH divergence you are looking at. Without this
    # number the arm's verdict is uninterpretable. Sampling from outside every 200 ms drags the
    # sim down (bisect.ps1 measured 15 -> 4 logic fps), so use the in-process `fpswin`.
    Start-Sleep -Seconds 6
    $lA = Invoke-Flctl "logicframe" "" $proc.Id
    $tA = Get-Date
    $fps100 = Invoke-Flctl "fpswin" "10" $proc.Id
    $lB = Invoke-Flctl "logicframe" "" $proc.Id
    $secs = ((Get-Date) - $tA).TotalSeconds
    $renderFps = [math]::Round($fps100 / 100.0, 2)
    $logicFps = 0.0
    if ($secs -gt 0) { $logicFps = [math]::Round(($lB - $lA) / $secs, 2) }
    Write-Host ("render {0} fps, logic {1} fps (measured inside the process)" -f $renderFps, $logicFps)
    if ($renderFps -lt 0.95 * $Fps) {
        Write-Host ("WARNING: render did not reach the target {0} -- this arm does NOT have the frame rate raised" -f $Fps)
    }
    $last = $lB

    $deadline = (Get-Date).AddMinutes($Minutes)
    # $last was already set from the fps readout above -- do not reset it to the older $frame
    $stalls = 0; $verdict = "survived"; $earlyDone = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 20
        if ($proc.HasExited) { $verdict = "process exited"; break }
        $now = Invoke-Flctl "logicframe" "" $proc.Id
        if ($now -lt 0) { $verdict = "lost contact with the process"; break }

        if ($now -le $last) {
            $stalls++
            Write-Host ("  logic frame stalled at {0} (was {1}, {2} in a row)" -f $now, $last, $stalls)
            # THE POINT OF THIS SCRIPT: look at the screen before believing the number.
            Show-Game $hwnd
            Start-Sleep -Milliseconds 700
            $png = Join-Path $dir ("stall{0}_read{1}_was{2}.png" -f $stalls, $now, $last)
            if (Capture $hwnd $png) { Write-Host ("    shot: {0}" -f (Split-Path $png -Leaf)) }
            Write-Host ("    {0}" -f (Describe-Foreground))
            if ($stalls -ge $StallAfter) {
                $verdict = "DESYNC / STALL"
                # two more shots a few seconds apart: a desync dialog is modal and stays put,
                # while a menu/score screen keeps animating. Comparing the shots separates them.
                for ($k = 1; $k -le 2; $k++) {
                    Start-Sleep -Seconds 6
                    if ($proc.HasExited) { break }
                    Show-Game $hwnd
                    Start-Sleep -Milliseconds 700
                    $png2 = Join-Path $dir ("stall_after{0}.png" -f $k)
                    if (Capture $hwnd $png2) { Write-Host ("    shot: {0}" -f (Split-Path $png2 -Leaf)) }
                }
                break
            }
        } else {
            $stalls = 0
            Write-Host ("  logic frame {0} (+{1})" -f $now, ($now - $last))
            if ((-not $earlyDone) -and $now -ge $EarlyFrame) {
                # reference shot from a healthy moment -- without it a "stall" image has no baseline
                $earlyDone = $true
                Show-Game $hwnd
                Start-Sleep -Milliseconds 700
                $png3 = Join-Path $dir ("reference_at{0}.png" -f $now)
                if (Capture $hwnd $png3) { Write-Host ("    reference shot: {0}" -f (Split-Path $png3 -Leaf)) }
            }
        }
        $last = $now
    }
    $endFrame = if ($proc.HasExited) { $last } else { Invoke-Flctl "logicframe" "" $proc.Id }
    Write-Host ("RESULT: {0}  (reached logic frame {1})" -f $verdict, $endFrame)
    Write-Host "NOTE: surviving is NOT proof -- it is a failed attempt to falsify."
} finally {
    # never call `disable` here (HANDOFF 9: restoring bytes under live threads freezes the game).
    # the patch lives only in this process's memory, so killing our own pid is the safe cleanup.
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 4
}
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
