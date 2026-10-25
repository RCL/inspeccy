; @needs-audio-out
; port $df captures dac samples while nr_08 bit 3 is clear.
; write a 32-byte sweep; the runner requires wav data beyond the header.

	include "common.inc"

; make sure nr_08 bit 3 is off - this is the case we're verifying.
; (default is 0 anyway; this just makes the intent explicit.)
	set_nr $08, $00

; write 32 samples to port $df. use a small loop so the .dot stays
; tiny; values are intentionally non-zero to force the capture to
; allocate the wav header + body.
	ld b, 32
	ld a, $01
.loop:
	out ($df), a
	add a, $04	; sweep the dac value 1, 5, 9, ..., 125
	djnz .loop

	pass
