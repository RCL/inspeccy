; nr_4b defaults to transparent index $e3 without an explicit write.
; top eight rows use red index 1; bottom eight use transparent index $e3.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; sprite-id lockstep
	set_nr $15, $01	; sprite engine enable
; nr_4b intentionally not set - relies on hardware default $e3.

	set_pal_entry 2, 1, $e0, $00	; entry 1 = red (opaque body)
	set_pal_entry 2, $e3, $1c, $00	; entry $e3 = green (visible iff
; the engine wrongly treats $e3
; pattern bytes as opaque)

; pattern: 128 bytes of $01 (red), then 128 bytes of $e3 (transparent
; iff nr_4b defaults to $e3).
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; attr: visible sprite at (128, 96), pattern 0.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0457
	otir

	pass

pattern:
	dup 128
	db $01
	edup
	dup 128
	db $e3
	edup

attr:
	db 128	; x
	db 96	; y
	db $00	; attr2 = palette offset 0, x8 = 0
	db $80	; attr3 = v=1, e=0, pattern 0
