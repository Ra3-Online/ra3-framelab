# read_series.ps1 -- sample a list of game globals repeatedly and log (wall ms, value).
#
# 2026-09-17 / animation-locating session (Claude)
# ASCII only: PowerShell 5.1 misreads BOM-less UTF-8 Chinese source.
#
# Why: the engine paces itself with a handful of globals in sub_5FE9D0, and reading them one
# value at a time tells you nothing -- a countdown and a timestamp are only interpretable as a
# TIME SERIES. This samples them together with a wall clock so the pattern (period, reset value,
# monotonic vs sawtooth) becomes visible.
#
# The globals of interest, all from sub_5FE9D0 (historical notes archived separately; summary in docs/TECHNICAL.md):
#   0x00CAFF64  dword_CAFF64  -- countdown; while > 1 the tick returns early (skips the 29 ms
#                                spin-wait and sub_5FD350). Reloaded from vtbl160(0x00CDB7B4).
#   0x00CE1AF0  dword_CE1AF0  -- spin-wait baseline; the loop waits until timeGetTime()-this >= 29
#   0x00CE1AF4  dword_CE1AF4  -- "baseline initialised" flag (bit0)
#   0x00CE1370  dword_CE1370  -- gate for sub_5FD350 (<=0 means call it)
#   0x00CAFDDC  dword_CAFDDC  -- last client frame number seen by the tick (for v5 = elapsed)
#   0x00CE1388  game clock (ms)         0x00CE176C  ms-per-client-frame (must stay 33)
#   0x00CDB750  TheGameClient pointer   0x00CDB7B4  the object whose vtbl160 drives the countdown
#
# SAFETY: the pid is required and nothing is ever launched or killed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\read_series.ps1 -GamePid 22580 -Count 40
param(
    [Parameter(Mandatory=$true)][int]$GamePid,
    [int]$Count = 40,
    [int]$IntervalMs = 90,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$Out = Get-FlabOutputPath $Out "build\logs\_series.txt"

$flctl = Join-Path (Get-FlabBuildDirectory) "flctl.exe"

$watch = @(
    @{ n = "CAFF64"; a = "0xCAFF64" },
    @{ n = "CE1AF0"; a = "0xCE1AF0" },
    @{ n = "CE1AF4"; a = "0xCE1AF4" },
    @{ n = "CE1370"; a = "0xCE1370" },
    @{ n = "CAFDDC"; a = "0xCAFDDC" },
    @{ n = "CE1388"; a = "0xCE1388" },
    @{ n = "CDB750"; a = "0xCDB750" },
    @{ n = "CDB7B4"; a = "0xCDB7B4" }
)

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

$game = Assert-FlabGameProcess $GamePid
if (-not $game) { Log "no process with that id"; Flush; exit 2 }

function Read-One([string]$addr) {
    $si = New-Object System.Diagnostics.ProcessStartInfo
    $si.FileName = $flctl
    $si.Arguments = ('read {0} {1}' -f $addr, $GamePid)
    $si.UseShellExecute = $false
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    $si.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($si)
    $o = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $m = [regex]::Match($o, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return $null
}

Log ("=== read_series pid={0} count={1} interval={2}ms ===" -f $GamePid, $Count, $IntervalMs)
Log ("columns: wallMs " + (($watch | ForEach-Object { $_.n }) -join " "))

$t0 = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
    $vals = @()
    foreach ($w in $watch) {
        $v = Read-One $w.a
        if ($null -eq $v) { $vals += "?" } else { $vals += [string]$v }
    }
    Log ("{0,8} {1}" -f $t0.ElapsedMilliseconds, ($vals -join " "))
    Flush
    Start-Sleep -Milliseconds $IntervalMs
}

Log ("total wall ms = {0}" -f $t0.ElapsedMilliseconds)
Flush
exit 0
