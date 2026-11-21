# Regression controls for the selector. Never builds or launches the game.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# Repository workflows are deliberately manual-only; test their event declarations.
$hookPath = Join-Path $root '.githooks\pre-push'
if (-not (Test-Path -LiteralPath $hookPath -PathType Leaf)) {
    throw 'The repository-local pre-push hook is missing.'
}
$hookText = Get-Content -LiteralPath $hookPath -Raw
if (-not $hookText.Contains('check_local.ps1') -or -not $hookText.Contains('-PushInput')) {
    throw 'The pre-push hook must pass Git ref updates to the local checker.'
}
$hookEntry = @(& git -C $root ls-files --stage -- .githooks/pre-push)
if ($LASTEXITCODE -ne 0 -or $hookEntry.Count -ne 1 -or $hookEntry[0] -notmatch '^100755 ') {
    throw 'The tracked pre-push hook must retain executable mode 100755.'
}
$installer = Get-Content -LiteralPath (Join-Path $root 'scripts\install_local_hooks.ps1') -Raw
if (-not $installer.Contains('config --local --get-all core.hooksPath') -or
    -not $installer.Contains('config --local --replace-all core.hooksPath')) {
    throw 'Hook installer must verify and replace only the local hooksPath.'
}
$workflowDirectory = Join-Path $root '.github\workflows'
$workflows = @(Get-ChildItem -LiteralPath $workflowDirectory -File |
               Where-Object { $_.Extension -in @('.yml', '.yaml') })
if ($workflows.Count -lt 2) { throw 'Expected GitHub Actions workflows are missing.' }
foreach ($workflow in $workflows) {
    $lines = [IO.File]::ReadAllLines($workflow.FullName)
    $onLines = @(for ($line = 0; $line -lt $lines.Length; ++$line) {
        if ($lines[$line] -match '^on:[ \t]*$') { $line }
    })
    if ($onLines.Count -ne 1) {
        throw "Ambiguous or missing workflow event declaration: $($workflow.Name)"
    }
    $events = @()
    for ($line = $onLines[0] + 1; $line -lt $lines.Length; ++$line) {
        if ($lines[$line] -match '^[^ #\t][^:]*:') { break }
        if ($lines[$line] -match '^  ([A-Za-z_][A-Za-z0-9_-]*):') {
            $events += $Matches[1]
        }
    }
    if ($events.Count -ne 1 -or $events[0] -ne 'workflow_dispatch') {
        throw "Hosted workflow must be manual-only: $($workflow.Name)"
    }
}
$runner = Join-Path $root 'scripts\check_local.ps1'
$shell = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
$cases = @(
    @{ Path = 'docs/TEKKEN_RUNTIME.md'; Required = @('SKIP'); Forbidden = @('BUILD') },
    @{ Path = 'scripts/resolve_local_changes.ps1'; Required = @('TEST local_checks_smoke.ps1'); Forbidden = @('BUILD') },
    @{ Path = 'ps2xRuntime/src/lib/vu/ps2_vu1_kzvu.cpp'; Required = @('BUILD local PS2Recomp default tests', 'BUILD local PS2Recomp KZVU reference tests'); Forbidden = @() },
    @{ Path = 'ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp'; Required = @('BUILD local PS2Recomp default tests', 'BUILD local PS2Recomp KZVU reference tests'); Forbidden = @() },
    @{ Path = 'ps2xRuntime/src/lib/Kernel/Stubs/VU.h'; Required = @('BUILD local PS2Recomp default tests', 'BUILD local PS2Recomp KZVU reference tests'); Forbidden = @() },
    @{ Path = 'ps2xRuntime/include/ps2_runtime_macros.h'; Required = @('BUILD local PS2Recomp default tests', 'BUILD local PS2Recomp KZVU reference tests'); Forbidden = @() },
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
    $emptyPushRejected = $false
    try {
        & (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -PushLines @('') | Out-Null
    } catch { $emptyPushRejected = $true }
    if (-not $emptyPushRejected) { throw 'An empty pre-push input was accepted.' }
    $stalePush = "refs/heads/main $initial refs/heads/main $initial"
    $wrongRevisionRejected = $false
    try {
        & (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -PushLines $stalePush | Out-Null
    } catch { $wrongRevisionRejected = $true }
    if (-not $wrongRevisionRejected) {
        throw 'Pre-push accepted a commit other than the tested checkout HEAD.'
    }
    $dirtyFile = Join-Path $temporary 'untracked-push-probe.txt'
    [IO.File]::WriteAllText($dirtyFile, 'pending edit')
    $dirtyRejected = $false
    try {
        & (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -PushLines $push | Out-Null
    } catch { $dirtyRejected = $true }
    if (-not $dirtyRejected) {
        throw 'Pre-push accepted an uncommitted worktree.'
    }
    Remove-Item -LiteralPath $dirtyFile
    $unfetchedRemote = "refs/heads/main $latest refs/heads/main 1111111111111111111111111111111111111111"
    $remoteRejected = $false
    try {
        & (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -PushLines $unfetchedRemote | Out-Null
    } catch { $remoteRejected = $true }
    if (-not $remoteRejected) {
        throw 'Unknown remote history was accepted without knowing the changed paths.'
    }
    $newRemote = "refs/heads/main $latest refs/heads/main 0000000000000000000000000000000000000000"
    $newRemoteRejected = $false
    try {
        & (Join-Path $root 'scripts\resolve_local_changes.ps1') -ProjectRoot $temporary -PushLines $newRemote | Out-Null
    } catch { $newRemoteRejected = $true }
    if (-not $newRemoteRejected) {
        throw 'A new remote main was accepted without an explicit validation path.'
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
