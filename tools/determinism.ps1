# determinism.ps1 -- does the patch make a lockstep replay desync?
#
# 2026-09-17 / session 68ee9b9d (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# Why this exists: on 2026-09-17 the "animation gate" experiment made the replay pop the engine's
# own DESYNC dialog after ~1 minute. That was the first hard evidence that a patch of ours can
# change the simulation -- and it handed us a ruler we did not have before:
#
#   * replay playback IS a lockstep determinism check. The recorded command stream only replays
#     correctly if our simulation matches the one that recorded it. A desync dialog = falsified.
#
# It is far cheaper than the engine's desync dump (no poking, no 22 MB of files, no cleanup) and
# it runs unattended. It can only FALSIFY "we did not touch the sim" -- surviving N minutes is not
# proof, it is just a failed attempt to falsify. Report it that way.
#
# Detection: poll the engine's own logic frame. A desync dialog blocks the sim, so the frame number
# stops advancing while the process stays alive. Two consecutive stalled checks = desync/stall.
param(
    [int]$Fps = 90,
    [int]$Minutes = 8,
    [int]$Groups = 0,           # 0 = default set
    [switch]$BaselineToo        # also run an unpatched control arm
)

$ErrorActionPreference = "Stop"

# ------ 2026-09-17 (batch 9) sandbox compatibility: restore PATHEXT ------------------------------------------------------------------
# Symptom: every `& $flctl ...` call threw
#     "Cannot run a document in the middle of a pipeline: ...\build\flctl.exe"
# even though the file existed and ran fine from bash. Root cause found by probing:
#     $env:PATHEXT = [.CPL]        <-- the sandbox narrows PATHEXT to just .CPL
# PowerShell decides "is this an executable?" from PATHEXT, so with only .CPL in it,
# every .exe is treated as a *document* and cannot be run from a pipeline. This broke
# determinism.ps1 after it had waited 3h24m for the user's game to exit -- and, worse,
# it aborted *after* launching the game, with no cleanup.
# Fix: restore the standard list. Idempotent on a normal machine (we overwrite with the
# standard value), so it is safe to keep unconditionally. ASCII-only comment on purpose:
# PowerShell 5.1 misreads BOM-less UTF-8 Chinese.
if ($env:PATHEXT -notmatch '\.EXE') {
    $env:PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"
    Write-Host ("note: PATHEXT was missing .EXE; restored to " + $env:PATHEXT)
}

$lab       = Split-Path $PSScriptRoot -Parent
$build     = Join-Path $lab "build"
$flctl     = Join-Path $build "flctl.exe"
$gameRoot  = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe   = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef    = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playPath  = Join-Path $replayDir "_flab_play.RA3Replay"

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }
    $out = & $flctl $cmd $arg $targetPid 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

$src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
       Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
       Sort-Object Length -Descending | Select-Object -First 1
Copy-Item $src.FullName $playPath -Force
if ((Get-FileHash $src.FullName -Algorithm SHA1).Hash -ne (Get-FileHash $playPath -Algorithm SHA1).Hash) {
    throw "replay copy differs from source"
}
Write-Host ("replay: {0}" -f $src.Name)

function Run-Arm([string]$label, [bool]$patched) {
    while (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
        Write-Host "waiting for a game instance that is not ours to exit"
        Start-Sleep -Seconds 10
    }
    Write-Host ""
    Write-Host ("================ {0} ================" -f $label)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres 800 -yres 450 -replayGame "{0}" -config "{1}"' -f $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false

    # 2026-09-17 (batch 9): try/finally so that NO exception path can leave an orphaned game.
    #   Why: the 3h24m run on 2026-09-17 aborted *after* launching the game (every flctl call
    #   threw -- see the PATHEXT block at the top of this file), and the cleanup code sat *after*
    #   the throw point, so the game process was left running.
    #   do NOT call disable here: restoring code hooks while threads may be inside them is what
    #   froze the game on 2026-09-17. Killing the process is safe -- the patch lives only in memory.
    $proc = $null
    try {
        $proc = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Seconds 45
        if ($proc.HasExited) { Write-Host "game exited during startup"; return $null }

        [void](Invoke-Flctl "status" "" $proc.Id)
        $frame = -1
        for ($i = 0; $i -lt 30; $i++) {
            $frame = Invoke-Flctl "logicframe" "" $proc.Id
            if ($frame -gt 0 -and $frame -lt 100000000) { break }
            Start-Sleep -Seconds 2
        }
        if ($frame -le 0) { Write-Host "sim never advanced"; return $null }

        if ($patched) {
            if ($Groups -ne 0) { [void](Invoke-Flctl "groups" "$Groups" $proc.Id) }
            $rc = Invoke-Flctl "enable" "$Fps" $proc.Id
        } else {
            $rc = Invoke-Flctl "measure" "" $proc.Id
        }
        Write-Host ("install rc {0}, starting at logic frame {1}" -f $rc, $frame)
        if ($rc -ne 0) { return $null }

        $deadline = (Get-Date).AddMinutes($Minutes)
        $last = $frame; $stalls = 0; $verdict = "survived"; $stallFrame = -1
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 20
            if ($proc.HasExited) { $verdict = "process exited"; break }
            $now = Invoke-Flctl "logicframe" "" $proc.Id
            if ($now -lt 0) { $verdict = "lost contact with the process"; break }
            if ($now -le $last) {
                $stalls++
                Write-Host ("  logic frame stalled at {0} ({1} in a row)" -f $now, $stalls)
                if ($stalls -ge 2) { $verdict = "DESYNC / STALL"; $stallFrame = $now; break }
            } else {
                $stalls = 0
                Write-Host ("  logic frame {0} (+{1})" -f $now, ($now - $last))
            }
            $last = $now
        }
        $endFrame = if ($proc.HasExited) { $last } else { Invoke-Flctl "logicframe" "" $proc.Id }
        Write-Host ("{0}: {1}  (reached logic frame {2})" -f $label, $verdict, $endFrame)
        return [pscustomobject]@{ Arm = $label; Verdict = $verdict; EndFrame = $endFrame; StallFrame = $stallFrame }
    } finally {
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 4
    }
}

$results = @()
$r = Run-Arm ("patched-" + $Fps) $true
if ($r) { $results += $r }
if ($BaselineToo) {
    $r = Run-Arm "baseline-30" $false
    if ($r) { $results += $r }
}
Write-Host ""
Write-Host "================ RESULT ================"
$results | Format-Table -AutoSize
Write-Host "NOTE: surviving is NOT proof the simulation is untouched -- it is a failed attempt to"
Write-Host "      falsify it. A desync IS proof that it is touched."
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
