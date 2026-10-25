; nr $43: which ula palette is written, which is displayed, and whether the
; palette index auto-increments.
;
; the lores buffer holds indices 0..7 in eight vertical bands. the program
; writes an eight-step grey ramp into the ula *second* palette (nr $43 bits 6:4
; = 100) with auto-increment on, then selects that palette for display (bit 1).
; if either half of nr $43 is ignored, the bands come out of the first palette
; -- the default rgb332 identity, which is violently coloured -- instead of grey.
;
; regression: the display path used to hardcode ula palette 0 and the write path
; used to auto-increment unconditionally, so a program that wrote one palette
; and displayed the other, or that left the auto-increment disabled, rendered
; here in colours it would never show on hardware.

	include "common.inc"

	silence_ula
	set_nr $6a, $00	; lores 256c
	set_nr $15, $80	; lores enable

; --- autoinc disabled: 8 writes must all land on one entry ---
; entry 204 is set black first; if the gate is ignored the eight writes
; walk 200..207 and turn it red, which the last band below would show.
	set_nr $43, %01000000	; write ula-2, autoinc on
	set_nr $40, 204
	set_nr $41, $00	; entry 204 = black
	set_nr $43, %11000000	; write ula-2, autoinc off
	set_nr $40, 200
	set_nr $41, $e0	; all eight land on entry 1
	set_nr $41, $e0
	set_nr $41, $e0
	set_nr $41, $e0
	set_nr $41, $e0
	set_nr $41, $e0
	set_nr $41, $e0
	set_nr $41, $e0

; --- now the real ramp, autoinc on, still writing ula-2 ---
	set_nr $43, %01000000	; write ula-2, autoinc on
	set_nr $40, 0
	set_nr $41, $00	; the ramp, entries 0..7
	set_nr $41, $24
	set_nr $41, $49
	set_nr $41, $6d
	set_nr $41, $92
	set_nr $41, $b6
	set_nr $41, $db
	set_nr $41, $ff

; entry 1 must be the ramp value, not the $e0 written with autoinc off
	set_nr $43, %01000010	; display ula-2 as well

; nine vertical bands: indices 0..7, then 204 (black iff autoinc was off)
	ld hl, $4000
	ld c, 48	; rows 0..47
.top:
	ld a, 0
	ld b, 9
.topband:
	push bc
	ld b, 14
.topfill:
	ld (hl), a
	inc hl
	djnz .topfill
	pop bc
	inc a
	cp 8
	jr nz, .topnext
	ld a, 204
.topnext:
	djnz .topband
	dec c
	jr nz, .top

	ld hl, $6000	; rows 48..95
	ld c, 48
.bot:
	ld a, 0
	ld b, 9
.botband:
	push bc
	ld b, 14
.botfill:
	ld (hl), a
	inc hl
	djnz .botfill
	pop bc
	inc a
	cp 8
	jr nz, .botnext
	ld a, 204
.botnext:
	djnz .botband
	dec c
	jr nz, .bot

	pass
