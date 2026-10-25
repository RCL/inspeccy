; two overlapping sprites at the same position. slot 0 = red, slot 1 = blue.
; the sprite engine renders slots in ascending order (0..63), so slot 1
; paints over slot 0. the visible block should be blue.
;
; what this verifies: z-order = slot index. a regression to descending order
; would show red instead.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; lockstep
	set_nr $15, $01	; sprite engine on
	set_nr $4b, $e3	; sprite transparent index (real-hw default)
	set_pal_entry 2, 1, $e0, $00	; sprite palette entry 1 = red
	set_pal_entry 2, 2, $03, $01	; sprite palette entry 2 = blue (9-bit)

; --- pattern 0: all $01 (red) ---
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, patternred
	ld bc, $005b
	otir

; --- pattern 1: all $02 (blue) ---
; (pattern upload auto-advances slot after 256 bytes, so we're already
; on slot 1; but re-select for clarity.)
	ld a, 1
	ld bc, $303b
	out (c), a
	ld hl, patternblue
	ld bc, $005b
	otir

; --- attr slot 0: red sprite at (128, 96) ---
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attrred
	ld bc, $0457
	otir

; --- attr slot 1: blue sprite at same (128, 96) ---
; lockstep advanced nr_34 to 1 after the previous otir. re-select to be
; safe.
	ld a, 1
	ld bc, $303b
	out (c), a
	ld hl, attrblue
	ld bc, $0457
	otir

	pass

patternred:
	dup 256
	db $01
	edup
patternblue:
	dup 256
	db $02
	edup
attrred:
	db 128	; x
	db 96	; y
	db $00	; attr2 = palette off 0, x8 = 0
	db $80	; attr3 = visible, 4-byte, pattern 0
attrblue:
	db 128
	db 96
	db $00
	db $81	; visible, 4-byte, pattern 1
