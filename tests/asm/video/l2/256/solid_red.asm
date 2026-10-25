; l2_256 basic rendering: fill the framebuffer with palette index 1, where
; palette 1 entry 1 = $e0 $00 = pure red (r=7, g=0, b=0).
;
; what this verifies:
;   - l2_256 mode is enabled and renders the centred 256x192 block at (32,32)
;   - l2 reads from the bank that nr_12 points to
;   - l2 looks up via the l2-first palette (palette 1, per nr_43 bit 2 = 0)
;   - decodenxentry maps hi=$e0 -> r=7 -> $ff in the red channel
;
; doesn't verify (other tests do): transparency, palette routing (nr_43 bit 2),
; column-major layout (320 mode), 9th blue bit, fallback colour.

	include "common.inc"

l2_bank_16k equ 9	; bank 9 -> 8k pages 18..23
l2_fill_value equ 1	; palette index every byte resolves to

; setup.
	silence_ula	; nr_14 := $e3; ula pal 0 ent 0 := transparent
	set_nr $70, $00	; l2 mode 256x192
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; bit 7 = l2 display enable
	set_pal_entry 1, 1, $e0, $00	; palette 1 entry 1 = pure red

; fill the l2 framebuffer (48 kb = 6 8k pages) with $01. we temporarily
; map each page into slot 2 ($4000..$5fff) and ldir-fill 8k each time.
page_base equ l2_bank_16k * 2	; first 8k page of bank 9 = 18

	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), l2_fill_value
	ldir
	endr

; restore slot-2 mapping to its dot-cmd-launch state (page 4 = bank 2).
	set_nr $52, 4

	pass
