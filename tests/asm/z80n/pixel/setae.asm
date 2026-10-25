; setae (ed 95) - a := 1 << (7 - (e & 7)). builds a single-pixel bitmask.
; spec: a = $80 >> (e & 7). 8 cycles. flags unaffected.

	include "common.inc"

	ld e, 0
	db $ed, $95
	check_a $80

	ld e, 7
	db $ed, $95
	check_a $01

	ld e, $15	; & 7 = 5
	db $ed, $95
	check_a $04

	pass
