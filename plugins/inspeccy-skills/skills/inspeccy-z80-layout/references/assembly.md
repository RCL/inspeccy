# Assembly commands

Supply the assembler's working directory as `--source-root`; SLD paths can be
relative to it. Substitute the installed helper and actual project paths.

```text
inspeccy --asm-query Routine --sld output/program.sld --source-root code --limit 40 --json
inspeccy --asm-layout --sld output/program.sld --source-root code --asm-config assembly.json --json
inspeccy --asm-banks --sld output/program.sld --source-root code --asm-config assembly.json --json
inspeccy --write-manifest output/program.build.json --image output/program.nex --sld output/program.sld
```

Without `--json` each of the first three prints a plain table instead.

Example configuration, not a default memory layout:

```json
{"version":1,"regions":[
  {"name":"Resident","page":5,"start":40960,"end":"CodeEnd","limit":"STACK_TOP-256"}
],"fixed":{"4":4,"5":5},"routines":{
  "Gate":{"end":"GateEnd","requires":{"7":31},"returns":{"7":31}},
  "Target":{"external":true,"requires":{"7":30},"returns":{"7":30},"clobbers":[6]}
}}
```

Routine ends are exclusive. `requires` sets entry mappings; `returns` states
return mappings, or `"entry"` for the mapping at call entry. `clobbers` makes a
slot unknown. Slots absent from returns and clobbers are promised preserved.
Use the code's actual contract. Execution-context annotations reject incompatible
annotated calls, not all reentry.

Queries report truncation and unresolved targets. Bank reports separate errors,
unverified cases and coverage/assumptions. `--asm-banks` exits 1 for reported
errors; exit 2 means invalid inputs or unavailable source data.
