; lores 256c y scroll: top 48 lines = $01 (red), bottom 48 = $02 (blue),
; with nr_33 = 24 set. the composite reads source y = (y + 24) mod 96.
;
; expected wrap:
;   composite y    0..23 -> source 24..47 -> red   (still in top half)
;   composite y   24..71 -> source 48..95 -> blue  (whole bottom half = 48 rows)
;   composite y   72..95 -> source  0..23 -> red   (wrap)
; so the 256x192 area (composite y maps 2x to lores y) shows three bands:
; 48px red | 96px blue | 48px red.
;
; what this catches: a renderer that ignores nr_33 - the original layout has
; a single horizontal red/blue split, with no wrap. honouring nr_33 makes
; the red appear at both top and bottom.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

; scroll y = 24 (= 1/4 of lores height).
	set_nr $32, 0
	set_nr $33, 24

; fill: lines 0..47 = $01 (red), lines 48..95 = $02 (blue).
; 256c top half = bank-5 $0000-$17ff; bottom half = $2000-$37ff.
	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 2
	ldir

	pass
