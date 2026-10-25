#include "autotype.h"

namespace
{

// ZX Spectrum keyboard matrix (row = the address line pulled low on port $FE,
// bit = position within the half-row). Only the keys the auto-typer needs.
constexpr KeyTarget kKJ{false, 6, 3}; // row6 = ENTER,L,K,J,H
constexpr KeyTarget kKP{false, 5, 0}; // row5 = P,O,I,U,Y
constexpr KeyTarget kKEnter{false, 6, 0};
constexpr KeyTarget kKSymsh{false, 7, 1}; // row7 = SPACE,SYM-SHIFT,M,N,B
constexpr KeyTarget kKCaps{false, 0, 0}; // row0 bit0
constexpr KeyTarget kKR{false, 2, 3}; // RUN keyword at K cursor
constexpr KeyTarget kKT{false, 2, 4}; // RANDOMIZE keyword at K cursor
constexpr KeyTarget kKL{false, 6, 1}; // USR (extended mode)
constexpr KeyTarget kKNum6{false, 4, 4}; // '6'; CAPS+6 = cursor DOWN

// Matrix position for a letter A-Z / digit 0-9 (row, bit). Index 0..25 = A..Z,
// 26..35 = 0..9.
struct Pos
{
	uint8_t m_Row, m_Bit;
};
constexpr Pos kLetters[26] = {
	{1, 0}, {7, 4}, {0, 3}, {1, 2}, {2, 2}, {1, 3}, {1, 4}, {6, 4}, {5, 2}, {6, 3}, // A B C D E F G H I J
	{6, 2}, {6, 1}, {7, 2}, {7, 3}, {5, 1}, {5, 0}, {2, 0}, {2, 3}, {1, 1}, {2, 4}, // K L M N O P Q R S T
	{5, 3}, {0, 4}, {2, 1}, {0, 2}, {5, 4}, {0, 1} // U V W X Y Z
};
constexpr Pos kDigits[10] = {
	{4, 0}, {3, 0}, {3, 1}, {3, 2}, {3, 3}, {3, 4}, {4, 4}, {4, 3}, {4, 2}, {4, 1} // 0 1 2 3 4 5 6 7 8 9
};

// Resolve a printable char to its matrix targets. Upper-case letters add CAPS
// (the default L cursor produces lower case). Returns false for unsupported.
bool TargetsForChar(char C, std::vector<KeyTarget>& Out)
{
	Out.clear();
	if (C >= 'a' && C <= 'z')
	{
		Out.push_back({false, kLetters[C - 'a'].m_Row, kLetters[C - 'a'].m_Bit});
		return true;
	}
	if (C >= 'A' && C <= 'Z')
	{
		Out.push_back(kKCaps);
		Out.push_back({false, kLetters[C - 'A'].m_Row, kLetters[C - 'A'].m_Bit});
		return true;
	}
	if (C >= '0' && C <= '9')
	{
		Out.push_back({false, kDigits[C - '0'].m_Row, kDigits[C - '0'].m_Bit});
		return true;
	}
	// TR-DOS filenames commonly contain these; all are SYM-SHIFT combos.
	if (C == '.')
	{
		Out.push_back(kKSymsh);
		Out.push_back({false, 7, 2});
		return true;
	} // SS+M
	if (C == ' ')
	{
		Out.push_back({false, 7, 0});
		return true;
	} // SPACE
	if (C == '-')
	{
		Out.push_back(kKSymsh);
		Out.push_back({false, 6, 3});
		return true;
	} // SS+J
	if (C == '+')
	{
		Out.push_back(kKSymsh);
		Out.push_back({false, 6, 2});
		return true;
	} // SS+K
	if (C == '/')
	{
		Out.push_back(kKSymsh);
		Out.push_back({false, 0, 4});
		return true;
	} // SS+V
	if (C == '*')
	{
		Out.push_back(kKSymsh);
		Out.push_back({false, 7, 4});
		return true;
	} // SS+B
	return false;
}

// Append a press+release pair for a keystroke (one or two matrix targets held
// together, e.g. SYM-SHIFT + P). Advances *Frame past the release + gap.
void Stroke(
	std::vector<InputEvent>& Out, uint64_t* Frame, int Hold, int Gap, const char* Name, std::vector<KeyTarget> Targets)
{
	InputEvent Press;
	Press.m_Frame = *Frame;
	Press.m_bPress = true;
	Press.m_Name = Name;
	Press.m_Targets = Targets;
	Out.push_back(Press);

	InputEvent Release;
	Release.m_Frame = *Frame + uint64_t(Hold);
	Release.m_bPress = false;
	Release.m_Name = Name;
	Release.m_Targets = std::move(Targets);
	Out.push_back(Release);

	*Frame += uint64_t(Hold + Gap);
}

} // namespace

std::vector<InputEvent> BuildLoadBlankEvents(const AutoTypeParams& P)
{
	std::vector<InputEvent> Ev;
	uint64_t F = P.m_StartFrame;
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "LOAD", {kKJ});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "\"", {kKSymsh, kKP});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "\"", {kKSymsh, kKP});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "ENTER", {kKEnter});
	return Ev;
}

std::vector<InputEvent> BuildEnterEvents(const AutoTypeParams& P)
{
	std::vector<InputEvent> Ev;
	uint64_t F = P.m_StartFrame;
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "ENTER", {kKEnter});
	return Ev;
}

std::vector<InputEvent> BuildTrdosRunEvents(const std::string& Name, const AutoTypeParams& P, uint64_t SecondStageFrame,
	uint64_t ThirdStageFrame, bool bVia128Menu)
{
	std::vector<InputEvent> Ev;

	// Stage 0 (128K only): navigate the boot menu to "128 BASIC" (one DOWN from
	// the default "Tape Loader"), ENTER. 128 BASIC leaves $7FFD paging unlocked.
	if (bVia128Menu)
	{
		uint64_t F0 = P.m_StartFrame;
		Stroke(Ev, &F0, P.m_HoldFrames, P.m_GapFrames, "DOWN", {kKCaps, kKNum6});
		Stroke(Ev, &F0, P.m_HoldFrames, P.m_GapFrames, "ENTER", {kKEnter});
	}

	// Stage 1: RANDOMIZE USR 15616 ENTER  ->  drop into TR-DOS (A> prompt).
	uint64_t F = SecondStageFrame;
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "RANDOMIZE", {kKT});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "EXTEND", {kKCaps, kKSymsh});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "USR", {kKL});
	for (char C : std::string("15616"))
	{
		std::vector<KeyTarget> T;
		TargetsForChar(C, T);
		Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "digit", T);
	}
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "ENTER", {kKEnter});

	// Stage 2: RUN "name" ENTER at the A> prompt (K cursor -> R = RUN token).
	F = ThirdStageFrame;
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "RUN", {kKR});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "\"", {kKSymsh, kKP});
	for (char C : Name)
	{
		std::vector<KeyTarget> T;
		if (!TargetsForChar(C, T))
		{
			continue; // skip unsupported chars
		}
		Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "name", T);
	}
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "\"", {kKSymsh, kKP});
	Stroke(Ev, &F, P.m_HoldFrames, P.m_GapFrames, "ENTER", {kKEnter});
	return Ev;
}
