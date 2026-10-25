; l2_640 x scroll. our half-resolution renderer treats source byte-columns
; 0..319 as composite x 0..319 (high nibble of each byte). the framebuffer
; (column-major) is left half $11 (red), right half $22 (blue) - same layout
; as stripe_left_right.
;
; scroll x = 96 (= 96 byte-columns = 192 source pixels in the underlying
; 640-pixel width). composite x = (x + 96) % 320:
;   composite x   0..63  -> source col  96..159 -> red
;   composite x  64..223 -> source col 160..319 -> blue (= 160 cols of blue)
;   composite x 224..319 -> source col   0..95  -> red
; three bands: 64px red | 160px blue | 96px red.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $20	; l2_640
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00
	set_pal_entry 1, 2, $03, $01

	set_nr $16, 96
	set_nr $71, 0
	set_nr $17, 0

; half/half column fill: first 5 pages = $11, last 5 pages = $22.
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
