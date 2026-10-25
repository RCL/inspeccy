; nr_15 priority mode 1 = lsu (layer2 > sprites > ula/lores).
; render order: ula bottom, sprites middle, l2 top.
; visible: l2 red covers everything in 256x192 (including the sprite area).
; solid red 256x192, no blue sprite visible.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $85	; priority 1 = lsu

	pass
