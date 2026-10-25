; single 4-byte sprite at (x=128, y=96) with pattern 0 filled with $01.
; palette 2 entry 1 = red. result: a 16x16 red block in the middle of the
; 320x256 canvas, on the default border (white).
;
; what this verifies:
;   - nr_15 bit 0 enables the sprite engine
;   - port $303b selects the slot for both pattern and attribute upload
;   - port $5b uploads pattern bytes sequentially
;   - port $57 uploads attribute bytes sequentially (with lockstep on)
;   - pattern index n -> sprite palette entry n (palette offset = 0)
;   - sprite palette = palette 2 (when nr_43 bit 3 = 0)
;   - sprite is drawn at hw (x, y) in the composite (no centring offset)

	include "common.inc"

; --- setup ----------------------------------------------------------
; ula must be transparent so nothing covers the sprite.
	silence_ula
; enable sprite lockstep (nr_09 bit 4) so port $57 auto-advances slot.
	set_nr $09, $10
; enable sprite engine (nr_15 bit 0). default priority = slu.
	set_nr $15, $01
; sprite palette entry 1 = red. sprpalidx = 2 when nr_43 bit 3 = 0
; (= default, since silence_ula only sets nr_43 to $00 via the ula
; palette write).
	set_pal_entry 2, 1, $e0, $00

; --- upload pattern slot 0 (256 bytes of $01) -----------------------
; otir: b = counter, c = port. ld bc, $0050  -> b=$00 (= 256 iters), c=$5b.
	ld a, 0
	ld bc, $303b
	out (c), a	; select slot 0 (for both pattern and attr)
	ld hl, patterndata
	ld bc, $005b	; b=0 -> 256 iters; c = $5b (sprite pattern port)
	otir

; --- upload sprite attribute slot 0 ---------------------------------
; re-select slot 0 (pattern upload may have left state different).
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attrdata
	ld bc, $0457	; b=4 (4 bytes); c = $57 (sprite attribute port)
	otir

	pass

patterndata:
; 256 bytes, all $01 (palette entry 1 = red)
	dup 256
	db $01
	edup

attrdata:
	db 128	; attr0 = x low = 128
	db 96	; attr1 = y = 96
	db $00	; attr2 = palette 0, no mirror, x bit 8 = 0
	db $80	; attr3 = v=1, e=0, pattern slot 0
