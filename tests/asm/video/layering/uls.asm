; nr_15 priority mode 5 = uls (ula/lores > layer2 > sprites).
; render order: sprites bottom, l2 middle, ula top.
; visible: green lores covers everything in 256x192. solid green.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $95	; priority 5 = uls

	pass
