; synthetic trace workload: nested calls, ram writes, ay output and im 2.
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $ff00
	nextreg $07, 0
	ld a, high vectors
	ld i, a
	im 2
	ei
.loop:
	halt
	call update
	jr .loop

update:
	ld hl, (counter)
	inc hl
	ld (counter), hl
	ld a, l
	and 7
	out ($fe), a
	call draw
	call audio
	ret

draw:
	ld a, (counter)
	ld ($4000), a
	ld a, 71
	ld ($5800), a
	ret

audio:
	ld bc, $fffd
	ld a, 8
	out (c), a
	ld bc, $bffd
	ld a, (counter)
	and 15
	out (c), a
	ret

counter:
	dw 0
irq_count:
	dw 0
	assert $ < $8181
	org $8181
isr:
	push hl
	ld hl, (irq_count)
	inc hl
	ld (irq_count), hl
	pop hl
	ei
	reti
	org $8200
vectors:
	ds 257, $81

	savenex open "../trace_workload.nex", start, $ff00, 0
	savenex core 3, 0, 0
	savenex cfg 1, 0, 0, 0
	savenex bank 2
	savenex close
