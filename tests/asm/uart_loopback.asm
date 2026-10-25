; Next UART register interface against a loopback link on the Pi channel.
; @uart-pi loop
	include "common.inc"

	; status before anything: tx empty, no rx data
	ld bc, $133b
	in a, (c)
	and $13			; tx_empty | tx_full | rx_avail
	check_a $10

	; select the Pi UART and read the selection back
	ld bc, $153b
	ld a, $40
	out (c), a
	in a, (c)
	and $47
	check_a $40

	; prescaler: write 7 low bits, 7 high bits, 3 msb (select write with bit 4)
	ld bc, $143b
	ld a, $00 | $33		; low 7 bits = $33
	out (c), a
	ld a, $80 | $01		; bits 13:7 = 1
	out (c), a
	ld bc, $153b
	ld a, $40 | $10 | $05	; keep Pi, write msb = 5
	out (c), a
	in a, (c)
	and $07
	check_a $05
	; back to 115200: prescaler 243 = $0F3 -> low $73, high $01, msb 0
	ld bc, $143b
	ld a, $73
	out (c), a
	ld a, $81
	out (c), a
	ld bc, $153b
	ld a, $50
	out (c), a

	; framing readback (hard reset default 8N1)
	ld bc, $163b
	in a, (c)
	check_a $18

	; send 8 bytes; the loopback returns them in order after the wire time
	ld bc, $133b
	ld hl, message
	ld e, 8
.send	ld a, (hl)
	out (c), a
	inc hl
	dec e
	jr nz, .send
	in a, (c)
	and $10			; transmitter is busy now
	check_a $00

	ld hl, received
	ld e, 8
.wait	ld bc, $133b
	in a, (c)
	and $01
	jr z, .wait
	ld bc, $143b
	in a, (c)
	ld (hl), a
	inc hl
	dec e
	jr nz, .wait

	; drained: no data, tx idle again
	ld bc, $133b
	in a, (c)
	and $13
	check_a $10
	ld bc, $143b
	in a, (c)		; empty FIFO reads 0
	check_a $00

	check_mem received+0, $55
	check_mem received+1, $aa
	check_mem received+2, $00
	check_mem received+3, $ff
	check_mem received+4, $01
	check_mem received+5, $02
	check_mem received+6, $03
	check_mem received+7, $5a

	; the ESP channel has no link: nothing ever arrives there
	ld bc, $153b
	xor a
	out (c), a
	ld bc, $133b
	ld a, $42
	out (c), a
	ld b, 0
.spin	nop
	djnz .spin
	ld bc, $133b
	in a, (c)
	and $01
	check_a $00
	pass

message	db $55, $aa, $00, $ff, $01, $02, $03, $5a
received ds 8
