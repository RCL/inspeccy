; layer 2 clip window ($18). a solid-red l2_256 framebuffer clipped to the
; l2-space rectangle x 64..191, y 48..143. only that rectangle shows red;
; everything else in the paper area falls through to the black fallback.
;
;   l2 coords (64,48)-(191,143) -> composite (32+64,32+48)-(32+191,32+143)
;                               =  (96,80)-(223,175)
; verified programmatically (red inside, black elsewhere in paper).

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00	; l2 256x192
	set_nr $12, l2_bank_16k
	set_nr $69, $80	; l2 display enable
	set_pal_entry 1, 1, $e0, $00	; palette 1 entry 1 = red

	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
	set_nr $52, 4

; clip l2 to the centred rectangle (x1,x2,y1,y2 in l2 space).
	set_clip $18, 1, 64, 191, 48, 143

	pass
