param(
    [string]$Version,
    [string]$Installer
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$versionText = Get-Content -LiteralPath (Join-Path $repository 'src\Shared\include\openreplay\Version.h') -Raw
$match = [regex]::Match($versionText, 'kVersion\{"(?<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)"\}')
if (-not $match.Success) { throw 'Unable to read kVersion from Version.h.' }
$sourceVersion = $match.Groups['version'].Value
if (-not $Version) { $Version = $sourceVersion }
if ($Version -ne $sourceVersion) { throw "Requested version $Version does not match Version.h ($sourceVersion)." }

$packageRoot = Join-Path $repository 'artifacts\package'
if (-not $Installer) { $Installer = Join-Path $packageRoot "OpenReplay-$Version-win-x64-installer.exe" }
$Installer = (Resolve-Path $Installer).Path
$portableStage = Join-Path $packageRoot "OpenReplay-$Version-win-x64"
$installRoot = Join-Path $env:TEMP "OpenReplay-installer-smoke-$PID"
$installLog = Join-Path $env:TEMP "OpenReplay-installer-smoke-$PID-install.log"
$uninstallLog = Join-Path $env:TEMP "OpenReplay-installer-smoke-$PID-uninstall.log"
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\G4F-Elite.OpenReplay_is1'
$appPathKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\OpenReplay.App.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\OpenReplay\OpenReplay.lnk'

if (Test-Path -LiteralPath $uninstallKey) { throw 'An OpenReplay installer registration already exists.' }
if ((Get-ItemProperty -LiteralPath $runKey -Name OpenReplay -ErrorAction SilentlyContinue).OpenReplay) {
    throw 'An OpenReplay startup registration already exists.'
}
Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installLog, $uninstallLog -Force -ErrorAction SilentlyContinue

try {
    $installArguments = @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        '/LANG=english',
        "/DIR=`"$installRoot`"",
        "/LOG=`"$installLog`""
    )
    foreach ($attempt in 1..2) {
        $install = Start-Process -FilePath $Installer -ArgumentList $installArguments -Wait -PassThru
        if ($install.ExitCode -ne 0) {
            throw "Installer attempt $attempt exited with code $($install.ExitCode)."
        }
    }

    $required = @(
        'OpenReplay.App.exe',
        'OpenReplay.Host.exe',
        'OpenReplay.Updater.exe',
        'obs-runtime\bin\64bit\obs.dll'
    )
    foreach ($relative in $required) {
        $installed = Join-Path $installRoot "app\$relative"
        if (-not (Test-Path -LiteralPath $installed)) { throw "Installed file is missing: $relative" }
        if (Test-Path -LiteralPath $portableStage) {
            $portable = Join-Path $portableStage $relative
            if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $portable -Algorithm SHA256).Hash) {
                throw "Installed file differs from the portable release: $relative"
            }
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'unins000.exe')) -or
        -not (Test-Path -LiteralPath $uninstallKey) -or
        -not (Test-Path -LiteralPath $appPathKey) -or
        -not (Test-Path -LiteralPath $shortcut)) {
        throw 'Installer registration or Start menu shortcut is missing.'
    }
    $registration = Get-ItemProperty -LiteralPath $uninstallKey
    $appPath = Get-ItemProperty -LiteralPath $appPathKey
    $expectedExecutable = Join-Path $installRoot 'app\OpenReplay.App.exe'
    if ($registration.DisplayVersion -ne $Version -or $appPath.'(default)' -ne $expectedExecutable) {
        throw 'Installer version or App Paths registration is incorrect.'
    }
    Set-ItemProperty -LiteralPath $runKey -Name OpenReplay -Value "`"$expectedExecutable`" --background"

    $uninstallArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$uninstallLog`"")
    $uninstall = Start-Process -FilePath (Join-Path $installRoot 'unins000.exe') `
        -ArgumentList $uninstallArguments -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "Uninstaller exited with code $($uninstall.ExitCode)." }
    Start-Sleep -Seconds 1
    $startup = (Get-ItemProperty -LiteralPath $runKey -Name OpenReplay -ErrorAction SilentlyContinue).OpenReplay
    if ((Test-Path -LiteralPath $installRoot) -or (Test-Path -LiteralPath $uninstallKey) -or
        (Test-Path -LiteralPath $appPathKey) -or (Test-Path -LiteralPath $shortcut) -or $startup) {
        throw 'Uninstaller left installation files or registrations behind.'
    }
} finally {
    $uninstaller = Join-Path $installRoot 'unins000.exe'
    if (Test-Path -LiteralPath $uninstaller) {
        Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') `
            -Wait | Out-Null
    }
    Remove-ItemProperty -LiteralPath $runKey -Name OpenReplay -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
}

'Installer install/uninstall smoke test passed.'
