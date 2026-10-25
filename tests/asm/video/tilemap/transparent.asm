; tilemap transparency + u/l compositing. layer 2 (256x192) is solid red beneath
; the tilemap. tile 0 is fully transparent (nibble 0 = the $4c default transparent
; index), tile 1 is solid green (nibble 1 -> palette 3 entry 1 = green). the map
; is all tile 0 except cell (2,2) = tile 1. so the whole l2 area shows red through
; the transparent tilemap, and one 8x8 cell at composite (16,16)-(23,23) is green.
; verified programmatically.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00	; l2 256x192
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 1, $e0, $00	; l2 palette 1 entry 1 = red
	set_pal_entry 3, 1, $1c, $00	; tilemap palette 3 entry 1 = green ($1c = 000'111'00)

; fill l2 with index 1 (red).
	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

; tile defs + map in bank 5 high page.
	set_nr $52, 11
; tile 0: all nibble 0 (transparent). $00 x 32 at $4000.
	ld hl, $4000
	ld de, $4001
	ld bc, 31
	ld (hl), $00
	ldir
; tile 1: all nibble 1 (green). $11 x 32 at $4020.
	ld hl, $4020
	ld de, $4021
	ld bc, 31
	ld (hl), $11
	ldir
; map: all tile 0 (transparent), attr 0.
	ld hl, $5000
	ld de, $5001
	ld bc, 2559
	ld (hl), $00
	ldir
; cell (2,2): entry offset = (2*40 + 2)*2 = 164 -> tile index byte at $5000+164.
	ld hl, $5000 + 164
	ld (hl), $01	; tile 1 (green) at cell (2,2)
	set_nr $52, 4

	set_nr $6f, $20
	set_nr $6e, $30
	set_nr $6b, $80	; tilemap enable, palette 3

	pass
