; zxndma preemption: an in-flight dma must be interruptible so the im 2 / ctc0
; isr can mutate the source byte between dma bytes. without preemption the dma
; reads the source once at start and writes the same value to every dest byte,
; leaving dest as a uniform fill. with preemption the isr fires ~once every
; few dozen dma bytes, incrementing the source byte, so dest[start] and
; dest[end] should differ.
;
; the isr returns with plain ret (not reti) on purpose: reti-only return
; detection would leak isrdepth and stall dma forever.

	include "common.inc"

nr_c0 equ $c0
nr_c4 equ $c4
nr_c5 equ $c5

ctc0_port equ $183b
dma_port equ $6b

im2_table_hi equ $80	; i=$80 ; table at $8000..$80ff
ctc0_vec equ $86	; (nr_c0[7:5] | (3<<1)) for base $80

counter equ $9000	; source byte; isr increments it
dest equ $a000
len equ 4096

	di

; install isr pointer at $8086 (ctc0 vector slot).
	ld hl, isr
	ld (im2_table_hi * 256 + ctc0_vec), hl

; counter starts at 0.
	xor a
	ld (counter), a

; sentinel-fill the dest so the dest[0]==dest[len-1] check below is
; actually testing what dma wrote (not stale ram that already varied).
	ld hl, dest
	ld (hl), 0
	ld de, dest + 1
	ld bc, len - 1
	ldir

	ld a, im2_table_hi
	ld i, a
	im 2

; nr_c0 = $81 : vector-base hi3 = $80, hw-vector mode on.
	set_nr nr_c0, $81
; nr_c5 = $01 : ctc0 int allowed.
	set_nr nr_c5, $01
; nr_c4 = $00 : ula frame int disabled (we don't want it competing).
	set_nr nr_c4, $00

; ctc0: control word + time constant. bit7=irq, bit6=0 timer, bit5=0
; presc 16, bit2=1 tc follows, bit0=1 control word.
	ld bc, ctc0_port
	ld a, %10000101
	out (c), a
	ld a, 8	; period = 8 * 16 = 128 t (~110 khz @ 14 mhz)
	out (c), a

	ei

; stream the dma program to port $6b. otir puts b on a8..a15; zxndma
; only decodes a0..a7 so every byte lands at $6b regardless.
	ld hl, dmaprogram
	ld bc, (dmaprogramend - dmaprogram) * 256 + dma_port
	otir

; burn enough wall-clock for the 4096-byte dma + ~32 ctc0 isrs to
; finish. 4096 * 4 = 16384 t for dma; isrs add modestly more. 256*256
; djnz iterations ~ 850k t - generous margin. djnz is interruptible
; between iterations so ctc0 isrs keep firing the whole time.
	ld de, 256
.waitouter:
	ld b, 0
.waitinner:
	djnz .waitinner
	dec de
	ld a, d
	or e
	jr nz, .waitouter

	di

; verify mutation made it through. without dma preemption the source
; byte gets latched at byte 0 and dest is a uniform fill - dest[0]
; would equal dest[len-1]. with preemption the isr increments the
; source many times during the transfer so the two bytes differ.
	ld a, (dest + 0)
	ld b, a
	ld a, (dest + len - 1)
	cp b
	jr nz, .destvaries
	fail

.destvaries:
; and confirm the isr actually ran (last byte should be ~ #isr-fires
; mod 256; the only way for it to stay 0 is if no isr fired).
	ld a, (counter)
	or a
	jr nz, .pass
	fail

.pass:
	pass

isr:
	push af
	ld a, (counter)
	inc a
	ld (counter), a
	pop af
	ei
	ret	; deliberately ret, not reti

; zxndma program: port a fixed at counter, port b incrementing from dest,
; length len, continuous-mode-stop-at-end.
dmaprogram:
	db $c3	; cmd_reset
	db $7d	; wr0: port a + transfer + block length follow
	dw counter
	dw len
	db $24	; wr1: port a fixed
	db $10	; wr2: port b inc
	db $ad	; wr4: continuous + port b addr follows
	dw dest
	db $82	; wr5: stop at end
	db $cf	; cmd_load
	db $b3	; cmd_force_ready
	db $87	; cmd_enable
dmaprogramend:
