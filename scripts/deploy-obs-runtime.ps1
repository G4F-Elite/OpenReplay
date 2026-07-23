param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$version = '32.1.2'
$archiveName = "OBS-Studio-$version-Windows-x64.zip"
$archiveUrl = "https://github.com/obsproject/obs-studio/releases/download/$version/$archiveName"
$expectedHash = '8d97e4563bd8d22d03e63042aa7dccede1d555c9bd35ce8a9e5019b0d0201bf6'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$cache = Join-Path $repository 'artifacts\cache\obs'
$archive = Join-Path $cache $archiveName
$extracted = Join-Path $cache "OBS-Studio-$version-Windows-x64"
$output = Join-Path $repository "artifacts\bin\x64\$Configuration"
$runtime = Join-Path $output 'obs-runtime'

if (-not (Test-Path -LiteralPath $output)) {
    throw "Build output does not exist: $output"
}
New-Item -ItemType Directory -Path $cache -Force | Out-Null

$validArchive = Test-Path -LiteralPath $archive
if ($validArchive) {
    $validArchive = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedHash
}
if (-not $validArchive) {
    $download = "$archive.download"
    Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue
    & curl.exe -L --fail --retry 3 --output $download $archiveUrl
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $actualHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        Remove-Item -LiteralPath $download -Force
        throw "OBS archive SHA-256 mismatch: $actualHash"
    }
    Move-Item -LiteralPath $download -Destination $archive -Force
}

if (-not (Test-Path -LiteralPath (Join-Path $extracted 'bin\64bit\obs.dll'))) {
    Remove-Item -LiteralPath $extracted -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $archive -DestinationPath $extracted
}

Remove-Item -LiteralPath $runtime -Recurse -Force -ErrorAction SilentlyContinue
$binaryTarget = Join-Path $runtime 'bin\64bit'
$pluginTarget = Join-Path $runtime 'obs-plugins\64bit'
$dataTarget = Join-Path $runtime 'data\obs-plugins'
New-Item -ItemType Directory -Path $binaryTarget -Force | Out-Null
New-Item -ItemType Directory -Path $pluginTarget -Force | Out-Null
New-Item -ItemType Directory -Path $dataTarget -Force | Out-Null

$binaryFiles = @(
    'avcodec-61.dll',
    'avdevice-61.dll',
    'avfilter-10.dll',
    'avformat-61.dll',
    'avutil-59.dll',
    'libcurl.dll',
    'libobs-d3d11.dll',
    'libobs-winrt.dll',
    'librist.dll',
    'libx264-164.dll',
    'obs.dll',
    'srt.dll',
    'swresample-5.dll',
    'swscale-8.dll',
    'w32-pthreads.dll',
    'zlib.dll'
)
$helperFiles = @(
    'obs-ffmpeg-mux.exe',
    'obs-nvenc-test.exe',
    'obs-amf-test.exe',
    'obs-qsv-test.exe'
)
$plugins = @('win-capture', 'win-wasapi', 'obs-ffmpeg', 'obs-nvenc', 'obs-qsv11', 'obs-x264')

foreach ($file in $binaryFiles) {
    Copy-Item -LiteralPath (Join-Path $extracted "bin\64bit\$file") -Destination $binaryTarget
}
foreach ($file in $helperFiles) {
    Copy-Item -LiteralPath (Join-Path $extracted "bin\64bit\$file") -Destination $output -Force
}
foreach ($plugin in $plugins) {
    Copy-Item -LiteralPath (Join-Path $extracted "obs-plugins\64bit\$plugin.dll") -Destination $pluginTarget
    Copy-Item -LiteralPath (Join-Path $extracted "data\obs-plugins\$plugin") -Destination $dataTarget -Recurse
}
Copy-Item -LiteralPath (Join-Path $extracted 'data\libobs') -Destination (Join-Path $runtime 'data') -Recurse

# Older development builds copied OBS binaries into the application directory,
# where they could collide with WinUI runtime dependencies.
foreach ($file in $binaryFiles) {
    Remove-Item -LiteralPath (Join-Path $output $file) -Force -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath (Join-Path $output 'obs-plugins') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $output 'data\libobs') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $output 'data\obs-plugins') -Recurse -Force -ErrorAction SilentlyContinue

@"
OBS Studio runtime $version
Source: https://github.com/obsproject/obs-studio/tree/$version
License: GNU General Public License v2.0 or later
License text: https://github.com/obsproject/obs-studio/blob/$version/COPYING
Archive SHA-256: $expectedHash
"@ | Set-Content -LiteralPath (Join-Path $runtime 'NOTICE.txt') -Encoding ASCII

"Deployed OBS Studio $version runtime to $runtime"
