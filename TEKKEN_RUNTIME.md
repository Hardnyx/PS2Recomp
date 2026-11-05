# Tekken runtime branch

This branch tracks runtime fixes maintained for TekkenTagPort. It is based on
upstream PS2Recomp commit `e5636981a1c9de368f6b71e819c3dc188dfedbf4`.
The upstream license and notices are retained in [LICENSE](LICENSE).

## Changes made on 5 November 2026

- Preserve constant VF0 across paired VU instructions.
- Saturate FMAC products before accumulation and retain sticky overflow.
- Expand VIF1 V2 and V3 UNPACK lanes before applying row and mask state.

The source patches and synthetic regressions are maintained in
[TekkenTagPort](https://github.com/Hardnyx/TekkenTagPort/tree/abc3e67bb2155ccd6937c77f75d1602a569e8ec9).
The three native probes pass in an isolated MSVC x64 runtime build. The probe
build disables FFmpeg and the debug UI. Game generation, integration, and
gameplay validation are performed separately in TekkenTagPort.
