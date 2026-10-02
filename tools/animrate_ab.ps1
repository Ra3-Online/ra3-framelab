# animrate_ab.ps1 -- launch a replay, inject the patch, MEASURE the animation rate, then quit.
#
# 2026-09-17 / animation-locating session (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why this exists next to watch.ps1:
#   watch.ps1 hands a live, patched game to a HUMAN so they can judge animations by eye.
#   This one is for when the agent must collect a NUMBER. It runs the whole sequence inside
#   ONE command -- launch, wait, inject, measure -- because in this environment a launched
#   game does not survive the command that started it. Everything the run produces is
#   written to one file, including machine-readable RESULT= lines.
#
# What it measures (see src/framelab.cpp):
#   MAIN  "clockrate" -- how many milliseconds of the GAME CLOCK advance per LOGIC frame, x1000.
#         The animation's time step is literally the game-clock delta (sub_90ECF0:
#         dt = clock - lastClock, then frame += dt_seconds * animFps), so this number IS
#         "how fast animations run relative to the sim". It is render-fps independent:
#           stock (r=2, 33 ms/frame)          -> 2 * 33.33 =  66.7 ms per logic frame
#           correct 90 fps (r=6, 11 ms/frame) -> 6 * 11.11 =  66.7 ms per logic frame  (same)
#           broken  90 fps (r=6, 33 ms/frame) -> 6 * 33.33 = 200.0 ms per logic frame  (3x)
#         No objects sampled, no dependence on where the replay is -> cannot be confounded.
#   AUX   "animrate" -- animation frames advanced per logic frame for the busiest sampled object.
#         Kept as a cross-check only. It IS confounded: different runs record different
#         objects at different replay positions, so its "fastest object" statistic is not
#         comparable across runs (measured 187 stock vs 41 broken -- the wrong direction).
#         Historical animation notes are archived separately; see docs/TECHNICAL.md for current boundaries.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\animrate_ab.ps1 -Groups 0x41FF -Out ...\_A.txt
#   powershell -ExecutionPolicy Bypass -File tools\animrate_ab.ps1 -Groups 0x61FF -Out ...\_B.txt
#   0x41FF = default patch set (0x1FF) + animation ruler probe (0x4000)
#   0x61FF = the same plus P8 (0x2000, client fps global written to the target rate)
param(
    [int]$Fps = 90,
    [int]$Groups = 0xC1FF,
    [int]$AnimFrames = 150,      # logic frames to watch while measuring (150 = 10 sim seconds)
    [int]$StartupWait = 45,      # seconds to let the game boot before attaching (see HANDOFF)
    [string]$Out = "",
    [switch]$Stock,              # 2026-09-17: probe only, no patch group -- the stock baseline.
                                 # This is the reference the whole A/B test is judged against:
                                 # stock (30 fps, r=2) is by definition the "correct" animation rate.
    [switch]$KeepOpen, # do NOT kill the game at the end (leaves it for a human)
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$Out = Get-FlabOutputPath $Out "build\logs\_ab.txt"

# Group mask cheat-sheet for this script (see src/framelab.cpp for the bit definitions):
#   0x4000 = animation probe only            -> stock baseline (nothing else is patched)
#   0x41FF = old default set + probe         -> the BROKEN 90 fps config (no clock fix)
#   0xC1FF = 0x41FF | FL_G_CLOCK(0x8000)     -> the FIXED 90 fps config (default now)

$lab      = Split-Path $PSScriptRoot -Parent
$build = Get-FlabBuildDirectory
$flctl    = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playName = New-FlabReplayName "animrate_ab"
$playPath  = Join-Path $replayDir $playName

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }

Log ("=== animrate_ab  fps={0} groups=0x{1:X4} animFrames={2} stock={3} ===" -f $Fps, $Groups, $AnimFrames, [bool]$Stock)

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Log "REFUSE: a game instance is already running (RA3 is single-instance)"
    $lines | Out-File -Encoding utf8 $Out
    exit 2
}

# Explicit -Replay selects the test basis; no personal replay is chosen implicitly.
$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    Log "REFUSE: replay copy differs from source"; $lines | Out-File -Encoding utf8 $Out; exit 2
}
Log ("replay: {0} ({1} bytes, sha1 ok)" -f $src.Name, $src.Length)

