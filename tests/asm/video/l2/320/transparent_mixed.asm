; l2_320 per-pixel transparency: left 160 columns opaque red, right 160
; transparent. with the column-major layout, this is "pages 18..22 use
; opaque-red index, pages 23..27 use transparent index".

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $10
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red (opaque)
	set_pal_entry 1, 2, $e3, $00	; transparent (hi == nr_14)

; first 5 pages -> red.
	rept 5, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

; last 5 pages -> transparent.
	rept 5, page
	set_nr $52, page_base + 5 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 2
	ldir
	endr

	set_nr $52, 4

	pass
