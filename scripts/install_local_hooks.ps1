# Install only the reviewed repository-local hook, preserving custom hooks.
[CmdletBinding()]
param([switch]$Force)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hook = Join-Path $root '.githooks\pre-push'
if (-not (Test-Path -LiteralPath $hook -PathType Leaf)) {
    throw 'Missing tracked pre-push hook. No Git configuration changed.'
}
& git -C $root rev-parse --show-toplevel | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Not a Git checkout.' }
$current = @(& git -C $root config --local --get core.hooksPath 2>$null)
if ($current.Count -gt 0 -and $current[0] -ne '.githooks' -and -not $Force) {
    throw 'An existing hooksPath is configured. Use -Force only after checking it.'
}
& git -C $root config --local core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) { throw 'Could not install repository-local Git hooks.' }
Write-Host 'Local pre-push hook installed for this checkout.'
