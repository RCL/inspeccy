; 128 sprites visible at once, arranged in an 8-wide x 16-tall grid.
;
; per https://wiki.specnext.dev/Sprites:
;   - core 3.0+ supports 128 sprite slots (was 64 in earlier cores)
;   - max 12 sprites per scanline by default; configurable to 100 via
;     nr_09 bit 0 ("extended sprite count"). our model doesn't enforce
;     either limit (we always render every visible sprite), but the test
;     layout still respects the 12-per-line classic limit so the same
;     setup would work on real hardware as well.
;
; layout: 8 columns, 16 rows. spacing of 32px (x) and 16px (y) keeps each
; row in its own 16-pixel scanline band -> exactly 8 sprites per line.
;   x = col*32             col 0..7  -> 0, 32, 64, 96, 128, 160, 192, 224
;   y = row*16             row 0..15 -> 0, 16, 32, ..., 240
;
; all x values fit in attr0 (8-bit) so we don't need the x8 bit (attr2 bit 0).
;
; every sprite shares pattern slot 0 (all-$01 fill). the colour comes from
; the per-sprite palette offset in attr2 bits 7:4, which we set to the row
; index - pixel value 1 + row offset gives palette entries 1..16, one
; distinct colour per row. this way the rendered grid has 16 horizontal
; colour bands; mis-rendered or dropped sprites are visually obvious.
;
; what this exercises:
;   - 128 attr slots (= the post-3.0 sprite engine, not the classic 64).
;   - port $57 otir auto-advance via nr_09 lockstep across the entire
;     128 x 4 = 512 byte attr table.
;   - per-sprite palette offset rendering (16 distinct row colours from
;     one pattern + offset arithmetic).

	include "common.inc"

	silence_ula
	set_nr $09, $10	; sprite-id lockstep
; bit 0 = engine enable, bit 1 = sprites-over-border so the grid is
; visible across the full 320x256 canvas (some columns/rows in this
; layout sit outside the centred 256x192 ula area).
	set_nr $15, $03
	set_nr $4b, $e3	; sprite transparent index

; 16 distinct palette entries for the 16 rows. the sprite engine
; computes the final palette index as
;     pattern_byte + (palette_offset << 4)
; so a pattern byte of $01 combined with palette offsets 0..15 lands
; on entries $01, $11, $21, ... $f1.
	set_pal_entry 2, 1, $e0, $00	; row 0  -> bright red
	set_pal_entry 2, 17, $e8, $00	; row 1  -> red-orange
	set_pal_entry 2, 33, $ec, $00	; row 2  -> orange
	set_pal_entry 2, 49, $fc, $00	; row 3  -> yellow
	set_pal_entry 2, 65, $58, $00	; row 4  -> yellow-green
	set_pal_entry 2, 81, $1c, $00	; row 5  -> green
	set_pal_entry 2, 97, $1e, $00	; row 6  -> green-cyan
	set_pal_entry 2, 113, $1f, $01	; row 7  -> cyan
	set_pal_entry 2, 129, $17, $01	; row 8  -> sky blue
	set_pal_entry 2, 145, $03, $01	; row 9  -> blue
	set_pal_entry 2, 161, $43, $01	; row 10 -> blue-purple
	set_pal_entry 2, 177, $83, $01	; row 11 -> purple
	set_pal_entry 2, 193, $c3, $01	; row 12 -> magenta
	set_pal_entry 2, 209, $e7, $01	; row 13 -> pink
	set_pal_entry 2, 225, $ff, $01	; row 14 -> white
	set_pal_entry 2, 241, $4c, $00	; row 15 -> dim green

; upload pattern slot 0: 256 bytes of $01 (solid colour block).
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b	; b=0 -> 256 iterations, c = sprite pattern port
	otir

; upload all 128 attribute groups in one otir (512 bytes).
; b is 8-bit (= 256 max), so we do it as two otirs of 256 bytes each:
; sprites 0..63 then 64..127. lockstep auto-advances the slot.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attrdata
	ld bc, $0057	; b=0 -> 256 bytes = 64 sprites
	otir
; nr_34 has now auto-advanced to 64. continue with the second half.
	ld bc, $0057	; another 256 bytes = next 64 sprites
	otir

	pass

pattern:
	dup 256
	db $01
	edup

; 128 attribute groups: 16 rows x 8 cols = 128 sprites.
; each group: attr0 = x lo (x < 256 always); attr1 = y; attr2 = palette
; offset << 4 | x8 bit (= 0); attr3 = visible, 4-byte, pattern 0.
attrdata:
	rept 16, row
	rept 8, col
	db col * 32	; attr0: x = 0, 32, ..., 224
	db row * 16	; attr1: y = 0, 16, ..., 240
	db row << 4	; attr2: palette offset = row
	db $80	; attr3: v=1, e=0, pattern 0
	edup
	edup
