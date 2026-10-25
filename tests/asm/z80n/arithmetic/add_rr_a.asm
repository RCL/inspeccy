; add hl,a / add de,a / add bc,a  (ed 31 / 32 / 33).
; spec: rr := rr + (unsigned a). 8 cycles. flags unaffected.

	include "common.inc"

; add hl,a
	ld hl, $1000
	ld a, $20
	db $ed, $31	; add hl, a
	check_hl $1020

	ld hl, $ffff
	ld a, $01
	db $ed, $31	; wraps to 0
	check_hl $0000

; add de,a
	ld de, $2000
	ld a, $05
	db $ed, $32	; add de, a
	check_de $2005

; add bc,a
	ld bc, $3000
	ld a, $80
	db $ed, $33	; add bc, a
	check_bc $3080

	pass
