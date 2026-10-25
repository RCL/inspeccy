; lores 16c palette decoder: every nibble = 1, palette entry 1 set to
; full-saturated blue using the 9th blue bit ($03 $01).

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $03, $01
	set_nr $6a, $20	; 16c
	set_nr $15, $80

	ld hl, $4000
	ld de, $4001
	ld bc, 6143
	ld (hl), $11
	ldir

	pass
