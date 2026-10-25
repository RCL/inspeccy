; l2_320 palette decoder: full-saturated blue via the 9th blue bit
; ($03 $01). whole 320x256 canvas should be pure blue.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $10
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $03, $01

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
