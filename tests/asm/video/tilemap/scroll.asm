; tilemap x scroll ($2f:$30). same red/blue vertical 8-pixel stripes as basic,
; scrolled right by 4 via nr_30 = 4. the tilemap samples (x + 4), so the red/blue
; boundary moves left by 4: composite x 0..3 red, 4..11 blue, 12..19 red, ...
; verified programmatically.

	include "common.inc"

	silence_ula
	set_pal_entry 3, 1, $e0, $00
	set_pal_entry 3, 2, $03, $01

	set_nr $52, 11
	ld hl, $4000
	ld de, $4001
	ld bc, 31
	ld (hl), $11
	ldir
	ld hl, $4020
	ld de, $4021
	ld bc, 31
	ld (hl), $22
	ldir
	ld hl, $5000
	ld (hl), $00
	inc hl
	ld (hl), $00
	inc hl
	ld (hl), $01
	inc hl
	ld (hl), $00
	ld hl, $5000
	ld de, $5004
	ld bc, 2556
	ldir
	set_nr $52, 4

	set_nr $6f, $20
	set_nr $6e, $30
	set_nr $6b, $80
	set_nr $30, 4	; tilemap x scroll lsb = 4

	pass
