#pragma once
#include <stdint.h>

// Per-instruction (or per-event) snapshot. The emulator's dispatcher fills
// these on the hot path; the writer thread consumes them and turns them
// into either text-format lines (via FormatTextLine) or binary records
// (via FormatAndWriteBinary). The binary->text decoder reconstructs them
// from the .btr stream and feeds them back through FormatTextLine, which
// is how the round-trip identity holds.
//
// Lives in the shared trace/ library because three different code paths
// touch it: the emulator-side writer, the shared text formatter, and the
// shared binary decoder. Keeping the layout in one header avoids the
// "format definition lives in two places" hazard.

struct TraceFormatOptions
{
	bool m_bVerbose = false;
	bool m_bIncludeR = false;
	bool m_bIncludeMemPtr = false;
	bool m_bDiffOnly = false; // when true, emit register effects only on Pre!=Post
	bool m_bB16KPageView = true; // true -> 16 KB page in address column (.SNA)
								 // false -> 8 KB page (.NEX/.DOT)
	uint64_t m_FramePeriod = 0; // T-states per frame; 0 means no frame column
};

struct TraceFrame
{
	enum : uint8_t
	{
		kFlagNormal = 0,
		kFlagInvalid = 1 << 0,
		kFlagHaltEnter = 1 << 1,
		kFlagInterrupt = 1 << 2,
		kFlagDma = 1 << 3,
		kFlagResetTrap = 1 << 4,
		kFlagInput = 1 << 5,
		// v4: not an instruction at all - a video-state change (NextReg or
		// I/O port write). Only StartCycles + the State* fields below are
		// meaningful; the register snapshot is not filled in.
		kFlagStateWrite = 1 << 6,
	};
	uint8_t m_Flags = 0;

	uint64_t m_StartCycles = 0;
	uint64_t m_Frame = 0;
	uint64_t m_EmuWallNanos = 0;
	uint16_t m_PrePC = 0, m_PreAF = 0, m_PreBC = 0, m_PreDE = 0, m_PreHL = 0;
	uint16_t m_PreIX = 0, m_PreIY = 0, m_PreSP = 0;
	uint16_t m_PreAFp = 0, m_PreBCp = 0, m_PreDEp = 0, m_PreHLp = 0;
	uint16_t m_PreMemPtr = 0;
	uint8_t m_PreI = 0, m_PreR = 0;
	uint8_t m_PreIFF1 = 0, m_PreIFF2 = 0;
	uint8_t m_PreHalted = 0;

	uint8_t m_OpcodeBytes[4] = {};
	uint8_t m_PrePageTag = 0;
	uint8_t m_InterruptVectorByte = 0;

	// Flag_StateWrite payload.
	uint16_t m_StateAddr = 0; // NextReg index, or I/O port
	uint8_t m_StateValue = 0;
	uint8_t m_StateKind = 0; // 0 = NextReg, 1 = I/O port
	uint8_t m_StateSource = 0; // BinaryTrace::NextRegSource (NextReg only)

	bool m_bInputPress = false;
	char m_InputName[16] = {};

	uint64_t m_EndCycles = 0;
	uint16_t m_PostPC = 0, m_PostAF = 0, m_PostBC = 0, m_PostDE = 0, m_PostHL = 0;
	uint16_t m_PostIX = 0, m_PostIY = 0, m_PostSP = 0;
	uint16_t m_PostAFp = 0, m_PostBCp = 0, m_PostDEp = 0, m_PostHLp = 0;
	uint16_t m_PostMemPtr = 0;
	uint8_t m_PostI = 0, m_PostR = 0;
	uint8_t m_PostIFF1 = 0, m_PostIFF2 = 0;
	uint8_t m_PostHalted = 0;

	struct MemWrite
	{
		uint16_t m_Addr;
		uint8_t m_Value;
		// Physical page the write LANDED in, encoded like PrePageTag. The
		// address alone is ambiguous on a paged machine: $C000 is a different
		// byte in every bank. Occupies what used to be padding, so the struct
		// and the .btr record layout are unchanged.
		uint8_t m_PageTag;
	};
	uint8_t m_NumMemWrites = 0;
	MemWrite m_MemWrites[4] = {};
};
