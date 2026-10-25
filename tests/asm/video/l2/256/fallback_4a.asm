; transparency fallback colour ($4a). l2_256 is enabled but filled with a
; transparent index (palette entry hi = nr_14), and the ula is silenced, so no
; layer paints the paper area. nr_4a sets the fallback to green ($1c = rrrgggbb
; 000'111'00 -> r0 g7 b0); the whole 256x192 paper area shows green, the border
; keeps its $fe colour. verified programmatically.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00	; l2 256x192
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 0, $e3, $00	; palette 1 entry 0 transparent (hi = nr_14 $e3)
	set_nr $4a, $1c	; fallback colour = green

; fill l2 with index 0 (transparent) so nothing paints.
	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 0
	ldir
	endr
	set_nr $52, 4

	pass
