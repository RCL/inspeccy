; pixelad (ed 94) - hl := classic-ula pixel address for d=y, e=x.
; spec: hl = $4000 | ((y & $c0) << 5) | ((y & $07) << 8) | ((y & $38) << 2) | (x >> 3).
; 8 cycles. flags unaffected.

	include "common.inc"

; (y=0, x=0) -> $4000
	ld d, 0
	ld e, 0
	db $ed, $94
	check_hl $4000

; (y=1, x=0) -> $4100
	ld d, 1
	ld e, 0
	db $ed, $94
	check_hl $4100

; (y=191, x=255) -> $57ff (bottom-right pixel)
	ld d, 191
	ld e, 255
	db $ed, $94
	check_hl $57ff

	pass
