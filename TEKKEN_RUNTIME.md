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
Fork `main` contains additional runtime corrections, component tests and
documentation beyond that pin. The reusable changes through `175f843` have
separate integration evidence; subsequent commits cover bounded VU1 execution,
D/T interrupt propagation and SDK vector semantics. None of those commits
is installed automatically by the port, and publishing `main` does not
replace an existing patched runtime checkout. Some corrections, including
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
| SDK XYZ vector arithmetic | Compute XYZ-only inner products and normalization, clear W, normalize small nonzero directions | `81784b0`; five regressions passed under both configurations, one unrelated KZVU state-sync failure in initial run |
| SDK matrix composition | Apply the second source matrix before the first for the SDK's column-vector composition ABI | `4f67469` (Windows CI pending) |
| VU1 cycle characterization | Add 17 synthetic cases for terminal-drain timing across NOP, IADDIU, LQ and ADD | `33ded8c`, `3909c55`, `ed06c15` |
| VU1 D/T interrupt propagation | Consume KZVU's VU1 interrupt latch and dispatch EE INTC cause 7 through the scheduler | `a65079b`, `597d0f6`, `946dd03`; Windows CI passed 453/455 |

Translator changes require regenerating affected local guest sources; relinking
old generated code does not change their address semantics. Runtime-only changes
require rebuilding the affected library and consumers. Neither generated guest
code nor proprietary test data belongs in this repository.

## SDK vector and matrix helpers

Commit `81784b0` moves two general VU SDK corrections from the port's
reversible source patches to `Kernel/Stubs/VU.cpp`. `sceVu0InnerProduct`
uses the X/Y/Z lanes only. `sceVu0Normalize` derives length from X/Y/Z,
normalizes any nonzero XYZ vector and clears W. Five component tests in
`ps2xTest/src/ps2_vu_tests.cpp` cover nonzero W, zero XYZ, small nonzero
vectors, in-place output and the raw returned scalar bits. The preceding
implementation included W in the inner product and normalized all four
components using a `1.0e-6f` cutoff, so these cases distinguish the change
from its previous behavior.

The initial Windows run at this source revision passed all five new
SDK tests in both configurations. Its optional job was not green because
an earlier KZVU state adapter failed the separate TOP/ITOP regression.
Fix `8a81b57` mirrors committed VI registers and current TOP/ITOP into
the public VU1 state at each slice boundary; the corresponding Windows
run is pending.

Commit `4f67469` also migrates the port's matrix composition correction.
The SDK's `sceVu0MulMatrix(dst, m0, m1)` composes transformations as
`m1*m0` under the internal row-vector multiply; composing `m0*m1`
reversed noncommuting operations. Two additional regressions compare
successive `sceVu0ApplyMatrix` calls and products with overlapping
destinations. The earlier unit test expecting `m0*m1` was updated to
express the corrected contract. This revision is under Windows CI.

Port installers `fix_vu_inner_product.ps1`,
`fix_vu_normalize.ps1` and `fix_vu_matrix_order.ps1` remain necessary
for the currently pinned engine. Do not apply them to source that already
contains these changes, and do not retire them until a clean integrated
dependency and the game's visual controls are validated. No normal
Tekken runner was rebuilt by these fork commits.

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

## Bounded VU1 execution and MSCNT

The adapter now distinguishes a completed microprogram from a cycle-budget
boundary with `VU1State::budgetExhausted`. Native execution retains its
instruction PC and in-flight pipelines; the reference backend uses
`kzvuContinue()` when its previous execution is still running. Calling
`kzvuExecute()` on that path would finish the previous program before
starting another and defeat the requested slice budget.

The synchronous VIF1 MSCAL/MSCNT integration continues in 65,536-cycle slices,
with a maximum of 1,048,576 allocated cycles before reporting an error.
This is a bounded, synchronous compatibility path, not EE/VU overlap or
cycle-accurate concurrency. VU data writes are copied at reference-backend
slice boundaries without overwriting its in-flight state.

