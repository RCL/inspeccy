; lores 16c with both x and y scroll set simultaneously.
;
; quadrant layout (in 16c packed bytes):
;   top-left  ($11 = palette idx 1 in both nibbles)  -> red
;   top-right ($22 = palette idx 2)                  -> blue
;   bottom-left  ($33)                               -> green
;   bottom-right ($44)                               -> yellow
;
; scroll: nr_32 = 64, nr_33 = 48 - diagonal quadrant rotation.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_pal_entry 0, 3, $1c, $00	; green
	set_pal_entry 0, 4, $fc, $00	; yellow
	set_nr $6a, $20	; 16c
	set_nr $15, $80

	set_nr $32, 64
	set_nr $33, 48

; 96 rows of 64 bytes total in 16c. each row's left 32 bytes = leftval
; (= packed pair of left-half palette indices), right 32 bytes = rightval.
; Keep the row loop compact: unrolling all 96 rows extends the dot command
; into $4000 and the framebuffer fill overwrites its own instructions.
	macro fill16crow __arg_leftval, __arg_rightval
	ld a, __arg_leftval
	dup 32
	ld (hl), a
	inc hl
	edup
	ld a, __arg_rightval
	dup 32
	ld (hl), a
	inc hl
	edup
	endm

	ld hl, $4000
	ld b, 48
.top:
	fill16crow $11, $22	; top half: red | blue
	dec b
	jp nz, .top
	ld b, 48
.bottom:
	fill16crow $33, $44	; bottom half: green | yellow
	dec b
	jp nz, .bottom

	pass
	assert $ <= $4000
