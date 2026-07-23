param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$PrivateKeyBase64,
    [string]$OutputDirectory = (Split-Path -Parent $Archive)
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be stable semantic version X.Y.Z.' }
$archivePath = (Resolve-Path $Archive).Path
$archiveName = Split-Path -Leaf $archivePath
$size = (Get-Item -LiteralPath $archivePath).Length
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
$tag = "v$Version"
$manifestPath = Join-Path $OutputDirectory 'OpenReplay-update.json'
$signaturePath = Join-Path $OutputDirectory 'OpenReplay-update.json.sig'

$manifest = [ordered]@{
    schema = 1
    product = 'OpenReplay'
    channel = 'stable'
    version = $Version
    tag = $tag
    published_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    release_notes_url = "https://github.com/G4F-Elite/OpenReplay/releases/tag/$tag"
    asset_url = "https://github.com/G4F-Elite/OpenReplay/releases/download/$tag/$archiveName"
    asset_name = $archiveName
    architecture = 'x64'
    size_bytes = $size
    sha256 = $hash
} | ConvertTo-Json

$bytes = [Text.UTF8Encoding]::new($false).GetBytes($manifest + "`n")
[IO.File]::WriteAllBytes($manifestPath, $bytes)

$rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider
try {
    $rsa.ImportCspBlob([Convert]::FromBase64String($PrivateKeyBase64.Trim()))
    $signature = $rsa.SignData($bytes, [System.Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))
    [IO.File]::WriteAllBytes($signaturePath, $signature)
} finally {
    $rsa.Dispose()
}

"Created signed update metadata in $OutputDirectory"
