; l2_640 y scroll. same half/half-column layout as l2_320/scroll_y -
; column-major with bytes 0..127 = $11 (red), bytes 128..255 = $22 (blue)
; in every column. nr_17 = 64.
;
; composite y = (y + 64) % 256:
;   composite y   0..63   -> source y  64..127 -> red
;   composite y  64..191  -> source y 128..255 -> blue
;   composite y 192..255  -> source y   0..63  -> red

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $20
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00
	set_pal_entry 1, 2, $03, $01

	set_nr $16, 0
	set_nr $71, 0
	set_nr $17, 64

	macro fillcolumnshalfhalf640
	ld hl, $4000
	ld c, 32
.colloop:
	ld b, 128
	ld a, $11
.redfill:
	ld (hl), a
	inc hl
	djnz .redfill
	ld b, 128
	ld a, $22
.bluefill:
	ld (hl), a
	inc hl
	djnz .bluefill
	dec c
	jr nz, .colloop
	endm

	rept 10, page
	set_nr $52, page_base + page
	fillcolumnshalfhalf640
	endr

	set_nr $52, 4

	pass
