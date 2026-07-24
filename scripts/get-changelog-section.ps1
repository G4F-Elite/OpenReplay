param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..").Path
$changelog = Join-Path $repository 'CHANGELOG.md'
$lines = Get-Content -LiteralPath $changelog
$escapedVersion = [regex]::Escape($Version)
$start = -1
for ($index = 0; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match "^## \[$escapedVersion\](?:\s+-\s+.*)?$") {
        $start = $index + 1
        break
    }
}
if ($start -lt 0) { throw "CHANGELOG.md does not contain [$Version]." }

$end = $lines.Count
for ($index = $start; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match '^## \[') {
        $end = $index
        break
    }
}

$body = ($lines[$start..($end - 1)] -join [Environment]::NewLine).Trim()
if (-not $body) { throw "CHANGELOG.md section [$Version] is empty." }
$parent = Split-Path -Parent $Output
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
[IO.File]::WriteAllText($Output, $body + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
