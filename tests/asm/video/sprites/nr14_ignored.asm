; nr_14 global transparency must not hide a sprite whose palette colour
; matches it. sprites use the separate nr_4b pattern-index comparison.
; the complete 16x16 red block must remain visible.

	include "common.inc"

; do not use silence_ula - it would reset nr_14 to $e3. we want to
; pick nr_14 ourselves below.
; instead, silence the ula by hand: zero the bottom screen and set
; the ula palette entry 0 to a different hi byte from nr_14 so the
; ula layer paints opaque black across the centre instead of being
; transparent against the sprite (we want to verify sprite drawing,
; not ula composition).

	set_nr $14, $e0	; global transparency hi = $e0
	set_pal_entry 0, 0, $00, $00	; ula paper = black (hi=$00),
; not matching nr_14 => opaque
	set_nr $09, $10	; sprite-id lockstep
	set_nr $15, $01	; sprite engine enable
	set_nr $4b, $e3	; explicit nr_4b (just in case)

; sprite palette entry 1: hi=$e0 deliberately equal to nr_14. a buggy
; renderer that applies nr_14 to sprites would skip every pixel.
; lo bit 0 = 0 => b[0]=0; the colour is fully-red ($e0 = rrr=111, ggg=000, bb=00).
	set_pal_entry 2, 1, $e0, $00

; pattern: every byte = $01 (palette entry 1 = red). no transparent
; pixels in the pattern itself.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; attr: visible sprite at (128, 96), pattern 0.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0457
	otir

	pass

pattern:
	dup 256
	db $01
	edup

attr:
	db 128	; x
	db 96	; y
	db $00	; attr2 = palette offset 0, x8 = 0
	db $80	; attr3 = v=1, e=0, pattern 0
