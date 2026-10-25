; l2_320 basic rendering: fill the entire 320x256 framebuffer with palette
; index 1 (= pure red). the 320 mode fills the whole composite canvas
; (no centring), so the full 320x256 area is red.
;
; what this verifies:
;   - nr_70 bits 5:4 = 01 selects l2_320 mode
;   - the renderer reads 80 kb (320 * 256) from nr_12's bank
;   - display is at offset (0, 0), not (32, 32) like l2_256

	include "common.inc"

l2_bank_16k equ 9	; pages 18..27 hold the 80 kb framebuffer
page_base equ l2_bank_16k * 2	; = 18

	silence_ula
	set_nr $70, $10	; mode bits 5:4 = 01 -> l2_320
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 1, $e0, $00	; palette 1 entry 1 = red

; fill 10 pages (80 kb total) with $01.
	rept 10, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

	set_nr $52, 4	; restore slot 2 to launch state

	pass
