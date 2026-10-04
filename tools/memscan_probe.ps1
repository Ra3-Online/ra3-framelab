# memscan_probe.ps1 -- find "moving units" by scanning writable memory for coordinate triples.
#
# Why: the user reports vehicles jitter up/down WHILE MOVING (steady when standing still),
# and says vanilla is slight while the patched build is clearly worse. So the problem is on
# the POSITION path -- but the project has no instrument that reads a unit's world
# coordinates, and the three quantities we CAN read (frac / logic advance / game clock) are
# bit-identical to vanilla at 30 fps.
#
# `flctl memscan` enumerates writable memory, keeps three-adjacent-float triples that look
# like world coordinates, sleeps 1 s, re-reads them, and prints only the ones that MOVED.
# It finds the moving units WITHOUT reversing the object graph.
#
# We call it 3 times a few seconds apart, because a unit has to actually be moving at the
# moment of the scan for it to show up.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\memscan_probe.ps1

param(
    [int]$Fps = 30,
    [int]$Groups = 0x2CFFF,     # the GUI's default set (includes FL_G_ANIMPROBE)
    [int]$StartupWait = 45,
    [int]$Settle = 20,
    [int]$Rounds = 3,
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$OutDir = "",
    [string]$GameRoot = "",
    [string]$Image = "",
    [string]$SkuDef = "",
    [string]$ReplayDir = "",
    [string]$Replay = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$OutDir = Get-FlabOutputPath $OutDir "build\logs" -Directory

# Preserve caller extensions and restore process environment even on early failure.
$probeOriginalPathExt = [Environment]::GetEnvironmentVariable('PATHEXT', 'Process')
try {
if ([string]::IsNullOrWhiteSpace($probeOriginalPathExt)) {
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
} elseif (($probeOriginalPathExt -split ';' | ForEach-Object { $_.Trim() }) -notcontains '.EXE') {
    $env:PATHEXT = $probeOriginalPathExt + ';.EXE'
}
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$lab = Get-FlabRoot
$build = Get-FlabBuildDirectory
$flctl     = Join-Path $build "flctl.exe"
$flConfig = Get-FlabLaunchConfig -GameRoot $GameRoot -Image $Image -SkuDef $SkuDef -ReplayDir $ReplayDir
$gameRoot = $flConfig.GameRoot
$gameExe = $flConfig.Image
$skudef = $flConfig.SkuDef
$replayDir = $flConfig.ReplayDir
$playPath = Join-Path $replayDir (New-FlabReplayName "memscan_probe")

if (-not (Test-Path $flctl)) { Write-Host "missing $flctl -- build first"; exit 2 }

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Write-Host "REFUSE: a game instance is already running (RA3 is single-instance)."
    Write-Host "        Not touching it -- it may belong to the user or another session."
    exit 3
}

$src = Get-FlabReplay -Replay $Replay -ReplayDir $replayDir
Copy-FlabReplay -Source $src.FullName -Destination $playPath
Write-Host ("replay: {0}  ({1:N0} bytes)" -f $src.Name, $src.Length)

function Invoke-FlctlRaw([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    return (& $flctl $cmd $arg $targetPid 2>&1 | Out-String)
}

$proc = $null
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false

    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("launched pid {0}, waiting {1}s for boot" -f $proc.Id, $StartupWait)
    Start-Sleep -Seconds $StartupWait
    if ($proc.HasExited) { Write-Host "game exited during startup"; exit 4 }

    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { }

    [void](Invoke-FlctlRaw "groups" ("{0}" -f $Groups) $proc.Id)
    [void](Invoke-FlctlRaw "enable" ("{0}" -f $Fps) $proc.Id)

    $frame = -1
    for ($i = 0; $i -lt 30; $i++) {
        $o = Invoke-FlctlRaw "logicframe" "" $proc.Id
        $m = [regex]::Match($o, 'RESULT=(-?\d+)')
        if ($m.Success) { $frame = [int64]$m.Groups[1].Value }
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if ($frame -le 0) { Write-Host "sim never advanced -- giving up"; exit 5 }
    Write-Host ("sim running at logic frame {0}" -f $frame)
    Start-Sleep -Seconds $Settle

    $all = @()
    for ($r = 1; $r -le $Rounds; $r++) {
        if ($proc.HasExited) { Write-Host "game exited before round $r"; break }
        Write-Host ""
        Write-Host ("================ memscan round {0} ================" -f $r)
        $o = Invoke-FlctlRaw "memscan" "" $proc.Id
        Write-Host $o
        $all += ("=== round $r ===`n" + $o)
        Start-Sleep -Seconds 3
    }

    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $txt = Join-Path $OutDir ("_memscan_{0}.txt" -f $stamp)
    ($all -join "`n") | Set-Content -Path $txt -Encoding UTF8
    Write-Host ("[written] {0}" -f $txt)
}
finally {
    # Only ever kill the process we started ourselves. Never call `flctl disable`.
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $playPath -Force -ErrorAction SilentlyContinue
}
} finally {
    [Environment]::SetEnvironmentVariable('PATHEXT', $probeOriginalPathExt, 'Process')
}
