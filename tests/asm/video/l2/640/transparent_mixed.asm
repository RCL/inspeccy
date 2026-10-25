; l2_640 per-pixel transparency: left 5 pages opaque red, right 5 pages
; transparent. visible result: red left half, black fallback right half.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $20
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red (opaque)
	set_pal_entry 1, 2, $e3, $00	; transparent

	rept 5, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $11
	ldir
	endr
	rept 5, page
	set_nr $52, page_base + 5 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $22
	ldir
	endr

	set_nr $52, 4

	pass
