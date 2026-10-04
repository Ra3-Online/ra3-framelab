# Portable x86 MSVC build. Requires Visual Studio C++ tools and a Windows SDK.
# Optional: FLAB_BUILD_DIR, FLAB_PYTHON, FLAB_IMAGE, FLAB_VSWHERE, FLAB_VSDEVCMD.
# Keep this script ASCII so Windows PowerShell 5.1 reads it without a BOM.
[CmdletBinding()]
param(
    [ValidateSet('test', 'loader', 'contract', 'ctl', 'dll', 'gui', 'all')]
    [string]$Target = 'all',
    [string]$BuildDir = $env:FLAB_BUILD_DIR,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $root 'build' }
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$out = [IO.Path]::GetFullPath($BuildDir)
$logDir = Join-Path $out 'logs'
$tmpDir = Join-Path $out 'tmp'
@($out, $logDir, $tmpDir) | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
$utf8 = New-Object Text.UTF8Encoding($true)
$savedEnv = @{}

function Set-BuildEnvironment([string]$Name, [string]$Value) {
    if (-not $savedEnv.ContainsKey($Name)) { $savedEnv[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process') }
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}

function Find-VsDevCmd {
    if ($env:FLAB_VSDEVCMD) {
        if (-not (Test-Path -LiteralPath $env:FLAB_VSDEVCMD -PathType Leaf)) { throw 'FLAB_VSDEVCMD does not name an existing batch file.' }
        return (Resolve-Path -LiteralPath $env:FLAB_VSDEVCMD).Path
    }
    $vswhere = $env:FLAB_VSWHERE
    if (-not $vswhere) {
        $command = Get-Command vswhere.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $vswhere = $command.Path }
        else { $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }
    }
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found. Install Visual Studio C++ Build Tools, or set FLAB_VSWHERE / FLAB_VSDEVCMD.'
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'No Visual Studio installation with x86/x64 C++ tools was found.' }
    $devCmd = Join-Path ([string]($installation | Select-Object -First 1)) 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) { throw "VsDevCmd.bat was not found: $devCmd" }
    return $devCmd
}

