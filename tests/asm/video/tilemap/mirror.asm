; tilemap per-tile x-mirror (attribute bit 3). tile 0 is red on its left half
; (columns 0-3, nibble 1) and blue on its right half (columns 4-7, nibble 2).
; map cell (0,0) uses attribute $00 (normal) -> red|blue; cell (1,0) uses $08
; (x-mirror) -> blue|red. so the first two 8-pixel cells read red,blue | blue,red.
; verified programmatically.

	include "common.inc"

	silence_ula
	set_pal_entry 3, 1, $e0, $00	; red
	set_pal_entry 3, 2, $03, $01	; blue

	set_nr $52, 11
; tile 0: each of the 8 rows = 11 11 22 22 (cols 0-3 nibble1, 4-7 nibble2).
	ld hl, $4000
	ld b, 8
.row:
	ld (hl), $11
	inc hl
	ld (hl), $11
	inc hl
	ld (hl), $22
	inc hl
	ld (hl), $22
	inc hl
	djnz .row

; map: cell(0,0)= tile0 attr $00; cell(1,0)= tile0 attr $08 (x-mirror);
; fill the rest with tile0 attr $00 so only the first two cells differ.
	ld hl, $5000
	ld (hl), $00
	inc hl
	ld (hl), $00	; cell 0 normal
	inc hl
	ld (hl), $00
	inc hl
	ld (hl), $08	; cell 1 x-mirror
	inc hl
; remaining cells: tile0 attr0 (repeat 2-byte pattern)
	ld (hl), $00
	inc hl
	ld (hl), $00
	ld hl, $5004
	ld de, $5006
	ld bc, 2554
	ldir
	set_nr $52, 4

	set_nr $6f, $20
	set_nr $6e, $30
	set_nr $6b, $80

	pass
