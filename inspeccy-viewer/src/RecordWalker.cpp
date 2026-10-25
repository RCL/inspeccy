#include "RecordWalker.h"

#include <string.h>

namespace ViewerWalk
{

namespace
{

// Decode a base-128 varint (LEB128-unsigned). On overlong (>10 bytes) or
// short-read, returns false and leaves cursor at the offending byte. On
// success, cursor advances past the varint.
bool DecodeVarint(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& OutValue)
{
	OutValue = 0;
	int Shift = 0;
	for (int IdxI = 0; IdxI < 10; ++IdxI)
	{
		if (Cursor >= Eof)
		{
			return false;
		}
		const uint8_t B = *Cursor++;
		OutValue |= uint64_t(B & 0x7F) << Shift;
		if ((B & 0x80) == 0)
		{
			return true;
		}
		Shift += 7;
	}
	return false; // overlong
}

// Little-endian readers over an advancing cursor.
bool ReadU8(const uint8_t*& C, const uint8_t* E, uint8_t& InOutV)
{
	if (C >= E)
	{
		return false;
	}
	InOutV = *C++;
	return true;
}
bool ReadU16(const uint8_t*& C, const uint8_t* E, uint16_t& InOutV)
{
	if (C + 2 > E)
	{
		return false;
	}
	InOutV = uint16_t(C[0]) | (uint16_t(C[1]) << 8);
	C += 2;
	return true;
}
bool ReadBytes(const uint8_t*& C, const uint8_t* E, void* Dst, size_t N)
{
	if (C + N > E)
	{
		return false;
	}
	memcpy(Dst, C, N);
	C += N;
	return true;
}

// Recognise the same tag set as trace_decode.cpp's main switch - anything
// outside this list (including the reserved-but-unemitted NextRegWrite and
// TrappedCall slots) is treated as "unknown, bail" by both consumers. When
// the writer starts emitting those tags this list expands AND the decoder
// list expands in lockstep; the .btr version bump signals it.
bool IsInstrLikeTag(uint8_t Tag)
{
	using namespace BinaryTrace;
	return Tag == kRecord_Instr || Tag == kRecord_LdirIter || Tag == kRecord_Interrupt || Tag == kRecord_DmaDrip ||
		   Tag == kRecord_InvalidOpcode || Tag == kRecord_RestartTrap;
}

// Body of an instr-like record. `cursor` is positioned at the start of the
// varint cycle-delta (i.e., just past the tag byte). On success, cursor ends
// up past the last MemWrite triple of this record.
bool DecodeInstrBody(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& InOutRunCycle, DecodedRecord* OutValue)
{
	using namespace BinaryTrace;

	uint64_t CycleDelta = 0;
	if (!DecodeVarint(Cursor, Eof, CycleDelta))
	{
		return false;
	}
	InOutRunCycle += CycleDelta;
	if (OutValue)
	{
		OutValue->m_Cycle = InOutRunCycle;
	}

	uint16_t RecPC = 0;
	uint8_t PageTag = 0;
	uint8_t Opcodes[4] = {};
	if (!ReadU16(Cursor, Eof, RecPC))
	{
		return false;
	}
	if (!ReadU8(Cursor, Eof, PageTag))
	{
		return false;
	}
	if (!ReadBytes(Cursor, Eof, Opcodes, 4))
	{
		return false;
	}
	if (OutValue)
	{
		OutValue->PC = RecPC;
		OutValue->m_PageTag = PageTag;
		memcpy(OutValue->m_Opcodes, Opcodes, 4);
		// PostPC defaults to recPC: when the diff mask doesn't include
		// Diff_PC, the writer's running PC stayed at recPC. The CLI decoder
		// does the same (initialises PostPC = RunPC before reading the diff
		// mask) - this match is what makes "conditional CALL not taken"
		// detectable as `PostPC == PrePC + length` downstream.
		OutValue->m_PostPC = RecPC;
	}

	uint16_t Mask = 0;
	if (!ReadU16(Cursor, Eof, Mask))
	{
		return false;
	}
	if (OutValue)
	{
		OutValue->m_Mask = Mask;
	}

	// Read each masked field in stream order. If `out` is null we still need
	// to advance the cursor by the correct number of bytes; use a scratch.
	auto Rd16 = [&](uint16_t Bit, uint16_t* Dst) -> bool
	{
		if ((Mask & Bit) == 0)
		{
			return true;
		}
		uint16_t V = 0;
		if (!ReadU16(Cursor, Eof, V))
		{
			return false;
		}
		if (Dst)
		{
			*Dst = V;
		}
		return true;
	};
	auto Rd8 = [&](uint16_t Bit, uint8_t* Dst) -> bool
	{
		if ((Mask & Bit) == 0)
		{
			return true;
		}
		uint8_t V = 0;
		if (!ReadU8(Cursor, Eof, V))
		{
			return false;
		}
		if (Dst)
		{
			*Dst = V;
		}
		return true;
	};

	if (!Rd16(kRegDiffBitDiffPc, OutValue ? &OutValue->m_PostPC : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffAf, OutValue ? &OutValue->m_PostAF : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffBc, OutValue ? &OutValue->m_PostBC : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffDe, OutValue ? &OutValue->m_PostDE : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffHl, OutValue ? &OutValue->m_PostHL : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffIx, OutValue ? &OutValue->m_PostIX : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffIy, OutValue ? &OutValue->m_PostIY : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffSp, OutValue ? &OutValue->m_PostSP : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffAFp, OutValue ? &OutValue->m_PostAFp : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffBCp, OutValue ? &OutValue->m_PostBCp : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffDEp, OutValue ? &OutValue->m_PostDEp : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffHLp, OutValue ? &OutValue->m_PostHLp : nullptr))
	{
		return false;
	}
	if (!Rd16(kRegDiffBitDiffMemPtr, OutValue ? &OutValue->m_PostMemPtr : nullptr))
	{
		return false;
	}
	if (!Rd8(kRegDiffBitDiffI, OutValue ? &OutValue->m_PostI : nullptr))
	{
		return false;
	}
	if (!Rd8(kRegDiffBitDiffR, OutValue ? &OutValue->m_PostR : nullptr))
	{
		return false;
	}
	if (!Rd8(kRegDiffBitDiffIFFs, OutValue ? &OutValue->m_PostIFFsPacked : nullptr))
	{
		return false;
	}

	uint8_t NumMem = 0;
	if (!ReadU8(Cursor, Eof, NumMem))
	{
		return false;
	}
	if (NumMem > 4)
	{
		return false;
	}
	if (OutValue)
	{
		OutValue->m_NumMemWrites = NumMem;
	}
	for (uint8_t IdxI = 0; IdxI < NumMem; ++IdxI)
	{
		uint16_t Addr = 0;
		uint8_t Val = 0;
		if (!ReadU16(Cursor, Eof, Addr))
		{
			return false;
		}
		if (!ReadU8(Cursor, Eof, Val))
		{
			return false;
		}
		if (OutValue)
		{
			OutValue->m_MemAddr[IdxI] = Addr;
			OutValue->m_MemVal[IdxI] = Val;
		}
	}
	return true;
}

// v4 video-state event body. `cursor` starts just past the tag byte.
bool DecodeStateBody(
	const uint8_t*& Cursor, const uint8_t* Eof, uint8_t Tag, uint64_t& InOutRunCycle, DecodedRecord* OutValue)
{
	using namespace BinaryTrace;
	uint64_t CycleDelta = 0;
	if (!DecodeVarint(Cursor, Eof, CycleDelta))
	{
		return false;
	}
	InOutRunCycle += CycleDelta;
	if (OutValue)
	{
		OutValue->m_Cycle = InOutRunCycle;
	}

	if (Tag == kRecord_NextRegWrite)
	{
		uint8_t Reg = 0, Val = 0, Src = 0;
		if (!ReadU8(Cursor, Eof, Reg))
		{
			return false;
		}
		if (!ReadU8(Cursor, Eof, Val))
		{
			return false;
		}
		if (!ReadU8(Cursor, Eof, Src))
		{
			return false;
		}
		if (OutValue)
		{
			OutValue->m_StateAddr = Reg;
			OutValue->m_StateValue = Val;
			OutValue->m_StateSource = Src;
		}
		return true;
	}
	uint16_t Port = 0;
	uint8_t Val = 0;
	if (!ReadU16(Cursor, Eof, Port))
	{
		return false;
	}
	if (!ReadU8(Cursor, Eof, Val))
	{
		return false;
	}
	if (OutValue)
	{
		OutValue->m_StateAddr = Port;
		OutValue->m_StateValue = Val;
	}
	return true;
}

} // namespace

bool DecodeOne(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& InOutRunCycle, uint64_t& InOutRunFrame,
	DecodedRecord& OutValue)
{
	using namespace BinaryTrace;

	if (Cursor >= Eof)
	{
		return false;
	}
	const uint8_t Tag = *Cursor++;
	OutValue.m_Tag = Tag;

	if (Tag == kRecord_FrameBoundary)
	{
		uint64_t CycleDelta = 0;
		uint64_t FrameNumber = 0;
		if (!DecodeVarint(Cursor, Eof, CycleDelta))
		{
			return false;
		}
		if (!DecodeVarint(Cursor, Eof, FrameNumber))
		{
			return false;
		}
		InOutRunCycle += CycleDelta;
		InOutRunFrame = FrameNumber;
		OutValue.m_Cycle = InOutRunCycle;
		OutValue.m_NewFrame = FrameNumber;
		return true;
	}
	if (Tag == kRecord_NextRegWrite || Tag == kRecord_PortWrite)
	{
		return DecodeStateBody(Cursor, Eof, Tag, InOutRunCycle, &OutValue);
	}
	if (!IsInstrLikeTag(Tag))
	{
		return false; // unknown / reserved -> bail
	}

	return DecodeInstrBody(Cursor, Eof, InOutRunCycle, &OutValue);
}

bool SkipOne(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& InOutRunCycle, uint64_t& InOutRunFrame,
	uint8_t& OutTag, uint64_t& OutCycle)
{
	using namespace BinaryTrace;

	if (Cursor >= Eof)
	{
		return false;
	}
	const uint8_t Tag = *Cursor++;
	OutTag = Tag;

	if (Tag == kRecord_FrameBoundary)
	{
		uint64_t CycleDelta = 0;
		uint64_t FrameNumber = 0;
		if (!DecodeVarint(Cursor, Eof, CycleDelta))
		{
			return false;
		}
		if (!DecodeVarint(Cursor, Eof, FrameNumber))
		{
			return false;
		}
		InOutRunCycle += CycleDelta;
		InOutRunFrame = FrameNumber;
		OutCycle = InOutRunCycle;
		return true;
	}
	if (Tag == kRecord_NextRegWrite || Tag == kRecord_PortWrite)
	{
		if (!DecodeStateBody(Cursor, Eof, Tag, InOutRunCycle, nullptr))
		{
			return false;
		}
		OutCycle = InOutRunCycle;
		return true;
	}
	if (!IsInstrLikeTag(Tag))
	{
		return false;
	}

	if (!DecodeInstrBody(Cursor, Eof, InOutRunCycle, nullptr))
	{
		return false;
	}
	OutCycle = InOutRunCycle;
	return true;
}

} // namespace ViewerWalk
