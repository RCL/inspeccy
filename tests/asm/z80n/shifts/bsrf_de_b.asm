; bsrf de,b (ed 2b) - "shift right and fill" - shift de right and one-fill
; from the top.
; spec: de := (de >> (b & 31)) | (top-n bits all set).
; shift 0 leaves de unchanged. 8 cycles. flags unaffected.

	include "common.inc"

	ld de, $0000
	ld b, 1
	db $ed, $2b	; bsrf de, b
	check_de $8000

	ld de, $1234
	ld b, 0	; no shift -> unchanged
	db $ed, $2b
	check_de $1234

	ld de, $0000
	ld b, 16	; shift >= 16 -> all ones
	db $ed, $2b
	check_de $ffff

	pass
