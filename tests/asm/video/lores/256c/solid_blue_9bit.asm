; lores 256c palette decoder: verifies the 9th blue bit propagates.
; palette entry $03 $01 -> r=0 g=0 b=7 -> pure blue. a decoder that drops
; the lo byte would render dim blue ($db instead of $ff) or wrong colour.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $03, $01	; full-saturated blue via 9th bit
	set_nr $6a, $00	; 256c, palette offset 0
	set_nr $15, $80	; lores enable

	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 1
	ldir

	pass
