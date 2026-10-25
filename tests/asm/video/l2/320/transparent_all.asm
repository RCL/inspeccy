; l2_320 transparency: fill the whole framebuffer with a palette index
; whose entry hi byte matches nr_14. every l2 pixel is transparent, so
; the whole 320x256 canvas shows black fallback (no centring, so no border
; strip - just the canvas-fill colour everywhere).

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula	; nr_14 := $e3
	set_nr $70, $10
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e3, $00	; palette entry's hi matches nr_14 -> transparent

	rept 10, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

	set_nr $52, 4

	pass
