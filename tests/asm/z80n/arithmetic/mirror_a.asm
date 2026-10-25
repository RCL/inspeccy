; mirror a (ed 24) - bit-reverse a.
; spec: a := reverse_bits(a). 8 cycles. flags unaffected.

	include "common.inc"

	ld a, $12	; 0b00010010
	db $ed, $24	; mirror a
	check_a $48	; 0b01001000

	ld a, $81	; palindrome
	db $ed, $24
	check_a $81

	ld a, $ff
	db $ed, $24
	check_a $ff

	ld a, $00
	db $ed, $24
	check_a $00

	pass
