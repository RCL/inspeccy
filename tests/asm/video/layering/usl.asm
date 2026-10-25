; nr_15 priority mode 4 = usl (ula/lores > sprites > layer2).
; render order: l2 bottom, sprites middle, ula top.
; visible: green lores covers l2 and sprites across the whole 256x192 area.
; solid green, no sprite visible.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $91	; priority 4 = usl

	pass
