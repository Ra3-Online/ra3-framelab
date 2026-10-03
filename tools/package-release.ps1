# Package this research tree as a labeled candidate; nothing is uploaded.
# Run tools/build.ps1 -Target all -RunTests first, or supply -Rebuild.
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?$')]
    [string]$Version = '0.2.1-research-r1',
    [string]$BuildDir = $env:FLAB_BUILD_DIR,
    [string]$PackageDir,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
# The published 0.2.1 name is reserved for the unchanged main/tag baseline.
# Future numeric releases remain available after their source and binary versions are updated.
$numericVersion = [regex]::Match($Version, '^[0-9]+\.[0-9]+\.[0-9]+').Value
if ($numericVersion -eq '0.2.1' -and $Version -notmatch '^0\.2\.1-research-[A-Za-z0-9][A-Za-z0-9.-]*$') {
    throw 'This research tree cannot package the published 0.2.1 label. Use 0.2.1-research-r1 (or another research suffix); build the unchanged main/tag for public 0.2.1.'
}

$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $root 'build' }
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$build = [IO.Path]::GetFullPath($BuildDir)
if ([string]::IsNullOrWhiteSpace($PackageDir)) { $PackageDir = Join-Path $build 'release' }
if (-not [IO.Path]::IsPathRooted($PackageDir)) { $PackageDir = Join-Path $root $PackageDir }
$packages = [IO.Path]::GetFullPath($PackageDir)
New-Item -ItemType Directory -Force -Path $packages | Out-Null
if ($Rebuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Target all -BuildDir $build -RunTests }

$binaryNames = @('Ra3FpsTest.exe', 'Ra3FrameLab.dll', 'flctl.exe', 'test_schedule.exe', 'test_loader.exe')
$documents = @('README.md', 'LICENSE', 'NOTICE.md', 'CHANGELOG.md', 'SETUP.md', 'docs\TECHNICAL.md')
$researchDocument = 'docs\CROSSFPS_RESEARCH.md'
if (Test-Path -LiteralPath (Join-Path $root $researchDocument) -PathType Leaf) { $documents += $researchDocument }
foreach ($name in $binaryNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name) -PathType Leaf)) { throw "Missing build artifact: $name. Build -Target all first." }
}
foreach ($name in $documents) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) { throw "Missing release document: $name" }
}
$guiVersion = (Get-Item -LiteralPath (Join-Path $build 'Ra3FpsTest.exe')).VersionInfo.ProductVersion
if ($guiVersion -ne ($numericVersion + '.0')) { throw "GUI product version $guiVersion does not match release $Version." }

# A release may be packaged from an existing build; validate its actual payload again.
$python = $null
foreach ($name in @($env:FLAB_PYTHON, 'python.exe', 'python3.exe', 'python', 'python3')) {
    if (-not $name) { continue }
    $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $python = $command.Path; break }
    if ($name -eq $env:FLAB_PYTHON) { throw 'FLAB_PYTHON does not name an available executable.' }
}
if (-not $python) { throw 'Python 3 is required to validate the release. Add it to PATH or set FLAB_PYTHON.' }
$previousPythonEncoding = $env:PYTHONIOENCODING
$previousPythonUtf8 = $env:PYTHONUTF8
$previousNoBytecode = $env:PYTHONDONTWRITEBYTECODE
try {
    $env:PYTHONIOENCODING = 'utf-8'
    $env:PYTHONUTF8 = '1'
    $env:PYTHONDONTWRITEBYTECODE = '1'
    & $python -B (Join-Path $root 'tools\re\check_exports.py') (Join-Path $build 'Ra3FrameLab.dll') (Join-Path $root 'src\framelab.def')
    if ($LASTEXITCODE -ne 0) { throw 'Release export validation failed.' }
    & $python -B (Join-Path $root 'tools\re\check_deps.py') (Join-Path $build 'Ra3FpsTest.exe') (Join-Path $build 'Ra3FrameLab.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Release portability validation failed.' }
    Push-Location -LiteralPath $build
    try {
        $loaderOutput = @(& (Join-Path $build 'test_loader.exe') (Join-Path $build 'Ra3FrameLab.dll') 2>&1)
        if ($LASTEXITCODE -ne 0) { throw 'Release DLL offline self-test failed.' }
        $versionPattern = '(?m)^\s*[^=\r\n]*=\s*' + [regex]::Escape($numericVersion) + '\s*$'
        if (($loaderOutput -join "`n") -notmatch $versionPattern) { throw 'Release DLL version does not match the requested release.' }
        New-Item -ItemType Directory -Force -Path (Join-Path $build 'logs') | Out-Null
        [IO.File]::WriteAllLines((Join-Path $build 'logs\release-loader.check.txt'), [string[]]$loaderOutput, (New-Object Text.UTF8Encoding($true)))
        Write-Host "Release GUI/DLL version: $numericVersion; offline DLL self-test passed."
    } finally { Pop-Location }
} finally {
    $env:PYTHONIOENCODING = $previousPythonEncoding
    $env:PYTHONUTF8 = $previousPythonUtf8
    $env:PYTHONDONTWRITEBYTECODE = $previousNoBytecode
}

