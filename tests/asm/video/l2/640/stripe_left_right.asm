; l2_640 column-major layout: left 160 source-byte-columns = $11 (red),
; right 160 = $22 (blue). in the composite (which renders the high nibble
; at composite x = byte_col), this shows up as red|blue split at x=160.
;
; what this catches: a row-major reader (top/bottom halves), or a layout
; bug that mixes the two halves.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $20	; l2_640
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red
	set_pal_entry 1, 2, $03, $01	; blue (9-bit)

; column-major: first 5 pages (= byte-columns 0..159) = $11 (red),
; last 5 pages (= byte-columns 160..319) = $22 (blue).
	rept 5, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $11
	ldir
	endr
	rept 5, page
	set_nr $52, page_base + 5 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $22
	ldir
	endr

	set_nr $52, 4

	pass
