; relative (anchor-following) sprites: attr4 bit 6 (h) = 1 marks a relative
; sprite whose attr0/attr1 are signed offsets from the most recent anchor
; (h=0), hidden when the anchor is hidden (visibility anded).
;
;   slot 0  anchor    red   at (100,100)             -> red block (100,100)
;   slot 1  relative  blue  offset (+30, 0)          -> blue block (130,100)
;   slot 2  relative  blue  offset (0, +30)          -> blue block (100,130)
;   slot 3  relative  blue  offset (-20,+10)         -> blue block (80,110)
;   slot 4  anchor    (invisible) at (200,100)       -> nothing
;   slot 5  relative  blue  offset (+10,+10)         -> hidden (anchor off)
; all sprites are 5-byte (e=1). positions verified programmatically.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; lockstep ($57 auto-advance, 5 bytes for e=1)
	set_nr $15, $01	; sprite engine on
	set_pal_entry 2, 1, $e0, $00	; entry 1 = red
	set_pal_entry 2, 2, $03, $01	; entry 2 = blue

; pattern slot 0 = red ($01), slot 1 = blue ($02)
	xor a
	ld bc, $303b
	out (c), a
	ld hl, patred
	ld bc, $005b
	otir
	ld a, 1
	ld bc, $303b
	out (c), a
	ld hl, patblue
	ld bc, $005b
	otir

	xor a
	ld bc, $303b
	out (c), a	; select slot 0 for attribute upload
	ld hl, attrdata
	ld bc, (attrend - attrdata) * 256 + $57
	otir

	pass

patred:
	dup 256
	db $01
	edup
patblue:
	dup 256
	db $02
	edup

attrdata:
	db 100, 100, $00, $c0, $00	; 0 anchor red  (v,e,pat0; attr4 h=0)
	db 30, 0, $00, $c1, $40	; 1 rel blue +30,+0  (pat1; attr4 h=1)
	db 0, 30, $00, $c1, $40	; 2 rel blue +0,+30
	db 236, 10, $00, $c1, $40	; 3 rel blue -20,+10 (236 = -20 signed)
	db 200, 100, $00, $40, $00	; 4 anchor invisible (v=0,e=1) at (200,100)
	db 10, 10, $00, $c1, $40	; 5 rel blue +10,+10 under hidden anchor
attrend:
