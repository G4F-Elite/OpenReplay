param(
    [string]$Version,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$versionHeader = Join-Path $repository 'src\Shared\include\openreplay\Version.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw
$match = [regex]::Match($versionText, 'kVersion\{"(?<version>\d+\.\d+\.\d+)"\}')
if (-not $match.Success) { throw 'Unable to read kVersion from Version.h.' }
$sourceVersion = $match.Groups['version'].Value
if (-not $Version) { $Version = $sourceVersion }
if ($Version -ne $sourceVersion) { throw "Requested version $Version does not match Version.h ($sourceVersion)." }

if (-not $SkipBuild) {
    & "$PSScriptRoot\build.ps1" Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$output = Join-Path $repository 'artifacts\bin\x64\Release'
$packageRoot = Join-Path $repository 'artifacts\package'
$stage = Join-Path $packageRoot "OpenReplay-$Version-win-x64"
$archive = Join-Path $packageRoot "OpenReplay-$Version-win-x64.zip"
if (-not (Test-Path -LiteralPath (Join-Path $output 'OpenReplay.App.exe'))) {
    throw "Release output does not exist: $output"
}

Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $output '*') -Destination $stage -Recurse -Force

$excludedExtensions = @('.pdb', '.ilk', '.exp', '.lib')
Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension.ToLowerInvariant() -in $excludedExtensions
} | Remove-Item -Force
Remove-Item -LiteralPath (Join-Path $stage 'OpenReplay.UnitTests.exe') -Force -ErrorAction SilentlyContinue

$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
$nugetRoot = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget\packages' }
$windowsAppSdk = Join-Path $nugetRoot 'microsoft.windowsappsdk\1.8.260529003'
$cppWinRt = Join-Path $nugetRoot 'microsoft.windows.cppwinrt\2.0.250303.1'
if (-not (Test-Path -LiteralPath (Join-Path $windowsAppSdk 'NOTICE.txt')) -or
    -not (Test-Path -LiteralPath (Join-Path $cppWinRt 'LICENSE'))) {
    throw "Restored NuGet licenses were not found under $nugetRoot"
}
Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $licenses 'OpenReplay-GPL-2.0-or-later.txt')
Copy-Item -LiteralPath (Join-Path $repository 'THIRD-PARTY-NOTICES.md') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repository 'src\App\Assets\PublicSans-OFL.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $windowsAppSdk 'NOTICE.txt') -Destination (Join-Path $licenses 'WindowsAppSDK-NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $windowsAppSdk 'license.txt') -Destination (Join-Path $licenses 'WindowsAppSDK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $cppWinRt 'LICENSE') -Destination (Join-Path $licenses 'CppWinRT-LICENSE.txt')

$commit = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { 'local' }
@"
OpenReplay $Version
Repository: https://github.com/G4F-Elite/OpenReplay
Commit: $commit
Platform: Windows x64

Run OpenReplay.App.exe. No OBS Studio installation is required.
"@ | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8

$required = @(
    'OpenReplay.App.exe',
    'OpenReplay.Host.exe',
    'OpenReplay.Updater.exe',
    'OpenReplay.App.pri',
    'App.xbf',
    'MainWindow.xbf',
    'obs-ffmpeg-mux.exe',
    'obs-runtime\bin\64bit\obs.dll',
    'obs-runtime\obs-plugins\64bit\win-capture.dll',
    'obs-runtime\obs-plugins\64bit\win-wasapi.dll',
    'obs-runtime\NOTICE.txt',
    'Assets\PublicSans-Variable.ttf'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $relative))) {
        throw "Release package is missing required file: $relative"
    }
}

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path -Leaf $archive)" | Set-Content -LiteralPath "$archive.sha256" -Encoding ASCII
"Created $archive"
