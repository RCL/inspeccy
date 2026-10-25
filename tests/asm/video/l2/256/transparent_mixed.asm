; l2_256 per-pixel transparency: left half = opaque red, right half =
; transparent. composite should show red on the left half of the 256x192
; area and black fallback on the right half.
;
; what this catches: a "if any pixel transparent in row, whole row falls
; through" bug, or a "transparency is row-aligned to 32px boundaries" bug
; - both would warp the left/right boundary at column 128.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; index 1 = red (opaque)
	set_pal_entry 1, 2, $e3, $00	; index 2's hi == nr_14 -> transparent

; per-page fill in a macro so each rept iteration gets its own local
; label scope (and no call/ret overhead).
	macro fillmixedpage
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
	fillmixedpage
	endr

	set_nr $52, 4

	pass
