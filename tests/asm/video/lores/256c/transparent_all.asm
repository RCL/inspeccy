; lores 256c transparency: every pixel uses a palette entry whose hi byte
; equals nr_14, so every lores pixel is transparent. with l2 disabled and
; ula silenced, the visible result is the black fallback across the whole
; 256x192 lores area.

	include "common.inc"

	silence_ula	; nr_14 := $e3
	set_pal_entry 0, 1, $e3, $00	; palette 0 entry 1 -> transparent
	set_nr $6a, $00	; 256c
	set_nr $15, $80	; lores enable

	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 1
	ldir

	pass
