# Offline configuration tests. Does not launch, attach to, or require RA3.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$repo = Get-FlabRoot
$fixture = Join-Path $repo ('build\portability-tests\' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path (Join-Path $fixture 'game\Data') -Force)
[void](New-Item -ItemType Directory -Path (Join-Path $fixture 'replays') -Force)
$game = Join-Path $fixture 'game'
$image = Join-Path $game 'Data\ra3_1.12.game'
$sku = Join-Path $game 'RA3_example_1.12.SkuDef'
$replay = Join-Path $fixture 'replays\test.RA3Replay'
[IO.File]::WriteAllText($image, 'offline fixture')
[IO.File]::WriteAllText($sku, 'offline fixture')
[IO.File]::WriteAllText($replay, 'offline fixture')

function Check([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL $Name" }
    Write-Host "PASS $Name"
}
function Check-Throws([scriptblock]$Action, [string]$Name) {
    $thrown = $false
    try { & $Action | Out-Null } catch { $thrown = $true }
    Check $thrown $Name
}

$saved = @{}
foreach ($name in @('FLAB_GAME_ROOT', 'FLAB_IMAGE', 'FLAB_SKUDEF', 'FLAB_REPLAY_DIR', 'FLAB_BUILD_DIR', 'FLAB_PYTHON')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
try {
    Check ((Get-FlabRoot) -eq (Split-Path $PSScriptRoot -Parent)) 'root comes from helper location'
    Check ((Get-FlabBuildDirectory) -eq (Join-Path $repo 'build')) 'default build directory'
    $env:FLAB_BUILD_DIR = Join-Path $fixture 'alternate build'
    Check ((Get-FlabBuildDirectory) -eq $env:FLAB_BUILD_DIR) 'environment build directory'
    Check ((Get-FlabOutputPath '' 'build\logs\test.txt') -eq (Join-Path $env:FLAB_BUILD_DIR 'logs\test.txt')) 'default output follows custom build directory'
    $env:FLAB_BUILD_DIR = 'build\relative-config-test'
    Push-Location $fixture
    try {
        Check ((Get-FlabBuildDirectory) -eq (Join-Path $repo 'build\relative-config-test')) 'relative build override is repo-based from an external working directory'
    } finally { Pop-Location }
    $env:FLAB_BUILD_DIR = $null
    Check-Throws { Get-FlabLaunchConfig } 'missing game root fails explicitly'
    $config = Get-FlabLaunchConfig -GameRoot $game -ReplayDir (Split-Path $replay -Parent)
    Check ($config.Image -eq $image) 'default executable under selected game root'
    Check ($config.SkuDef -eq $sku) 'language-independent SkuDef discovery'
    Check ((Get-FlabReplay 'test' $config.ReplayDir).FullName -eq $replay) 'explicit replay base name'
    Check ((Get-FlabReplay $replay $config.ReplayDir).FullName -eq $replay) 'explicit full replay path'
    Check-Throws { Get-FlabReplay '' $config.ReplayDir } 'no automatic replay selection'
    Check-Throws { Get-FlabReplay 'missing' $config.ReplayDir } 'missing requested replay does not fall back'
    $env:FLAB_GAME_ROOT = $game
    $env:FLAB_REPLAY_DIR = $config.ReplayDir
    Check ((Get-FlabLaunchConfig).GameRoot -eq $game) 'environment game root'
    $secondSku = Join-Path $game 'RA3_other_1.12.SkuDef'
    [IO.File]::WriteAllText($secondSku, 'offline fixture')
    Check-Throws { Get-FlabLaunchConfig } 'ambiguous SkuDef requires an explicit selection'
    $env:FLAB_SKUDEF = [IO.Path]::GetFileName($sku)
    Check ((Get-FlabLaunchConfig).SkuDef -eq $sku) 'explicit environment SkuDef overrides discovery'
    $override = Join-Path $fixture 'alternate.game'
    [IO.File]::WriteAllText($override, 'alternate fixture')
    $env:FLAB_IMAGE = $override
    Check ((Get-FlabLaunchConfig).Image -eq $override) 'explicit image environment override'
    Check ((Get-FlabLaunchConfig -Image $image).Image -eq $image) 'parameter overrides environment image'
    $copy = Join-Path $config.ReplayDir (New-FlabReplayName 'offline test')
    Copy-FlabReplay $replay $copy
    Check ((Get-FileHash -LiteralPath $copy).Hash -eq (Get-FileHash -LiteralPath $replay).Hash) 'replay copy remains identical'
    Check-Throws { Copy-FlabReplay $replay $copy } 'existing replay copy is never overwritten'
    $output = Get-FlabOutputPath (Join-Path $fixture 'output\result.txt') 'unused'
    Check (Test-Path -LiteralPath (Split-Path $output -Parent) -PathType Container) 'output parent is created'
    $runA = Get-FlabRunDirectory (Join-Path $fixture 'runs') 'unused'
    $runB = Get-FlabRunDirectory (Join-Path $fixture 'runs') 'unused'
    Check ($runA -ne $runB) 'each capture run uses a fresh output directory'
    $python = Get-FlabPython
    $argumentProbe = Join-Path $fixture 'python-arguments.py'
    [IO.File]::WriteAllText($argumentProbe, "import sys; print('|'.join(sys.argv[1:]))")
    $probeOutput = Invoke-FlabPython $python $argumentProbe --out 'path with spaces'
    Check ($probeOutput -eq '--out|path with spaces') 'Python forwarding preserves option names and spaced arguments'
    $env:FLAB_PYTHON = $python.Command
    Check ((Get-FlabPython).Command -eq $python.Command) 'Python executable environment override'
    Check-Throws { Get-FlabPython (Join-Path $fixture 'missing-python.exe') } 'missing explicit Python does not silently change runtimes'
    $badParses = @()
    Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File | ForEach-Object {
        $tokens = $null; $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($_.FullName, [ref]$tokens, [ref]$errors)
        if ($errors) { $badParses += $_.Name }
    }
    Check ($badParses.Count -eq 0) 'PowerShell parser accepts every top-level tool'
    $movedTools = Join-Path $fixture 'relocated checkout\tools'
    [void](New-Item -ItemType Directory -Path $movedTools -Force)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'common.ps1') -Destination $movedTools
    . (Join-Path $movedTools 'common.ps1')
    Check ((Get-FlabRoot) -eq (Split-Path $movedTools -Parent)) 'helper works after moving the checkout'
    Write-Host ('Offline configuration tests passed; fixtures: ' + $fixture)
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
