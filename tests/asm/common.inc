; shared header + assertion macros for inspeccy assembly tests.
;
; each test is assembled into a dot command (org $2000) and executed via
; `inspeccy --dot`. outcome signalling:
;
;   pass macro -> jp $0000 -> inspeccy exits with restarttrap (code 5).
;                the test runner expects exit code 5.
;
;   fail macro -> di / nop / halt -> haltedinterruptsdisabled (code 2).
;                any exit code other than 5 is treated as a test failure.
;                the runner reports which assertion fired by reading the
;                stderr summary (lastpc tells you which fail it hit).
;
; check_* macros compare a register against a constant. they expand to
; `cp / sbc hl, nn / ...` followed by `jr z, ok / fail / ok:`. on mismatch
; the test crashes via fail immediately, so the lastpc in inspeccy's exit
; summary points at the halt right after the failed assertion.
;
; local labels: sjasmplus macros use temporary labels prefixed with `.`
; that are scoped to the macro invocation. each check_* needs its own
; local label so cascaded checks don't collide.

	device zxspectrumnext
	org $2000

start:

; ---- outcome signalling ----------------------------------------------------

	macro pass
	jp $0000	; restarttrap -> inspeccy exit 5
	endm

	macro fail
	di
	nop
	halt	; haltedinterruptsdisabled -> inspeccy exit 2
	endm

; ---- register checks -------------------------------------------------------
; each check macro: compare register against immediate; if mismatch, fail.
; all scratch state (hl, flags) is preserved as much as practical so a
; sequence of checks can run without explicit save/restore.

	macro check_a __arg_val
	cp __arg_val
	jr z, .ok
	fail
.ok:
	endm

	macro check_bc __arg_val
	push hl
	ld hl, __arg_val
	or a	; clear carry
	sbc hl, bc
	pop hl
	jr z, .ok
	fail
.ok:
	endm

	macro check_de __arg_val
	push hl
	ld hl, __arg_val
	or a
	sbc hl, de
	pop hl
	jr z, .ok
	fail
.ok:
	endm

	macro check_hl __arg_val
	push de
	ld de, __arg_val
	or a
	sbc hl, de
	add hl, de	; restore hl
	pop de
	jr z, .ok
	fail
.ok:
	endm

; check_mem addr, val -- assert byte at memory address equals val.
	macro check_mem __arg_addr, __arg_val
	ld a, (__arg_addr)
	cp __arg_val
	jr z, .ok
	fail
.ok:
	endm

; ---- nextreg helpers ------------------------------------------------------
; cross-test scaffolding; not the subject under test. nextreg-instruction
; tests must use the raw ed 91 / ed 92 opcode form, not these macros.

port_reg_sel equ $243b
port_reg_access equ $253b

; set_nr rr, vv -- write 8-bit immediate vv to nextreg rr via the
; classic $243b / $253b port pair. used by all video setup code.
	macro set_nr __arg_rr, __arg_vv
	ld a, __arg_rr
	ld bc, port_reg_sel
	out (c), a
	ld a, __arg_vv
	ld bc, port_reg_access
	out (c), a
	endm

; set_pal_entry palette, entry, hi, lo -- write a 9-bit colour
; (hi, lo) to entry `entry` of palette `palette`. palette index
; goes into bits 6:4 of nr_palette_ctrl ($43). nr_pal_val_9b ($44)
; takes the hi byte on the first write, lo byte on the second.
; conventions: palette 0 = ula, 1 = l2 first, 2 = sprites,
; 5 = l2 second; see activel2paletteindex in bmp.cpp.
	macro set_pal_entry __arg_palette, __arg_entry, __arg_hi, __arg_lo
	set_nr $40, __arg_entry
	set_nr $43, (__arg_palette & 7) << 4
	set_nr $44, __arg_hi
	set_nr $44, __arg_lo
	endm

; silence_ula -- mark ula palette 0 entry 0 as transparent so the
; ula renderer (which always draws over the 256x192 centre block,
; including for default zero attribute bytes) falls through to the
; layer below. nr_14 must already be set to the value chosen here
; ($e3 = the next default transparent index).
	macro silence_ula
	set_nr $14, $e3	; nr_global_trans
	set_pal_entry 0, 0, $e3, $00	; ink 0: the ULA palette keeps inks at 0..15
	set_pal_entry 0, 16, $e3, $00	; paper 0 at 16..31, where the border sits too
	endm

; set_clip reg, ctrlbit, x1, x2, y1, y2 -- program a layer clip window.
; reg = $18 (l2) / $19 (sprites) / $1a (ula) / $1b (tilemap); ctrlbit =
; the nr_1c bit that resets that layer's write index (1/2/4/8). then the
; four window bytes x1,x2,y1,y2 are written to reg in order.
	macro set_clip __arg_reg, __arg_ctrlbit, __arg_x1, __arg_x2, __arg_y1, __arg_y2
	set_nr $1c, __arg_ctrlbit
	set_nr __arg_reg, __arg_x1
	set_nr __arg_reg, __arg_x2
	set_nr __arg_reg, __arg_y1
	set_nr __arg_reg, __arg_y2
	endm
