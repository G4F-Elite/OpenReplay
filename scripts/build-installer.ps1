param(
    [string]$Version,
    [switch]$SkipPackage,
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$versionHeader = Join-Path $repository 'src\Shared\include\openreplay\Version.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw
$match = [regex]::Match($versionText, 'kVersion\{"(?<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)"\}')
if (-not $match.Success) { throw 'Unable to read kVersion from Version.h.' }
$sourceVersion = $match.Groups['version'].Value
if (-not $Version) { $Version = $sourceVersion }
if ($Version -ne $sourceVersion) { throw "Requested version $Version does not match Version.h ($sourceVersion)." }

if (-not $SkipPackage) {
    & "$PSScriptRoot\package-release.ps1" -Version $Version
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$packageRoot = Join-Path $repository 'artifacts\package'
$stage = Join-Path $packageRoot "OpenReplay-$Version-win-x64"
if (-not (Test-Path -LiteralPath (Join-Path $stage 'OpenReplay.App.exe'))) {
    throw "Portable release stage does not exist: $stage"
}

$compilerCandidates = @(
    $Compiler,
    $env:ISCC_PATH,
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
) | Where-Object { $_ }
if (-not $Compiler) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $compilerCandidates += $command.Source }
}
$iscc = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) {
    throw 'Inno Setup 6 is required. Install JRSoftware.InnoSetup or pass -Compiler/ISCC_PATH.'
}

$installer = Join-Path $packageRoot "OpenReplay-$Version-win-x64-installer.exe"
Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$installer.sha256" -Force -ErrorAction SilentlyContinue

& $iscc "/DAppVersion=$Version" "/DSourceDir=$stage" "/DOutputDir=$packageRoot" `
    (Join-Path $repository 'installer\OpenReplay.iss')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Test-Path -LiteralPath $installer)) { throw "Installer was not created: $installer" }

$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path -Leaf $installer)" | Set-Content -LiteralPath "$installer.sha256" -Encoding ASCII
"Created $installer"
