# SPDX-License-Identifier: GPL-3.0-or-later
# Windows-only incremental validation. No hosted workflow is started.
[CmdletBinding()]
param(
    [ValidateSet('Changed', 'Default', 'Reference', 'Build')][string]$Mode = 'Changed',
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BaseRef = '',
    [string]$HeadRef = 'HEAD',
    [string[]]$ChangedFiles = @(),
    [string]$KzvuSourceRoot = '',
    [string]$Pcsx2Root = '',
    [ValidateSet('NMake Makefiles')][string]$Generator = 'NMake Makefiles',
    [switch]$PushInput,
    [string[]]$PushLines = @(),
    [switch]$ListOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($ProjectRoot)
$runDefault = $Mode -in @('Default', 'Build')
$runReference = $Mode -eq 'Reference'
$runBuild = $Mode -eq 'Build'
$runSelectionTest = $false
if ($Mode -eq 'Changed') {
    $paths = @(& (Join-Path $PSScriptRoot 'resolve_local_changes.ps1') -ProjectRoot $root -BaseRef $BaseRef -HeadRef $HeadRef -ChangedFiles $ChangedFiles -IncludeWorkingTree:(!$PushInput -and $PushLines.Count -eq 0) -PushInput:$PushInput -PushLines $PushLines)
    foreach ($item in $paths) {
        $path = ($item -replace '\\', '/') -replace '^\./', ''
        if ($path -eq '*') {
            $runDefault = $true
            $runReference = $true
            $runBuild = $true
            $runSelectionTest = $true
            break
        }
        if ($path -in @('scripts/check_local.ps1', 'scripts/resolve_local_changes.ps1',
                        'scripts/install_local_hooks.ps1', '.githooks/pre-push',
                        'tests/local_checks_smoke.ps1') -or $path -match '^\.github/workflows/') {
            $runSelectionTest = $true
            continue
        }
        if ($path -match '^docs/' -or ($path -match '\.(md|txt)$' -and $path -ne 'CMakeLists.txt')) {
            continue
        }
        if ($path -match '(^|/)CMakeLists\.txt$' -or $path -match '\.cmake$' -or
            $path -match '^ps2x(Recomp|Analyzer)/') {
            $runBuild = $true
            $runDefault = $true
            $runReference = $true
            continue
        }
        if ($path -match '^ps2x(Runtime|Test|IOP)/') {
            $runDefault = $true
            if ($path -match '(?i)(kzvu|(^|/)vu[01]?(_|/)|ps2_vu|vu_cycle|vu_hazard|(^|/)vu\.(cpp|h)$|(^|/)ps2_runtime_macros\.h$)') {
                $runReference = $true
            }
            continue
        }
        # Unclassified executable changes require all build variants.
        $runBuild = $true
        $runDefault = $true
        $runReference = $true
    }
}
if ($ListOnly) {
    if (-not ($runDefault -or $runReference -or $runSelectionTest)) {
        Write-Output 'SKIP no relevant executable changes'
    }
    if ($runSelectionTest) { Write-Output 'TEST local_checks_smoke.ps1' }
    if ($runDefault) { Write-Output $(if ($runBuild) { 'BUILD local PS2Recomp complete default' } else { 'BUILD local PS2Recomp default tests' }) }
    if ($runReference) { Write-Output 'BUILD local PS2Recomp KZVU reference tests' }
    return
}
if ($runSelectionTest) {
    & (Join-Path $root 'tests\local_checks_smoke.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Local change-selection regression failed.' }
}
if (-not ($runDefault -or $runReference)) {
    if (-not $runSelectionTest) { Write-Host 'No compiled changes require validation.' }
} else {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'CMake is required in the local Visual Studio developer environment.'
    }
    $referenceArgs = @()
    if ($runReference) {
        if (-not $KzvuSourceRoot -or -not $Pcsx2Root) {
            throw 'KZVU tests selected. Pass -KzvuSourceRoot and -Pcsx2Root with the existing pinned source checkouts.'
        }
        $vu = [IO.Path]::GetFullPath($KzvuSourceRoot)
        $pcsx2 = [IO.Path]::GetFullPath($Pcsx2Root)
        $kzRoot = [IO.Path]::GetFullPath((Join-Path $vu '..\..'))
        foreach ($record in @(
            @{ Root = $kzRoot; Expected = '63917763d68532048edbabc9e7d6d01c9738e55a' },
            @{ Root = $pcsx2; Expected = '646df006a4688063e56c0826e7ea4b8bb0c112c4' }
        )) {
            $head = @(& git -C $record.Root rev-parse HEAD 2>$null)
            if ($LASTEXITCODE -ne 0 -or $head.Count -ne 1 -or $head[0] -ne $record.Expected) {
                throw "Reference SHA does not match the pinned revision: $($record.Root)"
            }
            $dirty = @(& git -C $record.Root status --porcelain --untracked-files=normal)
            if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) {
                throw "Reference source is not clean: $($record.Root)"
            }
        }
        if (-not (Test-Path (Join-Path $vu 'CMakeLists.txt')) -or
            -not (Test-Path (Join-Path $pcsx2 'pcsx2\GS\GS.cpp'))) {
            throw 'The pinned KZVU or PCSX2 source tree is incomplete.'
        }
        $referenceArgs = @("-DPS2X_KZVU_SOURCE_DIR=$vu", "-DKZVU_PCSX2_DIR=$pcsx2", '-DKZVU_BUILD_TEST=OFF')
    }
    function Invoke-NativeTests([bool]$UseReference, [bool]$BuildAll) {
        $build = Join-Path $root $(if ($UseReference) { 'out\local-kzvu' } else { 'out\local-default' })
        $cache = Join-Path $build 'CMakeCache.txt'
        if (Test-Path -LiteralPath $cache) {
            $existing = @(Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$')
            if ($existing.Count -ne 1 -or $existing[0].Matches[0].Groups[1].Value -ne $Generator) {
                throw "The existing CMake cache uses another generator: $build. Use a separate build directory."
            }
        }
        $args = @('-S', $root, '-B', $build, '-G', $Generator,
                  '-DPS2X_BUILD_RECOMP=ON', '-DPS2X_BUILD_RUNTIME=ON',
                  '-DPS2X_BUILD_ANALYZER=ON', '-DPS2X_BUILD_TEST=ON',
                  '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_DEBUG_UI=OFF',
                  '-DPS2X_ENABLE_FFMPEG=OFF',
                  '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                  ('-DPS2X_ENABLE_KZVU_VU1=' + $(if ($UseReference) { 'ON' } else { 'OFF' })))
        if ($UseReference) { $args += $referenceArgs }
        & cmake @args
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $build" }
        if ($BuildAll) {
            & cmake --build $build --config Release --parallel 4
        } else {
            & cmake --build $build --config Release --target ps2x_tests --parallel 4
        }
        if ($LASTEXITCODE -ne 0) { throw "Local build failed: $build" }
        $candidates = @((Join-Path $build 'ps2xTest\Release\ps2x_tests.exe'),
                        (Join-Path $build 'ps2xTest\ps2x_tests.exe'))
        $exe = @($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)
        if ($exe.Count -ne 1) { throw "Native test executable is missing: $build" }
        Push-Location $root
        try {
            & $exe[0]
            if ($LASTEXITCODE -ne 0) { throw "Native component tests failed: $build" }
        } finally { Pop-Location }
    }
    if ($runDefault) { Invoke-NativeTests $false $runBuild }
    if ($runReference) { Invoke-NativeTests $true $false }
}
# A successful explicit comparison or pre-push run must not certify other commits.
if ($Mode -eq 'Changed' -and -not $PushInput -and $PushLines.Count -eq 0 -and
    $ChangedFiles.Count -eq 0 -and -not $BaseRef -and $HeadRef -eq 'HEAD') {
    $branch = @(& git -C $root symbolic-ref --short HEAD 2>$null)
    $dirty = @(& git -C $root status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect worktree before recording validation.' }
    if ($branch.Count -eq 1 -and $branch[0] -eq 'main' -and $dirty.Count -eq 0) {
        $gitDir = @(& git -C $root rev-parse --absolute-git-dir)
        $sha = @(& git -C $root rev-parse HEAD)
        if ($LASTEXITCODE -ne 0 -or $gitDir.Count -ne 1 -or $sha.Count -ne 1) {
            throw 'Cannot record validated commit.'
        }
        $folder = Join-Path $gitDir[0] 'local-checks'
        New-Item -Path $folder -ItemType Directory -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $folder 'verified-main'), ('v3:' + $sha[0]), [Text.UTF8Encoding]::new($false))
    } else {
        Write-Host 'Validation was not recorded: checkout is dirty or is not main.'
    }
}
Write-Host 'Selected local validation finished; no GitHub Actions run was requested.'
