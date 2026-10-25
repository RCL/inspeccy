; technique b -- left-side duplication, copper build.
;
; the copper resets nr $32 (lores x-offset) mid-scanline so the same source
; content is drawn twice across the line. per scanline it issues:
;     wait(line, h=0)  move nr $32 = 0      ; left half: srcx = lox
;     wait(line, h=28) move nr $32 = 64     ; right half: srcx = lox + 64
; h=28 is the middle of the 56-unit line, i.e. display column 128. source cols
; 0..63 hold palette indices 1..64; both display halves therefore show those
; 64 columns -- the right half duplicates the left. scale_soft's counterpart,
; dup_soft.asm, reaches the same image with a pre-duplicated source and no
; copper. rendering this correctly requires the --pixel-accurate compositor to
; sample nr $32 per display column (sub-scanline), not once per line.

	include "common.inc"
	include "_common.inc"

	setup_lores_256c

; --- framebuffer: every row = [cols 0..63: index c+1][cols 64..127: 0]. ---
; build one 128-byte row at $4000, then replicate it to all 96 rows.
	ld hl, $4000
	ld c, 0	; column 0..127
buildrowc:
	ld a, c
	cp 64
	jr nc, buildzeroc	; c >= 64 -> unused (0)
	inc a	; c < 64 -> index c+1
	jr buildputc
buildzeroc:
	xor a
buildputc:
	ld (hl), a
	inc hl
	inc c
	ld a, c
	cp 128
	jr nz, buildrowc
; replicate row 0 to rows 1..47 (top bank) and 48..95 (bottom bank).
	ld hl, $4000
	ld de, $4080
	ld bc, 47 * 128
	ldir
	ld hl, $4000
	ld de, $6000
	ld bc, 48 * 128
	ldir

; --- upload the copper program (4 words per scanline) ---
	set_nr $61, $00
	set_nr $62, $00	; write addr = 0, mode = stop
	ld bc, port_reg_sel
	ld a, $63
	out (c), a	; select nr $63
	ld bc, port_reg_access
	ld d, 0	; raster line 0..191
uploadloop:
; wait(line = d, h = 0). copper wait line is display-row-relative (line 0 =
; display row 0); the emulator adds the top border.
	ld a, $80
	out (c), a
	ld a, d
	out (c), a
; move nr $32 = 0  (left half: no x offset)
	ld a, $32
	out (c), a
	xor a
	out (c), a
; wait(line = d, h = 28)  ->  hi=$b8 (h=28 in bits [14:9]), lo=d
	ld a, $b8
	out (c), a
	ld a, d
	out (c), a
; move nr $32 = 64  (right half: srcx = (lox + 64) % 128 -> cols 0..63)
	ld a, $32
	out (c), a
	ld a, 64
	out (c), a
	inc d
	ld a, d
	cp 192
	jr nz, uploadloop
; terminal park: wait $ffff
	ld a, $ff
	out (c), a
	ld a, $ff
	out (c), a

; run, mode 3 (run + restart every frame).
	set_nr $62, $c0

	halt_frames 40
	pass
