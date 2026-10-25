---
name: inspeccy-z80-layout
description: Inspect sjasmplus Z80 assembly layout, bounded routine bodies and declared bank-switching contracts with Inspeccy's assembly tools. Use when locating banked code, checking space or changing cross-bank calls; not as a generic assembly tutorial.
---

Locate the project's build command, assembler working directory, current SLD
files and assembly configuration. Respect its existing assembler and repository
workflow. These are commands of the `inspeccy` binary itself (`--asm-query`,
`--asm-layout`, `--asm-banks`, `--write-manifest`); they read the build's SLD
and sources and run nothing, so no emulator setup or interpreter is involved.

Use a bounded `--asm-query` for a routine and its direct callers/callees when
that answers the question. Read relevant source when the index reports missing
or unresolved information. Before allocating code or moving a routine, run
`--asm-layout` with the project's actual reservations. After assembly, verify the layout and
affected declared contracts. Read [command examples](references/assembly.md)
when needed.

Generate a build manifest only after successful assembly and packaging, pairing
the image and SLD from that build. Do not certify a stale image by hashing it
with today's unrelated symbols. Treat failed assembly output as incomplete.

Physical pages in these tools are 8K, including when the SLD describes 16K
banks. CPU addresses alone cannot identify banked code. Preserve aliases and
report ambiguous/unresolved symbols. Layout free space is measured against
declared limits; it is not a measured stack high-water mark. Keep assembler
assertions too.

Bank checks cover declared ranges and supported direct control/data accesses.
External contracts are assumptions, and unverified accesses remain unverified.
A clean report does not prove indirect pointer correctness, DMA ownership or
interrupt safety. Do not insert DI/EI or invent a bridge solely to satisfy the
checker; match the actual calling convention.

Report changed regions' used/free bytes, relevant contract findings and remaining
unknowns. Reuse existing configuration and boundary labels rather than duplicating
project layout facts inside this skill.
