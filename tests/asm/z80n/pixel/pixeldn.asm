; pixeldn (ed 93) - hl := next pixel down in the classic 256x192 ula layout.
; spec (ula address encoding):
;   h = 0 1 0 y7 y6 y2 y1 y0
;   l = y5 y4 y3 x4 x3 x2 x1 x0
; pixeldn advances y by 1 propagating carry through the three y sub-fields.
; 8 cycles. flags unaffected.

	include "common.inc"

; within-char-row advance: bump h[2..0].
	ld hl, $4000
	db $ed, $93
	check_hl $4100

; char-row advance: y[2..0] was already 7, so it wraps to 0 and y[5..3]
; (= l[7..5]) bumps from 0 to 1 (= l += $20).
	ld hl, $4700
	db $ed, $93
	check_hl $4020

; third-of-screen advance: y[2..0]=7 and y[5..3]=7, so y[7..6] (= h[4..3])
; bumps. from $47e0: h[2..0] -> 0, l[7..5] -> 0, h[4..3] += 1.
; $47e0 -> $4800.
	ld hl, $47e0
	db $ed, $93
	check_hl $4800

	pass
