; technique b -- left-side duplication, software build (no copper).
;
; same final image as dup_copper.asm, reached without the copper: the source is
; pre-duplicated (cols 0..63 and 64..127 both hold indices 1..64) and nr $32
; stays 0. display column dx shows source col (dx>>1); with the doubled source
; both display halves show indices 1..64 -- byte-identical to the copper build.

	include "common.inc"
	include "_common.inc"

	setup_lores_256c

; --- framebuffer: every row = index ((c & 63) + 1) for cols 0..127. ---
	ld hl, $4000
	ld c, 0	; column 0..127
buildrows:
	ld a, c
	and 63
	inc a	; ((c & 63) + 1) -> 1..64, repeated twice
	ld (hl), a
	inc hl
	inc c
	ld a, c
	cp 128
	jr nz, buildrows
; replicate row 0 to rows 1..47 (top bank) and 48..95 (bottom bank).
	ld hl, $4000
	ld de, $4080
	ld bc, 47 * 128
	ldir
	ld hl, $4000
	ld de, $6000
	ld bc, 48 * 128
	ldir

	pass
