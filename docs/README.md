# Inspeccy documentation

Start with the [project README](../README.md) to build Inspeccy and run a
sample program. The guides below cover the individual interfaces.

- [Operator guide](operator-guide.md): input formats, ROMs, timing, output,
  keyboard scripts, traces and the limits of dump/resume.
- [Batch experiments](batch.md): JSON steps for patches, runs, routine calls
  and assertions, including failure reports and exit codes.
- [Diagnostics](diagnostics.md): stopped-state reports, device queries and
  interrupt attribution.
- [Tools](../tools/README.md): build manifests, assembly analysis, Python
  adapters, captures and measurement helpers.
- [Trace viewer](../inspeccy-viewer/README.md): the optional Qt application.
- [Interactive emulator](../inspeccy-emul/README.md): SDL, handheld and browser
  builds.
- [Tests](../tests/README.md): running checks and maintaining references.
- [Test data](../tests/data/README.md): fixture sources and provenance.
- [Agent skills](../plugins/inspeccy-skills/README.md): optional installation
  for Codex and Claude Code.

Run `inspeccy --help` for the flags implemented by your executable and
`inspeccy --capabilities` for machine-readable diagnostic support.
