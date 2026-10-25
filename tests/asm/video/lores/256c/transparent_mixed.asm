; lores 256c per-pixel transparency: left half opaque red, right half
; transparent. composite shows red on the left half of the lores area and
; black fallback on the right half.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red (opaque)
	set_pal_entry 0, 2, $e3, $00	; transparent (hi == nr_14)
	set_nr $6a, $00
	set_nr $15, $80

	macro fillrowmixed
	ld b, 64
	ld a, 1
.leftfill:
	ld (hl), a
	inc hl
	djnz .leftfill
	ld b, 64
	ld a, 2
.rightfill:
	ld (hl), a
	inc hl
	djnz .rightfill
	endm

	ld hl, $4000
	rept 48
	fillrowmixed
	endr

	ld hl, $6000
	rept 48
	fillrowmixed
	endr

	pass
