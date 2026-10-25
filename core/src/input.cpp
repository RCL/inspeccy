#include "input.h"

#include <algorithm>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

bool ParseDecimal(const std::string& Text, uint64_t& OutValue)
{
	if (Text.empty())
	{
		return false;
	}
	uint64_t Value = 0;
	for (const char C : Text)
	{
		if (C < '0' || C > '9' || Value > (UINT64_MAX - uint64_t(C - '0')) / 10)
		{
			return false;
		}
		Value = Value * 10 + uint64_t(C - '0');
	}
	OutValue = Value;
	return true;
}

// Keyboard matrix definition. Each entry: (name, row 0..7, bit 0..4).
// Row encoding follows the Spectrum ULA keyboard matrix.
struct KbDef
{
	const char* m_Name;
	uint8_t m_Row;
	uint8_t m_Bit;
};

const KbDef kKeyboardDefs[] = {
	// Row 0 ($FEFE): CAPS, Z, X, C, V
	{"CAPS", 0, 0},
	{"Z", 0, 1},
	{"X", 0, 2},
	{"C", 0, 3},
	{"V", 0, 4},
	// Row 1 ($FDFE): A, S, D, F, G
	{"A", 1, 0},
	{"S", 1, 1},
	{"D", 1, 2},
	{"F", 1, 3},
	{"G", 1, 4},
	// Row 2 ($FBFE): Q, W, E, R, T
	{"Q", 2, 0},
	{"W", 2, 1},
	{"E", 2, 2},
	{"R", 2, 3},
	{"T", 2, 4},
	// Row 3 ($F7FE): 1, 2, 3, 4, 5
	{"NUM1", 3, 0},
	{"NUM2", 3, 1},
	{"NUM3", 3, 2},
	{"NUM4", 3, 3},
	{"NUM5", 3, 4},
	// Row 4 ($EFFE): 0, 9, 8, 7, 6 (note ordering)
	{"NUM0", 4, 0},
	{"NUM9", 4, 1},
	{"NUM8", 4, 2},
	{"NUM7", 4, 3},
	{"NUM6", 4, 4},
	// Row 5 ($DFFE): P, O, I, U, Y
	{"P", 5, 0},
	{"O", 5, 1},
	{"I", 5, 2},
	{"U", 5, 3},
	{"Y", 5, 4},
	// Row 6 ($BFFE): ENTER, L, K, J, H
	{"ENTER", 6, 0},
	{"L", 6, 1},
	{"K", 6, 2},
	{"J", 6, 3},
	{"H", 6, 4},
	// Row 7 ($7FFE): SPACE, SYMBOL, M, N, B
	{"SPACE", 7, 0},
	{"SYMBOL", 7, 1},
	{"M", 7, 2},
	{"N", 7, 3},
	{"B", 7, 4},
};

// Aliases: shorter or historical names.
struct AliasDef
{
	const char* m_From;
	const char* m_To;
};
const AliasDef kAliases[] = {
	{"CS", "CAPS"},
	{"SS", "SYMBOL"},
	{"CR", "ENTER"},
	{"SP", "SPACE"},
	{"SHIFT", "CAPS"},
};

// BASIC editor composite keys - these resolve to TWO targets.
struct CompositeDef
{
	const char* m_Name;
	const char* m_First;
	const char* m_Second;
};
const CompositeDef kComposites[] = {
	{"LEFT", "CAPS", "NUM5"},
	{"DOWN", "CAPS", "NUM6"},
	{"UP", "CAPS", "NUM7"},
	{"RIGHT", "CAPS", "NUM8"},
	{"DELETE", "CAPS", "NUM0"},
	{"EDIT", "CAPS", "NUM1"},
	{"TRUE_VIDEO", "CAPS", "NUM3"},
	{"INV_VIDEO", "CAPS", "NUM4"},
	{"EXTENDED", "CAPS", "SYMBOL"},
	{"BREAK", "CAPS", "SPACE"},
};

// Kempston joystick bit assignments.
struct KempDef
{
	const char* m_Name;
	uint8_t m_Bit;
};
const KempDef kKempstonDefs[] = {
	{"KEMP_RIGHT", 0},
	{"KEMP_LEFT", 1},
	{"KEMP_DOWN", 2},
	{"KEMP_UP", 3},
	{"KEMP_FIRE", 4},
};

std::string Upper(const std::string& S)
{
	std::string R = S;
	for (auto& C : R)
	{
		C = char(toupper(uint8_t(C)));
	}
	return R;
}

