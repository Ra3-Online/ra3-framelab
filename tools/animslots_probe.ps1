# animslots_probe.ps1 -- dump the animation-slot table TWICE, ~1 s apart, and print both.
#
# Why this exists (2026-09-18, batch 11 follow-up):
#   The black box records only the FIRST 4 animation slots, and measurement showed all 4
#   held STATIC objects (slot 0 constant at 4.0164, slots 1/2 constant at 0, across a full
#   512-frame window). But the "cut animation" hook runs ~24 times/second, so something IS
#   animating -- it just lives in a later slot. `flctl animslots` dumps all 12 slots.
#
#   A single dump only shows "who has ever been registered". The slot whose "current"
#   column MOVES between two dumps is the object that is actually animating -- that owner
#   is what we should watch next when hunting the vehicle bob.
#
# It launches the game, enables the patch with FL_G_ANIMPROBE, waits for the sim to run,
# dumps the slots twice, then closes ONLY the game process it started itself.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\animslots_probe.ps1
#   powershell -ExecutionPolicy Bypass -File tools\animslots_probe.ps1 -Fps 30 -Settle 30

param(
    [int]$Fps = 30,
    # Must include FL_G_ANIMPROBE (0x4000) or there is no probe and no slots at all.
    # 0x2CFFF = the GUI's default set = FL_G_ALL | FL_G_ANIMPROBE.
    [int]$Groups = 0x2CFFF,
    [int]$StartupWait = 45,
    [int]$Settle = 30,
    [int]$Width = 1024,
    [int]$Height = 576,
    # * -Replay: use this replay instead of auto-picking the largest one.
    #   Needed to sample animation scores from the user's dedicated test replay
    #   ("90 FPS Test.RA3Replay") while vehicles are actually driving.
    [string]$Replay = "",
    [string]$OutDir = "G:\Ra3 FrameLab\build\logs"
)

# The sandbox narrows PATHEXT to ".CPL", which makes every .exe look like a document and
# refuses to run it inside a pipeline. Restore it before calling anything.
$env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"

# flctl writes its table in UTF-8. Without this, PowerShell decodes that stream using the
# console codepage (cp936 on this machine) and the Chinese labels come out as mojibake --
# the numbers survive, the labels do not. Setting both of these covers the interactive and
# the redirected case. (If it still garbles, the numbers are still readable and correct.)
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

$lab       = "G:\Ra3 FrameLab"
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe   = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
# Our own copy name -- other sessions use _flab_play / _flabre_play / _raco_play / _simre_play
# / _flabjit_play, and clobbering theirs would corrupt their runs.
$playPath  = Join-Path $replayDir "_flabanim_play.RA3Replay"

if (-not (Test-Path $flctl)) { Write-Host "missing $flctl -- build first"; exit 2 }

# Refuse rather than wait. RA3 is single-instance and an existing process may belong to the
# user or to another session. We never touch a process we did not start.
if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Write-Host "REFUSE: a game instance is already running (RA3 is single-instance)."
    Write-Host "        Not touching it -- it may belong to the user or another session."
    exit 3
}

if ($Replay -ne "") {
    if (Test-Path $Replay) { $src = Get-Item $Replay }
    else {
        $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
               Where-Object { $_.Name -eq $Replay -or $_.BaseName -eq $Replay } |
               Select-Object -First 1
    }
    if (-not $src) { Write-Host ("replay not found: {0}" -f $Replay); exit 2 }
} else {
    $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
           Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
           Sort-Object Length -Descending | Select-Object -First 1
}
if (-not $src) { Write-Host "no usable replay found in $replayDir"; exit 2 }
Copy-Item $src.FullName $playPath -Force
Write-Host ("replay: {0}  ({1:N0} bytes)" -f $src.Name, $src.Length)

# Raw call: we need the FULL stdout (the slot table), not just the RESULT= line.
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

    # A covered/occluded window throttles its render loop; we want a normal, focused window.
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    } catch { }

    Write-Host ("groups -> 0x{0:X}" -f $Groups)
    [void](Invoke-FlctlRaw "groups" ("{0}" -f $Groups) $proc.Id)
    $rc = Invoke-FlctlRaw "enable" ("{0}" -f $Fps) $proc.Id
    Write-Host ("enable rc: {0}" -f (($rc -split "`n") | Where-Object { $_ -match "RESULT=" }))

    # Wait for the sim to actually run before trusting anything: a slot table collected
    # while the replay is still loading would be empty for the wrong reason.
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

    Write-Host ("settling {0}s so the scene is steady and objects get registered" -f $Settle)
    Start-Sleep -Seconds $Settle

    if ($proc.HasExited) { Write-Host "game exited before the dump"; exit 6 }

    Write-Host ""
    Write-Host "================ dump 1 ================"
    $d1 = Invoke-FlctlRaw "animslots" "" $proc.Id
    Write-Host $d1

    Start-Sleep -Seconds 1

    Write-Host "================ dump 2 (1 s later) ================"
    $d2 = Invoke-FlctlRaw "animslots" "" $proc.Id
    Write-Host $d2

    Write-Host ""
    Write-Host "Compare the 'current' column. A slot whose value MOVED is the object that is"
    Write-Host "actually animating -- that owner is what we watch next."

    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $txt = Join-Path $OutDir ("_animslots_{0}.txt" -f $stamp)
    ("=== dump 1 ===`n" + $d1 + "`n=== dump 2 ===`n" + $d2) | Set-Content -Path $txt -Encoding UTF8
    Write-Host ("[written] {0}" -f $txt)
}
finally {
    # Only ever kill the process we started ourselves. Never call `flctl disable`:
    # restoring code hooks while threads may be running inside them freezes the game.
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $playPath -Force -ErrorAction SilentlyContinue
}