$stage = Join-Path $packages ('stage-' + [guid]::NewGuid().ToString('N'))
$portable = Join-Path $stage 'portable'
$source = Join-Path $stage 'source'
@($portable, $source) | ForEach-Object { New-Item -ItemType Directory -Path $_ -Force | Out-Null }
function Copy-ReleaseFile([string]$FromRoot, [string]$RelativePath, [string]$ToRoot) {
    $target = Join-Path $ToRoot $RelativePath
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $FromRoot $RelativePath) -Destination $target -Force
}
foreach ($name in $binaryNames) { Copy-ReleaseFile $build $name $portable }
foreach ($name in $documents) {
    Copy-ReleaseFile $root $name $portable
    Copy-ReleaseFile $root $name $source
}
foreach ($name in @('HANDOFF.md', 'HARDCODED-PATHS.md')) {
    if (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf) { Copy-ReleaseFile $root $name $source }
}

# Source whitelist: no archive, game/replay data, local reports, build logs or private sessions.
$extensions = @('.cpp', '.c', '.h', '.hpp', '.def', '.rc', '.manifest', '.ps1', '.py', '.sh', '.md', '.txt', '.ico', '.yml', '.yaml')
foreach ($folder in @('src', 'tests', 'tools', 'docs', '.github')) {
    $folderPath = Join-Path $root $folder
    if (-not (Test-Path -LiteralPath $folderPath -PathType Container)) { continue }
    foreach ($file in (Get-ChildItem -LiteralPath $folderPath -Recurse -File)) {
        if ($file.Extension -notin $extensions -and $file.Name -ne 'CODEOWNERS') { continue }
        $relative = $file.FullName.Substring($root.Length + 1)
        if ($relative -match '(^|[\\/])(__pycache__|cache|\.cache|logs|tmp|build|archive|\.git|private|sessions|memory)([\\/]|$)') { continue }
        if ($file.Name -match '^(_symcache\.txt|\.env(?:\..*)?|.*\.local\..*|.*\.(log|pyc|pdb|obj|exe|dll|game|RA3Replay))$') { continue }
        # Text/data under tools is generated input rather than buildable source.
        if ($folder -eq 'tools' -and $file.Extension -in @('.txt', '.yml', '.yaml')) { continue }
        Copy-ReleaseFile $root $relative $source
    }
}
foreach ($name in @('.gitignore', '.gitattributes')) {
    if (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf) { Copy-ReleaseFile $root $name $source }
}
$binaryHashes = foreach ($name in $binaryNames) {
    $hash = Get-FileHash -LiteralPath (Join-Path $portable $name) -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), $name
}
[IO.File]::WriteAllLines((Join-Path $portable 'SHA256SUMS.txt'), [string[]]$binaryHashes, [Text.Encoding]::ASCII)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$portableZip = Join-Path $packages ("Ra3FrameLab-$Version-windows-x86.zip")
$sourceZip = Join-Path $packages ("Ra3FrameLab-$Version-source.zip")
foreach ($zip in @($portableZip, $sourceZip)) {
    # Only overwrite these exact non-recursive output files under the resolved package directory.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($zip)) -ne $packages.TrimEnd('\', '/')) { throw 'Unsafe archive destination.' }
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
}
[IO.Compression.ZipFile]::CreateFromDirectory($portable, $portableZip, [IO.Compression.CompressionLevel]::Optimal, $false)
[IO.Compression.ZipFile]::CreateFromDirectory($source, $sourceZip, [IO.Compression.CompressionLevel]::Optimal, $false)
$zipHashes = foreach ($zip in @($portableZip, $sourceZip)) {
    $hash = Get-FileHash -LiteralPath $zip -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), [IO.Path]::GetFileName($zip)
}
$checksums = Join-Path $packages 'SHA256SUMS.txt'
[IO.File]::WriteAllLines($checksums, [string[]]$zipHashes, [Text.Encoding]::ASCII)

# The unique staging path was created above, and must stay directly inside this package directory.
$resolvedStage = [IO.Path]::GetFullPath($stage)
if ([IO.Path]::GetDirectoryName($resolvedStage) -ne $packages.TrimEnd('\', '/') -or [IO.Path]::GetFileName($resolvedStage) -notmatch '^stage-[a-f0-9]{32}$') {
    throw 'Unsafe staging cleanup path.'
}
Remove-Item -LiteralPath $resolvedStage -Recurse -Force
Write-Host "Portable release: $portableZip"
Write-Host "Source release:   $sourceZip"
Write-Host "SHA256 manifest:  $checksums"