function Import-VsEnvironment([string]$DevCmd) {
    # The installation path is passed through the environment; the batch file remains ASCII.
    Set-BuildEnvironment 'FLAB_SELECTED_VSDEVCMD' $DevCmd
    $batch = Join-Path $out '_msvc_environment.cmd'
    [IO.File]::WriteAllText($batch, "@echo off`r`ncall `"%FLAB_SELECTED_VSDEVCMD%`" -no_logo -arch=x86 -host_arch=x64 >nul`r`nif errorlevel 1 exit /b %errorlevel%`r`nset`r`n", [Text.Encoding]::ASCII)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /u /s /c ""' + $batch + '""'
    $start.WorkingDirectory = $out
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::Unicode
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    [void]$process.Start()
    $errors = $process.StandardError.ReadToEndAsync()
    $environmentText = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    $errorText = $errors.Result
    if ($process.ExitCode -ne 0) { throw "VsDevCmd failed ($($process.ExitCode)): $errorText" }
    foreach ($line in ($environmentText -split "`r?`n")) {
        if ($line -match '^([^=]+)=(.*)$') { Set-BuildEnvironment $Matches[1] $Matches[2] }
    }
    $process.Dispose()
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x86') { throw 'The discovered MSVC environment is not targeting x86.' }
    foreach ($name in @('cl.exe', 'link.exe', 'rc.exe')) {
        if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { throw "Missing tool in the selected environment: $name" }
    }
    Write-Host "MSVC $env:VCToolsVersion, Windows SDK $env:WindowsSDKVersion, target x86"
}

function Find-Python {
    if ($env:FLAB_PYTHON) {
        $command = Get-Command $env:FLAB_PYTHON -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Path }
        throw 'FLAB_PYTHON must name a Python executable available on PATH or an existing executable path.'
    }
    foreach ($name in @('python.exe', 'python3.exe', 'python', 'python3')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Path }
    }
    throw 'Python 3 is required for the export and portable dependency checks. Add it to PATH or set FLAB_PYTHON.'
}

function Invoke-BuildTool([string]$Exe, [string[]]$ToolArgs, [string]$LogName, [string]$Artifact = '') {
    $before = if ($Artifact -and (Test-Path -LiteralPath $Artifact)) { (Get-Item -LiteralPath $Artifact).LastWriteTimeUtc } else { [datetime]::MinValue }
    $logFile = Join-Path $logDir $LogName
    # Native stderr is diagnostic text, not a PowerShell terminating error.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& $Exe @ToolArgs 2>&1); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previousPreference }
    [IO.File]::WriteAllLines($logFile, [string[]]@($lines | ForEach-Object { $_.ToString() }), $utf8)
    if ($code -ne 0) {
        $lines | ForEach-Object { Write-Host $_ }
        throw "$Exe failed (exit $code). See $logFile"
    }
    $lines | Where-Object { $_.ToString() -match 'warning|error|FAIL' } | ForEach-Object { Write-Host $_ }
    if ($Artifact) {
        if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) { throw "No artifact produced: $Artifact" }
        $file = Get-Item -LiteralPath $Artifact
        if ($file.LastWriteTimeUtc -le $before) { throw "Artifact was not refreshed: $Artifact" }
        Write-Host ("built: {0} ({1} bytes)" -f $Artifact, $file.Length)
    } else { Write-Host "passed: $LogName" }
}

$common = @('/nologo', '/EHsc', '/W4', '/O2', '/MT', '/utf-8')
function Build-Executable([string]$Name, [string]$Source, [string[]]$LinkArgs = @()) {
    $artifact = Join-Path $out $Name
    Invoke-BuildTool 'cl.exe' ($common + @("/Fe:$artifact", $Source, '/link', '/MACHINE:X86', '/INCREMENTAL:NO') + $LinkArgs) ($Name + '.build.txt') $artifact
}

function Build-Dll {
    $artifact = Join-Path $out 'Ra3FrameLab.dll'
    Invoke-BuildTool 'cl.exe' ($common + @('/LD', "/Fe:$artifact", (Join-Path $root 'src\framelab.cpp'), '/link', '/MACHINE:X86', '/INCREMENTAL:NO', ('/DEF:' + (Join-Path $root 'src\framelab.def')), 'kernel32.lib', 'user32.lib')) 'dll.build.txt' $artifact
    Invoke-BuildTool $python @('-B', (Join-Path $root 'tools\re\check_exports.py'), $artifact, (Join-Path $root 'src\framelab.def')) 'exports.check.txt'
    if ($env:FLAB_IMAGE) {
        $image = $env:FLAB_IMAGE
        if (-not [IO.Path]::IsPathRooted($image)) { $image = Join-Path $root $image }
        if (-not (Test-Path -LiteralPath $image -PathType Leaf)) { throw 'FLAB_IMAGE was set but the game image does not exist.' }
        Invoke-BuildTool $python @('-B', (Join-Path $root 'tools\re\dryscan.py'), (Join-Path $root 'src\framelab.cpp'), $image) 'signatures.check.txt'
    } else {
        Write-Host 'Signature scan NOT RUN: set FLAB_IMAGE to your own supported RA3 1.12 .game image for offline signature validation.'
    }
}

function Build-Gui {
    # Stage both inputs beside the resource script so an overridden build directory is respected.
    $stage = Join-Path $out 'gui-resources'
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    foreach ($name in @('ra3fps_gui.rc', 'ra3fps_gui.manifest', 'gui_res.h')) {
        Copy-Item -LiteralPath (Join-Path $root ("tools\gui\$name")) -Destination (Join-Path $stage $name) -Force
    }
    Copy-Item -LiteralPath (Join-Path $out 'Ra3FrameLab.dll') -Destination (Join-Path $stage 'Ra3FrameLab.dll') -Force
    $resource = Join-Path $out 'ra3fps_gui.res'
    Push-Location -LiteralPath $stage
    try { Invoke-BuildTool 'rc.exe' @('/nologo', '/c65001', '/fo', $resource, 'ra3fps_gui.rc') 'gui.resources.txt' $resource }
    finally { Pop-Location }
    $artifact = Join-Path $out 'Ra3FpsTest.exe'
    Invoke-BuildTool 'cl.exe' ($common + @('/DUNICODE', '/D_UNICODE', "/Fe:$artifact", (Join-Path $root 'tools\gui\ra3fps_gui.cpp'), $resource, '/link', '/MACHINE:X86', '/INCREMENTAL:NO', '/SUBSYSTEM:WINDOWS')) 'gui.build.txt' $artifact
    Invoke-BuildTool $python @('-B', (Join-Path $root 'tools\re\check_deps.py'), $artifact, (Join-Path $out 'Ra3FrameLab.dll')) 'portable.check.txt'
}

try {
    Set-BuildEnvironment 'TEMP' $tmpDir
    Set-BuildEnvironment 'TMP' $tmpDir
    Set-BuildEnvironment 'PYTHONIOENCODING' 'utf-8'
    Set-BuildEnvironment 'PYTHONUTF8' '1'
    Set-BuildEnvironment 'PYTHONDONTWRITEBYTECODE' '1'
    Import-VsEnvironment (Find-VsDevCmd)
    $python = if ($Target -in @('dll', 'gui', 'all')) { Find-Python } else { $null }
    Push-Location -LiteralPath $out
    try {
        if ($Target -in @('test', 'all')) { Build-Executable 'test_schedule.exe' (Join-Path $root 'tests\test_schedule.cpp') }
        if ($Target -in @('loader', 'all')) { Build-Executable 'test_loader.exe' (Join-Path $root 'tests\test_loader.cpp') }
        if ($Target -in @('contract', 'all')) {
            # Only this test owns a bounded, non-executable IMAGE data fixture.
            # Its canonical fake page is checked inside that declared array before use.
            Build-Executable 'test_sim_contract.exe' (Join-Path $root 'tests\test_sim_contract.cpp') @('/BASE:0x00400000', '/DYNAMICBASE:NO', '/SECTION:.flfix,RW', '/MANIFEST:EMBED', "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
        }
        if ($Target -in @('ctl', 'all')) { Build-Executable 'flctl.exe' (Join-Path $root 'tools\flctl.cpp') @('advapi32.lib') }
        if ($Target -in @('dll', 'gui', 'all')) { Build-Dll }
        if ($Target -in @('gui', 'all')) { Build-Gui }
        if ($RunTests) {
            foreach ($name in @('test_schedule.exe', 'test_loader.exe', 'test_sim_contract.exe')) {
                $test = Join-Path $out $name
                if (-not (Test-Path -LiteralPath $test -PathType Leaf)) { throw "-RunTests requires $name; build -Target all first." }
                Invoke-BuildTool $test @() ($name + '.run.txt')
            }
        }
    } finally { Pop-Location }
    Write-Host "Build complete. Output: $out"
} finally {
    foreach ($name in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
}
