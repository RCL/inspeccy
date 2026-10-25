#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "input.h" // InputEvent / KeyTarget

// Auto-typed keystroke sequences for boot-and-run loaders (--tap auto-LOAD "").
// These build a frame-timed InputEvent list that drives the ZX Spectrum keyboard
// matrix exactly as a user would at the `K` prompt, so the ROM's BASIC editor
// enters and runs a command. Each keystroke is held for HoldFrames then released,
// with GapFrames of all-keys-up between strokes so the ROM's key debounce
// (KSTATE) accepts each one as a distinct press.

struct AutoTypeParams
{
	uint64_t m_StartFrame = 160; // first keypress; must be after the ROM reaches K
	int m_HoldFrames = 10; // frames a key is held down
	int m_GapFrames = 10; // all-up frames between consecutive keys
};

// Build the keystrokes for `LOAD ""` + ENTER at the 48K `K` cursor:
//   J            -> the LOAD keyword (single-key at the K cursor)
//   SYM-SHIFT+P  -> "   (twice)
//   ENTER
std::vector<InputEvent> BuildLoadBlankEvents(const AutoTypeParams& P);

// Build a single ENTER press. On the 128K boot menu the cursor starts on
// "Tape Loader", so one ENTER selects it and runs LOAD "" in 128K mode.
std::vector<InputEvent> BuildEnterEvents(const AutoTypeParams& P);

// Build the TR-DOS "run a named file" keystrokes as two stages:
//   1. RANDOMIZE USR 15619 ENTER  -> enters TR-DOS, drops to the `A>` prompt
//        (USR is typed via EXTENDED mode: CAPS+SYMBOL then L).
//   2. RUN "name" ENTER           -> TR-DOS runs the file (R = RUN keyword at the
//        A> prompt's K cursor; the case-sensitive `name` is typed with CAPS held
//        for its upper-case letters).
// Stage 2 starts SecondStageFrame frames after StartFrame to let TR-DOS come up.
// bVia128Menu: prepend a 128K boot-menu navigation (DOWN to "128 BASIC", ENTER)
// before the RANDOMIZE USR stage, so 128K demos enter TR-DOS with $7FFD paging
// still unlocked. StartFrame is the menu-nav (or stage 1 on 48K); the USR stage
// and RUN stage follow at SecondStageFrame and ThirdStageFrame.
std::vector<InputEvent> BuildTrdosRunEvents(const std::string& Name, const AutoTypeParams& P, uint64_t SecondStageFrame,
	uint64_t ThirdStageFrame, bool bVia128Menu);
