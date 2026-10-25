; tilemap text mode ($6b bit 3): 1bpp glyphs, 8 bytes per tile. tile 0's rows are
; $aa (10101010) -> pixel columns 0,2,4,6 = ink, 1,3,5,7 = paper. paper (pixel 0)
; is transparent ($4c default 0), so it falls through to the (white) border; ink
; (pixel 1) -> palette entry (offset<<1)|1. attribute $02 -> offset 1 -> entry 3 =
; red. result: red/white vertical 1-pixel stripes. verified programmatically.

	include "common.inc"

	silence_ula
	set_pal_entry 3, 3, $e0, $00	; tilemap palette 3 entry 3 = red

	set_nr $52, 11
; text tile 0: 8 rows of $aa (ink at columns 0,2,4,6).
	ld hl, $4000
	ld de, $4001
	ld bc, 7
	ld (hl), $aa
	ldir
; map: every cell = tile 0, attribute $02 (7-bit palette offset 1).
	ld hl, $5000
	ld (hl), $00	; tile 0
	inc hl
	ld (hl), $02	; attr: offset 1
	ld hl, $5000
	ld de, $5002
	ld bc, 2558
	ldir
	set_nr $52, 4

	set_nr $6f, $20
	set_nr $6e, $30
	set_nr $6b, $88	; enable + text mode (bit 3), palette 3

	pass