function Invoke-FlctlRaw([string]$cmd, [string]$arg, [int]$targetPid) {
    # Why not `& $flctl ...` : in this environment PowerShell resolves flctl.exe as a *document*
    # ("cannot run a document in the middle of a pipeline"), which looks like a PATHEXT problem.
    # Starting it explicitly and redirecting the streams sidesteps PowerShell's command lookup
    # entirely. flctl writes little, so reading stdout to the end before waiting cannot deadlock.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $flctl
    $psi.Arguments = ('{0} {1} {2}' -f $cmd, $arg, $targetPid)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEnd()
    $e = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    # 2026-09-17:记录退出码。一次 flctl 静默无输出(=进程崩溃,stdout 接管道是全缓冲,
    # 崩溃时缓冲区整个丢失)让 clockrate 的判据变成 -9999,而脚本里看不出任何原因。
    # 有退出码就能区分:0xC0000005=崩溃 / 7=没找到导出 / 8=远程调用失败 / 3=没找到游戏。
    $script:flctlExit = $p.ExitCode
    return ($o + "`n" + $e)
}
function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = Invoke-FlctlRaw $cmd $arg $targetPid
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -9999
}
# flctl's "read" takes a hex address; keep it a string so it is not re-parsed as decimal
function Read-Dword([int]$va, [int]$targetPid) {
    $out = Invoke-FlctlRaw "read" ("0x{0:X}" -f $va) $targetPid
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -9999
}

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
    Log ("waiting {0}s for the game to boot before attaching" -f $StartupWait)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Log "FAIL: game exited during startup"; throw "game exited" }

    # Diagnostic: if the window is iconic the engine skips its whole tick (see sub_5FE9D0:
    # `if (!hWnd || IsIconic(hWnd) == 0)` guards everything), so the sim would never advance
    # and every measurement would read 0. Log the window state so a zero is explainable.
    $proc.Refresh()
    Log ("window title = '{0}'  responding={1}  hwnd={2}" -f $proc.MainWindowTitle, $proc.Responding, $proc.MainWindowHandle)
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
        Log "AppActivate: attempted (brings the game window to the foreground)"
    } catch { Log ("AppActivate failed: {0}" -f $_) }

    # ---- bring the game window back to the foreground ----
    # Why: this run is driven from a console, so the console steals the foreground from the
    # game. An unfocused window is not merely cosmetic -- in the 13:15 Run A the renderer only
    # reached 52.66 fps instead of 90. That does NOT invalidate animrate (it is normalised to
    # logic frames, see the header), but it slows the run down and makes the fps columns
    # unreadable, so re-activate before each measurement.
    function Focus-Game {
        try {
            Add-Type -AssemblyName Microsoft.VisualBasic
            [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
        } catch { }
    }

    [void](Invoke-Flctl "status" "" $proc.Id)

    $frame = -1
    for ($i = 0; $i -lt 60; $i++) {
        $frame = Invoke-Flctl "logicframe" "" $proc.Id
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        if (($i % 5) -eq 0) { Log ("  poll {0}: logicframe={1}" -f $i, $frame) }
        Start-Sleep -Seconds 3
    }
    Log ("logic frame at attach = {0}" -f $frame)
    if ($frame -le 0) {
        $proc.Refresh()
        Log ("FAIL: sim never advanced. window title now = '{0}'" -f $proc.MainWindowTitle)
        throw "no sim"
    }

    if ($Stock) {
        # Stock baseline. NOT "inject nothing": the animation ruler only exists once install()
        # has redirected the one `call 0x8EDED0` site, so a run with no injection at all would
        # measure nothing. Instead enable with a mask that turns off EVERY patch group and keeps
        # only the probe (0x4000). install() gates every patch on its own group bit (see
        # src/framelab.cpp install()), so this run leaves the game byte-for-byte as shipped:
        # 30 fps render / 15 fps logic, r = 2 -- while still counting animation frames.
        # The probe wrapper records and then forwards to the original and returns its double
        # unchanged, so it is transparent to the caller too.
        Log "STOCK run: groups=0x4000 -- probe only, no patch group enabled (30 fps / r=2)"
        $g = Invoke-Flctl "groups" "16384" $proc.Id
        $rawEnable = Invoke-FlctlRaw "enable" ("{0}" -f $Fps) $proc.Id
        $mE = [regex]::Match($rawEnable, 'RESULT=(-?\d+)')
        if ($mE.Success) { $e = [int64]$mE.Groups[1].Value } else { $e = -9999 }
        Log ("groups rc={0}  enable rc={1}" -f $g, $e)
        if ($e -ne 0) { Log "FAIL: enable refused"; throw "enable failed" }
    } else {
        # ---- groups must be set BEFORE enable: install() reads the mask when it patches ----
        $g = Invoke-Flctl "groups" ("{0}" -f $Groups) $proc.Id
        $rawEnable = Invoke-FlctlRaw "enable" ("{0}" -f $Fps) $proc.Id
        $mE = [regex]::Match($rawEnable, 'RESULT=(-?\d+)')
        if ($mE.Success) { $e = [int64]$mE.Groups[1].Value } else { $e = -9999 }
        Log ("groups rc={0}  enable rc={1}" -f $g, $e)
        Log ("enable raw: {0}" -f (($rawEnable -replace "\s+", " ").Trim()))
        if ($e -ne 0) { Log "FAIL: enable refused - see build\logs\framelab-*.log"; throw "enable failed" }
    }

    Start-Sleep -Seconds 3
    Focus-Game
    Start-Sleep -Seconds 2

    # ---- globals (read-only): clock, client fps, logic fps, ms-per-frame ----
    $clock0 = Read-Dword 0x00CE1388 $proc.Id
    $clFps  = Read-Dword 0x00CAF9D4 $proc.Id
    $loFps  = Read-Dword 0x00CAF9D0 $proc.Id
    $msPf   = Read-Dword 0x00CE176C $proc.Id
    Log ("globals: clock(ms)={0}  clientFps={1}  logicFps={2}  msPerFrame={3}" -f $clock0, $clFps, $loFps, $msPf)

    # ---- two independent frame-rate rulers ----
    # In stock mode our own per-render-frame counter is not installed (its code block belongs to
    # the FL_G_PERFRAME group), so fpswin would always read 0. Skip it instead of logging a 0
    # that looks like a failure.
    if ($Stock) {
        $fps100 = -1
        $engine = Invoke-Flctl "enginefps" "" $proc.Id
        Log ("render fps: ours=n/a (stock: per-frame counter not installed) engine={0}" -f ($engine / 100.0))
    } else {
        $fps100 = Invoke-Flctl "fpswin" "6" $proc.Id
        $engine = Invoke-Flctl "enginefps" "" $proc.Id
        Log ("render fps: ours={0} engine={1}" -f ($fps100 / 100.0), ($engine / 100.0))
    }

    # ---- the animation ruler ----
    Focus-Game
    Start-Sleep -Seconds 1
    [void](Invoke-Flctl "animreset" "" $proc.Id)
    # Log flctl's raw text too: -9999 (no RESULT= line) is indistinguishable between
    # "the export is missing", "the call failed" and "the game died mid-call" unless we
    # keep what flctl actually said. That cost a whole run on 2026-09-17.
    $rawRate = Invoke-FlctlRaw "animrate" ("{0}" -f $AnimFrames) $proc.Id
    Log ("animrate raw: {0}   [flctl exit={1}]" -f (($rawRate -replace "\s+", " ").Trim()), $script:flctlExit)
    $mR = [regex]::Match($rawRate, 'RESULT=(-?\d+)')
    if ($mR.Success) { $rate = [int64]$mR.Groups[1].Value } else { $rate = -9999 }
    $l2 = Invoke-Flctl "logicframe" "" $proc.Id
    $clock1 = Read-Dword 0x00CE1388 $proc.Id

    # ---- ★ 主判据:每逻辑帧推进多少毫秒游戏时钟 ----------------------------------------
    # 2026-09-17 加了它之后,animrate 退居辅助。理由(见 已归档的动画研究第 12 节):
    #   动画帧探针在两次运行里采到的是**不同对象**(对象地址都不一样),而录像播放位置也不同,
    #   所以「最快对象」这个统计量会被场景差异污染 —— 实测 stock 187 / broken 41,
    #   方向甚至是反的,完全不能当判据。
    #   时钟判据没有这个问题:它不依赖任何对象,只问「时钟相对 sim 走多快」。
    #   原版与正确的高帧率配置都应约 66667;坏配置约 200000(3 倍)。
    $rawClockRate = Invoke-FlctlRaw "clockrate" ("{0}" -f $AnimFrames) $proc.Id
    Log ("clockrate raw: {0}   [flctl exit={1}]" -f (($rawClockRate -replace "\s+", " ").Trim()), $script:flctlExit)
    $mC = [regex]::Match($rawClockRate, 'RESULT=(-?\d+)')
    if ($mC.Success) { $clockRate = [int64]$mC.Groups[1].Value } else { $clockRate = -9999 }

    Log ""
    Log "================ RESULT ================"
    Log ("RESULT_CLOCKRATE={0}" -f $clockRate)     # ★主判据:每逻辑帧毫秒×1000,应约 66667
    Log ("RESULT_ANIMRATE={0}" -f $rate)          # 辅助:动画帧/逻辑帧×1000(会被场景差异污染)
    Log ("RESULT_RENDERFPS={0}" -f $fps100)       # render fps x100
    Log ("RESULT_LOGICFRAME_END={0}" -f $l2)
    Log ("RESULT_CLOCK0={0}" -f $clock0)
    Log ("RESULT_CLOCK1={0}" -f $clock1)
    Log ("RESULT_CLIENTFPS_GLOBAL={0}" -f $clFps)
    Log ("RESULT_LOGFPS_GLOBAL={0}" -f $loFps)
    Log ("RESULT_MSPERFRAME_GLOBAL={0}" -f $msPf)   # 1000/帧率全局,原版 33;我们**不写它**
    Log "========================================"
} catch {
    Log ("EXCEPTION: {0}" -f $_)
    $rc = 1
} finally {
    if (-not $KeepOpen) {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Log "game killed (patches die with the process)" }
    } else {
        Log ("LEAVING GAME RUNNING  pid={0}" -f $proc.Id)
    }
    $lines | Out-File -Encoding utf8 $Out
}

exit $rc
