# Regression controls for the selector. Never builds or launches the game.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $root 'scripts\check_local.ps1'
$shell = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
$cases = @(
    @{ Path = 'docs/TEKKEN_RUNTIME.md'; Required = @('SKIP'); Forbidden = @('BUILD') },
    @{ Path = 'scripts/resolve_local_changes.ps1'; Required = @('TEST local_checks_smoke.ps1'); Forbidden = @('BUILD') },
    @{ Path = 'ps2xRuntime/src/lib/vu/ps2_vu1_kzvu.cpp'; Required = @('BUILD local PS2Recomp default tests', 'BUILD local PS2Recomp KZVU reference tests'); Forbidden = @() },
    @{ Path = 'ps2xRuntime/src/lib/Kernel/EeScheduler.cpp'; Required = @('BUILD local PS2Recomp default tests'); Forbidden = @('KZVU reference') },
    @{ Path = 'CMakeLists.txt'; Required = @('BUILD local PS2Recomp complete default', 'KZVU reference'); Forbidden = @() }
)
foreach ($case in $cases) {
    $output = @(& $shell -NoProfile -ExecutionPolicy Bypass -File $runner -ListOnly -ChangedFiles $case.Path)
    if ($LASTEXITCODE -ne 0) { throw "Selection failed: $($case.Path)" }
    $text = $output -join [Environment]::NewLine
    foreach ($expected in $case.Required) {
        if (-not $text.Contains($expected)) { throw "Missing '$expected' for $($case.Path): $text" }
    }
    foreach ($unexpected in $case.Forbidden) {
        if ($text.Contains($unexpected)) { throw "Unexpected '$unexpected' for $($case.Path): $text" }
    }
}
# Exercise actual Git history rather than stubbing a list of changed filenames.
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('ps2x-local-checks-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    & git -C $temporary init -q
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize synthetic repository.' }
    & git -C $temporary config user.name Hardnyx
    & git -C $temporary config user.email 213886497+Hardnyx@users.noreply.github.com
    $codePath = Join-Path $temporary 'ps2xRuntime\src\lib\vu'
    New-Item -ItemType Directory -Force -Path $codePath | Out-Null
    $source = Join-Path $codePath 'ps2_vu1_kzvu.cpp'
    [IO.File]::WriteAllText($source, 'int probe = 1;')
    & git -C $temporary add .
    & git -C $temporary commit -qm 'test: create synthetic reference source'
    $initial = @(& git -C $temporary rev-parse HEAD)[0]
    & git -C $temporary rm -q -- ps2xRuntime/src/lib/vu/ps2_vu1_kzvu.cpp
    & git -C $temporary commit -qm 'test: remove reference source'
    $removed = @(& git -C $temporary rev-parse HEAD)[0]
    [IO.File]::WriteAllText((Join-Path $temporary 'README.md'), 'documentation')
    & git -C $temporary add README.md
    & git -C $temporary commit -qm 'docs: update synthetic notes'
    $latest = @(& git -C $temporary rev-parse HEAD)[0]
    $fromInitial = @(& $shell -NoProfile -ExecutionPolicy Bypass -File $runner -ProjectRoot $temporary -BaseRef $initial -ListOnly)
    if ($LASTEXITCODE -ne 0 -or ($fromInitial -join ' ') -notmatch 'KZVU reference') {
        throw 'A deletion followed by a documentation commit was missed.'
    }
    $push = "refs/heads/main $latest refs/heads/main $initial"
    $fromPush = @(& $shell -NoProfile -ExecutionPolicy Bypass -File $runner -ProjectRoot $temporary -PushLines $push -ListOnly)
    if ($LASTEXITCODE -ne 0 -or ($fromPush -join ' ') -notmatch 'KZVU reference') {
        throw 'Git pre-push SHA range was not honored.'
    }
    & git -C $temporary mv README.md README-renamed.md
    & git -C $temporary commit -qm 'test: rename synthetic notes'
    $renamePaths = @(& (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -BaseRef $latest)
    if ($LASTEXITCODE -ne 0 -or
        $renamePaths -notcontains 'README.md' -or
        $renamePaths -notcontains 'README-renamed.md') {
        throw 'Both old and new paths must be reported on rename.'
    }
    $withoutBaseline = @(& $shell -NoProfile -ExecutionPolicy Bypass -File $runner -ProjectRoot $temporary -ListOnly)
    if ($LASTEXITCODE -ne 0 -or ($withoutBaseline -join ' ') -notmatch 'complete default') {
        throw 'Missing verified-baseline marker must select the complete suite.'
    }
    $fromLastOnly = @(& $shell -NoProfile -ExecutionPolicy Bypass -File $runner -ProjectRoot $temporary -BaseRef $removed -ListOnly)
    if ($LASTEXITCODE -ne 0 -or ($fromLastOnly -join ' ') -notmatch 'SKIP') {
        throw 'Synthetic latest documentation-only commit is not isolated.'
    }
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
Write-Host 'Local component selection and Git-range regressions passed.'
