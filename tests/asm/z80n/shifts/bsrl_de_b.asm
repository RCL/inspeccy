; bsrl de,b (ed 2a) - logical shift de right by (b & 31) (zero-fill).
; spec: de := de >> (b & 31). 8 cycles. flags unaffected.

	include "common.inc"

	ld de, $8000
	ld b, 1
	db $ed, $2a	; bsrl de, b
	check_de $4000

	ld de, $ffff
	ld b, 4
	db $ed, $2a
	check_de $0fff

	pass
