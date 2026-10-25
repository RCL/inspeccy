; lores 16c y scroll. framebuffer = top 48 rows $11 (red), bottom 48 rows
; $22 (blue). nr_33 = 24.
;
; composite source y = (y + 24) % 96. three horizontal bands:
;   composite y  0..47  (source y 24..47 then 48..71 ... etc) -> 48 lores
;     rows of red, then 96 lores rows of blue, then 48 lores rows of red.
;
; filling 96 rows of 64 bytes = 6144 bytes total. top 48 rows = $11
; (= 48 * 64 = 3072 bytes), bottom 48 rows = $22 (next 3072 bytes).

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red
	set_pal_entry 0, 2, $03, $01	; blue
	set_nr $6a, $20	; 16c
	set_nr $15, $80

	set_nr $32, 0
	set_nr $33, 24

; top 3072 bytes (= 48 rows) = $11.
	ld hl, $4000
	ld de, $4001
	ld bc, 3071
	ld (hl), $11
	ldir

; next 3072 bytes (= 48 rows) = $22.
	ld hl, $4c00
	ld de, $4c01
	ld bc, 3071
	ld (hl), $22
	ldir

	pass
