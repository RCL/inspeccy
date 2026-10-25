; shared setup for the nr_15 layering tests. configures:
;   - l2_256 mode, palette 1 entry 1 = red, framebuffer filled with $01
;     -> whole 256x192 area renders solid red
;   - lores 256c, palette 0 entry 2 = green, framebuffer filled with $02
;     -> whole 256x192 area renders solid green
;   - one 16x16 sprite at (x=128, y=96) with pattern all $03, sprite palette
;     entry 3 = blue
;
; after including this file each test only needs to write nr_15 (priority +
; lores-enable + sprite-enable) and pass. the visible composite depends on
; which layer the chosen nr_15 mode renders on top.
;
; filename starts with `_` so the test runner skips it (it's a fragment
; with no `start:` of its own, included into a real test).

; ---- 1. ula transparent so it doesn't fight lores (nr_15 routes lores to
;         the ula slot but we still set nr_14 = $e3 for the global rule) ----
	silence_ula

; ---- 2. l2_256 ----------------------------------------------------------
	set_nr $70, $00	; l2_256
	set_nr $12, 9	; bank 9 -> pages 18..23
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 1, $e0, $00	; l2 palette entry 1 = red

; fill 48 kb of l2 framebuffer with $01.
	rept 6, page
	set_nr $52, 18 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
; restore slot 2 to page 10 (= bank 5 first half). the lores 256c fill
; below writes lines 0..47 at $4000-$57ff, which needs slot 2 mapped to
; bank 5 page 10 (the standard 128k layout). on dot-cmd launch slot 2
; defaults to that mapping, so this restores the launch state.
	set_nr $52, 10

; ---- 3. lores 256c ------------------------------------------------------
	set_nr $6a, $00	; 256c, palette offset 0
	set_pal_entry 0, 2, $1c, $00	; ula palette entry 2 = green

; fill 256c framebuffer: lines 0..47 at $4000+, lines 48..95 at $6000+.
	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 2
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 2
	ldir

; ---- 4. sprite ---------------------------------------------------------
	set_nr $09, $10	; lockstep
	set_nr $4b, $e3	; sprite transparent index
	set_pal_entry 2, 3, $03, $01	; sprite palette entry 3 = blue

; upload pattern slot 0: 256 bytes of $03.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, _setuprgb_pattern
	ld bc, $005b
	otir

; upload attr slot 0: visible 4-byte sprite at (128, 96) pattern 0.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, _setuprgb_attr
	ld bc, $0457
	otir

; jump past the data blobs.
	jp _setuprgb_end

_setuprgb_pattern:
	dup 256
	db $03
	edup

_setuprgb_attr:
	db 128	; x low = 128
	db 96	; y = 96
	db $00	; attr2: palette off 0, x8 = 0
	db $80	; attr3: visible, 4-byte, pattern 0

_setuprgb_end:
