; zx next copper: a move executed at a wait'd raster line must actually write
; the target nextreg. this is the structural proof that the copper vm runs on
; the dispatcher's raster timeline (copper.h / copper.cpp) - not a rendering
; test.
;
; program uploaded to copper memory via nr $63 (16-bit upload path, hi byte
; then lo byte):
;   wait line 8      ($80,$08)  -- bit15=1, h=0, line=8
;   move nr $33 = $5a ($33,$5a) -- bit15=0, reg=$33 (lores y-offset), val=$5a
;   wait $ffff       ($ff,$ff)  -- line 511 = park until the frame restart
; start in mode 3 ($c0 = run + restart list every frame). nr $33 starts at 0;
; once the copper reaches line 8 each frame it writes $5a. after a delay of
; ~20 frames we read nr $33 back: it must be $5a. if the copper never runs
; (regression) the value stays 0 and check_a fires fail.

	include "common.inc"

; --- upload the copper list ---
	nextreg $61, 0	; write-addr low  = 0
	nextreg $62, 0	; write-addr high = 0, mode 0 (stopped)

	nextreg $63, $80	; wait  hi: bit15=1, h=0, line[8]=0
	nextreg $63, $08	; wait  lo: line = 8
	nextreg $63, $33	; move  hi: reg $33 (bit15=0)
	nextreg $63, $5a	; move  lo: value $5a
	nextreg $63, $ff	; wait  hi: terminal / park
	nextreg $63, $ff	; wait  lo

; --- start the copper (mode 3: run + restart each frame) ---
	nextreg $61, 0
	nextreg $62, $c0

; --- burn ~20 frames so the copper reaches line 8 several times ---
	ld hl, 0
.delay:
	dec hl
	ld a, h
	or l
	jr nz, .delay

; --- read nr $33 back via the register port ($243b select, $253b data) ---
	ld bc, $243b
	ld a, $33
	out (c), a	; select nr $33
	inc b	; bc = $253b
	in a, (c)	; a = nextregs[$33]

	check_a $5a	; the copper's move must have landed
	pass
