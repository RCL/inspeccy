; lros_smoke.asm - the smallest cartridge that proves the whole dock path.
;
; built into data/dck/lros_smoke.dck by make_lros_smoke.py, which puts this in
; dock chunk 0 and a single marker byte in dock chunk 1.
;
; what a passing run proves, in order:
;   * the .dck parsed and both chunks reached the dock;
;   * the home rom found the lros header and handed over (which only happens
;     if the chunk-specification byte is understood as the complement of the
;     hsr value - $fe here, not $01);
;   * this code is executing from cartridge rom at $0000-$1fff;
;   * hsr paging reaches a chunk other than the one we are running from: the
;     screen is painted with a byte fetched from chunk 1, so if dock paging is
;     broken the fill comes out as open-bus $ff and the test fails on the
;     bitmap rather than passing by accident.
;
; assembled with no device: raw bytes from $0000.

hsr equ $f4
hsr_self equ $01	; only chunk 0 (this code) from the dock
hsr_chunk1 equ $03	; chunk 0 + chunk 1
marker equ $2000	; chunk 1's window; the marker sits at +0
fill equ $5b00	; scratch in the printer buffer

	org $0000
	db $00
	db $01	; lros
	dw entry
	db (~hsr_self) & $ff	; chunk spec is complemented: $01 -> $fe

	defs $0038 - $, $ff
	ei
	ret

	defs $0066 - $, $ff
	retn

	defs $0100 - $, $ff
entry:
	di
	ld a, hsr_self
	out (hsr), a
	ld sp, $6000

; fetch the fill byte from a different dock chunk
	ld a, hsr_chunk1
	out (hsr), a
	ld a, (marker)
	ld (fill), a	; $5b00: home ram either way
	ld a, hsr_self
	out (hsr), a	; dock chunk 1 back out

	xor a
	out ($ff), a	; plain spectrum screen, deterministically
	out ($fe), a	; black border

	ld hl, $5800	; attributes: white ink on black paper
	ld de, $5801
	ld bc, 768 - 1
	ld (hl), $07
	ldir

	ld hl, $4000	; bitmap: the byte fetched from chunk 1
	ld de, $4001
	ld bc, 6144 - 1
	ld a, (fill)
	ld (hl), a
	ldir

hang:
	jr hang
