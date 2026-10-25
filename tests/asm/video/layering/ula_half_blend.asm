; nr_15 priority mode 7 = ula-half-blend.
;
; same as ula_blend (= mode 6) except the sum of the ula-slot pixel and
; the underlying layer 2 pixel is halved instead of clamped. in 3-bit rgb
; space: result_channel = (ula_channel + l2_channel) / 2. sprites still
; draw last on top, unaffected.
;
; with the shared setup (l2 = r=7 g=0 b=0, lores = r=0 g=7 b=0), the blend
; result everywhere they overlap is (r=3, g=3, b=0) = a dim olive. the
; sprite at (128, 96) overwrites that with blue.

	include "common.inc"
	include "video/layering/_setup_rgb.inc"

	set_nr $15, $9d	; priority 7 = ula-half-blend

	pass
