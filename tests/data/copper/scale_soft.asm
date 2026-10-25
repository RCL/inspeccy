; technique a -- vertical scanline scaling, software build (no copper).
;
; same final image as scale_copper.asm, reached without the copper: a 96-row
; source pre-doubled in software with constant nr $33 = 0. the lores hardware
; x2 then maps display row d -> source row (d>>1). filling source row s with
; palette index (s>>1)+1 makes display row d show palette[((d>>1)>>1)+1] =
; palette[(d>>2)+1] -- byte-identical to the copper build's output.

	include "common.inc"
	include "_common.inc"

	setup_lores_256c

; --- framebuffer: rows 0..95 = byte (s>>1)+1. ---
; 256c split layout: rows 0..47 at $4000, rows 48..95 at $6000.
	ld hl, $4000
	ld e, 0	; s = 0..47
filltop:
	ld a, e
	srl a
	inc a	; (s>>1)+1
	ld b, 128
filltop2:
	ld (hl), a
	inc hl
	djnz filltop2
	inc e
	ld a, e
	cp 48
	jr nz, filltop

	ld hl, $6000
fillbot:	; e = 48..95
	ld a, e
	srl a
	inc a	; (s>>1)+1
	ld b, 128
fillbot2:
	ld (hl), a
	inc hl
	djnz fillbot2
	inc e
	ld a, e
	cp 96
	jr nz, fillbot

	pass
