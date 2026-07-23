param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$Signature,
    [Parameter(Mandatory = $true)][string]$PrivateKeyBase64
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$versionHeader = Get-Content -LiteralPath (Join-Path $repository 'src\Shared\include\openreplay\Version.h') -Raw
$keyMatch = [regex]::Match($versionHeader, 'kUpdatePublicKeyBase64\{\s*"(?<key>[A-Za-z0-9+/=]+)"\s*\}')
if (-not $keyMatch.Success) { throw 'Unable to read the embedded update public key.' }

$rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider
try {
    $rsa.ImportCspBlob([Convert]::FromBase64String($PrivateKeyBase64.Trim()))
    $parameters = $rsa.ExportParameters($false)
    $header = New-Object byte[] 24
    [BitConverter]::GetBytes([uint32]0x31415352).CopyTo($header, 0)
    [BitConverter]::GetBytes([uint32]($parameters.Modulus.Length * 8)).CopyTo($header, 4)
    [BitConverter]::GetBytes([uint32]$parameters.Exponent.Length).CopyTo($header, 8)
    [BitConverter]::GetBytes([uint32]$parameters.Modulus.Length).CopyTo($header, 12)
    $publicBlob = New-Object byte[] ($header.Length + $parameters.Exponent.Length + $parameters.Modulus.Length)
    $header.CopyTo($publicBlob, 0)
    $parameters.Exponent.CopyTo($publicBlob, $header.Length)
    $parameters.Modulus.CopyTo($publicBlob, $header.Length + $parameters.Exponent.Length)
    if ([Convert]::ToBase64String($publicBlob) -ne $keyMatch.Groups['key'].Value) {
        throw 'The GitHub signing secret does not match the public key embedded in OpenReplay.'
    }

    $manifestBytes = [IO.File]::ReadAllBytes((Resolve-Path $Manifest))
    $signatureBytes = [IO.File]::ReadAllBytes((Resolve-Path $Signature))
    if (-not $rsa.VerifyData($manifestBytes, [System.Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'), $signatureBytes)) {
        throw 'Generated update metadata signature could not be verified.'
    }
} finally {
    $rsa.Dispose()
}

'Update signature and embedded public key verified.'
