; port $57 auto-advances after each four- or five-byte attribute group,
; including when nr_09 bit 4 is clear. upload four sprites in one otir.
; lockstep controls synchronization with the nextreg cursor.

	include "common.inc"

	silence_ula
; deliberately no lockstep (nr_09 left at $00).
	set_nr $15, $03	; sprite engine on, over-border on (sprites need
; to be visible across the full canvas)
	set_nr $4b, $e3
; each sprite below uses pattern byte $01 with palette offsets 0..3.
; final palette indices are $01 + (offset << 4) = $01, $11, $21, $31.
	set_pal_entry 2, 1, $e0, $00	; offset 0 -> red
	set_pal_entry 2, 17, $03, $01	; offset 1 -> blue
	set_pal_entry 2, 33, $1c, $00	; offset 2 -> green
	set_pal_entry 2, 49, $fc, $00	; offset 3 -> yellow

; pattern slot 0: solid pixel $01 (red palette entry 1).
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; reset attribute slot to 0 and otir 16 bytes = 4 attribute groups.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $1057	; b=$10 = 16 bytes = 4 sprites
	otir

	pass

pattern:
	dup 256
	db $01
	edup

attr:
; sprite 0: palette offset 0 -> palette entry 1 = red, at (64, 64)
	db 64
	db 64
	db $00
	db $80
; sprite 1: palette offset 1 -> palette entry 2 = blue, at (128, 64)
	db 128
	db 64
	db $10	; (palette offset 1) << 4
	db $80
; sprite 2: palette offset 2 -> green, at (64, 128)
	db 64
	db 128
	db $20
	db $80
; sprite 3: palette offset 3 -> yellow, at (128, 128)
	db 128
	db 128
	db $30
	db $80
