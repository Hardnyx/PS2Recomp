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
pins `97519ea4bb710e197e15962c02e1450eae53dddf` at this checkpoint.
Fork `main` contains 18 runtime/test commits beyond that pin, plus
later documentation-only updates. The six reusable runtime changes through
`175f843` have been reviewed separately; subsequent commits add bounded VU1
tests and the D/T interrupt integration. None of those commits is installed
automatically by the port, and publishing `main` does not replace an existing
patched runtime checkout. Some corrections, including
SDK matrix arithmetic, also have reversible port installers for those existing
checkouts. Do not apply such patches twice or discard local integration files
to reconcile different source revisions.

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
| VU1 cycle characterization | Add 17 synthetic cases for terminal-drain timing across NOP, IADDIU, LQ and ADD | `33ded8c`, `3909c55`, `ed06c15` |
| VU1 D/T interrupt propagation | Consume KZVU's VU1 interrupt latch and dispatch EE INTC cause 7 through the scheduler | `a65079b`, `597d0f6`, `946dd03`; Windows CI passed 453/455 |

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

The D/T interrupt integration consumes KZVU's pending VU1 interrupt
with `kzvuTakeInterrupt()` and forwards it as EE INTC cause 7 through the
scheduler. Its VIF1 MSCAL regression exercises D-bit and T-bit stops enabled,
plus a disabled-D control. Windows run `42574393855` passed 453/453 default
and 455/455 optional tests at candidate `946dd03`. This validates registered
scheduler-handler delivery, not raw INTC_STAT/INTC_MASK register semantics.

This public option selects the reference interpreter. It does not enable the
port's private dual-microVU-JIT experiments. Those CPU experiments use separate
precision and flag controls and still have unresolved cycle-accounting differences.
Their faster bounded game runs are not the fork's default behavior or a stable
60 Hz claim. See the port's
[vector qualification report](https://github.com/Hardnyx/TekkenTagPort/blob/main/docs/combat-vector-controls.md).

## VU1 cycle-accounting characterization

The 17-case regression at `ed06c15` compares the native interpreter with the
pinned KZVU/PCSX2 reference path. It covers NOP and IADDIU controls plus LQ and
ADD vector writes in four-pair and six-pair E-bit programs. The six-pair cases
issue at PCs 0, 8 and 16 to bracket the four-cycle writeback boundary.

GitHub Actions run `42572369030` passed 452/452 default tests and 454/454
optional-backend tests. The controls match. LQ and ADD show a one-cycle delta
only when the four-cycle writeback remains pending at terminal drain: in the
six-pair program, issue at PC 0 or 8 matches, while issue at PC 16 differs by
one cycle. The four-pair early and late cases also differ by one cycle.

This isolates an internal terminal-drain convention difference; it does not
prove which implementation matches physical hardware or that the delta is
guest-visible. Do not subtract a fixed cycle or promote a timing change without
an independent hardware or guest-observable contract.

Sony's `VU User's Manual` v3.1 (§3.4.4 and §3.5.3) independently documents
the FMAC M/T/X/Y/Z/S stages with writeback at S, the E-bit delay slot, and
in-flight FDIV/EFU operations continuing after termination. The timing
figures support a real terminal pipeline-completion requirement, but do not
by themselves establish the exact one-cycle delta in these tests. See the
[manual](https://www.manuallib.com/download/pdf0/VU-USER-MANUAL.PDF).

## Windows regression commands

Run from the fork root in an initialized MSVC x64 tools environment. These
commands build synthetic/component tests without a game image, generated game
code or BIOS. CMake may fetch build dependencies on a fresh setup.

Default backend, using NMake:

```powershell
cmake -S . -B out/fork-default -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_KZVU_VU1=OFF
cmake --build out/fork-default --target ps2x_tests
.\out\fork-default\ps2xTest\ps2x_tests.exe
```

For the optional backend, use a separate build directory and substitute your
local source paths. The compared adapter sources are KillzoneRecomp
`63917763d68532048edbabc9e7d6d01c9738e55a` and PCSX2
`646df006a4688063e56c0826e7ea4b8bb0c112c4`.

```powershell
cmake -S . -B out/fork-kzvu -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_KZVU_VU1=ON -DPS2X_KZVU_SOURCE_DIR="C:/sources/KillzoneRecomp/ext/kzvu" -DKZVU_PCSX2_DIR="C:/sources/pcsx2" -DKZVU_BUILD_TEST=OFF
cmake --build out/fork-kzvu --target ps2x_tests
.\out\fork-kzvu\ps2xTest\ps2x_tests.exe
```

Check each command's exit status before continuing. The source paths above are
examples, not installed dependencies or implicit revision updates.

## Verified checkpoint and remaining work

At cycle-test candidate `ed06c15`, GitHub Actions run `42572369030` passed
452 default tests and 454 optional-backend tests, including the 17-case
cycle-characterization matrix. D/T interrupt candidate `946dd03` passed
Windows run `42574393855`: 453/453 default and 455/455 optional tests.
TekkenTagPort repository checks passed in run `42574954782`. Counts refer
to exact tested revisions and should not be carried forward without rerunning
CI. These results do not substitute for live gameplay validation or a
physical-hardware timing capture.

The port's visible matrix controls remove matched long fighter streaks and reach
short combat intervals on additional teams. Mesh, fog/lighting and texture errors
remain; a complete correctly timed fight is not verified. Performance comparisons
also retain substantial wall-time variability.

Next fork work must follow a demonstrated runtime discrepancy: add
cross-backend MSCNT continuation and budget-exhaustion tests, characterize
FDIV/EFU terminal completion, and audit LQ/SQ hazard classification against
the VU manual. The scheduler test does not model raw INTC_STAT/INTC_MASK
registers. Keep the built-in interpreter as the default; do not promote a JIT
or timing change based only on synthetic parity. Update this inventory when
accepting another runtime change; update the port checkpoint with its
integration decision and visible evidence.

Do not commit the ISO, ELF, proprietary IRX, BIOS, generated game C++, memory
captures, compiled game runners or private launchers. Preserve dependency notices
and document source revisions when reusing adapter code.
