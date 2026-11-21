# SPDX-License-Identifier: GPL-3.0-or-later
# Incremental Windows component validation. Never starts hosted workflows.
[CmdletBinding()]
param(
    [ValidateSet('Changed', 'Default', 'Reference', 'Build')][string]$Mode = 'Changed',
    [string]$BaseRef = 'origin/main',
    [string[]]$ChangedFiles = @(),
    [string]$KzvuSourceRoot = '',
    [string]$Pcsx2Root = '',
    [switch]$ListOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$choice = $Mode
if ($Mode -eq 'Changed') {
    if ($ChangedFiles.Count -gt 0) {
        $changes = @($ChangedFiles)
    }
    else {
        $base = $BaseRef
        & git -C $root rev-parse --verify "$base^{commit}" 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $base = 'HEAD~1'
            & git -C $root rev-parse --verify "$base^{commit}" 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'No comparison commit. Use -Mode Default explicitly.' }
        }
        $committed = @(& git -C $root -c core.quotepath=false diff --name-only --diff-filter=ACMR $base HEAD)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot compare commits.' }
        $working = @(& git -C $root -c core.quotepath=false diff --name-only HEAD)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot compare worktree.' }
        $untracked = @(& git -C $root -c core.quotepath=false ls-files --others --exclude-standard)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect untracked source.' }
        $changes = @($committed + $working + $untracked | Where-Object { $_ } | Sort-Object -Unique)
    }
    $choice = 'Skip'
    foreach ($item in $changes) {
        $path = ($item -replace '\\', '/') -replace '^\./', ''
        if ($path -match '^(docs/|\.github/workflows/|\.githooks/)' -or
            $path -match '\.(md|txt)$' -or $path -in @('scripts/check_local.ps1', 'scripts/install_local_hooks.ps1')) {
            continue
        }
        $choice = 'Default'
        break
    }
}
if ($ListOnly) {
    if ($choice -eq 'Skip') { Write-Output 'SKIP no compiled source changes' }
    else { Write-Output "BUILD local PS2Recomp $choice" }
    return
}
if ($choice -eq 'Skip') {
    Write-Host 'No relevant source changes; no local build required.'
    return
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake is required locally.' }
$reference = $choice -eq 'Reference'
if ($reference) {
    if (-not $KzvuSourceRoot -or -not $Pcsx2Root) {
        throw 'Reference mode requires -KzvuSourceRoot and -Pcsx2Root.'
    }
    $vuSource = [IO.Path]::GetFullPath($KzvuSourceRoot)
    $pcsx2 = [IO.Path]::GetFullPath($Pcsx2Root)
    if (-not (Test-Path -LiteralPath (Join-Path $vuSource 'CMakeLists.txt')) -or
        -not (Test-Path -LiteralPath (Join-Path $pcsx2 'pcsx2\GS\GS.cpp'))) {
        throw 'Local pinned KZVU and PCSX2 source files are required; no checkout is performed.'
    }
    $kzRepository = [IO.Path]::GetFullPath((Join-Path $vuSource '..\..'))
    foreach ($record in @(
        @{ Root = $kzRepository; Expected = '63917763d68532048edbabc9e7d6d01c9738e55a' },
        @{ Root = $pcsx2; Expected = '646df006a4688063e56c0826e7ea4b8bb0c112c4' }
    )) {
        $actual = @(& git -C $record.Root rev-parse HEAD)
        if ($LASTEXITCODE -ne 0 -or $actual.Count -ne 1 -or $actual[0] -ne $record.Expected) {
            throw "Reference dependency revision mismatch: $($record.Root)"
        }
    }
}
$build = Join-Path $root $(if ($reference) { 'out\local-kzvu' } else { 'out\local-default' })
$args = @('-S', $root, '-B', $build, '-DPS2X_BUILD_STUDIO=OFF',
          '-DPS2X_ENABLE_FFMPEG=OFF', ('-DPS2X_ENABLE_KZVU_VU1=' + $(if ($reference) { 'ON' } else { 'OFF' })))
if ($reference) {
    $args += @("-DPS2X_KZVU_SOURCE_DIR=$vuSource", "-DKZVU_PCSX2_DIR=$pcsx2", '-DKZVU_BUILD_TEST=OFF')
}
Push-Location $root
try {
    & cmake @args
    if ($LASTEXITCODE -ne 0) { throw 'Local CMake configure failed.' }
    if ($choice -eq 'Build') {
        & cmake --build $build --config Release --parallel 4
    }
    else {
        & cmake --build $build --config Release --target ps2x_tests --parallel 4
    }
    if ($LASTEXITCODE -ne 0) { throw 'Local build failed.' }
    $candidates = @((Join-Path $build 'ps2xTest\Release\ps2x_tests.exe'),
                    (Join-Path $build 'ps2xTest\ps2x_tests.exe'))
    $testExe = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $testExe) { throw 'Native tests were not produced.' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Local component tests failed.' }
    Write-Host 'Local component tests passed; no hosted runner was used.'
}
finally { Pop-Location }
