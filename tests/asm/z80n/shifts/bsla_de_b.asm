; bsla de,b (ed 28) - logical shift de left by (b & 31).
; spec: de := de << (b & 31). shifts of 16+ yield 0.
; 8 cycles. flags unaffected.

	include "common.inc"

	ld de, $0001
	ld b, 4
	db $ed, $28	; bsla de, b
	check_de $0010

	ld de, $1234
	ld b, 16
	db $ed, $28	; shift >= 16 -> all zero
	check_de $0000

	ld de, $0001
	ld b, 0	; shift by 0 -> unchanged
	db $ed, $28
	check_de $0001

	pass
