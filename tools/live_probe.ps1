# live_probe.ps1 -- measure an ALREADY-RUNNING game instance. Launches nothing, kills nothing.
#
# 2026-09-17 / animation-locating session (Claude)
# ASCII only: PowerShell 5.1 misreads BOM-less UTF-8 Chinese source.
#
# Why this exists: the other two tools (look.ps1, fps_capacity.ps1) each start their own game,
# which costs a 45 s boot per data point and refuses outright when a game is already up. When a
# patched game is ALREADY running there is no reason to pay that -- this attaches to it.
#
# SAFETY (this box runs several sessions against the same game):
#   - the pid is REQUIRED. This script never looks a game up by process name, because on this
#     machine that could attach to somebody else's live match.
#   - it never calls Stop-Process. Nothing is terminated, ever.
#   - every flctl call passes the pid explicitly, so flctl cannot fall back to name lookup.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\live_probe.ps1 -GamePid 22580
param(
    [Parameter(Mandatory=$true)][int]$GamePid,
    [int]$Window = 10,           # seconds for the render-frame window
    [int]$ClockFrames = 60,      # logic frames for the clock criterion
    [string]$Out = "G:\Ra3 FrameLab\build\logs\_live.txt"
)

$ErrorActionPreference = "Stop"
$lab    = Split-Path $PSScriptRoot -Parent
$build  = Join-Path $lab "build"
$flctl  = Join-Path $build "flctl.exe"
$logDir = Join-Path $build "logs"

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

Log ("=== live_probe  pid={0} window={1}s clockFrames={2} === " -f $GamePid, $Window, $ClockFrames)

$game = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
if (-not $game) { Log "no process with that id"; Flush; exit 2 }
Log ("process: {0}" -f $game.ProcessName)

function Run-Flctl([string]$cmd, [string]$arg) {
    $si = New-Object System.Diagnostics.ProcessStartInfo
    $si.FileName = $flctl
    $si.Arguments = ('{0} {1} {2}' -f $cmd, $arg, $GamePid)
    $si.UseShellExecute = $false
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    $si.CreateNoWindow = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($si)
    $o = $p.StandardOutput.ReadToEnd()
    $e = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    $code = "?"
    try {
        $c = $p.ExitCode
        if ($null -eq $c) { $code = "<null>" } else { $code = [string]$c }
    } catch { $code = "<threw:" + $_.Exception.GetType().Name + ">" }
    $game.Refresh()
    Log ("--- flctl {0} {1}   exit={2} ms={3} gameAlive={4}" -f $cmd, $arg, $code, $sw.ElapsedMilliseconds, (-not $game.HasExited))
    $flat = ($o -replace "\s+", " ").Trim()
    Log ("    out='{0}'" -f $flat)
    if ($e.Trim().Length -gt 0) { Log ("    err='{0}'" -f (($e -replace "\s+", " ").Trim())) }
    return $flat
}

function Result([string]$s) {
    $m = [regex]::Match($s, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return "?"
}

try {
    Log ""
    Log "### state ###"
    [void](Run-Flctl "status" "0")

    Log ""
    Log "### battery ###"
    # fpswin blocks INSIDE the game, so it is one remote call for the whole window rather than
    # hundreds of polled ones (polling from outside drags the game down to ~4 logic fps).
    $game.Refresh(); $cpu0 = $game.TotalProcessorTime.TotalSeconds
    $t0 = Get-Date
    $rFps = Run-Flctl "fpswin" ("{0}" -f $Window)
    $t1 = Get-Date
    $game.Refresh(); $cpu1 = $game.TotalProcessorTime.TotalSeconds

    $fps100 = Result $rFps
    $wall = ($t1 - $t0).TotalSeconds
    if ($fps100 -ne "?") { Log ("    => render {0:N2} fps" -f ($fps100 / 100.0)) }
    if ($wall -gt 0) {
        Log ("    => game CPU {0:N2} s / {1:N2} s wall = {2} core(s) busy" -f ($cpu1 - $cpu0), $wall, [math]::Round(($cpu1 - $cpu0) / $wall, 2))
    }

    $rClk = Run-Flctl "clockrate" ("{0}" -f $ClockFrames)
    Log ("    => clock per logic frame = {0} (x1000 ms)" -f (Result $rClk))

    $rMs = Run-Flctl "read" "0xCE176C"
    Log ("    => msPerClientFrame global 0xCE176C = {0}  (must stay 33)" -f (Result $rMs))
    $rClock = Run-Flctl "read" "0xCE1388"
    Log ("    => game clock 0xCE1388 = {0}" -f (Result $rClock))
    $rLf = Run-Flctl "logicframe" "0"
    Log ("    => logic frame = {0}" -f (Result $rLf))
    $rFr = Run-Flctl "frames" "0"
    Log ("    => render frame = {0}" -f (Result $rFr))
    $rEng = Run-Flctl "enginefps" "0"
    Log ("    => engine's own fps estimate x100 = {0}" -f (Result $rEng))

    try {
        $load = (Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1).LoadPercentage
        Log ("    => machine CPU load now = {0}%" -f $load)
    } catch { Log "    => machine CPU load unavailable" }

    Log ""
    Log "### newest DLL log for this pid ###"
    $log = Join-Path $logDir ("framelab-{0}-{1}.log" -f (Get-Date).ToString("yyyyMMdd"), $GamePid)
    if (Test-Path $log) { Log ("file: {0} ({1} bytes)" -f $log, (Get-Item $log).Length) }
    else { Log ("no log at {0}" -f $log) }
} catch {
    Log ("EXCEPTION: {0}" -f $_)
} finally {
    Flush
}

exit 0
