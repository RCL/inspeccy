; lores 256c x scroll. framebuffer = left 64 source-cols red ($01), right 64
; source-cols blue ($02). nr_32 = 32, source x = (x + 32) % 128.
;
; composite renders lores 2x scaled. boundaries below in source-pixel coords;
; multiply by 2 for composite-pixel coords inside the centred 256x192 block.
;   source x   0..31 -> source x  32..63  -> red  (still in left half)
;   source x  32..95 -> source x  64..127 -> blue (= 64 cols of blue)
;   source x 96..127 -> source x   0..31  -> red  (wrap)
; three bands in the composite, each col 2 wide.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

	set_nr $32, 32	; x scroll = 32
	set_nr $33, 0

; fill: each of 96 rows = 64 bytes of $01 + 64 bytes of $02.
; in 256c the top half ($4000+) holds rows 0..47, the bottom half
; ($6000+) rows 48..95. same per-row layout in both halves.
	macro fillrowstripe256c
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

	ld hl, $4000	; rows 0..47
	rept 48
	fillrowstripe256c
	endr

	ld hl, $6000	; rows 48..95
	rept 48
	fillrowstripe256c
	endr

	pass
