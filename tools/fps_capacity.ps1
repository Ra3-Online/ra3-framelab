# fps_capacity.ps1 -- decide WHY the achieved frame rate falls short of the target.
#
# 2026-09-17 / animation-locating session (Claude)
# ASCII only: PowerShell 5.1 misreads BOM-less UTF-8 Chinese source.
#
# THE QUESTION
#   With the fixed config (groups 0xC1FF) and target 90, this box renders ~72 fps.
#   Two competing explanations:
#     (H1) CPU ceiling    -- the renderer needs ~13.9 ms of work per frame, so no target
#                            above ~72 fps can be reached. Then target 60 still reaches
#                            ~58..60, and target 120 still reaches ~72.
#     (H2) ratio artifact -- the patch scales the achieved rate to ~80% of whatever target
#                            is set. Then target 60 -> ~48 and target 120 -> ~96.
#   The 60 row separates them on its own; 120 is confirmation.
#
# WHY A FRESH GAME PER TARGET
#   RA3 is single-instance, and fl::install() refuses to re-enter (FL_ERR_ALREADY), so a new
#   target frame rate means a new process. Same reason look.ps1/animrate_ab.ps1 restart.
#
# WHAT ELSE IS RECORDED (all of it matters for the verdict)
#   - fpswin N        in-process render-frame counter, returns fps*100
#   - clockrate N     in-process game-clock advance per logic frame, *1000 (main criterion)
#   - read 0xCE176C   the engine's ms-per-client-frame global: must stay 33 (quarantined)
#   - enginefps       the ENGINE's own fps estimate (independent second opinion)
#   - game CPU seconds / wall seconds during the window: ~1.0 means one core fully busy,
#     which is what "CPU bound" has to look like from the outside
#   - machine CPU load before/after, so a loaded box can be told apart from a slow patch
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\fps_capacity.ps1
param(
    [int[]]$Targets = @(90, 60, 120),
    [int]$Groups = 0xC1FF,
    [int]$StartupWait = 45,
    [int]$Window = 8,
    [int]$Width = 1024,
    [int]$Height = 576,
    [string]$Out = "G:\Ra3 FrameLab\build\logs\_fpscap.txt"
)

$ErrorActionPreference = "Stop"
$lab      = Split-Path $PSScriptRoot -Parent
$build    = Join-Path $lab "build"
$flctl    = Join-Path $build "flctl.exe"
$logDir   = Join-Path $build "logs"
$gameRoot = "C:\Users\Mithlan\Documents\Tencent Files\873194676\Red Alert 3\Red Alert 3"
$gameExe  = Join-Path $gameRoot "Data\RA3_1.12.game"
$skudef   = Join-Path $gameRoot "RA3_chinese_t_1.12.SkuDef"
$replayDir = Join-Path $env:USERPROFILE "Documents\Red Alert 3\Replays"
$playPath  = Join-Path $replayDir "_flab_cap.RA3Replay"

$lines = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { $lines.Add($s) | Out-Null; Write-Host $s }
function Flush() { $lines | Out-File -Encoding utf8 $Out }

# One flctl call. Returns the raw stdout+stderr with whitespace collapsed, so the caller can
# regex RESULT= out of it. Kept identical to look.ps1 on purpose: same measuring stick.
function Invoke-Flctl([string]$cmd, [string]$arg, [int]$targetPid) {
    $si = New-Object System.Diagnostics.ProcessStartInfo
    $si.FileName = $flctl
    $si.Arguments = ('{0} {1} {2}' -f $cmd, $arg, $targetPid)
    $si.UseShellExecute = $false
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    $si.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($si)
    $o = $p.StandardOutput.ReadToEnd()
    $e = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return (($o + " " + $e) -replace "\s+", " ").Trim()
}

# Pull the machine-readable RESULT=<n> line out of an flctl reply. Returns "?" when absent
# (that is a DIFFERENT outcome from a legitimate 0, so it must not be flattened to 0).
function Result([string]$s) {
    $m = [regex]::Match($s, 'RESULT=(-?\d+)')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return "?"
}

function MachineLoad() {
    try {
        $p = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
        return [int]$p.LoadPercentage
    } catch { return -1 }
}

Log ("=== fps_capacity  groups=0x{0:X4} targets={1} window={2}s ===" -f $Groups, ($Targets -join ","), $Window)

if (Get-Process -Name "ra3_1.12.game" -ErrorAction SilentlyContinue) {
    Log "REFUSE: a game instance is already running (RA3 is single-instance, and it may be someone else's)"
    Flush; exit 2
}

$src = Get-ChildItem $replayDir -Filter "*.RA3Replay" |
       Where-Object { $_.Name -notlike "_*" -and $_.Name -notlike "*ra3battle.net*" } |
       Sort-Object Length -Descending | Select-Object -First 1
if (-not $src) { Log "no usable replay found"; Flush; exit 2 }
Copy-Item $src.FullName $playPath -Force
Log ("replay: {0}" -f $src.Name)

