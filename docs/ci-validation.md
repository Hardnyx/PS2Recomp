# Component validation and execution budget

## Scope

PS2Recomp component regressions run from this public repository. The workflow
must not retrieve, upload, or depend on commercial game images, game-derived
executables, local TekkenTagPort workspaces, or private repository contents.
An ISO-dependent game acceptance test belongs in the user's local environment.

## Workflow ownership

- `.github/workflows/build.yml` verifies Linux GCC, Linux Clang, and Windows MSVC.
- `.github/workflows/vu-component.yml` verifies the optional KZVU interpreter
  against fixed KZVU and PCSX2 source revisions on standard Windows.
- Each workflow uses source-path filters, `workflow_dispatch`, a single
  per-ref concurrency group with superseded-run cancellation, and job timeouts.
- Every job has a repository-visibility guard. A private repository must not
  allocate runners for these workflows.
- Documentation-only commits are excluded from automatic execution.
- The workflows use standard, not larger, GitHub-hosted runners.

GitHub's standard hosted runners are free for public repositories. That
exemption does not apply to private TekkenTagPort Actions: the account's
monthly allowance is shared among its private repositories. Never launch
private-repository Windows CI merely to validate a public engine change.
Check repository visibility and billing policy before changing runner types
or enabling a new CI trigger.

## Validation sequence

1. Inspect the current `main` revision and isolate one reproducible engine
   issue, preferably with a regression test.
2. Review parser state, resource bounds, reset semantics, and the expected
   hardware/reference behavior before editing production code.
3. Commit the change directly to `main` with a conventional English message
   only after ensuring it contains a coherent testable change.
4. Inspect both public workflow results. Capture any failing assertion and
   its corresponding source revision; do not call a candidate validated when
   a job is still running or a separate test has failed.
5. Avoid pushing superseding revisions while a useful run is executing:
   the concurrency guard will cancel the older result.
6. Update the technical checkpoint with the exact tested commit, run links,
   native/reference test totals, and remaining unverified behavior.
7. Keep TekkenTagPort's private dependency pin unchanged until its separate
   integration gate can be exercised without paid CI and, where needed,
   the user's own ISO can be tested locally.

## Manual reproduction

Configure and run the default component suite using MSVC and CMake:

```powershell
cmake -S . -B out/ci-default -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_KZVU_VU1=OFF
cmake --build out/ci-default --config Release --target ps2x_tests --parallel 2
.\out\ci-default\ps2xTest\Release\ps2x_tests.exe
```

The reference configuration requires the pinned KZVU and PCSX2 sources
identified in `.github/workflows/vu-component.yml`. Its CMake parameters
are recorded there; do not replace those revisions with floating branch tips.

## Acceptance boundaries

Component tests can establish deterministic parser, register, and interpreter
behavior. They do not establish PlayStation 2 cycle accuracy, graphics and
audio equivalence, live VIF/VU/DMAC overlap, a complete playable match,
or acceptable local performance. Those are separate acceptance gates.
