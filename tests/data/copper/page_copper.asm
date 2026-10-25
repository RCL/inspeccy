; technique c -- mid-frame layer-2 page flip (double-buffer tear), copper build.
;
; the copper rewrites nr $12 (l2 base 16k bank) once, mid-frame, to switch which
; buffer the beam reads: buffer a (bank 9, all colour 1) for display rows 0..95,
; buffer b (bank 12, all colour 2) for rows 96..191. the result is a clean
; horizontal tear at row 96 -- exactly what a beam-synced buffer swap produces on
; hardware. page_soft.asm reaches the identical split image from a single static
; buffer (top half colour 1, bottom half colour 2) with no copper.
;
; this is the case regview alone cannot render: nr $12 is a register, but the
; pixel comes from a memory fetch, so the l2/256 compositor must fetch each
; scanline's byte from the bank live at that scanline (de-hoisted fetch). the
; flip is on a line boundary (h=0), so it is correct regardless of whether the
; hardware latches the bank per-pixel or per-line.

	include "common.inc"
	include "_common.inc"	; delay_long

bank_a equ 9	; buffer a: 16k bank 9  -> 8k pages 18..23
bank_b equ 12	; buffer b: 16k bank 12 -> 8k pages 24..29
page_a equ bank_a * 2
page_b equ bank_b * 2

	silence_ula
	set_nr $70, $00	; l2 256x192
	set_nr $12, bank_a
	set_nr $69, $80	; l2 enable
	set_pal_entry 1, 1, $e0, $00	; colour 1 (red)
	set_pal_entry 1, 2, $03, $01	; colour 2 (blue)
	set_nr $16, 0
	set_nr $17, 0
	set_nr $71, 0

; only the pages the beam reads need filling (keeps the ldir budget small):
;   rows 0..95   -> bank a pages 0..2 (colour 1)
;   rows 96..191 -> bank b pages 3..5 (byte index stays row-major over 0..191)
	rept 3, page
	set_nr $52, page_a + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
	rept 3, page
	set_nr $52, page_b + 3 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 2
	ldir
	endr
	set_nr $52, 4	; restore default $4000 paging

; --- copper: bank a for rows 0..95, bank b for 96..191 (flip at row 96). ---
; wait line is display-row-relative (line 0 = display row 0).
	set_nr $61, $00
	set_nr $62, $00
	ld bc, port_reg_sel
	ld a, $63
	out (c), a
	ld bc, port_reg_access
; wait(line 0, h=0) ; move nr $12 = bank_a
	ld a, $80
	out (c), a
	xor a
	out (c), a
	ld a, $12
	out (c), a
	ld a, bank_a
	out (c), a
; wait(line 96, h=0) ; move nr $12 = bank_b
	ld a, $80
	out (c), a
	ld a, 96
	out (c), a
	ld a, $12
	out (c), a
	ld a, bank_b
	out (c), a
; park
	ld a, $ff
	out (c), a
	ld a, $ff
	out (c), a

	set_nr $62, $c0	; run, restart every frame

	di
	delay_long
	pass
