#pragma once
#include <trace/trace_binary.h>
#include <stdint.h>

// Memory-mapped record walker. The CLI decoder (trace/src/trace_decode.cpp)
// walks records through a FILE*; the viewer needs the same logic over a
// QFile::map() byte range. Pure consumer of the .btr format - no Qt deps so
// it could move to trace/ later if the emulator side ever needs it.
//
// Format reference: trace/include/trace/trace_binary.h.

namespace ViewerWalk
{

// Everything one DecodeOne() call gives you about a single record. Fields
// not relevant to a given tag are left at their default (0). FrameBoundary
// records only populate Tag, Cycle, and NewFrame; "instr-like" records
// populate Tag, Cycle, PC, PageTag, Opcodes, Mask, the masked Post* fields,
// and the mem-write array.
struct DecodedRecord
{
	uint8_t m_Tag = 0;
	uint64_t m_Cycle = 0; // absolute cycle after applying delta
	uint16_t PC = 0; // PC at record entry (instr-like only)
	uint8_t m_PageTag = 0;
	uint8_t m_Opcodes[4] = {};
	uint16_t m_Mask = 0; // BinaryTrace::RegDiffBit OR'd together

	uint16_t m_PostPC = 0, m_PostAF = 0, m_PostBC = 0, m_PostDE = 0, m_PostHL = 0, m_PostIX = 0, m_PostIY = 0,
			 m_PostSP = 0;
	uint16_t m_PostAFp = 0, m_PostBCp = 0, m_PostDEp = 0, m_PostHLp = 0;
	uint16_t m_PostMemPtr = 0;
	uint8_t m_PostI = 0, m_PostR = 0;
	uint8_t m_PostIFFsPacked = 0; // bit0=IFF1, bit1=IFF2, bit7=Halted

	uint8_t m_NumMemWrites = 0;
	uint16_t m_MemAddr[4] = {};
	uint8_t m_MemVal[4] = {};

	uint64_t m_NewFrame = 0; // FrameBoundary only

	// v4 video-state events (kRecord_NextRegWrite / kRecord_PortWrite).
	// StateAddr is the NextReg index or the I/O port; StateSource is a
	// BinaryTrace::NextRegSource and is meaningful for NextReg writes only.
	uint16_t m_StateAddr = 0;
	uint8_t m_StateValue = 0;
	uint8_t m_StateSource = 0;
};

// True for the v4 zero-width video-state events. Every walker that only cares
// about executed instructions must skip these: they carry no PC, no page tag
// and no register deltas, so treating them as instructions corrupts anything
// that tracks running state (ROM intervals key off PageTag, which would read
// as RAM page 0).
inline bool IsStateRecord(uint8_t Tag)
{
	return Tag == BinaryTrace::kRecord_NextRegWrite || Tag == BinaryTrace::kRecord_PortWrite;
}

// Decode one record at `cursor`. On success cursor advances to the byte after
// the record and the function returns true; runCycle and runFrame are updated
// with the record's effect on the running state. On EOF (cursor == eof at
// entry, or short read), corrupt varint, or unknown tag, returns false and
// cursor is left at the offending byte for diagnostics.
//
// runCycle: caller initialises to Header::StartCycle (the cycle just before
//   the first record), then DecodeOne adds the per-record delta to it.
// runFrame: caller initialises to Header::StartFrame; FrameBoundary records
//   overwrite it to the absolute frame number from the stream.
bool DecodeOne(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& InOutRunCycle, uint64_t& InOutRunFrame,
	DecodedRecord& OutValue);

// Cheap variant: same accepted-tag set, but stops parsing as soon as Cycle
// and (for FrameBoundary) NewFrame are known. Use when you don't need the
// register deltas or memwrite list - e.g. per-frame CPU utilisation scans.
// `runCycle` and `runFrame` are updated identically to DecodeOne.
//
// `outTag` is the tag byte. `outCycle` is the absolute cycle. Cursor advances
// to the next record (so this still reads the full record body to advance).
bool SkipOne(const uint8_t*& Cursor, const uint8_t* Eof, uint64_t& InOutRunCycle, uint64_t& InOutRunFrame,
	uint8_t& OutTag, uint64_t& OutCycle);

} // namespace ViewerWalk
