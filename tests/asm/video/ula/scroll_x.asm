; ula hardware x-scroll (nr_26) - a next-only feature the classic ula never had.
; the ula bitmap is filled so the leftmost pixel of every 8-pixel cell is ink
; ($80 in every pixel byte), giving red vertical lines at screen x 0,8,16,...
; (composite 32,40,48,...). nr_26 = 4 scrolls the ula right by 4, so the lines
; move to screen x 4,12,20,... (composite 36,44,52,...). verified programmatically.

	include "common.inc"

	silence_ula	; nr_14 := $e3; ula pal 0 entry 0 transparent
	set_nr $43, $00	; ula palette (0), classic mode
	set_pal_entry 0, 2, $e0, $00	; ula palette 0 entry 2 = red (ink 2)

; map bank 5 (the ula screen) low page into $4000..$5fff and fill it.
	set_nr $52, 10	; mmu2 ($4000-$5fff) = bank 5 low 8k
; pixels $4000..$57ff (6144) = $80: leftmost pixel of each cell is ink.
	ld hl, $4000
	ld de, $4001
	ld bc, 6143
	ld (hl), $80
	ldir
; attributes $5800..$5aff (768) = $02: ink 2, paper 0 (paper 0 = transparent).
	ld hl, $5800
	ld de, $5801
	ld bc, 767
	ld (hl), $02
	ldir
	set_nr $52, 4	; restore launch mapping

	set_nr $26, 4	; ula x scroll = 4 pixels

	pass
