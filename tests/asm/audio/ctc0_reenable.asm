; ctc channel 0 must count after irq enable is restored without a new
; time constant. load tc=100, write $01 then $a1, and compare timed reads.

	include "common.inc"

ctc0_port equ $183b

; configure ctc0: control word + time constant follows.
;   bit 7 = 1: enable irq
;   bit 6 = 0: timer mode
;   bit 5 = 0: prescaler = 16
;   bit 4 = 0: falling edge
;   bit 3 = 0: timer starts on tc load
;   bit 2 = 1: time constant follows
;   bit 1 = 0: no reset
;   bit 0 = 1: control word
	ld bc, ctc0_port
	ld a, %10000101
	out (c), a
	ld a, 200	; tc = 200; period = 200 * 16 = 3200 t-states
	out (c), a

; burn ~4000 cycles so the counter ticks down some way (not all the way to
; zero, which would wrap before we can check it).
	ld b, 0
.waita:
	nop
	nop
	nop
	nop	; 4 nops + djnz (13/8) ~ 21 t per iter
	djnz .waita	; 256 * 21 = 5376 t-states

; suspend the channel (the disablectc0 byte). bit 7 = 0 disables
; irq; on real hardware the counter keeps going but no interrupt fires.
; in inspeccy we drop nextfirecycle to uint64_max, which also stops the
; visible down-counter - that's what the bug was masking.
	ld bc, ctc0_port
	ld a, %00000001
	out (c), a

; re-arm with the enablectc0 byte. bit 7 = 1, bit 5 = 1
; (256-prescaler). the prescaler bit differs from the original $85 above
; (which was 16-prescaler), which is fine - we just need the channel to
; start ticking again from tc, using whatever prescaler is in the
; re-enable byte. new period = 200 * 256 = 51200 t-states, far longer
; than the wait that follows, so the counter should stay close to tc.
	ld a, %10100001
	out (c), a

; read $183b; if the re-arm worked, the channel is enabled and we get
; the current down-counter (which should be close to 200, since the
; period is huge). if the bug is back, the channel stays disabled and
; the read returns the stored tc = 200 unchanged - i.e. the test can
; only catch the bug indirectly via a second read after a wait.
	ld bc, ctc0_port
	in a, (c)
	ld (counterafterreenable), a

; wait ~5000 t more.
	ld b, 0
.waitb:
	nop
	nop
	nop
	nop
	djnz .waitb

; read counter again. with re-arm working, it should be noticeably less
; than counterafterreenable (timer is ticking). with the bug, both reads
; return tc=200, i.e. equal.
	ld bc, ctc0_port
	in a, (c)
	ld hl, counterafterreenable
	cp (hl)
	jr c, .pass	; a < (hl) -> counter decreased -> pass
; either a == (hl) (bug: channel never re-armed) or a > (hl) (channel
; wrapped past zero - possible if our waits exceeded the period, but
; they're far below 51200 t so this branch shouldn't happen in
; practice; treat as fail since it would indicate runaway counter).
	fail

.pass:
	pass

counterafterreenable:
	db 0
