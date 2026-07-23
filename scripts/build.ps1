$Configuration = if ($args.Count -gt 0) { $args[0] } else { 'Debug' }
if ($Configuration -notin @('Debug', 'Release')) {
    throw 'Configuration must be Debug or Release.'
}

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = if (Test-Path -LiteralPath $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

$msbuild = if ($installation) {
    Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
} else {
    $candidatePaths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            $candidatePath
            break
        }
    }
}
if (-not $msbuild) { throw 'Visual Studio 2022 with the C++ toolchain is required.' }

& $msbuild "$PSScriptRoot\..\OpenReplay.sln" /restore /m /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\deploy-obs-runtime.ps1" -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\..\artifacts\bin\x64\$Configuration\OpenReplay.UnitTests.exe"
exit $LASTEXITCODE
