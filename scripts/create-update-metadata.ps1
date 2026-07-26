param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$PrivateKeyBase64,
    [ValidateSet('stable', 'dev')][string]$Channel = 'stable',
    [string]$Tag,
    [string]$OutputDirectory = (Split-Path -Parent $Archive)
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$') {
    throw 'Version must be a semantic version.'
}
if ($Channel -eq 'stable' -and $Version.Contains('-')) { throw 'Stable metadata cannot use a prerelease version.' }
if ($Channel -eq 'dev' -and -not $Version.Contains('-')) { throw 'Developer metadata requires a prerelease version.' }
$archivePath = (Resolve-Path $Archive).Path
$archiveName = Split-Path -Leaf $archivePath
$size = (Get-Item -LiteralPath $archivePath).Length
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
$releaseTag = if ($Tag) { $Tag } elseif ($Channel -eq 'stable') { "v$Version" } else { 'dev' }
$manifestPath = Join-Path $OutputDirectory 'OpenReplay-update.json'
$signaturePath = Join-Path $OutputDirectory 'OpenReplay-update.json.sig'

$manifest = [ordered]@{
    schema = 1
    product = 'OpenReplay'
    channel = $Channel
    version = $Version
    tag = $releaseTag
    published_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    release_notes_url = "https://github.com/G4F-Elite/OpenReplay/releases/tag/$releaseTag"
    asset_url = "https://github.com/G4F-Elite/OpenReplay/releases/download/$releaseTag/$archiveName"
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
