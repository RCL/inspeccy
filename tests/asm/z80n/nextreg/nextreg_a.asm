; nextreg rr,a (ed 92 rr) - write a to nextreg rr.
; spec: nextreg[rr] := a. 17 cycles. flags unaffected.

	include "common.inc"

nr_scratch equ $5f

	ld a, $aa
	db $ed, $92, nr_scratch	; nextreg $5f, a

	ld bc, $243b
	ld a, nr_scratch
	out (c), a
	ld bc, $253b
	in a, (c)
	check_a $aa

	pass
