; l2_640 transparency: fill with $11, palette entry 1 set to transparent.
; whole canvas should show black fallback.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $20
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e3, $00	; hi == nr_14 -> transparent

	rept 10, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $11
	ldir
	endr

	set_nr $52, 4

	pass
