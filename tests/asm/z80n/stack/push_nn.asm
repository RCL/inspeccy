; push nn (ed 8a nh nl) - push 16-bit big-endian immediate to stack.
; spec: sp -= 2; [sp] = nl; [sp+1] = nh. 23 cycles. flags unaffected.
; note: immediate is big-endian in the instruction stream (unique to z80n).

	include "common.inc"

	ld sp, $c100
	db $ed, $8a, $12, $34	; push $1234 (be: nh=$12 nl=$34)
; expect sp = $c0fe, byte at c0fe = $34, byte at c0ff = $12

; pop into hl and check
	pop hl
	check_hl $1234

; sp should have advanced back to $c100
	ld hl, 0
	add hl, sp
	check_hl $c100

	pass
