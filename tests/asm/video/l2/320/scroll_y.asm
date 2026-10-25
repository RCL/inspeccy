; l2_320 y scroll. framebuffer (column-major) has the top half of every
; column = red, bottom half = blue. with nr_17 = 64, source y = (y + 64) % 256.
;
; each column in ram = 256 bytes (rows 0..255). for half/half: bytes 0..127
; = red, bytes 128..255 = blue. so within each 8k page (32 columns * 256
; bytes/col), pattern = 128 red + 128 blue per column, repeated 32 times.
;
; expected (composite y = source y wrapped by 64):
;   composite y   0..63   -> source y  64..127 -> red
;   composite y  64..191  -> source y 128..255 -> blue  (= 128 rows)
;   composite y 192..255  -> source y   0..63  -> red
; three bands: 64px red | 128px blue | 64px red.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $10	; l2_320
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00
	set_pal_entry 1, 2, $03, $01

	set_nr $16, 0
	set_nr $71, 0
	set_nr $17, 64

; each 8k page holds 32 columns of 256 bytes. fill each column with 128
; red + 128 blue.
	macro fillcolumnshalfhalf
	ld hl, $4000
	ld c, 32	; 32 columns / page
.colloop:
	ld b, 128
	ld a, 1
.redfill:
	ld (hl), a
	inc hl
	djnz .redfill
	ld b, 128
	ld a, 2
.bluefill:
	ld (hl), a
	inc hl
	djnz .bluefill
	dec c
	jr nz, .colloop
	endm

	rept 10, page
	set_nr $52, page_base + page
	fillcolumnshalfhalf
	endr

	set_nr $52, 4

	pass
