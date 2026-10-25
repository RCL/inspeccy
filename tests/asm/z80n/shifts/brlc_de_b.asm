; brlc de,b (ed 2c) - rotate de left by (b & 15) positions.
; spec: de := rotl(de, b & 15). 8 cycles. flags unaffected.

	include "common.inc"

	ld de, $8001
	ld b, 1
	db $ed, $2c	; brlc de, b
	check_de $0003

	ld de, $1234
	ld b, 0
	db $ed, $2c
	check_de $1234

	ld de, $1234
	ld b, 16	; & 15 -> 0, no rotation
	db $ed, $2c
	check_de $1234

	pass
