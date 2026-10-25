; sprite pattern using $e3 for transparent pixels (default nr_4b value).
; pattern: top 8 rows = $01 (red), bottom 8 rows = $e3 (transparent).
; sprite at (128, 96). expected: only the top 8 rows visible -> 16x8 red
; block at (128, 96..103).
;
; what this verifies:
;   - nr_4b sentinel match: pattern byte == nr_4b -> pixel not drawn
;   - default sentinel value works (= $e3)

	include "common.inc"

	silence_ula
	set_nr $09, $10
	set_nr $15, $01
	set_nr $4b, $e3	; explicit default
	set_pal_entry 2, 1, $e0, $00	; red

; upload pattern: 128 bytes (8 rows x 16) of $01, then 128 bytes of $e3.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; upload attr.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0457
	otir

	pass

pattern:
	dup 128
	db $01
	edup
	dup 128
	db $e3
	edup

attr:
	db 128
	db 96
	db $00
	db $80
