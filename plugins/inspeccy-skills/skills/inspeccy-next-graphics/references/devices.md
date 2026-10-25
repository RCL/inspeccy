# Compact device queries

Place operations inside `{"version":1,"steps":[...]}` and run with `--batch`.
`read_device` returns `device_state`; `expect_device` reports bounded mismatches.
Use the project's fixture and `--no-dump` when the final state need not be resumed.

```json
{"op":"read_device","device":"palette","layer":"layer2","bank":1,"start":0,"count":16}
{"op":"expect_device","device":"palette","layer":"layer2","bank":1,"start":0,"values":[0,511]}
{"op":"read_device","device":"sprite_attrs","start":0,"count":8}
{"op":"read_device","device":"sprite_pattern","slot":3,"offset":0,"length":256}
{"op":"expect_device","device":"sprite_attrs","start":0,"hex":"2020008000"}
{"op":"read_device","device":"ay","chip":0,"start":0,"count":16}
```

Palette layers: `ula`, `layer2`, `sprites`, `tilemap`; banks: 1/2. Default
`rgb333` encoding returns `values` in 0..511. `encoding:"raw"` returns `hex`,
two bytes per entry. RGB333 is `(byte0 << 1) | (byte1 & 1)`; other byte1 bits
remain raw flags. Snapshot palette IDs follow NR43 bits 6:4: ULA-1, L2-1,
Sprites-1, Tilemap-1, then those four layers' second banks.

Attributes contain five stored bytes for each of 128 slots, including the stored
fifth byte for four-byte sprites. Pattern slots are 0..63 with 256 bytes each;
queries stay inside one pattern. Sprite results include upload counters/cursors,
allowing before/after checks for unintended writes.

Expectations infer length: `values` for RGB333 and AY, `hex` for raw palette or
sprites, without count/length. Raw expectations must contain complete entries.
AY chips are 0/1; chip 1 requires `--ts`, and Next guest code must enable NR08's
TurboSound gate to switch chips. `enabled` describes global AY emulation;
`latch` and `active_chip` describe stored selection. An absent device is an
error, not a set of zeros.
