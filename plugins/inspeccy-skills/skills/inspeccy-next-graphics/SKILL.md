---
name: inspeccy-next-graphics
description: Diagnose Spectrum Next palette, transparency, sprite and layer-composition problems using Inspeccy device observations and reproducible captures. Use for wrong colours, missing or stale sprites, and preview-versus-game discrepancies.
---

Start from the visual symptom and the project's actual asset pipeline. Identify
whether the suspect image is a source asset, a conversion preview or captured
emulator output. Preview rendering can be wrong even when indexed bytes and the
hardware palette are correct. Keep original source assets unless their deletion
was requested.

Locate Inspeccy and check `--capabilities`. For a state question, use native
`read_device` or `expect_device` for relevant palette entries, sprite attributes
or pattern bytes. Read [device recipes](references/devices.md) for selectors and
encodings. Use the build manifest to bind symbols. Inspect state at a known
completed update boundary: half-finished uploads can resemble missing sprites.
A stopped state represents that instant; a completed-frame capture may involve
replay. State which evidence you used.

Choose palette layer/bank explicitly. Distinguish RGB333 from two-byte hardware
encoding and retain raw flags when investigating priority. Do not assume index
zero is transparent in every asset or display mode. Inspect raw sprite attributes
and patterns before interpreting transforms/scaling, using the project's validated
hardware reference for bit definitions. Pattern slots and attribute slots have
different limits. Consider scanline rendering limits for overlap-dependent
symptoms, without treating a sprite count alone as proof of hardware behaviour.

Device queries do not emulate port reads or disturb upload cursors. For layer
ordering, blending, scrolling or final appearance, reproduce through real input
and obtain a fresh pixel-accurate capture. Compare it with device observations.
Reuse the project's converter for asset changes and inspect the regenerated
preview before deciding whether game code also needs correction.

Return the concrete mismatch, relevant palette/slot/byte selection, and capture
or reproducible input sequence. Distinguish emulator evidence from confirmed
hardware behaviour.
