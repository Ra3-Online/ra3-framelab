# Shared, machine-independent configuration for the optional developer probes.
# Dot-sourcing this file only defines helpers; it does not launch or attach to a game.

function Get-FlabRoot {
    return [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
}

function Get-FlabBuildDirectory {
    $directory = $env:FLAB_BUILD_DIR
    if ([string]::IsNullOrWhiteSpace($directory)) { $directory = Join-Path (Get-FlabRoot) 'build' }
    elseif (-not [IO.Path]::IsPathRooted($directory)) { $directory = Join-Path (Get-FlabRoot) $directory }
    return [IO.Path]::GetFullPath($directory)
}

function Get-FlabExistingFile([string]$Path, [string]$Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Description is required." }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Description not found: $Path" }
    return (Get-Item -LiteralPath $Path).FullName
}

function Get-FlabReplayDirectory([string]$ReplayDir = '') {
    if ([string]::IsNullOrWhiteSpace($ReplayDir)) { $ReplayDir = $env:FLAB_REPLAY_DIR }
    if ([string]::IsNullOrWhiteSpace($ReplayDir)) {
        $documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
        if ([string]::IsNullOrWhiteSpace($documents)) {
            throw 'Cannot locate Documents. Pass -ReplayDir or set FLAB_REPLAY_DIR.'
        }
        $ReplayDir = Join-Path $documents 'Red Alert 3\Replays'
    }
    return [IO.Path]::GetFullPath($ReplayDir)
}

function Get-FlabLaunchConfig {
    param([string]$GameRoot = '', [string]$Image = '', [string]$SkuDef = '', [string]$ReplayDir = '')
    if ([string]::IsNullOrWhiteSpace($GameRoot)) { $GameRoot = $env:FLAB_GAME_ROOT }
    if ([string]::IsNullOrWhiteSpace($GameRoot)) {
        throw 'Pass -GameRoot or set FLAB_GAME_ROOT to the game installation directory.'
    }
    if (-not (Test-Path -LiteralPath $GameRoot -PathType Container)) { throw "Game directory not found: $GameRoot" }
    $GameRoot = (Get-Item -LiteralPath $GameRoot).FullName
    if ([string]::IsNullOrWhiteSpace($Image)) { $Image = $env:FLAB_IMAGE }
    if ([string]::IsNullOrWhiteSpace($Image)) { $Image = Join-Path $GameRoot 'Data\ra3_1.12.game' }
    $Image = Get-FlabExistingFile $Image 'RA3 1.12 executable (-Image / FLAB_IMAGE)'
    if ([string]::IsNullOrWhiteSpace($SkuDef)) { $SkuDef = $env:FLAB_SKUDEF }
    if ([string]::IsNullOrWhiteSpace($SkuDef)) {
        $candidates = @(Get-ChildItem -LiteralPath $GameRoot -Filter '*_1.12.SkuDef' -File)
        if ($candidates.Count -ne 1) {
            throw "Expected one *_1.12.SkuDef in $GameRoot; found $($candidates.Count). Pass -SkuDef or set FLAB_SKUDEF."
        }
        $SkuDef = $candidates[0].FullName
    } elseif (-not [IO.Path]::IsPathRooted($SkuDef)) { $SkuDef = Join-Path $GameRoot $SkuDef }
    $SkuDef = Get-FlabExistingFile $SkuDef 'Game SkuDef (-SkuDef / FLAB_SKUDEF)'
    return [pscustomobject]@{ GameRoot = $GameRoot; Image = $Image; SkuDef = $SkuDef; ReplayDir = (Get-FlabReplayDirectory $ReplayDir) }
}

function Get-FlabReplay([string]$Replay, [string]$ReplayDir) {
    if ([string]::IsNullOrWhiteSpace($Replay)) {
        throw 'Pass -Replay with an explicit test replay path or name. Replays are never selected automatically.'
    }
    if (Test-Path -LiteralPath $Replay -PathType Leaf) { return Get-Item -LiteralPath $Replay }
    $candidate = Join-Path $ReplayDir $Replay
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -and [IO.Path]::GetExtension($candidate) -eq '') {
        $candidate += '.RA3Replay'
    }
    $candidate = Get-FlabExistingFile $candidate 'Test replay (-Replay)'
    return Get-Item -LiteralPath $candidate
}

function New-FlabReplayName([string]$Label) {
    $safeLabel = $Label -replace '[^A-Za-z0-9_]', '_'
    return ('_flab_{0}_{1}_{2}.RA3Replay' -f $safeLabel, $PID, [Guid]::NewGuid().ToString('N'))
}

function Copy-FlabReplay([string]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { throw "Refusing to overwrite a replay: $Destination" }
    $parent = Split-Path $Destination -Parent
    [void](New-Item -ItemType Directory -Path $parent -Force)
    Copy-Item -LiteralPath $Source -Destination $Destination
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash) {
        throw 'Test replay copy differs from its source.'
    }
}

function Get-FlabOutputPath {
    param([string]$Path, [string]$DefaultRelative, [switch]$Directory)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        if ($DefaultRelative -match '^build[\\/](.*)$') { $Path = Join-Path (Get-FlabBuildDirectory) $Matches[1] }
        else { $Path = Join-Path (Get-FlabRoot) $DefaultRelative }
    }
    $Path = [IO.Path]::GetFullPath($Path)
    $parent = if ($Directory) { $Path } else { Split-Path $Path -Parent }
    [void](New-Item -ItemType Directory -Path $parent -Force)
    return $Path
}

function Get-FlabRunDirectory([string]$Path, [string]$DefaultRelative) {
    $parent = Get-FlabOutputPath $Path $DefaultRelative -Directory
    $name = ('{0}_{1}_{2}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $PID, [Guid]::NewGuid().ToString('N'))
    return Get-FlabOutputPath (Join-Path $parent $name) '' -Directory
}

function Get-FlabPython([string]$Python = '') {
    if ([string]::IsNullOrWhiteSpace($Python)) { $Python = $env:FLAB_PYTHON }
    if (-not [string]::IsNullOrWhiteSpace($Python)) {
        $command = Get-Command $Python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $command) { throw "Python executable not found: $Python" }
        return [pscustomobject]@{ Command = $command.Source; Prefix = @() }
    }
    foreach ($name in @('python', 'python3', 'py')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) {
            $prefix = @(); if ($name -eq 'py') { $prefix = @('-3') }
            return [pscustomobject]@{ Command = $command.Source; Prefix = $prefix }
        }
    }
    throw 'Python 3 not found. Put python/py on PATH or pass -Python / set FLAB_PYTHON.'
}

function Invoke-FlabPython {
    param([object]$Runtime, [Parameter(ValueFromRemainingArguments=$true)][object[]]$Arguments)
    $prefix = @($Runtime.Prefix)
    & $Runtime.Command @prefix @Arguments
}

function Assert-FlabGameProcess([int]$TargetPid) {
    if ($TargetPid -le 0) { throw 'An explicit game PID is required. Pass -GamePid (or -Pid for watch_game.ps1).' }
    $process = Get-Process -Id $TargetPid -ErrorAction Stop
    if ($process.ProcessName -ine 'ra3_1.12.game') { throw "PID $TargetPid is not ra3_1.12.game." }
    return $process
}
