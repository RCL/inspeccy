; sprite x-mirror / y-mirror / rotate use attr2 bits 3/2/1 in both formats.
; the pattern's top-left 8x4 rectangle is red, the rest transparent, so each
; transform lands the filled block in a distinct, distinguishable place:
;   identity -> [0..7]x[0..3]   xmir -> [8..15]x[0..3]
;   ymir     -> [0..7]x[12..15] rot  -> [12..15]x[0..7]   (90 deg clockwise)
; six sprites in a row at y=40, x=40/80/120/160/200/240:
;   0 identity(4b) 1 xmir(4b,attr2) 2 ymir(4b,attr2) 3 rot(4b,attr2)
;   4 rot(5b,attr2 bit1) 5 xmir(5b,attr2 bit3)

	include "common.inc"

	silence_ula
	set_nr $09, $10	; port $57 lockstep auto-advance
	set_nr $15, $01	; sprite engine on
	set_pal_entry 2, 1, $e0, $00	; sprite palette entry 1 = red

; pattern slot 0: top-left 8x4 red, rest $e3 transparent
	xor a
	ld bc, $303b
	out (c), a
	ld hl, patterndata
	ld bc, $005b
	otir

	xor a
	ld bc, $303b
	out (c), a	; select slot 0 for attribute upload
	ld hl, attrdata
	ld bc, (attrend - attrdata) * 256 + $57	; b=byte count, c=$57
	otir

	pass

patterndata:
; 16 rows of 16 bytes. rows 0..3: cols 0..7 = $01, cols 8..15 = $e3.
; rows 4..15: all $e3.
	dup 4
	dup 8
	db $01
	edup
	dup 8
	db $e3
	edup
	edup
	dup 12
	dup 16
	db $e3
	edup
	edup

attrdata:
	db 40, 40, $00, $80	; 0 identity (4-byte)
	db 80, 40, $08, $80	; 1 x-mirror (attr2 bit3)
	db 120, 40, $04, $80	; 2 y-mirror (attr2 bit2)
	db 160, 40, $02, $80	; 3 rotate   (attr2 bit1)
	db 200, 40, $02, $c0, $00	; 4 rotate   (5-byte, attr2 bit1)
	db 240, 40, $08, $c0, $00	; 5 x-mirror (5-byte, attr2 bit3)
attrend:
