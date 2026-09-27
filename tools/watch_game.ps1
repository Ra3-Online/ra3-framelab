# watch_game.ps1 -- observe a running game process and record HOW it ended.
#
# 2026-09-18 / batch 11 (Claude).
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# WHY THIS EXISTS
#   The black box (src/framelab.cpp, namespace bb) records crashes from INSIDE the process, but it
#   can only speak about what it saw. It cannot tell you whether the process then actually died.
#   Two facts are needed to answer "did the game crash, or was the exception swallowed?":
#
#     1. in-process: was there a first-chance exception, and where?   <- the black box
#     2. out-of-process: did the process exit, when, and with what code?  <- this script
#
#   A Windows process killed by an unhandled exception exits with the exception code as its exit
#   code (0xC0000005 access violation, 0xC0000094 integer divide by zero, ...). A clean quit is 0.
#   So the exit code is the tie-breaker: a crash-*.log WITHOUT a matching non-zero exit code is a
#   first-chance exception the game handled itself (normal, and common -- see the header of every
#   crash report). A non-zero exit code WITHOUT a crash-*.log means we missed the moment.
#
# WHAT IT DOES NOT DO
#   It never kills anything and never writes to the game. It is a pure observer, because several
#   sessions share this machine and a game may belong to the user. Read-only by construction.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\watch_game.ps1                 # newest instance
#   powershell -ExecutionPolicy Bypass -File tools\watch_game.ps1 -Pid 12345
#   powershell -ExecutionPolicy Bypass -File tools\watch_game.ps1 -Minutes 30 -DumpEvery 120
param(
    [int]$Pid = 0,
    [int]$Minutes = 20,            # give up watching after this long
    [int]$PollSeconds = 5,
    [int]$DumpEvery = 0,           # >0: every N seconds ask the black box for a scene snapshot
    [string]$Out = "G:\Ra3 FrameLab\build\logs\_watch.txt"
)

$ErrorActionPreference = "Stop"

# Sandbox: PATHEXT is narrowed to ".CPL", so PowerShell treats every .exe as a document and
# refuses to run it from a pipeline. Restore the standard list (idempotent on a normal machine).
if ($env:PATHEXT -notmatch '\.EXE') {
    $env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"
}

$lab   = Split-Path $PSScriptRoot -Parent
$build = Join-Path $lab "build"
$flctl = Join-Path $build "flctl.exe"
$logDir = Join-Path $build "logs"

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s; $lines | Out-File -Encoding utf8 $Out }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

# flctl returns its status as "RESULT=<n>" in stdout. Same helper shape as the other scripts.
function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if (-not (Test-Path $flctl)) { return -1 }
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

# ---- pick the target ------------------------------------------------------------------------
if ($Pid -le 0) {
    $p = Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue |
         Sort-Object StartTime -Descending | Select-Object -First 1
    if (-not $p) { Write-Host "no ra3_1.12.game process found"; exit 2 }
    $Pid = $p.Id
}
$proc = Get-Process -Id $Pid -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "pid $Pid is not running"; exit 2 }

$startedAt = $proc.StartTime
Log ("=== watch_game  pid={0}  started {1:yyyy-MM-dd HH:mm:ss} ===" -f $Pid, $startedAt)
Log ("polling every {0}s, giving up after {1} min" -f $PollSeconds, $Minutes)

# Snapshot the crash reports that already exist for this pid, so we only ever report NEW ones.
$seenCrash = @(Get-ChildItem $logDir -Filter ("crash-" + $Pid + "-*.log") -ErrorAction SilentlyContinue |
               ForEach-Object { $_.FullName })
Log ("existing crash reports for this pid: {0}" -f $seenCrash.Count)

# Ask the black box what it thinks its state is, so the log records whether it was even armed.
$bbState = Invoke-Flctl "blackboxtest" "" $Pid
Log ("black box self-test rc = {0}  (0 = VEH live and writing)" -f $bbState)

$deadline = (Get-Date).AddMinutes($Minutes)
$lastDump = Get-Date
$exitCode = $null
$endedAt = $null

try {
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds $PollSeconds
        $proc.Refresh()
        if ($proc.HasExited) {
            $endedAt = Get-Date
            # ExitCode is only meaningful after the process object has observed the exit.
            $exitCode = $proc.ExitCode
            break
        }
        if ($DumpEvery -gt 0 -and ((Get-Date) - $lastDump).TotalSeconds -ge $DumpEvery) {
            # Periodic scene snapshots: if the process dies in a way that leaves no report, the
            # most recent snapshot is still a scene from just before the event.
            $frame = Invoke-Flctl "logicframe" "" $Pid
            Log ("  [{0:HH:mm:ss}] alive, logic frame {1}, snapshot requested" -f (Get-Date), $frame)
            [void](Invoke-Flctl "blackboxdump" "" $Pid)
            $lastDump = Get-Date
        }
    }
} catch {
    Log ("exception while watching: {0}" -f $_)
}

Log ""
Log "================ VERDICT ================"
if ($null -eq $exitCode) {
    Log ("still running after {0} min -- no verdict (this is not a failure, just no data)" -f $Minutes)
} else {
    Log ("process exited at {0:yyyy-MM-dd HH:mm:ss} (ran {1:hh\:mm\:ss})" -f $endedAt, ($endedAt - $startedAt))
    Log ("exit code = 0x{0:X8}  ({1})" -f $exitCode, $exitCode)
    if ($exitCode -eq 0) {
        Log "  -> clean exit (normal quit, or the game closed itself). Not a crash."
    } elseif ($exitCode -eq 0xC0000005) {
        Log "  -> ACCESS VIOLATION. Real crash. Look for a crash-*.log for this pid."
    } elseif ($exitCode -eq 0xC000001D) {
        Log "  -> ILLEGAL INSTRUCTION. Real crash."
    } elseif ($exitCode -eq 0xC0000094) {
        Log "  -> INTEGER DIVIDE BY ZERO. Real crash. Check any ratio we compute (r = fps/logicFps)."
    } elseif ($exitCode -eq 0xC0000096) {
        Log "  -> PRIVILEGED INSTRUCTION. Usually means we redirected an operand to a bad address."
    } elseif ($exitCode -eq 0x80000003) {
        Log "  -> BREAKPOINT. Something called DebugBreak / an assert fired."
    } else {
        Log "  -> not a standard clean exit; treat as a crash and read the report below."
    }
    Log ""
    Log "new crash reports for this pid:"
    $new = @(Get-ChildItem $logDir -Filter ("crash-" + $Pid + "-*.log") -ErrorAction SilentlyContinue |
             Where-Object { $seenCrash -notcontains $_.FullName })
    if ($new.Count -eq 0) {
        Log "  (none) -- either no first-chance exception was raised, or the black box was not armed."
        Log "  Reminder: a first-chance exception WITHOUT a non-zero exit code is normal and means"
        Log "  the game handled it. A non-zero exit code WITHOUT a report means we missed it."
    } else {
        foreach ($f in $new) { Log ("  {0}  ({1:N0} bytes, {2:HH:mm:ss})" -f $f.FullName, $f.Length, $f.LastWriteTime) }
    }
}
Flush
exit 0
