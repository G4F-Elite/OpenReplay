param(
    [string]$Version = '6.7.3',
    [string]$Sha256 = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$toolsRoot = Join-Path $repository "artifacts\tools\inno-setup-$Version"
$compiler = Join-Path $toolsRoot 'ISCC.exe'

if (-not (Test-Path -LiteralPath $compiler)) {
    $download = Join-Path $env:TEMP "innosetup-$Version.exe"
    $urlVersion = $Version.Replace('.', '_')
    $url = "https://github.com/jrsoftware/issrc/releases/download/is-$urlVersion/innosetup-$Version.exe"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $download
    $actualHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $Sha256.ToLowerInvariant()) {
        Remove-Item -LiteralPath $download -Force
        throw "Inno Setup SHA-256 mismatch. Expected $Sha256, received $actualHash."
    }

    New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
    $arguments = @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        '/NOICONS',
        "/DIR=`"$toolsRoot`""
    )
    $process = Start-Process -FilePath $download -ArgumentList $arguments -Wait -PassThru
    Remove-Item -LiteralPath $download -Force
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler)) {
        throw "Inno Setup installation failed with exit code $($process.ExitCode)."
    }
}

$env:ISCC_PATH = $compiler
if ($env:GITHUB_ENV) { "ISCC_PATH=$compiler" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding UTF8 }
"Inno Setup compiler: $compiler"
