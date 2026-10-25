; lores 16c x scroll. framebuffer = left half $11 (= two red nibbles per
; byte), right half $22 (= two blue nibbles per byte). nr_32 = 32.
;
; in 16c each row = 64 bytes (= 128 pixels / 2 per byte). left half = 32
; bytes of $11, right half = 32 bytes of $22.
;
; composite reads source x = (x + 32) % 128. three bands like the 256c
; counterpart: red 64px-wide composite | blue 128px | red 64px.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_nr $6a, $20	; 16c (bit 5 = 1)
	set_nr $15, $80	; lores enable

	set_nr $32, 32
	set_nr $33, 0

	macro fillrowstripe16c
	ld b, 32
	ld a, $11
.leftfill:
	ld (hl), a
	inc hl
	djnz .leftfill
	ld b, 32
	ld a, $22
.rightfill:
	ld (hl), a
	inc hl
	djnz .rightfill
	endm

	ld hl, $4000	; 16c framebuffer is contiguous at $4000
	rept 96
	fillrowstripe16c
	endr

	pass
