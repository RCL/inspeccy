; add the attr2 palette offset to the pattern index high nibble.
; pattern $05 plus offset $f0 must select black entry $f5, not entry $14.

	include "common.inc"

	silence_ula
	set_nr $09, $10
	set_nr $15, $01	; sprite engine on
	set_nr $4b, $e3	; sprite transparent index

; "Wrong-answer" entry - what the low-nibble bug would land on.
; painting it a glaring colour makes a misrender visible at a glance.
	set_pal_entry 2, $14, $e0, $00	; entry $14 = bright red (= the bug)
; the correct entry the high-nibble add lands on: $05 + ($f << 4) = $f5.
	set_pal_entry 2, $f5, $00, $00	; entry $f5 = black (rgb 0,0,0)
; also paint $05 itself something noticeable in case any pixel ends
; up there during the test (none should).
	set_pal_entry 2, $05, $fc, $00	; entry $05 = yellow

; pattern: all $05.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; attr: one visible sprite at (128, 96) with palette offset $0f.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0457
	otir

	pass

pattern:
	dup 256
	db $05
	edup

attr:
	db 128	; x
	db 96	; y
	db $f0	; attr2: palette offset = $f (= bits 7:4), x8 = 0
	db $80	; attr3: v=1, e=0, pattern 0
