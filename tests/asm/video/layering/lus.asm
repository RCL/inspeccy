; nr_15 priority mode 3 = lus (layer2 > ula/lores > sprites).
; render order: sprites bottom, ula middle, l2 top.
; visible: l2 red covers everything in 256x192. solid red.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $8d	; priority 3 = lus

	pass
