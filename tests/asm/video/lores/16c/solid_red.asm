; lores 16c basic rendering: every byte = $11 -> nibble pair (1, 1) ->
; both pixels at palette index 1. palette 0 entry 1 set to red. whole
; 256x192 (= 128x96 scaled 2x) area must be solid red.
;
; what this verifies:
;   - nr_15 bit 7 enables lores
;   - nr_6a bit 5 = 1 selects 16-colour mode (2 pixels / byte, nibble-packed)
;   - compositor reads 6144 contiguous bytes from $4000 (= bank-5 $0000),
;     not the split 256c layout.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_nr $6a, $20	; bit 5 = 1 -> 16c, palette offset 0
	set_nr $15, $80	; lores enable

; 16c layout: 6144 contiguous bytes at $4000. each byte = (high, low)
; nibble pair. fill with $11 so both nibbles resolve to index 1.
	ld hl, $4000
	ld de, $4001
	ld bc, 6143
	ld (hl), $11
	ldir

	pass
