# Inspeccy experiment recipes

Substitute the project's actual artifact paths. These CLI commands work from
either agent's shell; check `--capabilities` before selecting newer operations.

```text
inspeccy --capabilities
inspeccy --nex output/program.nex --build-manifest output/program.build.json --batch experiment.json --batch-out result.json --no-dump --no-bmp --no-status
inspeccy --resume seed.json --build-manifest output/program.build.json --cycles 100000 --inspect-out stopped.json --no-dump --no-bmp --no-status
```

An ordinary run needs a duration or stop condition; a batch owns its per-step
budgets. `run.cycles` is an additional budget; `run.until_pc` optionally has
`until_page` to distinguish banks. A `call` pushes its `return_pc`; select a
valid stack and sentinel for the fixture rather than copying another program.

```json
{"version":1,"steps":[
  {"op":"read_value","symbol":"Counter","type":"u16le","name":"before"},
  {"op":"run","cycles":10000},
  {"op":"expect_value","symbol":"Counter","type":"u16le","value":10},
  {"op":"read_state","stack_words":8,"pc_bytes":16}
]}
```

All steps are validated before batch mutation. Exit 4 means invalid input;
exit 9 means execution/assertion failure. Read `error`, `failed_step` and
`failure`, including its bounded `state`, before escalating to a dump.
Observations carry step/name/cycles; `read_state` returns a `state` object.
`--inspect-out` writes a wrapper with `stop` and the same `state` object.

IRQ configuration (use the project's actual guard, not this illustrative name):

```json
{"version":1,"sources":["frame"],"max_events":64,"guards":[
  {"name":"update_busy","source":"frame","symbol":"UpdateGuard","type":"u8","idle_value":0}
]}
```

Pass `--irq-diagnostics irq.json --irq-diagnostics-out irq-result.json` with an
ordinary run or batch. For an EQU without a page, replace `symbol` with explicit
`page` and `offset` from the verified project mapping. Probes cannot cross 8K
boundaries. Omit `guards` to capture only expired pulses. `sources` accepts
frame/line; `max_events:0` retains aggregate counts only. Inspect `summary`,
`locations`, `events` and omitted/overflow counts. History starts afresh each
invocation, including when resuming a snapshot.

The optional Python adapter lives in Inspeccy's `tools/inspeccy_tools`
(installed at `share/inspeccy/tools`). `run_batch` accepts step dicts and
additional CLI flags in `extra`; snapshots require `dump=True`. The C++
interfaces work without the adapter.
