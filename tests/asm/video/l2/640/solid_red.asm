; l2_640 basic rendering: fill the 80 kb framebuffer with $11. both nibbles
; of each byte = 1, so palette index 1 lights up. set palette 1 entry 1 to
; red. composite should show solid red across the whole 320x256 canvas
; (we render the high nibble at composite x = byte_col; both halves match).
;
; what this verifies:
;   - nr_70 bits 5:4 = 10 selects l2_640 mode
;   - the renderer reads the 4bpp packed framebuffer
;   - the high-nibble lookup maps to palette 1 correctly

	include "common.inc"

l2_bank_16k equ 9	; pages 18..27 hold the 80 kb framebuffer
page_base equ l2_bank_16k * 2	; = 18

	silence_ula
	set_nr $70, $20	; mode bits 5:4 = 10 -> l2_640
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 1, $e0, $00	; palette 1 entry 1 = red

; fill 10 pages (80 kb total) with $11.
	rept 10, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), $11
	ldir
	endr

	set_nr $52, 4

	pass
