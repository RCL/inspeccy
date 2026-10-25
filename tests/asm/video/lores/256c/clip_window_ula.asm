; ula/lores clip window ($1a), programmed the documented way: reset the write
; index through nr $1c, then four writes of x1,x2,y1,y2.
;
; a solid-red 256c lores buffer clipped to the ula-space rectangle
; x 32..223, y 48..143. only that rectangle shows red; outside it the lores
; layer is clipped away and the paper area falls through to the black fallback.
;
;   ula coords (32,48)-(223,143) -> composite (32+32,32+48)-(32+223,32+143)
;                                =  (64,80)-(255,175)
;
; regression: nr $1c used to carry the "layer 2 is in use" hint. it is the
; shared clip index reset (one bit per layer), so this perfectly ordinary
; sequence latched the hint, sent the compositor down the layer 2 path, and
; painted a blank layer 2 over everything - a completely black frame from a
; program that never mentions layer 2. without the fix this test renders black.

	include "common.inc"

	silence_ula

	ld a, 4	; green outer border distinguishes it from black fallback
	out ($fe), a

	set_pal_entry 0, 1, $e0, $00	; red
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

; fill both halves of the 256c layout: rows 0..47 at $4000, 48..95 at $6000.
	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 1
	ldir

; clip the ula/lores layer. bit 2 of nr $1c resets this layer's index.
	set_clip $1a, 4, 32, 223, 48, 143

	pass
