; 28 MHz read wait micro-test: 10000 iterations of a loop with 7 memory
; reads (5 opcode fetches, 1 operand, 1 data) and no writes, then exit.
; cycles= in the summary with --contention on minus off must be 70000.
	device zxspectrumnext
	org $2000
	di
	ld a, 7
	ld bc, $243b
	out (c), a
	ld a, 3	; 28 MHz
	ld bc, $253b
	out (c), a
	ld hl, $c000
	ld bc, 10000
.loop:
	ld a, (hl)	; 7 T + 2 waits
	dec bc	; 6 + 1
	ld a, b	; 4 + 1
	or c	; 4 + 1
	jr nz, .loop	; 12 + 2
	jp $0000
	savebin "../next28_readwait.dot", $2000, $ - $2000
