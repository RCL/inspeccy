; l2_256 x scroll: same left-red / right-blue framebuffer as stripe_left_right,
; but with nr_16 = 64 set. the composite reads source x = (x + 64) mod 256.
;
; expected wrap (composite x measured from the centred top-left at 32):
;   composite x   0..63  -> source 64..127  -> red   (left half of source)
;   composite x  64..191 -> source 128..255 -> blue  (whole right half)
;   composite x 192..255 -> source 0..63    -> red   (wrap)
; so the visible 256x192 block is: 64px red | 128px blue | 64px red.
;
; what this catches: a renderer that ignores nr_16 entirely would still show
; the original red-left / blue-right split.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; red
	set_pal_entry 1, 2, $03, $01	; blue

; scroll x = 64. nr_71 stays 0 (high bit unused in 256 mode).
	set_nr $16, 64
	set_nr $17, 0
	set_nr $71, 0

; fill: 192 rows, left 128 = $01, right 128 = $02.
	macro fillstripepage
	ld hl, $4000
	ld c, 32
.rowloop:
	ld b, 128
	ld a, 1
.leftfill:
	ld (hl), a
	inc hl
	djnz .leftfill
	ld b, 128
	ld a, 2
.rightfill:
	ld (hl), a
	inc hl
	djnz .rightfill
	dec c
	jr nz, .rowloop
	endm

	rept 6, page
	set_nr $52, page_base + page
	fillstripepage
	endr

	set_nr $52, 4

	pass
