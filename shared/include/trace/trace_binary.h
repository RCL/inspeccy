#pragma once

// Binary trace format (".btr") + frame-index sidecar (".btr.fidx"), v2.
// Authoritative byte layout - writer + reader + index builder all #include this.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace BinaryTrace
{

constexpr uint32_t kMagic = 0x42544E49; // "INTB"
constexpr uint32_t kIndexMagic = 0x49464E49; // "INFI"
// Binary trace versions are intentionally incompatible; readers require
// kVersion. Version 2 added native 8K page tags, version 3 the nominal
// frame period, and version 4 initial display state plus register/port
// write records. Video changes include copper writes and all OUT forms.
constexpr uint16_t kVersion = 4;
constexpr uint64_t kSentinel = 0xFFFFFFFFFFFFFFFFull;

// --- Per-instruction page tag encoding (v2) ---------------------------------
// One byte records the native 8K page the CPU executed the instruction in:
//   0x00..0xF9  RAM 8K page 0..249   (the ZX Next's real page range is 0..223)
//   0xFA..0xFF  ROM 8K page 0..5     (128K editor 0/1, 48K BASIC 2/3, TR-DOS 4/5)
// v1 packed a ROM flag into bit 7 and truncated the page to the low nibble,
// losing every RAM page >= 16. v2 stores the page natively and moves ROM into
// the reserved top range, so all 224 Next pages round-trip; the R0..R2 ROM
// distinction survives too (index 0..5 >> 1 for the 16K address column).
constexpr uint8_t kPageTagRomBase = 0xFA; // 0xFA..0xFF -> ROM pages 0..5

inline bool PageTagIsRom(uint8_t Tag)
{
	return Tag >= kPageTagRomBase;
}
inline uint8_t PageTagRomIndex(uint8_t Tag)
{
	return uint8_t(Tag - kPageTagRomBase);
}

// Encode the emulator's (bIsRom, 8K page index) into the tag byte. ROM index is
// 0..5. A RAM page in the reserved range (>= 250 - never real executing code)
// clamps to 0xF9 so it can't be mistaken for a ROM sentinel.
inline uint8_t MakePageTag(bool bIsRom, uint8_t Index)
{
	if (bIsRom)
	{
		return uint8_t(kPageTagRomBase + (Index <= 5 ? Index : 5));
	}
	return (Index < kPageTagRomBase) ? Index : uint8_t(kPageTagRomBase - 1);
}

// --- Record tag bytes -------------------------------------------------------

enum RecordTag : uint8_t
{
	kRecord_Instr = 0x01, // one Z80 instruction completed
	kRecord_LdirIter = 0x02, // LDIR-class repeated-block iteration
	kRecord_Interrupt = 0x03, // IM 1 / IM 2 / CTC vector accepted
	kRecord_DmaDrip = 0x04, // CPU instr running during a DMA window
	kRecord_InvalidOpcode = 0x05, // dispatcher trip - final record before exit
	kRecord_RestartTrap = 0x06, // PC reached $0000 with ROM at slot 0 - final
	kRecord_FrameBoundary = 0x07, // frame number changed
	kRecord_NextRegWrite = 0x08, // NR_xx write (CPU NEXTREG / port $253B / copper)
	kRecord_TrappedCall = 0x09, // NextZXOS / esxDOS trap intercepted an API
	kRecord_PortWrite = 0x0C, // v4: write to a video-relevant I/O port
	// Reserved (decoders refuse unknown tags):
	// 0x0A  kRecord_Copper
	// 0x0B  kRecord_DmaWindow
	kRecord_Comment = 0xF0, // length-prefixed UTF-8 annotation
};

// --- Header flags -----------------------------------------------------------

enum HeaderFlag : uint16_t
{
	kFlag_Verbose = 1 << 0,
	kFlag_IncludeR = 1 << 1,
	kFlag_IncludeMemPtr = 1 << 2,
	kFlag_DiffOnly = 1 << 3,
	kFlag_TraceDrip = 1 << 4,
	kFlag_16KPageView = 1 << 5,
};

// --- Register-diff bitmask for delta-encoded instruction records ------------

enum RegDiffBit : uint16_t
{
	kRegDiffBitDiffPc = 1 << 0, // u16
	kRegDiffBitDiffAf = 1 << 1, // u16
	kRegDiffBitDiffBc = 1 << 2, // u16
	kRegDiffBitDiffDe = 1 << 3, // u16
	kRegDiffBitDiffHl = 1 << 4, // u16
	kRegDiffBitDiffIx = 1 << 5, // u16
	kRegDiffBitDiffIy = 1 << 6, // u16
	kRegDiffBitDiffSp = 1 << 7, // u16
	kRegDiffBitDiffAFp = 1 << 8, // u16 - emitted only when kFlag_Verbose
	kRegDiffBitDiffBCp = 1 << 9, // u16 - verbose
	kRegDiffBitDiffDEp = 1 << 10, // u16 - verbose
	kRegDiffBitDiffHLp = 1 << 11, // u16 - verbose
	kRegDiffBitDiffMemPtr = 1 << 12, // u16 - emitted only with kFlag_IncludeMemPtr
	kRegDiffBitDiffI = 1 << 13, // u8
	kRegDiffBitDiffR = 1 << 14, // u8 - emitted only with kFlag_IncludeR
	kRegDiffBitDiffIFFs = 1 << 15, // u8: bit0=IFF1, bit1=IFF2, bit7=bHalted
};

// --- Machine enum (stable across versions; append at end) -------------------

enum MachineCode : uint8_t
{
	kMachine_Spectrum48K = 0,
	kMachine_Spectrum128K = 1,
	kMachine_Pentagon128 = 2,
	kMachine_Next50 = 3,
	kMachine_Next60 = 4,
	// Timex machines (Spectrum-class timing, but the SCLD renders bright
	// black as a visible dark grey - viewers key their palette off these).
	kMachine_Ts2068 = 5,
	kMachine_Tc2068 = 6,
	kMachine_Tc2048 = 7,
	// Post-Spectrum clones (appended; Pentagon/128K-class timing).
	kMachine_ZxEvo = 8,
	kMachine_ZxUno = 9,
};

// --- Trap kinds for kRecord_TrappedCall -------------------------------------

enum TrapKind : uint8_t
{
	kTrap_None = 0,
	kTrap_EsxDosRst08 = 1, // RST $08 file API
	kTrap_NextzxosM3Dos = 2, // M_P3DOS via NextZXOS gate
	kTrap_Im1Stub = 3, // $5B00 IM 1 ROM stub
};

// --- Interrupt-record opcode field ------------------------------------------
// A kRecord_Interrupt event fetches no instruction, so its four opcode bytes
// carry the interrupt-acknowledge bus value instead - the byte the CPU reads
// off the data bus during the INT acknowledge cycle, which is exactly what
// identifies the device that raised the interrupt:
//   [0] = bus value. $FF means nothing drove the bus, i.e. the classic ULA
//         frame interrupt. On a Next with NR_C0 bit 0 set the hardware drives
//         NR_C0[7:5] | (source_index << 1).
//   [1] = kIntAckMarker. Traces written before this field was populated left
//         all four bytes zero, and a zero bus value is legal, so consumers
//         need the marker to tell "vector $00" from "nothing recorded".
//   [2..3] = 0.
// This is a semantic use of a previously-dead field, not a layout change: the
// record size is unchanged and v2 readers keep parsing it correctly.
constexpr uint8_t kIntAckMarker = 0xA5;

// Decoded source index from a hardware-vectored bus value: (bus & 0x1F) >> 1,
// in the Next's interrupt priority order. Only Ula and Ctc0..7 are ever emitted
// by this emulator today - the rest are listed so a decoder names them rather
// than showing a bare number if they ever appear.
enum IntSourceIndex : uint8_t
{
	kIntSrc_Line = 0, // raster/line interrupt
	kIntSrc_Uart0Rx = 1,
	kIntSrc_Uart1Rx = 2,
	kIntSrc_Ctc0 = 3, // CTC channel N = 3 + N, so CTC7 = 10
	kIntSrc_Ctc7 = 10,
	kIntSrc_Ula = 11, // ULA frame interrupt
	kIntSrc_Uart0Tx = 12,
	kIntSrc_Uart1Tx = 13,
	kIntSrc_Count = 14,
};

// --- NextReg-write sources --------------------------------------------------

enum NextRegSource : uint8_t
{
	kNRSource_CpuNextregOpcode = 0,
	kNRSource_Port253B = 1,
	kNRSource_Copper = 2, // reserved; not yet emitted
};

// --- v4 video state ---------------------------------------------------------
// The Next display registers whose values decide what the screen looks like.
// This is MemoryBus::IsDisplayReg's set plus $05 (50/60 Hz) and $11 (video
// timing), which fix the frame geometry.
//
// The order is fixed and append-only: Header::InitDisplayRegs is indexed by
// POSITION in this array, not by NextReg number, because the set is sparse.
constexpr uint8_t kDisplayRegOrder[] = {
	0x05,
	0x11,
	0x12,
	0x13,
	0x14,
	0x15,
	0x16,
	0x17,
	0x18,
	0x19,
	0x1A,
	0x1B,
	0x1C,
	0x32,
	0x33,
	0x40,
	0x41,
	0x42,
	0x43,
	0x44,
	0x4A,
	0x4B,
	0x4C,
	0x68,
	0x69,
	0x6A,
	0x6B,
	0x6E,
	0x6F,
	0x70,
	0x71,
};
constexpr size_t kDisplayRegCount = sizeof(kDisplayRegOrder);

// Position of a NextReg in kDisplayRegOrder, or -1 when it does not affect the
// display. Used both to filter what gets recorded and to index the header.
inline int DisplayRegSlot(uint8_t Idx)
{
	for (size_t IdxI = 0; IdxI < kDisplayRegCount; ++IdxI)
	{
		if (kDisplayRegOrder[IdxI] == Idx)
		{
			return int(IdxI);
		}
	}
	return -1;
}

// I/O ports that change what is displayed, recorded as kRecord_PortWrite.
// Matching is on the whole 16-bit port for the specific ones and on the decode
// the hardware actually uses for the rest ($FE and $FF decode on one bit / the
// low byte; $7FFD on the 128K pattern; the Evo ATM select on the low byte).
enum VideoPort : uint16_t
{
	kVideoPort_UlaFE = 0x00FE, // border
	kVideoPort_Scld = 0x00FF, // Timex / ZX-Uno SCLD mode
	kVideoPort_7FFD = 0x7FFD, // bit 3 = displayed screen
	kVideoPort_EvoFF77 = 0x0077, // ZX Evo ATM video-mode select ($xx77)
	kVideoPort_UnoSel = 0xFC3B, // ZX-Uno register select
	kVideoPort_UnoData = 0xFD3B, // ZX-Uno register data (Radastan mode)
};

// --- v4 record payloads -----------------------------------------------------
// kRecord_NextRegWrite:  tag, varint cycle_delta, u8 reg, u8 value, u8 source
//     `source` is a NextRegSource. The COPPER source is the important one: a
//     copper list is what changes video mode mid-frame, and its writes never
//     pass through the CPU's OUT path.
// kRecord_PortWrite:     tag, varint cycle_delta, u16 port, u8 value
//     `port` is the full 16-bit port as presented on the bus, normalised to the
//     VideoPort value the write matched.
// Neither record carries register state; both are zero-width events at a cycle.

// --- .btr header ------------------------------------------------------------

#pragma pack(push, 1)
struct Header
{
	uint32_t m_Magic;
	uint16_t m_Version;
	uint16_t m_Flags;
	uint64_t m_StartCycle;
	uint64_t m_EndCycle; // sentinel if not finalised
	uint64_t m_StartFrame;
	uint64_t m_EndFrame; // sentinel if not finalised
	uint64_t m_StartTimeNanos; // emulated wall-clock at StartCycle
	uint64_t m_EndTimeNanos; // sentinel if not finalised
	uint64_t m_TotalRecordsHint; // sentinel if not finalised
	uint64_t m_BaseMhzX1e6; // CPU clock at trace start
	uint32_t m_FrameIntPeriod; // nominal T-states per frame (0 = unknown / no interrupts)

	uint16_t m_InitPC, m_InitSP, m_InitAF, m_InitBC, m_InitDE, m_InitHL, m_InitIX, m_InitIY;
	uint16_t m_InitAFp, m_InitBCp, m_InitDEp, m_InitHLp;
	uint16_t m_InitMemPtr;
	uint8_t m_InitI, m_InitR, m_InitIFF1, m_InitIFF2, m_InitIM;

	uint8_t m_InitNR5057[8];
	uint8_t m_Machine;
	uint8_t m_TrdosActive;
	uint16_t m_Reserved;

	// --- v4 initial video state -------------------------------------------
	// Where the display started, so a consumer replaying the change records
	// begins from the real state instead of assuming zero.
	uint8_t m_InitBorder; // ULA $FE bits 2:0
	uint8_t m_InitPort7FFD; // 128K paging; bit 3 selects the shown screen
	uint8_t m_InitScldMode; // Timex / ZX-Uno port $FF
	uint8_t m_InitEvoFf77; // ZX Evo ATM video-mode select ($xx77)
	// Next display registers, indexed by kDisplayRegOrder below (NOT by raw
	// NextReg number - the set is sparse).
	uint8_t m_InitDisplayRegs[kDisplayRegCount];
};
#pragma pack(pop)

constexpr size_t kFixedHeaderSize = sizeof(Header);
constexpr size_t kStartCycleOff = offsetof(Header, m_StartCycle);
constexpr size_t kEndCycleOff = offsetof(Header, m_EndCycle);
constexpr size_t kEndFrameOff = offsetof(Header, m_EndFrame);
constexpr size_t kEndTimeOff = offsetof(Header, m_EndTimeNanos);
constexpr size_t kTotalRecOff = offsetof(Header, m_TotalRecordsHint);

// --- .btr.fidx layout -------------------------------------------------------

#pragma pack(push, 1)
struct IndexHeader
{
	uint32_t m_Magic; // = kIndexMagic
	uint16_t m_Version; // = kVersion
	uint16_t m_Reserved;
	uint32_t m_ParentMagic; // = kMagic
	uint32_t m_ParentHeaderCrc; // CRC32 of the parent .btr header (matched on load)
	uint64_t m_StartFrame;
	uint64_t m_EndFrame;
	uint32_t m_EntryCount;
	uint32_t m_Reserved2;
};

struct IndexEntry
{
	uint32_t m_FrameNumber;
	uint32_t m_Pad;
	uint64_t m_StartCycle;
	uint64_t m_StartTimeNanos;
	uint64_t m_FirstByteOffset;
	uint16_t PC, AF, BC, DE, HL, IX, IY, SP;
};
#pragma pack(pop)

static_assert(sizeof(IndexEntry) == 48, "IndexEntry size is locked at 48 bytes");

// --- CRC32 helper (zlib-compatible polynomial 0xEDB88320) -------------------

uint32_t Crc32(const void* Data, size_t Length, uint32_t Seed = 0);

} // namespace BinaryTrace
