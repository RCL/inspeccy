; self-checking esxdos write + directory test (lane e).
;
; exercises f_open(create/write), f_write, f_sync, f_close, f_open(read),
; f_read, then f_opendir / f_readdir over the mount. on full success it
; `jp $0000` (inspeccy reset trap -> exit 5); any failure `di : halt`
; (-> exit 2). nex programs pass buffer pointers in ix; we also set hl to the
; same pointer since inspeccy's readnamehlorix tries hl first.
;
; run against a writable scratch mount:
;   inspeccy --nex esxdos_rw.nex --vfs <scratchdir> --machine next50 ...
;
; build (from this directory): sjasmplus esxdos_rw.asm  -> ../esxdos_rw.nex

	device zxspectrumnext
	org $8000
start:
; ---- f_open "test.dat", create/truncate + write ----
	ld ix, fnameout
	ld hl, fnameout
	ld a, '*'
	ld b, $0c | $02	; esx_mode_creat_trunc | esx_mode_write
	rst $08
	db $9a	; f_open
	jp c, fail
	ld (handle), a

; ---- f_write 8 bytes ----
	ld a, (handle)
	ld ix, payload
	ld hl, payload
	ld bc, 8
	rst $08
	db $9e	; f_write
	jp c, fail
	ld a, c
	cp 8	; bc must come back = 8 (all written)
	jp nz, fail

	ld a, (handle)
	rst $08
	db $9c	; f_sync
	ld a, (handle)
	rst $08
	db $9b	; f_close

; ---- f_open "test.dat" for read, f_read, verify ----
	ld ix, fnameout
	ld hl, fnameout
	ld a, '*'
	ld b, $01	; esx_mode_read
	rst $08
	db $9a
	jp c, fail
	ld (handle), a

	ld a, (handle)
	ld ix, readbuf
	ld hl, readbuf
	ld bc, 8
	rst $08
	db $9d	; f_read
	jp c, fail

	ld a, (handle)
	rst $08
	db $9b	; f_close

	ld hl, payload
	ld de, readbuf
	ld b, 8
.cmp:
	ld a, (de)
	cp (hl)
	jp nz, fail
	inc hl
	inc de
	djnz .cmp

; ---- f_opendir "." then f_readdir until end; require >=1 entry ----
	ld ix, dirname
	ld hl, dirname
	ld a, '*'
	ld b, 0
	rst $08
	db $a3	; f_opendir
	jp c, fail
	ld (handle), a
.dloop:
	ld a, (handle)
	ld ix, dirent
	ld hl, dirent
	rst $08
	db $a4	; f_readdir
	jp c, fail
	or a	; a = 0 -> end of listing
	jr z, .ddone
	ld a, 1
	ld (sawentry), a
	jr .dloop
.ddone:
	ld a, (handle)
	rst $08
	db $9b	; f_close (dir)
	ld a, (sawentry)
	or a
	jp z, fail

; ---- all checks passed: write a 4-byte success marker ----
; only reachable on full success, so vfs.stats.bytes_written = 8 + 4 = 12
; and files_opened = 3 distinguish pass from any earlier failure.
	ld ix, fnamepass
	ld hl, fnamepass
	ld a, '*'
	ld b, $0c | $02	; creat_trunc | write
	rst $08
	db $9a	; f_open
	jp c, fail
	ld (handle), a
	ld a, (handle)
	ld ix, passdata
	ld hl, passdata
	ld bc, 4
	rst $08
	db $9e	; f_write "GOOD"
	jp c, fail
	ld a, (handle)
	rst $08
	db $9b	; f_close
	di
	halt	; pass: halted, bytes_written = 12
fail:
	di
	halt	; fail: halted, bytes_written < 12

fnameout:
	db "test.dat", 0
fnamepass:
	db "pass.ok", 0
dirname:
	db ".", 0
payload:
	db "ESXWRITE"	; 8 bytes
passdata:
	db "GOOD"	; 4 bytes
handle:
	db 0
sawentry:
	db 0
readbuf:
	ds 8
dirent:
	ds 288

	savenex open "../esxdos_rw.nex", start, $bffe, 0
	savenex core 3, 0, 0
	savenex cfg 1, 0, 0, 0
	savenex bank 2	; holds the code at $8000
	savenex close
