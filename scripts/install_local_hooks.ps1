# Configure only this checkout. No global Git or cloud settings are modified.
[CmdletBinding()]
param([switch]$Force)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& git -C $root rev-parse --show-toplevel | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Not a Git checkout.' }

$hook = Join-Path $root '.githooks\pre-push'
if (-not (Test-Path -LiteralPath $hook -PathType Leaf)) {
    throw 'The repository-local pre-push hook is missing.'
}
$tracked = @(& git -C $root ls-files --stage -- .githooks/pre-push)
if ($LASTEXITCODE -ne 0 -or $tracked.Count -ne 1 -or $tracked[0] -notmatch '^100755 ') {
    throw 'The pre-push hook must be tracked with executable mode 100755.'
}

$current = @(& git -C $root config --local --get-all core.hooksPath 2>$null)
if ($current.Count -gt 0 -and
    ($current.Count -ne 1 -or $current[0] -ne '.githooks') -and -not $Force) {
    throw 'A custom hooksPath already exists. Use -Force only if it may be replaced.'
}
& git -C $root config --local --replace-all core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) { throw 'Failed to configure local Git hooks.' }

$configured = @(& git -C $root config --local --get-all core.hooksPath 2>$null)
if ($LASTEXITCODE -ne 0 -or $configured.Count -ne 1 -or $configured[0] -ne '.githooks') {
    throw 'Git did not retain the expected repository-local hooksPath.'
}
Write-Host 'Local pre-push checks enabled for this checkout.'
