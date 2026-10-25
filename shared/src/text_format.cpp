#include <trace/text_format.h>
#include <trace/trace_binary.h>
#include <trace/trace_frame.h>

#include <trace/disasm.h>
#include <trace/sld.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace
{

void AppendStr(char*& InOutCursor, char* End, const char* S)
{
	while (*S && InOutCursor < End - 1)
	{
		*InOutCursor++ = *S++;
	}
}

void AppendChar(char*& InOutCursor, char* End, char C)
{
	if (InOutCursor < End - 1)
	{
		*InOutCursor++ = C;
	}
}

void AppendFmt(char*& InOutCursor, char* End, const char* Fmt, ...)
{
	va_list Args;
	va_start(Args, Fmt);
	const int Avail = int(End - InOutCursor) - 1;
	if (Avail > 0)
	{
		const int W = std::vsnprintf(InOutCursor, size_t(Avail + 1), Fmt, Args);
		if (W > 0)
		{
			InOutCursor += (W < Avail) ? W : Avail;
		}
	}
	va_end(Args);
}

} // namespace

void FormatTextLine(const TraceFrame& Frame, const TraceFormatOptions& Options, const SldSymbolMap* Symbols,
	FILE* OutFile, uint64_t& InOutLastEmittedFrame)
{
	char Line[512];
	char* C = Line;
	char* E = Line + sizeof(Line);

	// Horizontal divider whenever this line crosses into a new frame. Fires for
	// every frame boundary the trace observes - independent of whether /INT was
	// accepted in that frame.
	if (Frame.m_Frame != InOutLastEmittedFrame)
	{
		if (InOutLastEmittedFrame != UINT64_MAX)
		{
			char Divider[110];
			const int Prefix =
				snprintf(Divider, sizeof(Divider), "----- Frame %llu begins ", (unsigned long long)Frame.m_Frame);
			if (Prefix > 0 && Prefix < 99)
			{
				for (int IdxI = Prefix; IdxI < 99; ++IdxI)
				{
					Divider[IdxI] = '-';
				}
				Divider[99] = '\n';
				fwrite(Divider, 1, 100, OutFile);
			}
		}
		InOutLastEmittedFrame = Frame.m_Frame;
	}

	// Column 1: cycles, right-aligned to 12.
	AppendFmt(C, E, "%12llu | ", (unsigned long long)Frame.m_StartCycles);

	// Column 2: frame counter, right-aligned to 6.
	AppendFmt(C, E, "%6llu | ", (unsigned long long)Frame.m_Frame);

	// Column 3: address. For 16 KB page view (.SNA inputs, default): RAM bank
	// 0..7 or ROM R0..R2 (collapsed pairs of 8 KB pages), 14-bit offset.
	// For 8 KB page view (.NEX/.DOT): RAM page 0..223 or ROM R0..R5.
	{
		const bool bIsRom = BinaryTrace::PageTagIsRom(Frame.m_PrePageTag);
		const uint8_t Index8K = bIsRom ? BinaryTrace::PageTagRomIndex(Frame.m_PrePageTag) : Frame.m_PrePageTag;
		char AddrCol[24];
		if (Options.m_bB16KPageView)
		{
			const uint8_t Index16K = uint8_t(Index8K >> 1);
			const uint16_t Offset14 = uint16_t(Frame.m_PrePC & 0x3FFF);
			if (bIsRom)
			{
				snprintf(AddrCol, sizeof(AddrCol), "$%04X (R%u:%04X)", Frame.m_PrePC,
					static_cast<unsigned int>(Index16K), static_cast<unsigned int>(Offset14));
			}
			else
			{
				snprintf(AddrCol, sizeof(AddrCol), "$%04X (%2u:%04X)", Frame.m_PrePC,
					static_cast<unsigned int>(Index16K), static_cast<unsigned int>(Offset14));
			}
		}
		else
		{
			const uint16_t Offset13 = uint16_t(Frame.m_PrePC & 0x1FFF);
			if (bIsRom)
			{
				snprintf(AddrCol, sizeof(AddrCol), "$%04X (R%u:%04X)", Frame.m_PrePC,
					static_cast<unsigned int>(Index8K), static_cast<unsigned int>(Offset13));
			}
			else
			{
				snprintf(AddrCol, sizeof(AddrCol), "$%04X (%2u:%04X)", Frame.m_PrePC,
					static_cast<unsigned int>(Index8K), static_cast<unsigned int>(Offset13));
			}
		}
		AppendFmt(C, E, "%-16s | ", AddrCol);
	}

	// Symbol column (only when an SldSymbolMap is loaded). Width 24 left-padded.
	if (Symbols != nullptr)
	{
		std::string Sym;
		const uint8_t Page8K = Frame.m_PrePageTag; // RAM: the tag IS the native 8K page
		if (!BinaryTrace::PageTagIsRom(Frame.m_PrePageTag) // RAM only - sjasmplus rarely emits ROM symbols
			&& Symbols->LookupNearest(Page8K, Frame.m_PrePC, 256, Sym))
		{
			const_cast<SldSymbolMap*>(Symbols)->NoteMatch();
			AppendFmt(C, E, "%-24s | ", Sym.c_str());
		}
		else
		{
			const_cast<SldSymbolMap*>(Symbols)->NoteMiss();
			AppendStr(C, E, "                         | ");
		}
	}

	// Column N: mnemonic. Determine length first using the disassembler so we
	// can detect non-sequential PC moves in the post-state comparison.
	char Mnemonic[64];
	int InstrLen = 0;
	bool bIsHalt = false;
	bool bIsEi = false;
	bool bIsDi = false;

	if (Frame.m_Flags & TraceFrame::kFlagInterrupt)
	{
		// Interrupt acceptance event.
		//   mnemonic field: "Interrupt, vector=$NNNN"
		//   effects field:  "(NNNN)=NNNN, SP=NNNN, PC=NNNN"
		const uint16_t VectorAddr = uint16_t((uint16_t(Frame.m_PreI) << 8) | Frame.m_InterruptVectorByte);
		char Mn[64];
		snprintf(Mn, sizeof(Mn), "Interrupt, vector=$%04X", static_cast<unsigned int>(VectorAddr));
		const size_t MnLen = strlen(Mn);
		AppendStr(C, E, Mn);
		if (MnLen < 24)
		{
			const size_t Pad = 24 - MnLen;
			for (size_t IdxI = 0; IdxI < Pad && C < E - 1; ++IdxI)
			{
				*C++ = ' ';
			}
		}
		AppendStr(C, E, " | ");

		// Memory writes: PUSH16 emits high-byte-at-SP-1, then low-byte-at-SP-2.
		// Coalesce into a single (NewSP)=PushedPC entry.
		if (Frame.m_NumMemWrites == 2 && Frame.m_MemWrites[0].m_Addr == uint16_t(Frame.m_MemWrites[1].m_Addr + 1))
		{
			const uint16_t Val = uint16_t(Frame.m_MemWrites[1].m_Value) | (uint16_t(Frame.m_MemWrites[0].m_Value) << 8);
			AppendFmt(C, E, "(%04X)=%04X, ", Frame.m_MemWrites[1].m_Addr, Val);
		}
		AppendFmt(C, E, "SP=%04X, PC=%04X", Frame.m_PostSP, Frame.m_PostPC);

		// Show R only when --trace-r is enabled (interrupt accept always changes R).
		if (Options.m_bIncludeR)
		{
			AppendFmt(C, E, ", R=%02X", Frame.m_PostR);
		}

		AppendChar(C, E, '\n');
		fwrite(Line, 1, size_t(C - Line), OutFile);
		return;
	}

	if (Frame.m_Flags & TraceFrame::kFlagInput)
	{
		// Rewrite the line as a single comment so it doesn't have the cycles/
		// frame/address columns. The divider above (emitted by the divider
		// check) anchors this to the right frame visually.
		C = Line;
		AppendFmt(C, E, "# input frame=%llu %s %s\n", (unsigned long long)Frame.m_Frame,
			Frame.m_bInputPress ? "press" : "release", Frame.m_InputName);
		fwrite(Line, 1, size_t(C - Line), OutFile);
		return;
	}

	if (Frame.m_Flags & TraceFrame::kFlagResetTrap)
	{
		AppendStr(C, E, "<reset trap>             | RESET_TRAP, PC=0000\n");
		fwrite(Line, 1, size_t(C - Line), OutFile);
		return;
	}

	if (Frame.m_Flags & TraceFrame::kFlagInvalid)
	{
		// Render "invalid $NN[, $NN ...]". The dispatcher captured up to 4
		// bytes; emit until the byte that triggered the failure. We don't know
		// the exact count, so emit 1..4 bytes based on the OpcodeBytes[0]
		// pattern: prefix byte (DD/FD/ED/CB) -> 2 bytes; DDCB/FDCB -> 4 bytes.
		const uint8_t kB0 = Frame.m_OpcodeBytes[0];
		int N = 1;
		if (kB0 == 0xED || kB0 == 0xCB)
		{
			N = 2;
		}
		if (kB0 == 0xDD || kB0 == 0xFD)
		{
			const uint8_t kB1 = Frame.m_OpcodeBytes[1];
			N = (kB1 == 0xCB) ? 4 : 2;
		}
		AppendStr(C, E, "invalid ");
		for (int IdxI = 0; IdxI < N; ++IdxI)
		{
			if (IdxI > 0)
			{
				AppendStr(C, E, ", ");
			}
			AppendFmt(C, E, "$%02X", Frame.m_OpcodeBytes[IdxI]);
		}
		AppendStr(C, E, " | INVALID_OPCODE, PC=");
		AppendFmt(C, E, "%04X\n", Frame.m_PrePC);
		fwrite(Line, 1, size_t(C - Line), OutFile);
		return;
	}

	uint16_t DestMask = 0;
	InstrLen = Disassemble(Frame.m_OpcodeBytes, Frame.m_PrePC, Mnemonic, sizeof(Mnemonic), &DestMask);
	if (InstrLen < 1)
	{
		InstrLen = 1;
	}
	if (Options.m_bDiffOnly)
	{
		DestMask = 0;
	}

	// Identify a few specific instructions cheaply.
	if (Frame.m_OpcodeBytes[0] == 0x76)
	{
		bIsHalt = true;
	}
	if (Frame.m_OpcodeBytes[0] == 0xFB)
	{
		bIsEi = true;
	}
	if (Frame.m_OpcodeBytes[0] == 0xF3)
	{
		bIsDi = true;
	}

	// Render mnemonic left-aligned to ~24 chars for alignment-friendly output.
	{
		const size_t MnLen = strlen(Mnemonic);
		AppendStr(C, E, Mnemonic);
		if (MnLen < 24)
		{
			const size_t Pad = 24 - MnLen;
			for (size_t IdxI = 0; IdxI < Pad && C < E - 1; ++IdxI)
			{
				*C++ = ' ';
			}
		}
	}
	AppendStr(C, E, " | ");

	// Column 3: effects.
	bool bAnyEffect = false;
	auto Sep = [&]()
	{
		if (bAnyEffect)
		{
			AppendStr(C, E, ", ");
		}
		bAnyEffect = true;
	};

	// Diff post vs pre for the standard register file.
	const bool bVerbose = Options.m_bVerbose;

	auto Diff8 = [&](const char* Name, uint8_t Pre, uint8_t Post, uint16_t DestBit)
	{
		if (bVerbose || Pre != Post || (DestMask & DestBit))
		{
			Sep();
			AppendFmt(C, E, "%s=%02X", Name, Post);
		}
	};
	auto Diff16 = [&](const char* Name, uint16_t Pre, uint16_t Post, uint16_t DestBit)
	{
		if (bVerbose || Pre != Post || (DestMask & DestBit))
		{
			Sep();
			AppendFmt(C, E, "%s=%04X", Name, Post);
		}
	};

	const uint8_t PreA = uint8_t(Frame.m_PreAF >> 8);
	const uint8_t PostA = uint8_t(Frame.m_PostAF >> 8);
	const uint8_t PreF = uint8_t(Frame.m_PreAF & 0xFF);
	const uint8_t PostF = uint8_t(Frame.m_PostAF & 0xFF);
	Diff8("A", PreA, PostA, kDestA);
	Diff8("F", PreF, PostF, kDestF);
	Diff16("BC", Frame.m_PreBC, Frame.m_PostBC, kDestBc);
	Diff16("DE", Frame.m_PreDE, Frame.m_PostDE, kDestDe);
	Diff16("HL", Frame.m_PreHL, Frame.m_PostHL, kDestHl);
	Diff16("IX", Frame.m_PreIX, Frame.m_PostIX, kDestIx);
	Diff16("IY", Frame.m_PreIY, Frame.m_PostIY, kDestIy);
	Diff16("SP", Frame.m_PreSP, Frame.m_PostSP, kDestSp);

	// PC: emit if non-sequential, or DestMask says PC is a destination AND
	// the branch was actually taken (PostPC != sequential). For conditional
	// branches that didn't fire, PC is sequential - we suppress to keep the
	// "not taken" line clean.
	const uint16_t SequentialPC = uint16_t(Frame.m_PrePC + InstrLen);
	const bool bPCChanged = (Frame.m_PostPC != SequentialPC);
	if (bPCChanged || bVerbose)
	{
		Sep();
		AppendFmt(C, E, "PC=%04X", Frame.m_PostPC);
	}

	Diff8("I", Frame.m_PreI, Frame.m_PostI, kDestI);

	if (Options.m_bIncludeR || (Frame.m_PreR & 0x7F) != (Frame.m_PostR & 0x7F))
	{
		// Always emit when explicitly requested. When unset, R changes on every
		// instruction so we'd flood - suppress unless the user opts in.
		if (Options.m_bIncludeR)
		{
			Sep();
			AppendFmt(C, E, "R=%02X", Frame.m_PostR);
		}
		else
		{
			// Targeted-R-write instructions (LD R, A is ED 4F) - those legitimately
			// change R as a programmer-visible effect. ED 4F has OpcodeBytes[0]==ED
			// and OpcodeBytes[1]==0x4F.
			const bool bExplicitR = (Frame.m_OpcodeBytes[0] == 0xED && Frame.m_OpcodeBytes[1] == 0x4F);
			if (bExplicitR)
			{
				Sep();
				AppendFmt(C, E, "R=%02X", Frame.m_PostR);
			}
		}
	}

	if (Options.m_bIncludeMemPtr && Frame.m_PreMemPtr != Frame.m_PostMemPtr)
	{
		Sep();
		AppendFmt(C, E, "MemPtr=%04X", Frame.m_PostMemPtr);
	}

	Diff16("AF'", Frame.m_PreAFp, Frame.m_PostAFp, kDestAfp);
	Diff16("BC'", Frame.m_PreBCp, Frame.m_PostBCp, kDestAlt);
	Diff16("DE'", Frame.m_PreDEp, Frame.m_PostDEp, kDestAlt);
	Diff16("HL'", Frame.m_PreHLp, Frame.m_PostHLp, kDestAlt);

	// Memory writes. Coalesce PUSH-style adjacent writes into "(addr)=NNNN".
	if (Frame.m_NumMemWrites == 2)
	{
		const auto& kW0 = Frame.m_MemWrites[0];
		const auto& kW1 = Frame.m_MemWrites[1];
		// Z80 push: high byte at SP-1, then low byte at SP-2. So W0.Addr == W1.Addr+1.
		if (kW0.m_Addr == uint16_t(kW1.m_Addr + 1))
		{
			const uint16_t Val = uint16_t(kW1.m_Value) | (uint16_t(kW0.m_Value) << 8);
			Sep();
			AppendFmt(C, E, "(%04X)=%04X", kW1.m_Addr, Val);
		}
		else
		{
			Sep();
			AppendFmt(C, E, "(%04X)=%02X", kW0.m_Addr, kW0.m_Value);
			Sep();
			AppendFmt(C, E, "(%04X)=%02X", kW1.m_Addr, kW1.m_Value);
		}
	}
	else
	{
		for (int IdxI = 0; IdxI < int(Frame.m_NumMemWrites); ++IdxI)
		{
			const auto& Wr = Frame.m_MemWrites[IdxI];
			Sep();
			AppendFmt(C, E, "(%04X)=%02X", Wr.m_Addr, Wr.m_Value);
		}
	}

	// EI/DI: emit bare-string effect (matches the trace plan spec).
	if (bIsEi)
	{
		if (bAnyEffect)
		{
			AppendStr(C, E, ", ");
		}
		AppendStr(C, E, "interrupts enabled");
		bAnyEffect = true;
	}
	if (bIsDi)
	{
		if (bAnyEffect)
		{
			AppendStr(C, E, ", ");
		}
		AppendStr(C, E, "interrupts disabled");
		bAnyEffect = true;
	}

	// HALT enter: emit "halted=1".
	if (bIsHalt && Frame.m_PostHalted != 0 && Frame.m_PreHalted == 0)
	{
		if (bAnyEffect)
		{
			AppendStr(C, E, ", ");
		}
		AppendStr(C, E, "halted=1");
		bAnyEffect = true;
	}

	// Taken / not-taken annotation for conditional control transfers.
	// Walk past any DD/FD prefix bytes to find the real opcode.
	{
		int IPos = 0;
		uint8_t Op0 = Frame.m_OpcodeBytes[0];
		while ((Op0 == 0xDD || Op0 == 0xFD) && IPos < 3)
		{
			++IPos;
			Op0 = Frame.m_OpcodeBytes[IPos];
		}
		const bool bDjnz = (Op0 == 0x10);
		const bool bJrCc = (Op0 == 0x20 || Op0 == 0x28 || Op0 == 0x30 || Op0 == 0x38);
		const bool bRetCc = ((Op0 & 0xC7) == 0xC0);
		const bool bJpCc = ((Op0 & 0xC7) == 0xC2);
		const bool bCallCc = ((Op0 & 0xC7) == 0xC4);
		if (bDjnz || bJrCc || bRetCc || bJpCc || bCallCc)
		{
			const bool bTaken = (Frame.m_PostPC != SequentialPC);
			if (bAnyEffect)
			{
				AppendStr(C, E, ", ");
			}
			AppendStr(C, E, bTaken ? "taken" : "not taken");
			bAnyEffect = true;
		}
	}

	AppendChar(C, E, '\n');
	fwrite(Line, 1, size_t(C - Line), OutFile);
}
