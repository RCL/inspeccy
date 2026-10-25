; l2_320 column-major layout: left 160 columns = red, right 160 = blue.
;
; l2_320 stores the framebuffer column-major - bytes 0..255 = column 0
; (x=0, all y), bytes 256..511 = column 1, etc. so for a left|right stripe,
; the first half of memory (40 kb) holds the red columns and the second
; half (40 kb) holds the blue columns. pages 18..22 fill with $01, pages
; 23..27 with $02.
;
; what this catches: a renderer that reads l2_320 row-major (would show
; top/bottom halves), or one that doesn't apply the column-to-row transform
; (would show a transposed/mirrored layout).

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $10	; l2_320
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red
	set_pal_entry 1, 2, $03, $01	; blue (9-bit form)

; first 5 pages: red (columns 0..159).
	rept 5, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

; last 5 pages: blue (columns 160..319).
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
