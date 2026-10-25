; sprite attributes uploaded through the nextreg mirror ($34, $35-$39, $75-$79)
; instead of port $57. three 16x16 red blocks at hw (100,96), (160,96),
; (220,96), all using pattern slot 0 (solid palette entry 1 = red).
;
; what this verifies:
;   - nr_34 selects the sprite attribute slot (resets the upload cursors)
;   - nr_35..nr_38 write attribute bytes 0..3 without advancing the slot
;   - nr_78 (attr3 "with increment") writes attr3 and advances nr_34 by one,
;     so the next sprite lands in the following slot. sprites 1 and 2 rely on
;     that auto-advance: if the increment were dropped, sprite 2 would overwrite
;     sprite 1's slot and only two blocks (100, 220) would show instead of three.
;
; the picture is identical to the same three sprites uploaded via port $57;
; the reference bmp was cross-checked byte-for-byte against a port-$57 twin at
; authoring time.

	include "common.inc"

; --- setup ----------------------------------------------------------
	silence_ula
	set_nr $15, $01	; enable sprite engine (slu priority)
	set_pal_entry 2, 1, $e0, $00	; sprite palette (2) entry 1 = red

; --- upload pattern slot 0 (256 bytes of $01) -----------------------
	xor a
	ld bc, $303b
	out (c), a	; select slot 0 for pattern upload
	ld hl, patterndata
	ld bc, $005b	; b=0 -> 256 iters; c = $5b (pattern port)
	otir

; --- sprite 0 in slot 0, via nr_35..nr_38 (no increment) ------------
	set_nr $34, 0	; select attribute slot 0
	set_nr $35, 100	; attr0 = x low = 100
	set_nr $36, 96	; attr1 = y = 96
	set_nr $37, $00	; attr2 = palette 0, no mirror, x8=0
	set_nr $38, $80	; attr3 = v=1, e=0, pattern slot 0

; --- sprite 1 in slot 1, attr0..2 no-inc then attr3 with inc --------
	set_nr $34, 1	; select attribute slot 1
	set_nr $35, 160	; attr0 = x low = 160
	set_nr $36, 96	; attr1 = y = 96
	set_nr $37, $00	; attr2
	set_nr $78, $80	; attr3 with increment -> nr_34 becomes 2

; --- sprite 2 in slot 2, using the auto-advanced nr_34 = 2 ----------
	set_nr $35, 220	; attr0 = x low = 220 (slot 2)
	set_nr $36, 96	; attr1 = y = 96
	set_nr $37, $00	; attr2
	set_nr $78, $80	; attr3 with increment -> nr_34 becomes 3

	pass

patterndata:
	dup 256
	db $01
	edup
