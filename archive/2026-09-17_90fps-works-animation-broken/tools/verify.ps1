# verify.ps1 -- automated A/B verification for Ra3FrameLab.
#
# 2026-09-16 / session 68ee9b9d (Claude)
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
#
# What it does, twice (baseline then patched), with the SAME instrument in both arms:
#   1. launch the game windowed 800x450 playing a replay (does NOT touch the user's own game)
#   2. inject Ra3FrameLab.dll into THAT pid only
#   3. baseline arm -> FrameLabMeasureOnly (frame counter, fps unchanged)
#      patched  arm -> FrameLabEnable <fps>
#   4. sample render-frame counter + engine logic-frame counter over N seconds
#      -> render fps AND logic fps. Logic fps must stay 15 in BOTH arms: that is the
#         "game speed did not change" judge, measured with the engine's own counter.
#   5. arm the engine's own desync dump at the SAME ABSOLUTE logic frame in both arms
#   6. collect the dump files, compare arm A vs arm B byte for byte
#
# Discipline baked in:
#   - only ever touches the pid it launched itself (never the user's running game)
#   - snapshots the game Data dir first and removes ONLY DESYNC files that appear afterwards
#     (never deletes anything that was already there -- that folder is shared with other sessions)
#   - restores the two poked switches before killing the process
#   - the replay copy is named _flab_play (other sessions use _simre_play / _racol_play)
param(
    [int]$Fps = 90,
    [int]$SampleSeconds = 20,
    [int]$DumpFrame = 900,
    [string]$Replay = "",
    [int]$Groups = 0,          # 0 = all groups; see FL_G_* in framelab.cpp
    [switch]$BaselineOnly,
    [switch]$SkipDesync
)

$ErrorActionPreference = "Stop"
$lab      = Split-Path $PSScriptRoot -Parent
$build    = Join-Path $lab "build"
$flctl    = Join-Path $build "flctl.exe"
$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe  = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef   = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$dataDir  = Join-Path $gameRoot "Data"
$replayDir= Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playName = "_flab_play.RA3Replay"
$outRoot  = Join-Path $build "verify"

foreach ($p in @($flctl, $gameExe, $skudef, $replayDir)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

# ---- replay: copy to an ASCII name in the standard Replays dir (engine ignores other dirs) ----
if ($Replay -eq "") {
    $src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
           Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
           Sort-Object Length -Descending | Select-Object -First 1
    if (-not $src) { throw "no usable replay found in $replayDir" }
} else {
    $src = Get-Item (Join-Path $replayDir $Replay)
}
$playPath = Join-Path $replayDir $playName
Copy-Item $src.FullName $playPath -Force
$replaySha = (Get-FileHash $playPath -Algorithm SHA1).Hash
Write-Host ("replay basis: {0}  {1} bytes  sha1 {2}" -f $src.Name, $src.Length, $replaySha)

function Invoke-Flctl([string]$cmd, [string]$arg, [int]$pid_) {
    if ($arg -eq "" -or $null -eq $arg) { $arg = "0" }   # empty args get eaten by PS5.1 native call
    $out = & $flctl $cmd $arg $pid_ 2>&1 | Out-String
    $m = [regex]::Match($out, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return -1
}

function Run-Arm([string]$arm, [bool]$patched) {
    Write-Host ""
    Write-Host ("==== arm: {0} ====" -f $arm)
    $before = @{}
    Get-ChildItem $dataDir -Filter "DESYNC-*" -ErrorAction SilentlyContinue | ForEach-Object { $before[$_.Name] = $true }
    Write-Host ("pre-existing DESYNC files in game Data dir: {0} (these are NOT ours, will not be touched)" -f $before.Count)

    # .game has no shell association, so Start-Process (ShellExecute) fails -> create the process directly
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = '-win -xres 800 -yres 450 -replayGame "' + $playPath + '" -config "' + $skudef + '"'
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("launched pid {0}" -f $proc.Id)
    Start-Sleep -Seconds 12          # let it load; injecting too early hits a half-built process

    # inject (status also injects) and wait until the sim is actually advancing
    [void](Invoke-Flctl "status" "" $proc.Id)
    $frame = -1
    for ($i = 0; $i -lt 40; $i++) {
        $frame = Invoke-Flctl "logicframe" "" $proc.Id
        if ($frame -gt 0 -and $frame -lt 100000000) { break }
        Start-Sleep -Seconds 2
    }
    if ($frame -le 0 -or $frame -ge 100000000) {
        Write-Host "FAIL: replay never started advancing (logic frame stayed $frame)"
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        return $null
    }
    Write-Host ("sim is advancing, logic frame = {0}" -f $frame)

    if ($patched) {
        if ($Groups -ne 0) {
            $g = Invoke-Flctl "groups" "$Groups" $proc.Id
            Write-Host ("group mask set to 0x{0:X3}" -f $g)
        }
        $rc = Invoke-Flctl "enable" "$Fps" $proc.Id
    } else { $rc = Invoke-Flctl "measure" "" $proc.Id }
    Write-Host ("install returned {0} (0 = ok)" -f $rc)
    if ($rc -ne 0) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; return $null }

    # ---- sample: render frames from our counter, logic frames from the engine's own counter ----
    Start-Sleep -Seconds 2
    $r0 = Invoke-Flctl "frames" "" $proc.Id
    $l0 = Invoke-Flctl "logicframe" "" $proc.Id
    $t0 = Get-Date
    Start-Sleep -Seconds $SampleSeconds
    $r1 = Invoke-Flctl "frames" "" $proc.Id
    $l1 = Invoke-Flctl "logicframe" "" $proc.Id
    $elapsed = ((Get-Date) - $t0).TotalSeconds
    $renderFps = [math]::Round(($r1 - $r0) / $elapsed, 2)
    $logicFps  = [math]::Round(($l1 - $l0) / $elapsed, 2)
    Write-Host ("measured over {0:N1}s : render {1} fps ({2} frames) | logic {3} fps ({4} frames)" -f `
                $elapsed, $renderFps, ($r1 - $r0), $logicFps, ($l1 - $l0))

    $dumpDir = $null
    if (-not $SkipDesync) {
        $cur = Invoke-Flctl "logicframe" "" $proc.Id
        if ($DumpFrame -le $cur) {
            Write-Host ("SKIP desync: target frame {0} already passed (now {1})" -f $DumpFrame, $cur)
        } else {
            $rcD = Invoke-Flctl "desync" "$DumpFrame" $proc.Id
            Write-Host ("desync armed at absolute frame {0}, rc {1}" -f $DumpFrame, $rcD)
            for ($i = 0; $i -lt 90; $i++) {
                Start-Sleep -Seconds 2
                if ($proc.HasExited) { break }
                $c = Invoke-Flctl "logicframe" "" $proc.Id
                if ($c -lt 0 -or $c -gt $DumpFrame) { break }
            }
            Start-Sleep -Seconds 5
            $new = Get-ChildItem $dataDir -Filter "DESYNC-*" -ErrorAction SilentlyContinue |
                   Where-Object { -not $before.ContainsKey($_.Name) }
            Write-Host ("desync files produced by us: {0}" -f $new.Count)
            if ($new.Count -gt 0) {
                $dumpDir = Join-Path $outRoot $arm
                New-Item -ItemType Directory -Force $dumpDir | Out-Null
                foreach ($f in $new) { Move-Item $f.FullName (Join-Path $dumpDir $f.Name) -Force }
                Write-Host ("moved into {0} (game Data dir left as we found it)" -f $dumpDir)
            }
            if (-not $proc.HasExited) { [void](Invoke-Flctl "desyncrestore" "" $proc.Id) }
        }
    }

    if (-not $proc.HasExited) {
        [void](Invoke-Flctl "disable" "" $proc.Id)
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 3
    return [pscustomobject]@{ Arm = $arm; RenderFps = $renderFps; LogicFps = $logicFps; DumpDir = $dumpDir }
}

if (Test-Path $outRoot) { Remove-Item $outRoot -Recurse -Force }
$a = Run-Arm "baseline" $false
if ($BaselineOnly) { Write-Host "baseline only, done"; exit 0 }
$armName = "patched-" + $Fps
if ($Groups -ne 0) { $armName = $armName + ("-g{0:X3}" -f $Groups) }
$b = Run-Arm $armName $true

Write-Host ""
Write-Host "================ RESULT ================"
if (-not $a -or -not $b) { Write-Host "one arm failed; see above"; exit 1 }
"{0,-14} render {1,8} fps   logic {2,7} fps" -f $a.Arm, $a.RenderFps, $a.LogicFps
"{0,-14} render {1,8} fps   logic {2,7} fps" -f $b.Arm, $b.RenderFps, $b.LogicFps
$logicDrift = [math]::Abs($b.LogicFps - $a.LogicFps)
Write-Host ("logic fps drift between arms: {0}  (must be ~0: that is 'game speed unchanged')" -f [math]::Round($logicDrift,3))

if ($a.DumpDir -and $b.DumpDir) {
    $fa = Get-ChildItem $a.DumpDir | Sort-Object Name
    $fb = Get-ChildItem $b.DumpDir | Sort-Object Name
    $common = 0; $same = 0; $diff = @()
    foreach ($f in $fa) {
        $other = Join-Path $b.DumpDir $f.Name
        if (Test-Path $other) {
            $common++
            if ((Get-FileHash $f.FullName -Algorithm SHA1).Hash -eq (Get-FileHash $other -Algorithm SHA1).Hash) {
                $same++
            } else { $diff += $f.Name }
        }
    }
    Write-Host ("desync dump: arm A {0} files, arm B {1} files, comparable {2}, identical {3}" -f $fa.Count, $fb.Count, $common, $same)
    if ($diff.Count -gt 0) {
        Write-Host ("DIFFERING FILES ({0}): {1}" -f $diff.Count, ($diff -join ", "))
    }
    Write-Host "NOTE: the dump is the xfer (save-serialization) subset of sim state, NOT the whole sim."
    Write-Host "      All-identical FALSIFIES 'we changed the simulation'; it does not PROVE bit-equality."
} else {
    Write-Host "desync dump: not collected"
}
Remove-Item $playPath -Force -ErrorAction SilentlyContinue
