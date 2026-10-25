; Resume-equivalence workload for tests/python/test_resume_equivalence.py: a little of
; every piece of Next state a dump has to carry, kept changing every frame so a
; resume that drops one shows. At 14 MHz (NR $07 = 2), IM 2, and per frame:
;   - both TurboSound chips written (chip select through $FFFD), so the second
;     chip, its capture and the select itself must survive;
;   - a line interrupt (NR $22/$23) as well as the frame one;
;   - a zxnDMA programmed once at start and only LOADed + ENABLEd per frame;
;   - one byte into the ULA clip window (its write cursor walks round);
;   - the Layer 2 write window ($123B) opened over $0000 for one write;
;   - a CTC channel whose control word and time constant land in different
;     frames (the dump often falls between the two);
;   - a copper split whose tilemap offset is patched in copper memory.
; Assembled at test time: sjasmplus kitchen_sink.asm (writes kitchen_sink.nex).

	device zxspectrumnext
	org $8000
Start:
	di
	nextreg $07, 2			; 14 MHz
	; tilemap: two tiles, a stripe of the second down column 0
	nextreg $43, %00110000
	nextreg $40, 1
	nextreg $41, $e0
	nextreg $41, $03
	nextreg $52, 11
	ld hl, $4000
	ld de, $4001
	ld bc, 31
	ld (hl), $11
	ldir
	ld hl, $4020
	ld de, $4021
	ld bc, 31
	ld (hl), $22
	ldir
	ld hl, $5000
	ld c, 32
.Row:
	ld (hl), 1
	inc hl
	ld (hl), 0
	inc hl
	ld b, 39
.Col:
	ld (hl), 0
	inc hl
	ld (hl), 0
	inc hl
	djnz .Col
	dec c
	jr nz, .Row
	nextreg $52, 10
	nextreg $6f, $20
	nextreg $6e, $30
	nextreg $6b, $80
	; copper: tilemap X offset for display lines 0..191
	nextreg $62, 0
	nextreg $61, 0
	ld hl, CopperList
	ld b, CopperEnd - CopperList
.Up:
	ld a, (hl)
	inc hl
	nextreg $60, a
	djnz .Up
	nextreg $62, $c0
	; line interrupt at line 100, frame interrupt kept
	nextreg $23, 100
	nextreg $22, %00000010
	; zxnDMA: 64 bytes Source -> Dest, memory to memory, both incrementing,
	; programmed once (later frames only LOAD + ENABLE it)
	ld hl, DmaProgram
	ld b, DmaProgramEnd - DmaProgram
	ld c, $6b
	otir
	; IM 2, every vector to Isr
	ld hl, $fe00
	ld de, $fe01
	ld bc, 256
	ld (hl), $fd
	ldir
	ld a, $c3
	ld ($fdfd), a
	ld hl, Isr
	ld ($fdfe), hl
	ld a, $fe
	ld i, a
	im 2
	ei
Loop:
	halt
	ld hl, Frame
	inc (hl)
	ld a, (hl)
	ld e, a
	; TurboSound: chip 1 then chip 0, register 0 and the tone of channel A
	ld bc, $fffd
	ld a, $fe
	out (c), a			; select chip 1
	xor a
	out (c), a			; register 0
	ld b, $bf
	out (c), e
	ld b, $ff
	ld a, $ff
	out (c), a			; select chip 0
	ld a, 8
	out (c), a			; register 8: channel A volume
	ld b, $bf
	ld a, e
	and 15
	out (c), a
	ld b, $ff
	xor a
	out (c), a
	ld b, $bf
	out (c), e			; register 0 = frame
	; the DMA source changes, then LOAD + ENABLE only
	ld a, e
	ld (Source), a
	ld a, $cf
	out ($6b), a
	ld a, $87
	out ($6b), a
	; one byte into the ULA clip window
	ld a, e
	nextreg $1a, a
	; Layer 2 write window over $0000 for one byte
	ld bc, $123b
	ld a, %00000001
	out (c), a
	ld d, 0
	ld a, e
	ld (de), a
	xor a
	out (c), a
	; CTC channel 0: control word on odd frames, time constant on even ones
	ld bc, $183b
	bit 0, e
	jr z, .Tc
	ld a, %00000101			; control word, time constant follows
	out (c), a
	jr .Copper
.Tc:
	ld a, e
	out (c), a
.Copper:
	; the copper's offset, patched in its memory
	nextreg $61, 9
	ld a, e
	nextreg $60, a
	jr Loop

Isr:
	push af
	ld a, (Ints)
	inc a
	ld (Ints), a
	pop af
	ei
	reti

DmaProgram:
	db $83				; WR6: disable
	db $7d				; WR0: A->B transfer, port A address + length follow
	dw Source
	dw 64
	db $54, $02			; WR1: port A memory, incrementing, cycle length byte
	db $50, $02			; WR2: port B memory, incrementing, cycle length byte
	db $ad				; WR4: continuous mode, port B address follows
	dw Dest
	db $82				; WR5: stop at end
	db $cf				; WR6: load
DmaProgramEnd:

CopperList:
	db $2f,0, $30,0, $80,0, $2f,0, $30,0
	db $80,192, $2f,0, $30,0, $ff,$ff
CopperEnd:

Frame: db 0
Ints: db 0
Source: ds 64, $5a
Dest: ds 64

	savenex open "kitchen_sink.nex", Start, $bff0
	savenex core 3, 0, 0
	savenex auto
	savenex close
