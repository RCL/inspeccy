; l2_320 x scroll exercising nr_71 (x-scroll high bit).
;
; framebuffer: column-major, left 160 cols = red ($01), right 160 = blue ($02).
; scroll x = 256 -> nr_16 = 0, nr_71 bit 0 = 1.
;
; in 320 mode, source x = (x + 256) % 320. so:
;   composite x   0..63  -> source x 256..319 -> blue
;   composite x  64..223 -> source x   0..159 -> red
;   composite x 224..319 -> source x 160..255 -> blue
; three bands: 64px blue | 160px red | 96px blue.
;
; what this catches: a renderer that ignores nr_71 (would silently wrap at 256
; and show the wrong bands).

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $10	; l2_320
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red
	set_pal_entry 1, 2, $03, $01	; blue

	set_nr $16, 0	; x scroll low = 0
	set_nr $71, $01	; x scroll high bit = 1 -> total scroll = 256
	set_nr $17, 0

; column-major: first 5 pages = red, last 5 pages = blue.
	rept 5, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
	rept 5, page
	set_nr $52, page_base + 5 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 2
	ldir
	endr

	set_nr $52, 4

	pass
