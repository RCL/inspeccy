; l2_256 palette decoder: 9th blue bit must propagate.
;
; colour $03 $01 in next palette notation:
;   hi = $03 = 0b0000_0011 = (r3=0)(g3=0)(b2..b1=11)
;   lo = $01 = 0b0000_0001 = (b0=1)
; net 9-bit colour = b3..b0 = 0b111 plus the 9th bit. the next renderer
; (decodenxentry) reconstructs blue as ((hi&3)<<1) | (lo&1) = 0b111 = 7,
; then expands to 8 bits as 0xff. so this should render pure blue.
;
; what this catches: any palette decoder that drops the lo byte, treats it
; as a separate channel, or shifts the blue field wrong. a wrong decoder
; would render dim blue ($db) or a non-blue colour entirely.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula
	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $03, $01	; full-saturated blue via 9th bit

	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

	set_nr $52, 4

	pass
