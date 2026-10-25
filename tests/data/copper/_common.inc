; shared setup for the copper render tests (lores 256c + palette gradient).
;
; these tests prove that the copper vm + --pixel-accurate scanline compositor
; reproduce, pixel for pixel, an image that a plain (copper-less) program draws
; by hand. each technique ships a copper build and a software build; the python
; harness (test_copper_render.py) renders both and asserts the bmps are equal.
;
; lores 256c gives a clean 1-byte-per-pixel source (palette index == vram byte),
; so the framebuffer content and the register maths stay easy to reason about.

; setup_lores_256c -- silence_ula, enable lores 256c with zero scroll, and
; load a 48-entry gradient into ula palette 0 entries 1..48 (entry i -> a
; distinct rrrgggbb byte i). entry 0 stays transparent (silence_ula), so
; the framebuffer only ever uses opaque indices 1..48. leaves d=49, bc
; clobbered.
	macro setup_lores_256c
	silence_ula
	set_nr $6a, $00	; 256c mode, palette offset 0
	set_nr $15, $80	; lores enable
	set_nr $32, 0	; no x scroll
	set_nr $33, 0	; no y scroll (baseline)
	set_nr $43, $00	; select ula palette 0 for writes; bit1=0
	ld d, 1	; palette entry 1..48
.palloop:
	ld a, $40
	ld bc, port_reg_sel
	out (c), a
	ld a, d
	ld bc, port_reg_access
	out (c), a	; nr $40 = entry index
	ld a, $44
	ld bc, port_reg_sel
	out (c), a
	ld bc, port_reg_access
	ld a, d
	out (c), a	; nr $44 hi = i -> distinct colour
	xor a
	out (c), a	; nr $44 lo = 0 (9th blue bit)
	inc d
	ld a, d
	cp 49
	jr nz, .palloop
	endm

; delay_long -- spin ~24 frames so the copper populates several complete
; per-frame timelines; --pixel-accurate renders the last completed frame.
	macro delay_long
	ld hl, 0
.delayloop:
	dec hl
	ld a, h
	or l
	jr nz, .delayloop
	endm

; halt_frames n -- ei + im1, then halt for n frame interrupts. halt is the
; point: the emulator's halted-cpu fast-forward snaps the t-state clock to
; each copper wait deadline exactly, so a mid-scanline nr $32 reset lands on
; its precise cycle (a spin loop overshoots by an instruction => the split
; column jitters). hl survives the rom frame isr (pushed/popped), so it is a
; safe frame counter. leaves interrupts disabled.
	macro halt_frames __arg_n
	ei
	im 1
	ld hl, __arg_n
.haltloop:
	halt
	dec hl
	ld a, h
	or l
	jr nz, .haltloop
	di
	endm
