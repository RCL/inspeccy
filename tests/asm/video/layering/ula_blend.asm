; nr_15 priority mode 6 = ula-blend.
;
; per https://wiki.specnext.dev/Sprite_and_Layers_System the compositor in
; this mode sums the ula-slot pixel (= classic ula or lores per nr_15 bit 7)
; with the underlying layer 2 pixel, channel by channel in 3-bit rgb space,
; clamped at $07 per channel. sprites are drawn last on top, unaffected by
; the blend.
;
; this test's setup paints l2 solid red ($01 -> palette entry $e0 $00 =
; r=7 g=0 b=0) and lores solid green ($02 -> palette entry $1c $00 =
; r=0 g=7 b=0). the expected blend result is therefore (r=7, g=7, b=0)
; = saturated yellow everywhere they overlap. a single blue sprite at
; (128, 96) writes over the centre.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $99	; priority 6 = ula-blend

	pass
