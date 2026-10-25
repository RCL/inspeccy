; technique c -- mid-frame layer-2 page flip, software build (no copper).
;
; same final image as page_copper.asm from a single static l2 buffer: rows 0..95
; drawn colour 1, rows 96..191 colour 2, with nr $12 fixed. the end-of-frame
; renderer reads one bank the whole frame, so the split has to be baked into the
; framebuffer -- byte-identical to the copper build's beam-synced page flip.

	include "common.inc"

bank_a equ 9	; 16k bank 9 -> 8k pages 18..23
page_a equ bank_a * 2

	silence_ula
	set_nr $70, $00	; l2 256x192
	set_nr $12, bank_a
	set_nr $69, $80	; l2 enable
	set_pal_entry 1, 1, $e0, $00	; colour 1 (red)
	set_pal_entry 1, 2, $03, $01	; colour 2 (blue)
	set_nr $16, 0
	set_nr $17, 0
	set_nr $71, 0

; rows 0..95 (pages 0..2) = colour 1; rows 96..191 (pages 3..5) = colour 2.
	rept 3, page
	set_nr $52, page_a + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr
	rept 3, page
	set_nr $52, page_a + 3 + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 2
	ldir
	endr
	set_nr $52, 4	; restore default $4000 paging

	pass
