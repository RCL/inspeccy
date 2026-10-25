; bsra de,b (ed 29) - arithmetic shift de right by (b & 31) (sign-fill).
; spec: de := de >> (b & 31), bit 15 replicated. 8 cycles. flags unaffected.

	include "common.inc"

	ld de, $8000
	ld b, 1
	db $ed, $29	; bsra de, b
	check_de $c000

	ld de, $8000
	ld b, 16	; shift >= 16 -> all sign bits
	db $ed, $29
	check_de $ffff

	ld de, $4000	; positive -> zero-fill
	ld b, 1
	db $ed, $29
	check_de $2000

	pass
