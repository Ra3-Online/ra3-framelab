# build.ps1 — Ra3 FrameLab 构建脚本(独立目录,不依赖也不修改其它仓)
# 用法: powershell -ExecutionPolicy Bypass -File "G:\Ra3 FrameLab\tools\build.ps1" [-Target test|dll|all]
# NOTE: keep this file ASCII-only (PowerShell 5.1 misreads BOM-less UTF-8 Chinese).
# Paths contain spaces, so the compile line goes through a generated .bat (CRLF, no BOM) instead of
# nested quoting via `cmd /c` -- nested quotes get eaten and cl.exe just prints its usage banner.
param([string]$Target = "all")

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bt = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = "$bt\VC\Auxiliary\Build\vcvars32.bat"   # 32-bit: the game is x86
if (-not (Test-Path $vcvars)) { throw "vcvars32.bat not found: $vcvars" }

$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null
$bat = Join-Path $out "_build.bat"

function Invoke-Cl([string]$line, [string]$artifact) {
    $lines = @(
        "@echo off",
        "call `"$vcvars`" >nul",
        "cd /d `"$out`"",
        $line,
        "exit /b %ERRORLEVEL%"
    )
    [System.IO.File]::WriteAllText($bat, ($lines -join "`r`n") + "`r`n", (New-Object System.Text.ASCIIEncoding))
    $before = if (Test-Path $artifact) { (Get-Item $artifact).LastWriteTimeUtc } else { [datetime]::MinValue }
    cmd /c "`"$bat`"" | ForEach-Object { if ($_ -match 'error|warning C|LNK') { $_ } }
    if ($LASTEXITCODE -ne 0) { throw "compile failed (exit $LASTEXITCODE)" }
    if (-not (Test-Path $artifact)) { throw "no artifact produced: $artifact" }
    $after = (Get-Item $artifact).LastWriteTimeUtc
    if ($after -le $before) { throw "artifact not refreshed: $artifact" }   # exit code alone is not proof
    Write-Host ("built: {0} ({1} bytes)" -f $artifact, (Get-Item $artifact).Length)
}

if ($Target -eq "test" -or $Target -eq "all") {
    Invoke-Cl "cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:test_schedule.exe `"$root\tests\test_schedule.cpp`"" (Join-Path $out "test_schedule.exe")
}

if ($Target -eq "loader" -or $Target -eq "all") {
    Invoke-Cl "cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:test_loader.exe `"$root\tests\test_loader.cpp`"" (Join-Path $out "test_loader.exe")
}

if ($Target -eq "ctl" -or $Target -eq "all") {
    # flctl.exe must be 32-bit (vcvars32 above) so it can inject into the 32-bit game process.
    Invoke-Cl "cl.exe /nologo /EHsc /W4 /O2 /utf-8 /Fe:flctl.exe `"$root\tools\flctl.cpp`" /link advapi32.lib" (Join-Path $out "flctl.exe")
}

if ($Target -eq "dll" -or $Target -eq "all") {
    $dllSrc = Join-Path $root "src\framelab.cpp"
    if (Test-Path $dllSrc) {
        # /MT: static CRT so the DLL needs no VC++ redistributable on a player's machine.
        # /DEF: clean undecorated export names (plain FrameLabEnable, not _FrameLabEnable@4).
        Invoke-Cl "cl.exe /nologo /EHsc /W4 /O2 /MT /utf-8 /LD /Fe:Ra3FrameLab.dll `"$dllSrc`" /link /DEF:`"$root\src\framelab.def`" kernel32.lib user32.lib" (Join-Path $out "Ra3FrameLab.dll")
    } else {
        Write-Host "skip dll: $dllSrc not written yet"
    }
}
