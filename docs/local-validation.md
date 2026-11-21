# Local PS2Recomp validation

GitHub Actions workflows remain manual-only. No push, hook or local test
starts a cloud workflow.

## Windows development environment

Run the checker from an MSVC x64 developer command prompt with CMake and
PowerShell available:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check_local.ps1 -ListOnly
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check_local.ps1

The first normal check without a previously verified baseline selects the
complete build, both native test variants and the selector regression. It
does not compare only HEAD~1 and it cannot declare an API-created commit
validated merely because origin/main equals HEAD. After a successful,
clean, on-main check, the verified SHA is recorded under the local Git
directory in local-checks/verified-main. The marker is not published.
Subsequent checks cover every commit since that SHA, plus staged, unstaged
and untracked files. Deletions and both sides of a rename are visible.

Explicit modes: -Mode Default checks ordinary native components;
-Mode Reference checks optional KZVU; -Mode Build builds the complete
ordinary configuration and runs tests. KZVU-related source and CMake
changes select both ordinary and reference tests automatically.
Reference validation needs -KzvuSourceRoot and -Pcsx2Root pointing to
clean existing source trees at the pinned KillzoneRecomp and PCSX2 SHAs.
A missing reference is a failure, not a successful skipped test.

CMake uses NMake Makefiles and explicit build/component options.
Cached generator conflicts fail rather than deleting another build.
Dependency fetching is disabled; prepare the source dependencies locally
before running. Outputs are incremental in out/local-default and
out/local-kzvu. The normal build does not rewrite local repositories.

## Pre-push

The optional repository-local pre-push hook reads the actual local and
remote refs and SHA pairs provided by Git. It permits only main pushes,
rejects deletions/non-fast-forward updates, and validates every committed
change being pushed. If the remote SHA is unavailable locally, the hook rejects the push until
the remote history is fetched; unknown changes cannot receive a false green. No origin/main assumption is made.

Install only after the Windows regression checks pass:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install_local_hooks.ps1

An existing custom hooksPath is preserved unless -Force is supplied.
Commits created through a GitHub connector do not invoke local hooks;
run the normal checker after updating the workstation checkout.
A successful pre-push check does not write the last-verified marker.

The synthetic routing tests can be run separately:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\local_checks_smoke.ps1

These checks do not prove Tekken gameplay, timing or pixel fidelity.
