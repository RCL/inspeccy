; ulanext mode (nr_43 bit 0, nr_42 ink mask). with mask 7 the attribute byte
; splits: ink index = attr & 7 -> palette entries 0..7; paper index = attr >> 3
; -> palette entries 128+. attribute $05 everywhere: ink 5, paper 128+0=128.
; pixel bytes $80 make the leftmost pixel of each cell ink; so composite columns
; 32,40,48,... are palette entry 5 (red) and the rest are entry 128 (blue).
; verified programmatically.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 5, $e0, $00	; palette 0 entry 5 = red (ink)
	set_pal_entry 0, 128, $03, $01	; palette 0 entry 128 = blue (paper)

	set_nr $52, 10	; map bank 5 (ula screen) to $4000
	ld hl, $4000	; pixels: leftmost pixel of each cell ink
	ld de, $4001
	ld bc, 6143
	ld (hl), $80
	ldir
	ld hl, $5800	; attributes = $05 (ink 5, paper 0)
	ld de, $5801
	ld bc, 767
	ld (hl), $05
	ldir
	set_nr $52, 4

	set_nr $42, 7	; ulanext ink mask = 7
	set_nr $43, $01	; palette 0, ulanext enable (bit 0)

	pass
