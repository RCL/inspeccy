; 4-bit (16-colour) sprites: attr4 bit 7 (h) = 1. pattern is 128 bytes of
; nibbles (2 patterns per 256-byte slot); $303b bit 7 selects the upload half,
; attribute 4 bit 6 (n6) selects the render half.
;
;   pattern 0 (slot 0 low half):  left 8 cols nibble 1, right 8 cols nibble 2.
;   pattern 1 (slot 0 high half): all nibble 2.
;   sprite 0 at (100,100) uses pattern 0 -> red left half, blue right half.
;   sprite 1 at (140,100) uses pattern 1 -> all blue.
; palette (2): entry 1 = red, entry 2 = blue. verified programmatically.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; port $57 lockstep (5-byte groups for e=1)
	set_nr $15, $01	; sprite engine on
	set_nr $4b, $e3	; transparent index (low nibble 3, unused here)
	set_pal_entry 2, 1, $e0, $00	; entry 1 = red
	set_pal_entry 2, 2, $03, $01	; entry 2 = blue

; upload 4-bit pattern 0 into slot 0 low half (128 bytes).
	xor a
	ld bc, $303b
	out (c), a	; slot 0, half 0
	ld hl, pat4lo
	ld bc, $805b	; b=128 bytes, c=$5b
	otir
; upload 4-bit pattern into slot 0 high half (128 bytes, all nibble 2).
	ld a, $80
	ld bc, $303b
	out (c), a	; slot 0, half 1
	ld hl, pat4hi
	ld bc, $805b
	otir

; attributes: two 5-byte sprites, both 4-bit (attr4 bit 7 = 1).
	xor a
	ld bc, $303b
	out (c), a	; select slot 0 for attribute upload
	ld hl, attrdata
	ld bc, $0a57	; b=10 bytes (2 x 5), c=$57
	otir

	pass

pat4lo:
; 16 rows x 8 bytes: cols 0-7 nibble 1 ($11 x4), cols 8-15 nibble 2 ($22 x4).
	dup 16
	db $11, $11, $11, $11, $22, $22, $22, $22
	edup
pat4hi:
; 128 bytes of $22 (all nibble 2 = blue).
	dup 128
	db $22
	edup

attrdata:
	db 100, 100, $00, $c0, $80	; sprite 0: v,e,pattern 0; 4-bit (bit7)
	db 140, 100, $00, $c0, $c0	; sprite 1: v,e,slot 0; h=1,n6=1 selects upper half