Candidate `4b8f4f9` passed [Windows run 42577229400](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42577229400):
453/453 default tests and 456/456 optional-backend tests. The regressions
cover native and reference MSCNT state retention, E-bit delay-slot PC,
one-cycle budget slices, and completion of a pending FMAC output.
These tests do not establish a hardware timing rule for simultaneous EE,
VIF1 and VU1 execution.

A subsequent scalar-characterization test at `d5efa4b` examines FDIV Q
and EFU P across E-bit termination and an explicit WAITQ/WAITP program
started by MSCNT. Test commit `eb4785e` adds two LQ/SQ controls: same-pair
reads of the prior VF value and next-pair reads of the pending LQ result.
These compare the native register and memory outputs against the pinned
KZVU/PCSX2 interpreter without asserting identical elapsed cycles.

PCSX2 classifies LQ/SQ as `VUPIPE_FMAC` for register-hazard checks; the
native interpreter uses `PipelineLsu` with per-component VF availability.
The classification difference is not, by itself, a reproduced defect.
GitHub Actions runs `42664819709` and `42666102235` passed
respectively: 453/457 and 453/458 default/optional tests. FDIV Q and ESADD P
returned matching final scalar results, with a one-cycle terminal difference.
The LQ/SQ register and memory tests also matched, while the two compound
programs differed by one or two elapsed cycles.

A six-case control matrix at `4d9a627` passed run `42669177529`:
453/453 default and 459/459 optional tests. Its observed cycle counts were
NOP 4/4, LQ 4/5, SQ 4/5, ADD 4/5, independent LQ + ADD 4/5,
and dependent LQ -> SQ 7/9 (native/KZVU). This isolates a distinct
load-to-store dependency cost in addition to the one-cycle difference of
individual operations, without identifying which clock convention is
hardware-correct.

Fork test `aa014d3` passed [run 42676206344](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42676206344)
(453 default / 460 optional tests). With E-bit at PC 64 and LQ at PC 0,
distances 1 through 5 pairs to SQ had identical native/KZVU cycle counts.
At SQ PCs 48 and 56, reference completion took one and two extra cycles,
respectively. This suggested terminal draining rather than an issue stall.

Fork test `5b48458` repeated the ten controls with a second E-bit at
PC 96, yielding 20 programs. [Run 42677306854](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42677306854)
passed 453 default / 460 optional tests. With the later E-bit, all seven
LQ-to-SQ distances, including PCs 48/56, had identical cycle totals and
output data in both backends. The remaining discrepancy is localized to
short-program terminal completion; hardware-correct accounting remains
unproven.

Fork test `1c80a64` also passed [run 42677768061](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42677768061)
(454 default / 461 optional tests). An MSCAL microprogram lacking any
E-bit must terminate with a synchronous-budget error after exactly
1,048,576 cycles. Both backends retain `budgetExhausted = true`, rather
than reporting a completed program.

Fork test `89ee955` passed [run 42678754093](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42678754093)
(455 default / 462 optional tests). An MSCAL and MSCNT in a single VIF1
command stream preserve VI state, store it to VU memory and advance TPC
through both E-bit delay slots. Test `35263b3` extends this to BASE,
OFFSET, TOP/TOPS, ITOP and DBF through a double-buffered pair of VU
programs; [run 42681060581](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42681060581)
is pending. Neither test validates asynchronous EE/VU execution, raw DMA
stall flags, or physical-hardware timing.

## EE interrupt register semantics

Fork candidate `d83d2d4` implements two guest-visible INTC register rules:
`INTC_STAT` (`0x1000F000`) acknowledges pending sources on write-one,
and `INTC_MASK` (`0x1000F010`) toggles its low sixteen mask bits on
write-one. Sources latch independently of the mask. Follow-up fork commit `93e2eec`
centralizes this step in `EeScheduler::dispatchIrq`: every non-DMAC cause,
including a VU1 D/T stop, enters INTC_STAT before the scheduler's
syscall-level cause filter. DMAC interrupts retain their separate status. A PS2Memory regression covers independent
pending causes, selective acknowledgement, repeated mask toggles and
reserved high bits; the runtime VU1 interrupt regression checks the
enabled/disabled D/T cases and guest acknowledgement.

