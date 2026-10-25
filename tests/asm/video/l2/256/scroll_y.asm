; l2_256 y scroll: top-half red / bottom-half blue framebuffer, nr_17 = 48
; (= 1/4 of the 192-pixel height). composite reads source y = (y + 48) % 192.
;
; expected (composite y measured from the centred top-left at (32, 32)):
;   composite y  0..47  -> source y 48..95  -> red   (still in top half)
;   composite y 48..143 -> source y 96..191 -> blue  (whole bottom half = 96 rows)
;   composite y 144..191 -> source y 0..47  -> red   (wrap)
; three horizontal bands: 48px red | 96px blue | 48px red.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red
	set_pal_entry 1, 2, $03, $01	; blue

	set_nr $16, 0
	set_nr $17, 48	; y scroll = 48
	set_nr $71, 0

; top 96 rows = $01, bottom 96 rows = $02.
; each 8k page holds 32 rows of 256. 96 rows = 3 pages.
	rept 3, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
	rept 3, page
	set_nr $52, page_base + 3 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 2
	ldir
	endr

	set_nr $52, 4

	pass
