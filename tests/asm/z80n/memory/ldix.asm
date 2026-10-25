; ldix (ed a4) - ldi variant that skips when source byte equals a.
; spec: if [hl] != a then [de] := [hl]; hl++, de++, bc--.
; a is the "transparent" sentinel - source bytes equal to it are skipped.

	include "common.inc"

; --- case 1: source byte != a, gets copied ---
; place known data at $c000, dest at $c100.
	ld a, $42
	ld ($c000), a
	xor a
	ld ($c100), a	; pre-fill dest with 0 (sentinel)

	ld hl, $c000
	ld de, $c100
	ld bc, $0001
	ld a, $e3	; a = transparent sentinel; src ($42) != a -> copy
	db $ed, $a4

	check_mem $c100, $42
	check_hl $c001
	check_de $c101
	check_bc $0000

; --- case 2: source byte == a, write skipped ---
	ld a, $e3
	ld ($c000), a	; src = a -> skip
	ld a, $55
	ld ($c100), a	; dest sentinel; must remain $55

	ld hl, $c000
	ld de, $c100
	ld bc, $0001
	ld a, $e3
	db $ed, $a4

	check_mem $c100, $55	; unchanged
	check_hl $c001
	check_de $c101

	pass