The register behavior follows the fixed PCSX2 reference source
`pcsx2/HwWrite.cpp` at `646df006a4688063e56c0826e7ea4b8bb0c112c4`.
The initial candidate is checked by [run 42694902358](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42694902358).
The scheduler follow-up is pinned in TekkenTagPort `125a382` for
[run 42695742902](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42695742902).

This does not connect raw INTC_MASK to registered-handler delivery,
cover sources that do not reach the EE scheduler's dispatch boundary,
or establish asynchronous VIF1/VU1 timing. The default handler path
continues to use the scheduler's syscall-level cause mask. Do not
treat the MMIO test as full interrupt-controller compatibility.

### Hardware and SDK mask ownership

Candidate `49e57dd` removes the duplicate scheduler-only EE INTC cause mask.
`EeScheduler::dispatchIrq` now reads the guest-visible sixteen-bit
`INTC_MASK` (`0x1000F010`) before queuing a non-DMAC handler; its
`INTC_STAT` source remains latched even when masked. The mask starts at
zero after memory initialization. `EnableIntc` and `DisableIntc` modify
the same hardware register idempotently: they read the current bit and
write it only if a change is needed, because the register toggles on
write-one. Direct guest MMIO writes are therefore effective immediately
at subsequent interrupt dispatch boundaries.

A dedicated scheduler test covers repeated SDK calls, mixed SDK/MMIO
mask changes, independent sources, a distinct DMAC cause filter, and
VBlank delivery both masked and enabled. Existing VBlank, VU1 D/T and
Timer2 handler fixtures explicitly enable their sources rather than
relying on a permissive scheduler default. TekkenTagPort commit
`20fc089` pins this candidate in
[Windows run 42707197853](https://github.com/Hardnyx/TekkenTagPort/actions/runs/42707197853).

This is the synchronous scheduler's delivery gate, not a full EE
interrupt CPU model. A status bit already latched while masked does not
yet schedule a new handler solely because the guest unmasks it;
repeated triggers and CPU interrupt entry/return ordering still need
independent qualification. Do not claim physical-hardware timing or
complete interrupt compatibility from these component tests.

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
The resumable-budget candidate `4b8f4f9` passed run `42577229400`:
453/453 default and 456/456 optional tests. The scalar and isolated LQ/SQ
matrix at `4d9a627` passed run `42669177529`, with 453/453 default and
459/459 optional tests. Counts refer to exact tested revisions and should
not be carried forward without rerunning CI. These results do not substitute
for live gameplay validation or a physical-hardware timing capture.

The port's visible matrix controls remove matched long fighter streaks and reach
short combat intervals on additional teams. Mesh, fog/lighting and texture errors
remain; a complete correctly timed fight is not verified. Performance comparisons
also retain substantial wall-time variability.

Next fork work must follow a demonstrated runtime discrepancy: validate
FDIV/EFU completion at the E-bit boundary, compare LQ/SQ register hazards
and same-pair overlap, and test how VU busy state, DMA and VIF1 progress
are exposed to the EE. Raw INTC_STAT acknowledgment, mask toggling and scheduler-level source
latching now have bounded tests, but the raw mask is not wired to
scheduler handler delivery. Keep the built-in interpreter as the
default; do not promote a JIT or timing change based only on synthetic
parity. Update the port checkpoint with the integration decision and evidence.

Do not commit the ISO, ELF, proprietary IRX, BIOS, generated game C++, memory
captures, compiled game runners or private launchers. Preserve dependency notices
and document source revisions when reusing adapter code.
