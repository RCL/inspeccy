; nr_15 priority mode 0 = slu (sprites > l2 > ula/lores).
; render order: ula bottom, l2 middle, sprites top.
; visible: l2 red everywhere in 256x192 except where the blue sprite covers
; it. sprite at (128, 96) is fully within the 256x192 area, so a 16x16
; blue square sits on red, with red filling the rest of the area.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

; nr_15: bit 7 = lores enable, bit 0 = sprite enable, bits 4:2 = priority.
; priority 0 = slu.
	set_nr $15, $81

	pass
