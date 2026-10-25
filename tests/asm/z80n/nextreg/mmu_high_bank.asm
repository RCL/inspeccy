; @ram-mb 1
; on a 1 mb next, pages 128..255 read as $ff and discard writes.
; retain sentinels on valid pages while accessing pages 168 and 200.
; this catches accidental masking of the page number to seven bits.

	include "common.inc"

; --- save the live nr_56 so we can restore it before pass ---
	ld bc, $243b
	ld a, $56
	out (c), a	; select nr_56
	ld bc, $253b
	in a, (c)
	ld (savednr56), a

; --- 1) sentinel at page 50, slot 6 offset $0001 ---
	ld a, 50
	nextreg $56, a
	ld a, $5a
	ld ($c001), a

; --- 2) remap slot 6 -> page 168 (= "bank 84 first 8K"; doesn't exist
;        physically on a 1 mb next). read at the same z80 address.
;        expectation: $ff (open bus). on the old aliasing model the
;        an absent physical page must not alias a valid lower page.
	ld a, 168
	nextreg $56, a
	ld a, ($c001)
	check_a $ff

; --- 3) write something to slot 6 while it's mapped open-bus - it must
;        be silently dropped, not aliased to some other page. we have
;        no way of detecting this from the z80 side beyond step 4, but
;        the act of writing exercises the discard path. ---
	ld a, $a5
	ld ($c001), a	; should go to open bus (= no effect)

; --- 4) remap back to page 50; the original sentinel must survive. ---
	ld a, 50
	nextreg $56, a
	ld a, ($c001)
	check_a $5a

; --- 5) check that page 168 cannot overwrite the sentinel on page 40,
;        but mapped via page 168 - confirms no two distinct logical
;        pages alias onto the same physical page when one is open-bus.
	ld a, 40
	nextreg $56, a
	ld a, $11
	ld ($c100), a	; write to page 40 (= valid 1 mb ram)

	ld a, 168
	nextreg $56, a
	ld a, ($c100)	; read from "page 168" (= open bus = $ff)
	check_a $ff

; the sentinel at page 40 must still survive - open-bus writes don't
; leak into adjacent pages.
	ld a, 40
	nextreg $56, a
	ld a, ($c100)
	check_a $11

; --- restore nr_56 ---
	ld a, (savednr56)
	nextreg $56, a

	pass

savednr56:
	db 0
