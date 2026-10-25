; nr_15 priority mode 2 = sul (sprites > ula/lores > layer2).
; render order: l2 bottom, ula middle, sprites top.
; visible: green lores covers l2 across the whole 256x192 area; blue sprite
; sits on top at (128, 96). green field with 16x16 blue square.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $89	; priority 2 = sul

	pass