// Resolve a (case-insensitive) name into one or two KeyTarget entries.
// Returns true on success.
bool LookupName(const std::string& NameIn, std::vector<KeyTarget>& OutTargets)
{
	const std::string Name = Upper(NameIn);

	// Aliases first - substitute then re-resolve.
	for (const auto& A : kAliases)
	{
		if (Name == A.m_From)
		{
			return LookupName(A.m_To, OutTargets);
		}
	}

	// Composite shortcuts.
	for (const auto& C : kComposites)
	{
		if (Name == C.m_Name)
		{
			std::vector<KeyTarget> T1, T2;
			if (!LookupName(C.m_First, T1) || !LookupName(C.m_Second, T2))
			{
				return false;
			}
			OutTargets.insert(OutTargets.end(), T1.begin(), T1.end());
			OutTargets.insert(OutTargets.end(), T2.begin(), T2.end());
			return true;
		}
	}

	// Single keyboard keys.
	for (const auto& K : kKeyboardDefs)
	{
		if (Name == K.m_Name)
		{
			KeyTarget T;
			T.m_bKempston = false;
			T.m_Row = K.m_Row;
			T.m_Bit = K.m_Bit;
			OutTargets.push_back(T);
			return true;
		}
	}

	// Kempston.
	for (const auto& K : kKempstonDefs)
	{
		if (Name == K.m_Name)
		{
			KeyTarget T;
			T.m_bKempston = true;
			T.m_Row = 0;
			T.m_Bit = K.m_Bit;
			OutTargets.push_back(T);
			return true;
		}
	}

	return false;
}

// Split a line on spaces or tabs.
std::vector<std::string> SplitWS(const std::string& Line)
{
	std::vector<std::string> Out;
	size_t I = 0;
	while (I < Line.size())
	{
		while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
		{
			++I;
		}
		if (I >= Line.size())
		{
			break;
		}
		const size_t Start = I;
		while (I < Line.size() && Line[I] != ' ' && Line[I] != '\t')
		{
			++I;
		}
		Out.emplace_back(Line, Start, I - Start);
	}
	return Out;
}

} // namespace

InputState::InputState()
{
	Reset();
}

void InputState::Reset()
{
	for (int I = 0; I < 8; ++I)
	{
		m_KeyboardRows[I] = 0xFF;
	}
	m_KempstonState = 0x00;
	m_NextEvent = 0;
}

bool InputState::LoadScript(const char* Path, int DefaultTapFrames, std::string& OutError)
{
	if (DefaultTapFrames < 1)
	{
		DefaultTapFrames = 2;
	}

	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open input script: ") + Path;
		return false;
	}

	std::vector<char> Buf;
	int C;
	const size_t kMaxScriptBytes = 16 * 1024 * 1024;
	while ((C = fgetc(F)) != EOF)
	{
		if (C == 0 || Buf.size() >= kMaxScriptBytes)
		{
			fclose(F);
			OutError = "input script contains NUL or exceeds 16 MB";
			return false;
		}
		Buf.push_back(char(C));
	}
	const bool bReadError = ferror(F) != 0;
	if (fclose(F) != 0 || bReadError)
	{
		OutError = "cannot read input script";
		return false;
	}

	int LineNum = 0;
	size_t LineStart = 0;
	auto ProcessLine = [&](const std::string& RawLine) -> bool
	{
		++LineNum;
		// Strip CR / comment portion
		std::string Line = RawLine;
		const size_t Hash = Line.find('#');
		if (Hash != std::string::npos)
		{
			Line.resize(Hash);
		}
		// Trim trailing whitespace
		while (!Line.empty() && (Line.back() == ' ' || Line.back() == '\t' || Line.back() == '\r'))
		{
			Line.pop_back();
		}
		if (Line.empty())
		{
			return true;
		}

		const std::vector<std::string> Toks = SplitWS(Line);
		if (Toks.empty())
		{
			return true;
		}
		if (Toks.size() < 3)
		{
			char Msg[160];
			snprintf(Msg, sizeof(Msg), "input script line %d: need <frame> <action> <key>", LineNum);
			OutError = Msg;
			return false;
		}

		uint64_t Frame = 0;
		if (!ParseDecimal(Toks[0], Frame))
		{
			char Msg[160];
			snprintf(Msg, sizeof(Msg), "input script line %d: bad frame number '%s'", LineNum, Toks[0].c_str());
			OutError = Msg;
			return false;
		}

		const std::string Action = Upper(Toks[1]);
		int TapFrames = DefaultTapFrames;
		std::string KeyName;

		if (Action == "PRESS" || Action == "RELEASE")
		{
			if (Toks.size() != 3)
			{
				OutError = "input script line " + std::to_string(LineNum) + ": unexpected extra token";
				return false;
			}
			KeyName = Toks[2];
		}
		else if (Action == "TAP")
		{
			// Optional N between TAP and key.
			if (Toks.size() == 3)
			{
				KeyName = Toks[2];
			}
			else
			{
				uint64_t Duration = 0;
				if (Toks.size() != 4 || !ParseDecimal(Toks[2], Duration) || Duration < 1 || Duration > INT_MAX)
				{
					OutError = "input script line " + std::to_string(LineNum) + ": expected tap [positive N] key";
					return false;
				}
				TapFrames = int(Duration);
				KeyName = Toks[3];
			}
		}
		else
		{
			char Msg[160];
			snprintf(Msg, sizeof(Msg), "input script line %d: unknown action '%s' (use press/release/tap)", LineNum,
				Toks[1].c_str());
			OutError = Msg;
			return false;
		}

		std::vector<KeyTarget> Targets;
		if (!LookupName(KeyName, Targets))
		{
			char Msg[160];
			snprintf(Msg, sizeof(Msg), "input script line %d: unknown key '%s'", LineNum, KeyName.c_str());
			OutError = Msg;
			return false;
		}

		// Expand TAP into PRESS at Frame, RELEASE at Frame + TapFrames.
		if (Action == "TAP")
		{
			if (Frame > UINT64_MAX - uint64_t(TapFrames))
			{
				OutError = "input script line " + std::to_string(LineNum) + ": tap release frame overflows";
				return false;
			}
			InputEvent E1;
			E1.m_Frame = Frame;
			E1.m_bPress = true;
			E1.m_Name = Upper(KeyName);
			E1.m_Targets = Targets;
			m_Events.push_back(std::move(E1));

			InputEvent E2;
			E2.m_Frame = Frame + uint64_t(TapFrames);
			E2.m_bPress = false;
			E2.m_Name = Upper(KeyName);
			E2.m_Targets = Targets;
			m_Events.push_back(std::move(E2));
		}
		else
		{
			InputEvent Ev;
			Ev.m_Frame = Frame;
			Ev.m_bPress = (Action == "PRESS");
			Ev.m_Name = Upper(KeyName);
			Ev.m_Targets = std::move(Targets);
			m_Events.push_back(std::move(Ev));
		}
		return true;
	};

	for (size_t I = 0; I < Buf.size(); ++I)
	{
		if (Buf[I] == '\n')
		{
			std::string Line(Buf.data() + LineStart, I - LineStart);
			if (!ProcessLine(Line))
			{
				return false;
			}
			LineStart = I + 1;
		}
	}
	if (LineStart < Buf.size())
	{
		std::string Line(Buf.data() + LineStart, Buf.size() - LineStart);
		if (!ProcessLine(Line))
		{
			return false;
		}
	}

	// Sort by frame for in-order application. Within the same frame, preserve
	// file order via stable_sort.
	std::stable_sort(m_Events.begin(), m_Events.end(),
		[](const InputEvent& A, const InputEvent& B)
		{
			return A.m_Frame < B.m_Frame;
		});

	return true;
}

