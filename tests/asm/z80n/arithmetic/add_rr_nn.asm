; add hl,nn / add de,nn / add bc,nn  (ed 34/35/36 nn nn).
; spec: rr := rr + 16-bit immediate. 16 cycles. flags unaffected.

	include "common.inc"

; add hl,$0234
	ld hl, $1000
	db $ed, $34, $34, $02	; add hl, $0234
	check_hl $1234

; add de,$0100
	ld de, $2000
	db $ed, $35, $00, $01
	check_de $2100

; add bc,$0080
	ld bc, $3000
	db $ed, $36, $80, $00
	check_bc $3080

; wrap
	ld hl, $fffe
	db $ed, $34, $03, $00	; add hl, 3 -> 1 (wraps)
	check_hl $0001

	pass
