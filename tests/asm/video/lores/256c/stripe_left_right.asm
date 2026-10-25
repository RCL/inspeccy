; lores 256c split-half layout: left 64 lores columns = red, right 64 = blue.
;
; lores is 128 px wide; left/right split = columns 0..63 / 64..127.
; the composite scales 2x so this appears as left/right halves of the
; visible 256x192 block.
;
; critical test: with 256c's split layout, the top 48 lines live at $4000+
; and the bottom 48 at $6000+. if a renderer reads all 96 lines contiguously
; from $4000+ (= the 16c layout), the bottom half would show garbage from
; $5800-$5fff (= attribute / system area). this test draws the same stripe
; in both halves; a layout bug would distort or duplicate the bottom half.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue (9-bit form)
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

; per-row macro: 64 bytes of a (= red), 64 of b (= blue).
; each lores row is 128 bytes in 256c mode.
	macro fillrowstripe
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

; top half: lines 0..47 at $4000+. 48 rows.
	ld hl, $4000
	rept 48
	fillrowstripe
	endr

; bottom half: lines 48..95 at $6000+. 48 rows.
	ld hl, $6000
	rept 48
	fillrowstripe
	endr

	pass
