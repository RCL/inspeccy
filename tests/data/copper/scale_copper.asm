; technique a -- vertical scanline scaling, copper build.
;
; the copper rewrites nr $33 (lores y-offset) once per scanline so that display
; row d shows lores source row (d>>2): a 48-row source magnified x4 vertically.
; (the lores hardware already does x2; the copper's per-line nr $33 buys the
; other x2.) with the renderer sampling srcy = ((d>>1) + nr33) % 96, choosing
;   nr33(d) = ((d>>2) - (d>>1))  mod 96
; makes srcy collapse to (d>>2). source row r holds palette index r+1, so
; display row d shows palette[(d>>2)+1] -- identical to scale_soft.asm, which
; reaches the same image with a pre-doubled 96-row source and no copper.

	include "common.inc"
	include "_common.inc"

	setup_lores_256c

; --- framebuffer: rows 0..47 = byte (r+1); zero everything else. ---
	ld hl, $4000
	ld de, $4001
	ld bc, $3fff
	ld (hl), 0
	ldir	; zero the whole 16k bank first
	ld hl, $4000
	ld d, 1	; value for row r = r+1 (1..48)
fillrowc:
	ld b, 128
	ld a, d
fillbytec:
	ld (hl), a
	inc hl
	djnz fillbytec
	inc d
	ld a, d
	cp 49
	jr nz, fillrowc	; rows 0..47 -> values 1..48

; --- upload the copper program ---
; write address = 0, mode = stop (also clears the 16-bit assembly latch).
	set_nr $61, $00
	set_nr $62, $00
; stream 16-bit instructions through nr $63 (hi byte then lo byte).
	ld bc, port_reg_sel
	ld a, $63
	out (c), a	; select nr $63 once
	ld bc, port_reg_access	; bc = $253b for the whole upload
	ld d, 0	; d = raster line 0..191
uploadloop:
; wait(line = d, h = 0). the copper's wait line is display-row-relative on
; hardware (line 0 = display row 0); the emulator adds the top border.
	ld a, $80
	out (c), a
	ld a, d
	out (c), a
; move nr $33 = val(d)   ->  hi = $33 (register index), lo = val
	ld a, $33
	out (c), a
	ld a, d
	srl a	; d>>1
	ld e, a
	ld a, d
	srl a
	srl a	; d>>2
	sub e	; (d>>2) - (d>>1), 8-bit two's complement
	add a, 96
	cp 96
	jr c, uploadnoadj
	sub 96	; keep result in 0..95
uploadnoadj:
	out (c), a	; lo = val(d) -> commit the move
	inc d
	ld a, d
	cp 192
	jr nz, uploadloop
; terminal park: wait $ffff (off-screen line -> park until frame restart).
	ld a, $ff
	out (c), a
	ld a, $ff
	out (c), a

; run, mode 3 (run + restart the list every frame), write-addr hi = 0.
	set_nr $62, $c0

	di
	delay_long
	pass
