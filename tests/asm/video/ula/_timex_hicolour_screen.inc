; shared screen fixture for the next timex 8x1 hi-colour tests.
;
; paints a picture that can only look right if the renderer takes its colour
; from the scld hi-colour plane at $6000 (one attribute byte per scanline per
; cell) rather than from the 8x8 attribute plane at $5800:
;
;   bitmap  $4000..$57ff : $cc everywhere - 2 pixels on, 2 off, so ink and
;                          paper are both visible on every single line
;   colour  $6000..$77ff : attr = y & $7f, i.e. ink = y&7 (a new ink every
;                          scanline), paper = (y>>3)&7 (a band every 8), and
;                          bright for y in 64..127
;   attrs   $5800..$5aff : one flat value
;
; so the correct rendering is a fine per-scanline ink rainbow, and a renderer
; that ignores port $ff paints one flat colour over the whole screen instead.
; nothing here selects a display mode - each test does that its own way.

bitmap_pattern equ $cc	; %11001100
flat_attr equ $07	; what the 8x8 plane holds; only ever
; displayed if the scld decode is missed

; the 8x8 plane is written explicitly so that even the wrong rendering
; is deterministic - a regression gives one flat screen, not whatever
; nextzxos happened to leave behind.
	ld hl, $5800
	ld de, $5801
	ld bc, 768 - 1
	ld (hl), flat_attr
	ldir

	ld hl, $4000	; scanline 0
	ld c, 0	; scanline counter, 0..191
.line:
	ld a, c
	and $7f	; ink y&7 | paper (y>>3)&7 | bright y&64
	ld d, a
	push hl
	ld b, 32
.cell:
	ld (hl), bitmap_pattern
	set 5, h	; $4000 -> $6000: the hi-colour plane
	ld (hl), d
	res 5, h
	inc l	; a scanline's 32 bytes never cross a page
	djnz .cell
	pop hl

; standard "one scanline down" step for the interleaved screen layout.
	inc h
	ld a, h
	and 7
	jr nz, .next
	ld a, l
	add a, 32
	ld l, a
	jr c, .next	; carry = we moved into the next third
	ld a, h
	sub 8
	ld h, a
.next:
	inc c
	ld a, c
	cp 192
	jr nz, .line
