; l2_256 row-major layout: left 128 columns are colour a, right 128 are b.
;
; in l2_256 the framebuffer is row-major - each row is 256 contiguous bytes
; starting at nr_12's base. byte index = y * 256 + x. the left/right stripe
; pattern verifies that:
;   - the renderer walks rows x-major (not y-major)
;   - the palette resolves each pixel independently
;
; what this catches: a column-major reader (would show top/bottom halves
; instead of left/right), or a stride-off-by-n error (would show diagonal
; or repeated stripes).

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e0, $00	; index 1 -> red
	set_pal_entry 1, 2, $03, $01	; index 2 -> blue (9th-bit form)

; fill 192 rows of 256 bytes: bytes 0..127 = 1 (red), bytes 128..255 = 2.
; each 8k page holds 32 fully-formed 256-byte rows - no row-straddles-page.
; rept bodies share label scope across iterations, so the per-row fill
; lives in a macro: macros give each invocation its own `.label` scope,
; and inlining via rept-of-macro is faster than rept-of-call (no
; call/ret overhead, no stack pressure).
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
