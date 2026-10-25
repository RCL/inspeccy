---
name: inspeccy-debug
description: Reproduce and diagnose ZX Spectrum or Spectrum Next program failures with Inspeccy, using bounded batch experiments, matching build symbols and interrupt observations. Use for emulator state tests, stopped Z80 state or missed updates, rather than generic C++ debugging.
---

Use the installed Inspeccy CLI and the project's build/fixture configuration.
These instructions work in Claude Code and Codex; they require local execution
access to the executable and program artifacts. They do not assume a particular
game, filesystem location or version-control system.

Locate the project's Inspeccy executable from its configuration, build output or
PATH, and run `inspeccy --capabilities`. If that flag is unavailable, consult that
binary's help and use only supported operations. Locate the actual image, build
manifest and an appropriate input/state fixture before interpreting addresses.
Prefer an existing project fixture over recreating its title/menu navigation.

Use the narrowest experiment that can settle the question: batch memory/CPU
assertions for logic, device observations for hardware state, a stopped-state
report for mapping/stack context, IRQ diagnostics for missed pulses or a busy
reentry guard. Read [the CLI recipes](references/experiments.md) for syntax.
Batch dependent steps into one execution; request a dump when the next experiment
needs to resume or branch from that state. Retain full traces as files and query
the relevant span when compact reports are insufficient.

Bind symbolic operations to a manifest produced from one successful build. A
stale-state error is evidence to repair the fixture/build pairing. Use the
explicit mismatch override only for an intentional experiment and explain it.
Physical pages are 8K; a snapshot's `bank_N` is 16K. Address-only EQU symbols do
not supply a physical page. Do not guess a page for banked or self-modifying code.

A stack word resembling a label is only a candidate address. Current mappings
do not establish the mapping at a previous call. IRQ expiry means the CPU did
not accept a pulse; `guard_busy_at_accept` means it did accept one while the
configured memory value was non-idle. The latter does not by itself prove the
guest skipped an update. Event PCs identify the observed instruction boundary.

Put generated plans, reports and VFS state in a task output directory. Choose an
isolated VFS for experiments that may write game configuration or records. End
with the observed mismatch/location, the command or saved plan that reproduces
it, relevant build identity and the limits of what the experiment establishes.
Use a fresh pixel-accurate replay when the question needs visual evidence.
