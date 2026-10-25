; lores 16c nibble-packed layout: each byte holds two adjacent pixels,
; high nibble = left, low nibble = right. fill the left half of every row
; with index 1 and the right half with index 2. composite must show
; red|blue split.
;
; what this catches:
;   - nibble order swap (high == right): stripes would still split halfway
;     but red/blue would be swapped vs the l2 stripe reference.
;   - wrong row stride (e.g. 128 bytes instead of 64): the stripe boundary
;     would not align with the visible mid-column.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_nr $6a, $20	; 16c
	set_nr $15, $80

; each lores row in 16c = 64 bytes (128 pixels / 2 per byte).
; left half = 32 bytes of $11 (= two left-pixel indices), right half
; = 32 bytes of $22.
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

	ld hl, $4000
	rept 96
	fillrowstripe16c
	endr

	pass
