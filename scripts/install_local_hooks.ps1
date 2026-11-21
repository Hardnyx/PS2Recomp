# Install repository-local Git hooks without replacing a custom hooks path silently.
[CmdletBinding()]
param([switch]$Force)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& git -C $root rev-parse --is-inside-work-tree | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Not a Git checkout.' }
$current = @(& git -C $root config --local --get core.hooksPath 2>$null)
if ($current.Count -gt 0 -and $current[0] -ne '.githooks' -and -not $Force) {
    throw 'An existing hooksPath is configured. Use -Force to replace it.'
}
& git -C $root config --local core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) { throw 'Failed to configure local Git hooks.' }
Write-Host 'Repository-local pre-push checks enabled.'
