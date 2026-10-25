#include "invariants.h"
#include "dma.h"
#include "memory.h"
#include "z80.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace
{

// Parse a hex offset (0..$1FFF for 8K, 0..$3FFF for 16K). Accepts optional
// $ prefix. Returns true on success.
bool ParseHexOffset(const char* Begin, const char* End, uint32_t& Out)
{
	if (Begin >= End)
	{
		return false;
	}
	if (*Begin == '$')
	{
		++Begin;
	}
	if (Begin >= End)
	{
		return false;
	}
	uint32_t V = 0;
	for (const char* P = Begin; P < End; ++P)
	{
		const char C = *P;
		uint32_t D;
		if (C >= '0' && C <= '9')
		{
			D = uint32_t(C - '0');
		}
		else if (C >= 'a' && C <= 'f')
		{
			D = uint32_t(C - 'a' + 10);
		}
		else if (C >= 'A' && C <= 'F')
		{
			D = uint32_t(C - 'A' + 10);
		}
		else
		{
			return false;
		}
		V = (V << 4) | D;
		if (V > 0xFFFFu)
		{
			return false;
		}
	}
	Out = V;
	return true;
}

// Parse a decimal page index (0..N).
bool ParseDecPage(const char* Begin, const char* End, uint32_t& Out)
{
	if (Begin >= End)
	{
		return false;
	}
	uint32_t V = 0;
	for (const char* P = Begin; P < End; ++P)
	{
		const char C = *P;
		if (C < '0' || C > '9')
		{
			return false;
		}
		V = V * 10 + uint32_t(C - '0');
		if (V > 65535u)
		{
			return false;
		}
	}
	Out = V;
	return true;
}

struct Slice
{
	const char* m_B;
	const char* m_E;
};

// Trim leading/trailing ASCII whitespace.
Slice TrimWs(const char* B, const char* E)
{
	while (B < E && isspace(static_cast<unsigned char>(*B)))
	{
		++B;
	}
	while (E > B && isspace(static_cast<unsigned char>(E[-1])))
	{
		--E;
	}
	return {B, E};
}

bool SliceEq(Slice S, const char* Lit)
{
	const size_t N = strlen(Lit);
	if (size_t(S.m_E - S.m_B) != N)
	{
		return false;
	}
	return memcmp(S.m_B, Lit, N) == 0;
}

// Split at first occurrence of char C. Returns { before, after_inclusive_C }.
// If C not found, returns { S, {S.E, S.E} } (after is empty).
std::pair<Slice, Slice> SplitOnce(Slice S, char C)
{
	for (const char* P = S.m_B; P < S.m_E; ++P)
	{
		if (*P == C)
		{
			return {{S.m_B, P}, {P + 1, S.m_E}};
		}
	}
	return {S, {S.m_E, S.m_E}};
}

// Parse one region: [II:OOOO-II:OOOO], with [/] or (/) brackets per endpoint.
// Returns true and fills (Page8KStart..OffsetEnd) on success. PageUnit is
// 0x2000 for 8K keyword, 0x4000 for 16K. The page index ranges differ but
// we just validate offset against MaxOffset (0x1FFF for 8K, 0x3FFF for 16K).
bool ParseRegion(Slice S, uint32_t PageUnit, uint32_t MaxOffset, int& OutPage8KStart, uint16_t& OutOffsetStart,
	int& OutPage8KEnd, uint16_t& OutOffsetEnd, std::string& OutErr)
{
	if (S.m_E - S.m_B < 5)
	{
		OutErr = "region too short";
		return false;
	}
	const char OpenBr = *S.m_B;
	const char CloseBr = S.m_E[-1];
	if ((OpenBr != '[' && OpenBr != '(') || (CloseBr != ']' && CloseBr != ')'))
	{
		OutErr = "region must be enclosed in [ ] or ( )";
		return false;
	}
	Slice Inner{S.m_B + 1, S.m_E - 1};
	Inner = TrimWs(Inner.m_B, Inner.m_E);

	// Split inner on '-'.
	auto [LhsRaw, RhsRaw] = SplitOnce(Inner, '-');
	if (RhsRaw.m_B >= RhsRaw.m_E)
	{
		OutErr = "missing '-' between endpoints";
		return false;
	}
	Slice Lhs = TrimWs(LhsRaw.m_B, LhsRaw.m_E);
	Slice Rhs = TrimWs(RhsRaw.m_B, RhsRaw.m_E);

	auto ParseEndpoint = [&](Slice Ep, uint32_t& OutPage, uint32_t& OutOff) -> bool
	{
		auto [PageS, OffS] = SplitOnce(Ep, ':');
		if (OffS.m_B >= OffS.m_E)
		{
			OutErr = "endpoint missing ':' separator";
			return false;
		}
		Slice PageT = TrimWs(PageS.m_B, PageS.m_E);
		Slice OffT = TrimWs(OffS.m_B, OffS.m_E);
		if (!ParseDecPage(PageT.m_B, PageT.m_E, OutPage))
		{
			OutErr = "bad decimal page index";
			return false;
		}
		if (!ParseHexOffset(OffT.m_B, OffT.m_E, OutOff))
		{
			OutErr = "bad hex offset";
			return false;
		}
		if (OutOff > MaxOffset)
		{
			OutErr = "offset exceeds keyword's max (0x" + std::to_string(MaxOffset) + ")";
			return false;
		}
		return true;
	};

	uint32_t LhsPage = 0, LhsOff = 0, RhsPage = 0, RhsOff = 0;
	if (!ParseEndpoint(Lhs, LhsPage, LhsOff))
	{
		return false;
	}
	if (!ParseEndpoint(Rhs, RhsPage, RhsOff))
	{
		return false;
	}

	// Normalise to inclusive 8K-page byte indices.
	// - LhsBytes = LhsPage * PageUnit + LhsOff
	// - For inclusive '[' lower end: keep as-is.
	// - For exclusive '(' lower end: bump by +1.
	// - For inclusive ']' upper end: keep as-is.
	// - For exclusive ')' upper end: subtract 1.
	auto FlatBytes = [&](uint32_t Page, uint32_t Off) -> int64_t
	{
		return int64_t(Page) * int64_t(PageUnit) + int64_t(Off);
	};

	int64_t Lo = FlatBytes(LhsPage, LhsOff);
	int64_t Hi = FlatBytes(RhsPage, RhsOff);
	if (OpenBr == '(')
	{
		Lo += 1;
	}
	if (CloseBr == ')')
	{
		Hi -= 1;
	}
	if (Hi < Lo)
	{
		OutErr = "empty or inverted range after open/close-bracket normalisation";
		return false;
	}

	if (Hi >= 2 * 1024 * 1024)
	{
		OutErr = "range exceeds the physical 2 MB address space";
		return false;
	}

	// Convert back to 8K-page granularity. Internal canonical form is
	// always 8K-page so the runtime check has one common path regardless
	// of whether the keyword was 8K or 16K.
	OutPage8KStart = int(Lo / 0x2000);
	OutOffsetStart = uint16_t(Lo % 0x2000);
	OutPage8KEnd = int(Hi / 0x2000);
	OutOffsetEnd = uint16_t(Hi % 0x2000);
	return true;
}

} // namespace

