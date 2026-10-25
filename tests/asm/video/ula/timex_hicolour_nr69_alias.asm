; the same screen and the same mode, reached through nr $69 instead of the
; port. nextreg.txt: "0x69 (105) => display control 1 ... bits 5:0 = port 0xff
; bits 5:0 alias (timex display modes)".
;
; because it is an alias and not a second register, this test's reference image
; is byte-identical to timex_hicolour_port_ff's. if the two ever disagree, the
; alias has come apart in one direction or the other.

	include "common.inc"
	include "video/ula/_timex_hicolour_screen.inc"

	set_nr $69, $02	; bit 7 clear = l2 off; bits 5:0 = mode 2

	pass
