; nextreg rr,nn (ed 91 rr vv) - write immediate vv to nextreg rr.
; spec: nextreg[rr] := vv. 20 cycles. flags unaffected.
;
; we read back via the standard $243b / $253b port path.

	include "common.inc"

; pick a benign next register that has no side effects when written:
; nr_5f = "scratchpad". set it via nextreg immediate, then read back
; via ports.
nr_scratch equ $5f

	db $ed, $91, nr_scratch, $77	; nextreg $5f, $77

	ld bc, $243b
	ld a, nr_scratch
	out (c), a
	ld bc, $253b
	in a, (c)
	check_a $77

	pass
