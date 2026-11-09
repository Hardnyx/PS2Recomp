# Tekken runtime integration notes

Updated 9 November 2026. This document describes `main` in
[Hardnyx/PS2Recomp](https://github.com/Hardnyx/PS2Recomp/tree/main).
The fork is based on upstream commit
`e5636981a1c9de368f6b71e819c3dc188dfedbf4` from
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp).
The historical `tekken-runtime` ref remains frozen at `70295a878764252c47656eb4857b79bbc543095d`,
an ancestor of `main`; it is not used for new work. The upstream license and
notices remain in [LICENSE](LICENSE).

## Scope and integration

This fork maintains reusable recompiler and runtime corrections needed by
TekkenTagPort. The game's EE still runs as statically recompiled native code.
Game-specific integration, generated function bindings, source patch layering,
GS/VU0 adapters and visible game controls live in
[TekkenTagPort](https://github.com/Hardnyx/TekkenTagPort).

The port's [dependency manifest](https://github.com/Hardnyx/TekkenTagPort/blob/main/config/dependencies.json)
pins `97519ea4bb710e197e15962c02e1450eae53dddf` at this checkpoint. Later commits on `main` are not automatically installed by the port, and publishing
`main` does not replace an existing patched runtime checkout. Some corrections,
including SDK matrix arithmetic, also have reversible port installers for those
existing checkouts. Do not apply such patches twice or discard local integration
files to reconcile different source revisions.

The project-wide [development plan](https://github.com/Hardnyx/TekkenTagPort/blob/main/docs/development-plan.md)
and [current checkpoint](https://github.com/Hardnyx/TekkenTagPort/blob/main/docs/development-checkpoint.md)
remain in the port repository. They record milestones, adoption decisions and
local evidence. This document records the fork's implementation and test boundary.

## Change inventory

The following commits follow the upstream base. Descriptions identify the
changed contracts; they do not certify every game or full Tekken compatibility.

| Area | Change | Commits |
| --- | --- | --- |
| VU arithmetic | Preserve constant VF0; saturate FMAC products before accumulation and retain sticky overflow | `5b64050`, `e9d1a82` |
| VIF1 | Expand V2/V3 UNPACK lanes before row and mask application | `cfa5af6` |
| Debug UI | Start with a collapsed top bar on first use | `02d0289` |
| VU execution cost | Avoid duplicate pipeline scans and visit only referenced integer registers | `7a5d287`, `5fdd233` |
| EE translation | Align LQ/SQ effective addresses to quadwords, including resolved constant addresses | `ee4d574` |
| MPEG lifecycle | Initialize decoder status and drain queued output after explicit input end | `3667d45`, `2ff5f5e` |
| Native audio | Connect the optional SPU2 mixer to the Windows output stream | `97519ea` |
| VU1 adapter | Add the optional kzvu reference interpreter and expose thread-bound session ownership for borrowing | `d59926d`, `6c94546` |
| EFU arithmetic | Correct the EATAN coefficient and reduce inputs before polynomial evaluation | `1e0ab41`, `396899e` |
| Scheduler | Skip pending-event scans when no work or deadline requires them, preserving deferred preemption | `dc70c62` |
| SDK matrix arithmetic | Use PS2 multiply/add semantics so exponent-255 values do not contaminate zero-weighted matrix components | `175f843` |

Translator changes require regenerating affected local guest sources; relinking
old generated code does not change their address semantics. Runtime-only changes
require rebuilding the affected library and consumers. Neither generated guest
code nor proprietary test data belongs in this repository.

## Optional VU1 backend

The built-in VU1 interpreter remains the default. To compile the optional kzvu
reference interpreter, configure `PS2X_ENABLE_KZVU_VU1=ON` with
`PS2X_KZVU_SOURCE_DIR` and the adapter's `KZVU_PCSX2_DIR`. Select it at game-thread
startup with the environment variable `PS2X_VU1_BACKEND=kzvu`.

The runtime owns the session on that thread. Consumers may borrow the owned
session through the existing runtime interface; they must not create a competing
owner or reuse it from another thread. The adapter transports VIF starts,
continuations, memory and ordered PATH1 output through the runtime boundary.

This public option selects the reference interpreter. It does not enable the
port's private dual-microVU-JIT experiments. Those CPU experiments use separate
precision and flag controls and still have unresolved cycle-accounting differences.
Their faster bounded game runs are not the fork's default behavior or a stable
60 Hz claim. See the port's
[vector qualification report](https://github.com/Hardnyx/TekkenTagPort/blob/main/docs/combat-vector-controls.md).

## Windows regression commands

Run from the fork root in an initialized MSVC x64 tools environment. These
commands build synthetic/component tests without a game image, generated game
code or BIOS. CMake may fetch build dependencies on a fresh setup.

Default backend, using NMake:

```powershell
cmake -S . -B out/fork-default -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_KZVU_VU1=OFF
cmake --build out/fork-default --target ps2x_tests
.\out\fork-default\ps2xTest\ps2x_tests.exe
```

For the optional backend, use a separate build directory and substitute your
local source paths. The compared adapter sources are KillzoneRecomp
`63917763d68532048edbabc9e7d6d01c9738e55a` and PCSX2
`646df006a4688063e56c0826e7ea4b8bb0c112c4`.

```powershell
cmake -S . -B out/fork-kzvu -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_KZVU_VU1=ON -DPS2X_KZVU_SOURCE_DIR="C:/sources/KillzoneRecomp/ext/kzvu" -DKZVU_PCSX2_DIR="C:/sources/pcsx2" -DKZVU_BUILD_TEST=OFF
cmake --build out/fork-kzvu --target ps2x_tests
.\out\fork-kzvu\ps2xTest\ps2x_tests.exe
```

Check each command's exit status before continuing. The source paths above are
examples, not installed dependencies or implicit revision updates.

## Verified checkpoint and remaining work

At code commit `175f843`, the default configuration passes 451 tests and the
optional-backend configuration passes 452. The two new SDK matrix regressions
fail before that arithmetic fix. Test counts describe this revision; future
changes may add tests. These local results do not substitute for CI or live
gameplay validation.

The port's visible matrix controls remove matched long fighter streaks and reach
short combat intervals on additional teams. Mesh, fog/lighting and texture errors
remain; a complete correctly timed fight is not verified. Performance comparisons
also retain substantial wall-time variability.

Next fork work must follow a demonstrated runtime discrepancy: resolve the VU
cycle contract before JIT promotion, attribute residual EE/clock/IOP cost before
another optimization, and add independent synthetic regressions for confirmed
arithmetic or transport defects. Keep one material change per control and retain
working defaults. Update this inventory when accepting another runtime change;
update the port checkpoint with its integration decision and visible evidence.

Do not commit the ISO, ELF, proprietary IRX, BIOS, generated game C++, memory
captures, compiled game runners or private launchers. Preserve dependency notices
and document source revisions when reusing adapter code.
