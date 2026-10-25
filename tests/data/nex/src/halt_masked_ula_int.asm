; halt with the ula interrupt masked and a future line interrupt armed.
; the runner enforces a wall-clock timeout: stalled event deadlines must
; not keep the emulator from reaching its cycle budget.
; build here with sjasmplus halt_masked_ula_int.asm.

	device zxspectrumnext
	org $8000
start:
	di
	nextreg $22, %00000110	; bit1 = line-int enable, bit2 = ula-int mask
	nextreg $23, 100	; line interrupt at raster line 100
	ei
loop:
	halt	; wait - masked ula int must not wedge this
	jr loop

	savenex open "../halt_masked_ula_int.nex", start, $bffe, 0
	savenex core 3, 0, 0
	savenex cfg 1, 0, 0, 0
	savenex bank 2	; holds the code at $8000
	savenex close