void InputState::AddEvents(const std::vector<InputEvent>& More)
{
	m_Events.insert(m_Events.end(), More.begin(), More.end());
	std::stable_sort(m_Events.begin(), m_Events.end(),
		[](const InputEvent& A, const InputEvent& B)
		{
			return A.m_Frame < B.m_Frame;
		});
}

const InputEvent* InputState::PeekPending(uint64_t CurrentFrame) const
{
	if (m_NextEvent >= m_Events.size())
	{
		return nullptr;
	}
	if (m_Events[m_NextEvent].m_Frame > CurrentFrame)
	{
		return nullptr;
	}
	return &m_Events[m_NextEvent];
}

void InputState::ApplyAndAdvance()
{
	if (m_NextEvent >= m_Events.size())
	{
		return;
	}
	ApplyEvent(m_Events[m_NextEvent]);
	++m_NextEvent;
}

void InputState::ApplyEvent(const InputEvent& Ev)
{
	for (const KeyTarget& T : Ev.m_Targets)
	{
		if (T.m_bKempston)
		{
			// Active HIGH. Press -> set bit, release -> clear bit.
			if (Ev.m_bPress)
			{
				m_KempstonState |= uint8_t(1u << T.m_Bit);
			}
			else
			{
				m_KempstonState &= uint8_t(~(1u << T.m_Bit));
			}
		}
		else
		{
			// Keyboard: active LOW. Press -> CLEAR bit, release -> SET bit.
			if (Ev.m_bPress)
			{
				m_KeyboardRows[T.m_Row] &= uint8_t(~(1u << T.m_Bit));
			}
			else
			{
				m_KeyboardRows[T.m_Row] |= uint8_t(1u << T.m_Bit);
			}
		}
	}
}

bool InputState::MaybeReadPort(uint16_t Port, uint8_t& OutValue) const
{
	const uint8_t Lo = uint8_t(Port & 0xFF);
	if (Lo == 0xFE)
	{
		// Keyboard scan. For each row N where bit N of the HIGH byte (A8..A15)
		// is 0, AND in that row's state. Bits 5..7 floating (return 1 conv.);
		// bit 6 = EAR (1 = silent).
		const uint8_t Hi = uint8_t(Port >> 8);
		uint8_t Result = 0xFF;
		for (int N = 0; N < 8; ++N)
		{
			if ((Hi & (1u << N)) == 0)
			{
				Result &= m_KeyboardRows[N];
			}
		}
		OutValue = Result;
		return true;
	}
	if (Lo == 0x1F)
	{
		OutValue = m_KempstonState;
		return true;
	}
	return false;
}
