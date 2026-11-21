# Local PS2Recomp validation

Hosted GitHub workflows for both the normal build and the optional VU
reference backend are manual-only. Do not start them automatically on pushes.

In an existing Windows checkout, execute:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check_local.ps1

Changed mode compares the current checkout against origin/main and includes
uncommitted files. Documentation-only changes do not compile. Changes to
runtime sources, tests, CMake or unknown code run the existing default
ps2x_tests target. All local builds retain their CMake cache in out/local-default.

To force the default native tests:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check_local.ps1 -Mode Default

For a broader build, select -Mode Build. For reference VU1 tests, select
-Mode Reference with -KzvuSourceRoot and -Pcsx2Root, pointing to existing
source trees at the pinned revisions recorded in the workflow. The source
trees are never downloaded, updated or cleaned by this script. Reference
mode uses the separate out/local-kzvu build tree.

To preview whether a change needs compilation, use -ListOnly. Optional
local pre-push checks can be installed with scripts/install_local_hooks.ps1.
Only the local checkout is affected, and an existing custom hooksPath is
preserved unless -Force is specified. API-created commits do not execute
hooks on a workstation.

These unit tests are not an end-to-end game or hardware-equivalence control.
The port dependency pin remains unchanged until clean integration and visual
comparisons have passed. Run GitHub Actions manually only for cases that
cannot reasonably be tested locally and only after verifying billing.
