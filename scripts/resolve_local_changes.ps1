# SPDX-License-Identifier: GPL-3.0-or-later
# Resolve all pushed, committed, staged, unstaged and untracked path changes.
[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BaseRef = '',
    [string]$HeadRef = 'HEAD',
    [string[]]$ChangedFiles = @(),
    [switch]$IncludeWorkingTree,
    [switch]$PushInput,
    [string[]]$PushLines = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($ProjectRoot)

function Resolve-Commit([string]$Reference) {
    if (-not $Reference) { return '' }
    $value = @(& git -C $root rev-parse --verify "$Reference^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0 -or $value.Count -ne 1) { return '' }
    return $value[0].Trim()
}
function Get-RangePaths([string]$Base, [string]$Head) {
    & git -C $root merge-base --is-ancestor $Base $Head 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Refusing a non-fast-forward range: $Base -> $Head"
    }
    # Disabling rename detection reports both the removed and added paths.
    $names = @(& git -C $root -c core.quotepath=false diff --no-renames --name-only $Base $Head)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate committed paths.' }
    return $names
}
if ($ChangedFiles.Count -gt 0) {
    if ($PushInput -or $PushLines.Count -gt 0) {
        throw 'Injected changed-file lists cannot replace pre-push SHA validation.'
    }
    $ChangedFiles | Where-Object { $_ } | Sort-Object -Unique
    return
}
if ($PushInput -or $PushLines.Count -gt 0) {
    $lines = if ($PushLines.Count -gt 0) { @($PushLines) } else {
        @([Console]::In.ReadToEnd() -split '\r?\n' | Where-Object { $_.Trim() })
    }
    $checkedOut = Resolve-Commit 'HEAD'
    if (-not $checkedOut) { throw 'Cannot verify the checked-out commit before a push.' }
    $working = @(& git -C $root status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $working.Count -gt 0) {
        throw 'Refusing to validate a push using an uncommitted worktree.'
    }
    $paths = @()
    foreach ($line in $lines) {
        $fields = @($line.Trim() -split '\s+')
        if ($fields.Count -ne 4) { throw "Malformed pre-push update: $line" }
        $localRef, $localSha, $remoteRef, $remoteSha = $fields
        if ($localRef -ne 'refs/heads/main' -or $remoteRef -ne 'refs/heads/main') {
            throw 'Local validation permits pushes to refs/heads/main only.'
        }
        if ($localSha -notmatch '^[0-9a-fA-F]{40}$' -or
            $remoteSha -notmatch '^[0-9a-fA-F]{40}$') {
            throw 'Invalid commit identifier from Git pre-push input.'
        }
        if ($localSha -match '^0{40}$') {
            throw 'Refusing a deletion of main.'
        }
        $head = Resolve-Commit $localSha
        if (-not $head) { throw 'The commit to push is not available locally.' }
        if ($head -ne $checkedOut) {
            throw 'The commit being pushed differs from checked-out HEAD; tests would use the wrong sources.'
        }
        if ($remoteSha -match '^0{40}$') {
            throw 'Remote main does not exist. New branch creation requires separate explicit validation.'
        }
        $base = Resolve-Commit $remoteSha
        if (-not $base) {
            # The remote history must be available to identify every pushed path.
            throw 'Remote commit is unavailable locally. Fetch the remote ref before validating this push.'
        }
        $paths += @(Get-RangePaths $base $head)
    }
    $paths | Where-Object { $_ } | Sort-Object -Unique
    return
}
$head = Resolve-Commit $HeadRef
if (-not $head) { throw "Invalid HEAD reference: $HeadRef" }
$base = ''
if ($BaseRef) {
    $base = Resolve-Commit $BaseRef
} else {
    $gitDir = @(& git -C $root rev-parse --absolute-git-dir)
    if ($LASTEXITCODE -ne 0 -or $gitDir.Count -ne 1) { throw 'Not a Git checkout.' }
    $marker = Join-Path $gitDir[0] 'local-checks\verified-main'
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $record = [IO.File]::ReadAllText($marker).Trim()
        if ($record -match '^v3:([0-9a-fA-F]{40})$') {
            $base = Resolve-Commit $Matches[1]
        }
    }
}
if (-not $base) {
    # No verified baseline: run the complete suite, not just HEAD~1.
    Write-Output '*'
    return
}
& git -C $root merge-base --is-ancestor $base $head 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Output '*'
    return
}
$paths = @(Get-RangePaths $base $head)
if ($IncludeWorkingTree) {
    $working = @(& git -C $root -c core.quotepath=false diff --no-renames --name-only HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect staged and unstaged paths.' }
    $untracked = @(& git -C $root -c core.quotepath=false ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect untracked paths.' }
    $paths += $working
    $paths += $untracked
}
$paths | Where-Object { $_ } | Sort-Object -Unique
