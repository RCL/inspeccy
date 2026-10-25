; l2_256 transparency: every l2 pixel uses a palette entry whose hi byte
; equals nr_14 -> every l2 pixel is transparent -> the whole 256x192 area
; falls through to the layer beneath.
;
; in this test ula is also silenced (palette 0 entry 0 = nr_14), so the
; colour beneath everything in the display area is the black $4a fallback.
;
; what this verifies:
;   - l2 transparency check uses the palette-entry hi byte == nr_14 rule
;     (not a hardcoded index, not an alpha bit elsewhere)
;   - transparent l2 pixels do not clobber the underlying layer
;
; expected composite: black 256x192 display area inside the default white border.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula	; nr_14 := $e3; ula also transparent
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80

; palette 1 entry 1's hi byte matches nr_14 -> any l2 pixel reading
; index 1 will be transparent. the lo byte doesn't matter for the
; transparency test (it's only consulted when the pixel is opaque).
	set_pal_entry 1, 1, $e3, $55

; fill the full l2 framebuffer with index 1.
	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

	set_nr $52, 4

	pass