bool LoadInvariantsFile(const char* Path, InvariantsState& InOutState, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open invariants file: ") + Path;
		return false;
	}
	if (fseek(F, 0, SEEK_END) != 0)
	{
		fclose(F);
		OutError = "cannot seek invariants file";
		return false;
	}
	const long Sz = ftell(F);
	if (Sz < 0 || Sz > 16 * 1024 * 1024 || fseek(F, 0, SEEK_SET) != 0)
	{
		fclose(F);
		OutError = "cannot measure invariants file or file exceeds 16 MB";
		return false;
	}
	std::string Body(size_t(Sz), '\0');
	if (Sz > 0 && fread(Body.data(), 1, size_t(Sz), F) != size_t(Sz))
	{
		fclose(F);
		OutError = "short read on invariants file";
		return false;
	}
	fclose(F);

	int LineNo = 0;
	size_t I = 0;
	int RulesAdded = 0;
	while (I < Body.size())
	{
		++LineNo;
		const size_t LineStart = I;
		while (I < Body.size() && Body[I] != '\n')
		{
			++I;
		}
		const char* LineB = Body.data() + LineStart;
		const char* LineE = Body.data() + I;
		if (I < Body.size())
		{
			++I; // skip the '\n'
		}

		// Strip comment.
		for (const char* P = LineB; P < LineE; ++P)
		{
			if (*P == '#')
			{
				LineE = P;
				break;
			}
		}
		Slice L = TrimWs(LineB, LineE);
		if (L.m_B >= L.m_E)
		{
			continue; // blank
		}

		// Split at '='.
		auto [LhsRaw, RhsRaw] = SplitOnce(L, '=');
		if (RhsRaw.m_B >= RhsRaw.m_E)
		{
			OutError = std::string(Path) + ":" + std::to_string(LineNo) + ": missing '='";
			return false;
		}
		Slice Kw = TrimWs(LhsRaw.m_B, LhsRaw.m_E);
		Slice Reg = TrimWs(RhsRaw.m_B, RhsRaw.m_E);

		uint32_t PageUnit;
		uint32_t MaxOffset;
		if (SliceEq(Kw, "readonly8k"))
		{
			PageUnit = 0x2000;
			MaxOffset = 0x1FFFu;
		}
		else if (SliceEq(Kw, "readonly16k"))
		{
			PageUnit = 0x4000;
			MaxOffset = 0x3FFFu;
		}
		else
		{
			OutError = std::string(Path) + ":" + std::to_string(LineNo) +
					   ": unknown keyword (expected 'readonly8k' or 'readonly16k')";
			return false;
		}

		InvariantRange R;
		std::string RegErr;
		if (!ParseRegion(
				Reg, PageUnit, MaxOffset, R.m_Page8KStart, R.m_OffsetStart, R.m_Page8KEnd, R.m_OffsetEnd, RegErr))
		{
			OutError = std::string(Path) + ":" + std::to_string(LineNo) + ": " + RegErr;
			return false;
		}
		R.m_SourceText.assign(L.m_B, size_t(L.m_E - L.m_B));
		InOutState.m_ReadOnly.push_back(std::move(R));
		++RulesAdded;
	}

	if (RulesAdded == 0)
	{
		OutError = std::string(Path) + ": no rules parsed (empty file?)";
		return false;
	}
	return true;
}

