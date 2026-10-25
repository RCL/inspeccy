; test fixture builder for the nex layer 2 loading-screen path (lane f).
;
; produces loading_screen_l2.nex: a v1.3 nex whose program does nothing but
; `di : jr $`, so it never touches layer 2. the loader-applied loading screen
; therefore stays on screen forever, giving inspeccy a stable golden to diff
; regardless of which frame it renders.
;
; the screen comes from loading_l2.bmp (256x192, 8-bit indexed): diagonal
; stripes over palette indices 1..8 with a solid-white 16x16 marker in the
; top-left corner. the asymmetric marker makes any mirror/rotate/row-flip
; regression in the l2 blit or compositor visible in the diff.
;
; build (from this directory):
;   sjasmplus loading_screen_l2.asm
; output: ../loading_screen_l2.nex

	device zxspectrumnext

	org $6000	; lives in 16k bank 5 (slot 1, $4000-$7fff)
start:
	di
	jr start	; freeze; layer 2 is never written

	savenex open "../loading_screen_l2.nex", start, $bffe, 0
	savenex core 3, 0, 0	; next core 3.0.0 minimum
	savenex cfg 1, 0, 0, 0	; blue border, no file handle, reset nextregs, 768k
	savenex screen bmp "loading_l2.bmp"	; 256x192 indexed -> l2 screen + palette
	savenex bank 5	; the only file bank: holds the code at $6000
	savenex close
