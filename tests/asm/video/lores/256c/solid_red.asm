; lores 256c basic rendering: every pixel = palette index 1, palette 0
; entry 1 set to pure red. whole 256x192 (= 128x96 scaled 2x) area must be
; solid red.
;
; what this verifies:
;   - nr_15 bit 7 enables lores
;   - nr_6a bit 5 = 0 selects 256-colour mode (1 byte / pixel)
;   - compositor reads from both halves: $4000-$57ff (lines 0..47) and
;     $6000-$77ff (lines 48..95). a reader that ignores the second half
;     would show only the top 96 composite-pixels (= 48 lores lines).
;   - lores uses the ula palette (palette 0).

	include "common.inc"

	silence_ula	; sets nr_14 := $e3 + palette 0 entry 0 := transparent
	set_pal_entry 0, 1, $e0, $00	; palette 0 entry 1 -> red

; lores 256c, palette offset 0.
	set_nr $6a, $00	; bit 5 = 0 -> 256c, palette offset 0
; enable lores display.
	set_nr $15, $80	; bit 7 = lores; priority slu (default 0)

; fill the lores framebuffer with palette index 1.
; 256c layout: lines 0..47 at bank-5 $0000-$17ff (= $4000-$57ff here),
; lines 48..95 at bank-5 $2000-$37ff (= $6000-$77ff). on the
; dot-cmd-launch 128k layout slot 2 = bank 5 first half and slot 3 =
; bank 5 second half, so $4000 and $6000 already point at the right
; places. just ldir-fill both ranges.
	ld hl, $4000
	ld de, $4001
	ld bc, $17ff	; lines 0..47 (48 * 128 = 6144 bytes, minus first byte)
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff	; lines 48..95
	ld (hl), 1
	ldir

	pass
