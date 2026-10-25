; swapnib (ed 23) - swap high and low nibble of a.
; spec: a := (a << 4) | (a >> 4). 8 cycles. flags unaffected.

	include "common.inc"

	ld a, $ab
	db $ed, $23	; swapnib
	check_a $ba

	ld a, $00
	db $ed, $23
	check_a $00

	ld a, $f0
	db $ed, $23
	check_a $0f

	pass