$summary = New-Object System.Collections.Generic.List[object]

foreach ($fps in $Targets) {
    Log ""
    Log ("################ TARGET {0} fps ################" -f $fps)
    Flush

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gameExe
    $psi.Arguments = ('-win -xres {0} -yres {1} -replayGame "{2}" -config "{3}"' -f $Width, $Height, $playPath, $skudef)
    $psi.WorkingDirectory = $gameRoot
    $psi.UseShellExecute = $false
    $game = [System.Diagnostics.Process]::Start($psi)
    $gamePid = $game.Id
    Log ("launched pid {0} ({1}x{2} windowed)" -f $gamePid, $Width, $Height)
    Flush

    $row = [ordered]@{ target = $fps; pid = $gamePid; fps100 = "?"; clock = "?"; mspf = "?"; engine = "?"; cpuPerWall = "?"; loadBefore = -1; loadAfter = -1 }
    try {
        Start-Sleep -Seconds $StartupWait
        $game.Refresh()
        if ($game.HasExited) { Log "FAIL: game exited during startup"; continue }

        # Bring it to the front so the renderer is not throttled by being backgrounded.
        try {
            Add-Type -AssemblyName Microsoft.VisualBasic
            [void][Microsoft.VisualBasic.Interaction]::AppActivate($gamePid)
        } catch { }

        Log ("groups  : " + (Invoke-Flctl "groups" ("{0}" -f $Groups) $gamePid))
        Log ("enable  : " + (Invoke-Flctl "enable" ("{0}" -f $fps) $gamePid))

        $row.loadBefore = MachineLoad
        $game.Refresh(); $cpu0 = $game.TotalProcessorTime.TotalSeconds
        $t0 = Get-Date

        # --- the one measurement that matters: in-process render frame counter ---
        $rFps = Invoke-Flctl "fpswin" ("{0}" -f $Window) $gamePid
        Log ("fpswin  : " + $rFps)
        $row.fps100 = Result $rFps

        # --- main criterion: game-clock advance per logic frame (must stay ~66667) ---
        $rClk = Invoke-Flctl "clockrate" "60" $gamePid
        Log ("clockrate: " + $rClk)
        $row.clock = Result $rClk

        # --- the quarantined global must still read 33 even at target 90/120 ---
        $rMs = Invoke-Flctl "read" "0xCE176C" $gamePid
        Log ("mspf    : " + $rMs)
        $row.mspf = Result $rMs

        # NOTE: the arg must NOT be empty. flctl reads argv[2] as the fps and argv[3] as the pid,
        # so an empty argv[2] would shift the pid out of argv[3] and make flctl fall back to
        # "find the game by process name" -- i.e. it could touch someone else's session.
        $rEng = Invoke-Flctl "enginefps" "0" $gamePid
        Log ("enginefps: " + $rEng)
        $row.engine = Result $rEng

        $t1 = Get-Date
        $game.Refresh(); $cpu1 = $game.TotalProcessorTime.TotalSeconds
        $row.loadAfter = MachineLoad
        $wall = ($t1 - $t0).TotalSeconds
        if ($wall -gt 0) {
            $row.cpuPerWall = [math]::Round(($cpu1 - $cpu0) / $wall, 2)
            Log ("game CPU: {0:N2} s over {1:N2} s wall  =>  {2} core(s) busy" -f ($cpu1 - $cpu0), $wall, $row.cpuPerWall)
        }
        Log ("machine load: {0}% -> {1}%" -f $row.loadBefore, $row.loadAfter)
    } catch {
        Log ("EXCEPTION: {0}" -f $_)
    } finally {
        $game.Refresh()
        if (-not $game.HasExited) { Stop-Process -Id $gamePid -Force; Log "game killed (patches die with the process)" }
        else { Log "game had already exited" }
        $summary.Add([pscustomobject]$row)
        Flush
        Start-Sleep -Seconds 3   # let the box settle before the next target
    }
}

Log ""
Log "=== VERDICT TABLE ==="
Log ("{0,8} {1,10} {2,10} {3,9} {4,10} {5,9}" -f "target", "render", "clock/1lf", "msPerFrm", "enginefps", "cpu/wall")
foreach ($r in $summary) {
    $f = if ($r.fps100 -eq "?") { "?" } else { "{0:N2}" -f ($r.fps100 / 100.0) }
    Log ("{0,8} {1,10} {2,10} {3,9} {4,10} {5,9}" -f $r.target, $f, $r.clock, $r.mspf, $r.engine, $r.cpuPerWall)
}
Log ""
Log "READ IT LIKE THIS"
Log "  render ~ constant across 90/120, and ~=target at 60  => H1 CPU ceiling (machine is the limit)"
Log "  render ~= 0.8 * target at every row                  => H2 ratio artifact (patch defect)"
Log "  clock/1lf must stay ~66000-66700 in every row        => the animation fix holds regardless"
Log "  msPerFrm must stay 33 in every row                   => the writer quarantine holds"
Flush

exit 0