void EmitInvariantViolation(InvariantsState& InOutState, int Page8K, uint16_t OffsetInPage, uint8_t OldVal,
	uint8_t NewVal, int SlotIdx, const Z80State& Cpu, const MemoryBus& Mem)
{
	InOutState.m_bAnyViolation = true;
	++InOutState.m_ViolationsEmitted;
	if (InOutState.m_ViolationsEmitted > InOutState.m_MaxViolations)
	{
		return;
	}

	// Find first matching rule for the SourceText attribution. Linear scan
	// is fine on the rare violation path.
	const InvariantRange* Matched = nullptr;
	const int64_t Addr = int64_t(Page8K) * 0x2000 + OffsetInPage;
	for (const auto& R : InOutState.m_ReadOnly)
	{
		const int64_t Lo = int64_t(R.m_Page8KStart) * 0x2000 + R.m_OffsetStart;
		const int64_t Hi = int64_t(R.m_Page8KEnd) * 0x2000 + R.m_OffsetEnd;
		if (Addr >= Lo && Addr <= Hi)
		{
			Matched = &R;
			break;
		}
	}

	FILE* Out = (InOutState.m_ViolationsOut != nullptr) ? InOutState.m_ViolationsOut : stderr;
	const char* Source = InOutState.m_bInDmaStep ? "dma" : "cpu";
	fprintf(Out,
		"{\"rule\":\"%s\","
		"\"cycle\":%llu,"
		"\"phys\":{\"page\":%d,\"offset\":\"$%04X\"},"
		"\"old\":\"$%02X\",\"new\":\"$%02X\","
		"\"source\":\"%s\",",
		Matched ? Matched->m_SourceText.c_str() : "<no-match>", static_cast<unsigned long long>(Cpu.m_Cycles), Page8K,
		unsigned(OffsetInPage), unsigned(OldVal), unsigned(NewVal), Source);

	if (InOutState.m_DmaActive != nullptr)
	{
		fprintf(Out,
			"\"dma\":{\"src\":\"$%04X\",\"dst\":\"$%04X\",\"remaining\":%u,"
			"\"port_a_fixed\":%s,\"port_b_inc\":%s,\"port_b_io\":%s},",
			unsigned(InOutState.m_DmaActive->m_CurSrcAddr), unsigned(InOutState.m_DmaActive->m_CurDstAddr),
			unsigned(InOutState.m_DmaActive->m_BytesRemaining),
			InOutState.m_DmaActive->m_bPortAFixed ? "true" : "false",
			InOutState.m_DmaActive->m_bPortBInc ? "true" : "false",
			InOutState.m_DmaActive->m_bPortBIo ? "true" : "false");
	}

	fprintf(Out,
		"\"pc\":\"$%04X\",\"sp\":\"$%04X\","
		"\"af\":\"$%04X\",\"bc\":\"$%04X\",\"de\":\"$%04X\",\"hl\":\"$%04X\","
		"\"ix\":\"$%04X\",\"iy\":\"$%04X\","
		"\"iff1\":%d,\"im\":%d,\"isr_depth\":%u,"
		"\"mmu\":[%d,%d,%d,%d,%d,%d,%d,%d]}\n",
		unsigned(Cpu.PC), unsigned(Cpu.SP), unsigned(Cpu.AF), unsigned(Cpu.BC), unsigned(Cpu.DE), unsigned(Cpu.HL),
		unsigned(Cpu.IX), unsigned(Cpu.IY), int(Cpu.IFF1), int(Cpu.IM), unsigned(Cpu.m_IsrDepth),
		int(Mem.GetNextReg(0x50)), int(Mem.GetNextReg(0x51)), int(Mem.GetNextReg(0x52)), int(Mem.GetNextReg(0x53)),
		int(Mem.GetNextReg(0x54)), int(Mem.GetNextReg(0x55)), int(Mem.GetNextReg(0x56)), int(Mem.GetNextReg(0x57)));

	// The diagnostic kicker: which NR_5X mapped slot SlotIdx to the page
	// that contains the violated byte, and WHEN/WHERE did the demo set that
	// mapping? "set_at_pc" is the instruction that needs investigation -
	// the actual write at "pc" above is likely innocent (just trusts the
	// mapping). Trailing comma intentional -> the "}" closes the record.
	if (SlotIdx >= 0 && SlotIdx < 8)
	{
		const MemoryBus::MmuLastWrite& L = Mem.m_LastMmuWrite[SlotIdx];
		// NB: emitted as a SECOND top-level record on its own line so the
		// strict line-per-event JSONL shape survives. Consumers should
		// associate by adjacent cycle / shared-event grouping.
		fprintf(Out,
			"{\"rule\":\"mmu_last_set\",\"cycle\":%llu,"
			"\"slot\":%d,\"current_page\":%d,"
			"\"set_at_cycle\":%llu,\"set_at_pc\":\"$%04X\","
			"\"old_value\":%d,\"new_value\":%d}\n",
			static_cast<unsigned long long>(Cpu.m_Cycles), SlotIdx, int(Mem.GetNextReg(uint8_t(0x50 + SlotIdx))),
			static_cast<unsigned long long>(L.m_Cycle), unsigned(L.m_Pc), int(L.m_OldValue),
			int(Mem.GetNextReg(uint8_t(0x50 + SlotIdx))));
	}

	fflush(Out);
}
