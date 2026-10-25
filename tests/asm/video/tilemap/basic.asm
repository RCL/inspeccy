; tilemap layer, 40x32, 4bpp, per-tile attributes. two tile definitions (tile 0
; = solid nibble 1, tile 1 = solid nibble 2) and a map that alternates 0,1 per
; column, giving vertical red/blue 8-pixel stripes across the whole 320x256
; canvas. palette 3 entry 1 = red, entry 2 = blue.
;
;   composite x 0..7 -> tile 0 -> red ; 8..15 -> tile 1 -> blue ; 16..23 -> red ...
; verified programmatically.
;
; memory (bank 5): tile definitions at bank-5 $2000 ($6f=$20), map at bank-5
; $3000 ($6e=$30); we map bank-5 high page (11) at $4000 to write them.

	include "common.inc"

	silence_ula
	set_pal_entry 3, 1, $e0, $00	; palette 3 entry 1 = red
	set_pal_entry 3, 2, $03, $01	; palette 3 entry 2 = blue

	set_nr $52, 11	; bank 5 high 8k -> $4000..$5fff

; tile 0 definition: 32 bytes of $11 (every pixel nibble = 1) at $4000.
	ld hl, $4000
	ld de, $4001
	ld bc, 31
	ld (hl), $11
	ldir
; tile 1 definition: 32 bytes of $22 at $4020.
	ld hl, $4020
	ld de, $4021
	ld bc, 31
	ld (hl), $22
	ldir

; map at $5000 (bank-5 $3000): repeating 4-byte pattern tile0/attr0/tile1/attr0.
	ld hl, $5000
	ld (hl), $00	; tile index 0
	inc hl
	ld (hl), $00	; attribute 0 (palette off 0, no transform, tile priority)
	inc hl
	ld (hl), $01	; tile index 1
	inc hl
	ld (hl), $00	; attribute 0
; propagate the 4-byte pattern across the whole 40x32x2 = 2560-byte map.
	ld hl, $5000
	ld de, $5004
	ld bc, 2556
	ldir

	set_nr $52, 4	; restore launch mapping

	set_nr $6f, $20	; tile-definition base = bank-5 $2000
	set_nr $6e, $30	; tilemap base        = bank-5 $3000
	set_nr $6b, $80	; enable, 40x32, per-tile attr, palette 3

	pass
