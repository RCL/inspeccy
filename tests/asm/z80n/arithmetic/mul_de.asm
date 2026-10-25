; mul d,e (ed 30) - unsigned 8x8 multiply, result in de.
; spec: de := d * e. 8 cycles. flags unaffected.

	include "common.inc"

	ld d, 5
	ld e, 7
	db $ed, $30	; mul d, e
	check_de 35

	ld d, $ff
	ld e, $ff
	db $ed, $30
	check_de $fe01

	ld d, 0
	ld e, $ff
	db $ed, $30
	check_de 0

	pass
