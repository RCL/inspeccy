; lores 256c with both x and y scroll set simultaneously.
;
; the framebuffer is a 4-quadrant pattern: top-left red, top-right blue,
; bottom-left green, bottom-right yellow. each quadrant is 64x48 source
; pixels (= 128x96 / 2 in both axes).
;
; scroll: nr_32 = 64, nr_33 = 48 - exactly one quadrant in each axis.
; the (x+64)%128, (y+48)%96 wrap effectively rotates the quadrants
; diagonally: what was top-left becomes bottom-right, top-right becomes
; bottom-left, etc.
;
; what this catches: a renderer that applies only one axis of the scroll,
; or applies them with the wrong modulus, won't produce the full
; diagonal quadrant swap.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_pal_entry 0, 3, $1c, $00	; green
	set_pal_entry 0, 4, $fc, $00	; yellow
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

	set_nr $32, 64
	set_nr $33, 48

; top 48 source rows (rows 0..47) live at bank-5 $0000-$17ff.
; each row = 128 bytes; left 64 = $01, right 64 = $02.
; Loop over rows so the dot command stays below the framebuffer at $4000.
	macro fill48rows256c __arg_startaddr, __arg_leftval, __arg_rightval
	ld hl, __arg_startaddr
	ld b, 48
.row:
	ld a, __arg_leftval
	dup 64
	ld (hl), a
	inc hl
	edup
	ld a, __arg_rightval
	dup 64
	ld (hl), a
	inc hl
	edup
	dec b
	jp nz, .row
	endm

	fill48rows256c $4000, 1, 2	; top half: red | blue
	fill48rows256c $6000, 3, 4	; bottom half: green | yellow

	pass
	assert $ <= $4000
