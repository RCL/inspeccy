#pragma once
#include <array>
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <vector>

#include "copper.h"

#if INSPECCY_TRACE_SUPPORT
#include <trace/trace_binary.h>
struct TraceWriter;
#endif

// 8 KB slot model. 64 KB address space = 8 slots. Native granularity for ZX Next
// even though we only model 128K paging here.
constexpr int kSlotSize = 0x2000;
constexpr int kSlotCount = 8;
// Storage capacity: 2 MB (256 8K pages); --ram-mb selects 1 or 2 MB,
// defaulting to 2. The first 128 KB backs the classic 128K layout.
// Remaining pages are accessible through the Next MMU. Mappings beyond
// installed RAM read $FF and ignore writes instead of aliasing lower pages.
constexpr int kRamPageCount = 256;
constexpr int kRamPageCount1mb = 128;
constexpr int kRamPageCount2mb = 256;
constexpr int kRomPageCount = 17; // 3 base + Timex Home(16K) + EXROM(8K) + 4x +3 ROMs (16K each)

// ROM page indices inside the ROM pool.
constexpr int kRomPage128kEditor = 0; // pages 0,1
constexpr int kRomPage48kBasic = 2; // pages 2,3
constexpr int kRomPageTrdos = 4; // pages 4,5
constexpr int kRomPageTimexHome = 6; // pages 6,7 (16K Home ROM; tc2048 = Spectrum-compat)
constexpr int kRomPageTimexEx = 8; // page 8   (8K EXROM; 2068 only)
constexpr int kRomPagePlus30 = 9; // pages 9..16: the +2A/+3 four 16K ROMs
								  // (ROM r = pages 9+2r, 9+2r+1): 0=128 editor,
								  // 1=128 syntax, 2=+3DOS, 3=48 BASIC

struct MemSlot
{
	uint8_t* m_Ptr = nullptr; // reads and opcode fetches come from here
	uint8_t* m_WrPtr = nullptr; // writes land here when bWritable. Equal to Ptr
								// except under the Layer 2 paging window ($123B),
								// which can send writes to Layer 2 RAM while reads
								// still see the ROM / MMU page.
	bool m_bWritable = false;
};

struct MemoryBus;
struct SpritesState;

// Debug-only description; unlike trace page tags, this never aliases open bus
// or unknown backing to RAM page zero. Offset is within the physical 8K page.
struct DebugMapping
{
	enum Kind { Ram, Rom, OpenBus, Other, Unavailable } m_Kind = Unavailable;
	int m_Page = -1;
	uint16_t m_Offset = 0;
	bool m_Writable = false;
};

// ---- NextReg dispatch table -------------------------------------------------
// Every NextReg write lands in NextRegs[] through ONE funnel (WriteNextReg). A
// register that needs a side effect registers a write handler here; a register
// whose read-back is computed (identity, raster line) registers a read handler;
// the two classifications the compositor and the beam drawer need are flags in
// the same entry. Adding a register is therefore one table entry in the module
// that owns it.
enum NextRegFlag : uint8_t
{
	kNrDisplay = 0x01, // a compositor input: the eager drawer flushes before the write lands
	kNrL2Hint = 0x02, // writing it hints that Layer 2 is in use (bAnyL2Touched heuristic)
};
using NextRegWriteFn = void (*)(MemoryBus& Mem, uint8_t Index, uint8_t Value, uint64_t WriterCycle);
using NextRegReadFn = uint8_t (*)(const MemoryBus& Mem, uint8_t Index);
struct NextRegHook
{
	NextRegWriteFn m_Write = nullptr;
	NextRegReadFn m_Read = nullptr;
	uint8_t m_Flags = 0;
};

struct MemoryBus final
{
  public:
	MemoryBus();
	DebugMapping InspectMapping(uint16_t Addr, bool Write = false) const;
	bool DebugPeek(uint16_t Addr, uint8_t& Value) const;

#if INSPECCY_TRACE_SUPPORT
	// PageTag is the physical page the write landed in (BinaryTrace page-tag
	// encoding), resolved here where the slot mapping is already known.
	using WriteHook = void (*)(void* User, uint16_t Addr, uint8_t Value, uint8_t PageTag);
	WriteHook m_OnWrite = nullptr;
	void* m_OnWriteUser = nullptr;
#endif

	// Byte-level access coverage (--coverage-out). Optional; points at a
	// caller-owned PHYSICAL byte-map that parallels the RAM array: index =
	//   (NextRegs[$50 + (Addr>>13)] & (RAM_PAGE_COUNT-1)) * SLOT_SIZE
	//     + (Addr & 0x1FFF)
	// i.e. keyed by the live MMU 8K page (same resolution SetRamSlot/--watch-phys
	// use), so bank paging can't alias: a byte's coverage tracks its physical
	// page across bank swaps. Size must be RAM_PAGE_COUNT * SLOT_SIZE (2 MiB).
	// Rd() ORs in bit0 for any read (incl. opcode/operand/displacement fetch,
	// which go through Rd); Fetch marks bit1 (executed as instruction) via
	// MarkExec. A byte left at 0 across a full run was neither executed nor read.
	// Null (default) => the only cost on the hot path is one predictable
	// not-taken branch; the page resolution runs solely when enabled.
	// Classic 128K paging participates: WritePort7FFD mirrors the selected
	// bank into NR_56/57 (and MapRamPageToSlot mirrors into NR_5x), so
	// $C000 traffic is attributed to the bank actually mapped at the time.
	// (Reads from a ROM-mapped slot key by whatever page NextRegs[$50+slot]
	// last held - harmless for RAM-payload analysis, which lives in other slots.)
	uint8_t* m_Coverage = nullptr;
	size_t m_CoverageSize = 0; // bytes in the map (for the one-shot reset)
	// One-shot reset (--coverage-reset-at-pc): when execution first fetches an
	// opcode at this address, zero the WHOLE map before marking it. Use it to
	// discard everything the depacker did (its reads, and - for LZ formats -
	// the match-copy reads of already-emitted destination bytes) so coverage
	// reflects only the unpacked demo's runtime. -1 = disabled (accumulate from
	// the first instruction, correct for raw-loaded .nex where nothing unpacks).
	int32_t m_CoverageResetPC = -1;

	inline size_t CoveragePhysIndex(uint16_t Addr) const
	{
		const uint8_t Page8K = uint8_t(m_NextRegs[0x50 + (Addr >> 13)] & (kRamPageCount - 1));
		return size_t(Page8K) * kSlotSize + (Addr & 0x1FFF);
	}
	// 8K physical RAM page mapped at Addr's slot (0..RAM_PAGE_COUNT-1). Used by
	// the --until-pc banked breakpoint to disambiguate the same Z80 address in
	// different Next banks.
	inline uint8_t Phys8KPageAt(uint16_t Addr) const
	{
		return uint8_t(m_NextRegs[0x50 + (Addr >> 13)] & (kRamPageCount - 1));
	}
	// Memory-access mode, chosen ONCE per run by the dispatcher's policy:
	//   Full          - every per-access diagnostic branch present (the general path)
	//   Fast          - nothing armed: no coverage / contention / watch / invariant /
	//                   beam / trace hook checks at all (plain run)
	//   FastContended - as Fast but ULA contention IS on: only AccrueContention
	//                   remains (48K / 128K / Timex by default; a contended Next
	//                   if one is ever configured)
	enum Access : uint8_t
	{
		kAccessFull = 0,
		kAccessFast = 1,
		kAccessFastContended = 2
	};

	template <Access kMode = Access::kAccessFull> inline void MarkExec(uint16_t Addr)
	{
		if constexpr (kMode != Access::kAccessFull)
		{
			(void)Addr; // plain run: coverage is not armed
		}
		else
		{
			if (!m_Coverage)
			{
				return;
			}
			if (m_CoverageResetPC == int32_t(Addr)) // fires once, at the post-unpack entry
			{
				for (size_t IdxI = 0; IdxI < m_CoverageSize; ++IdxI)
				{
					m_Coverage[IdxI] = 0;
				}
				m_CoverageResetPC = -1;
			}
			m_Coverage[CoveragePhysIndex(Addr)] |= 0x02;
		}
	}

	// Each Set... call copies 16 KB. Call with nullptr to leave unset.
	void SetRom128(const uint8_t* Data);
	void SetRom48(const uint8_t* Data);
	void SetRomTrd(const uint8_t* Data);
	void SetRomTimexHome(const uint8_t* Data16K); // Home ROM (tc2048/tc2068-0)
	void SetRomTimexEx(const uint8_t* Data8K); // EXROM (tc2068-1); 2068 only

	// Populate one 8K DOCK chunk from a cartridge image. Data8K may be nullptr
	// for a blank RAM chunk. Chunks never set stay unpopulated and keep reading
	// as open bus, exactly like an empty cartridge slot.
	void SetDockChunk(int Chunk, const uint8_t* Data8K, bool bWritable);
	bool HasDock() const;

	bool HasRom128() const
	{
		return m_bRomLoaded[0];
	}
	bool HasRom48() const
	{
		return m_bRomLoaded[1];
	}
	bool HasRomTrd() const
	{
		return m_bRomLoaded[2];
	}

	// Map a 48K Spectrum layout: ROM48 + bank 5 + bank 2 + bank 0. Paging is locked.
	void Map48K();

	// Map a 128K Spectrum layout. bUseTrdosRom forces the TR-DOS ROM into slots 0,1
	// regardless of port 7FFD bit 4.
	void Map128K(uint8_t Port7FFDValue, bool bUseTrdosRom);

	// Map a Timex layout: Home ROM (slots 0,1) + 48K RAM (banks 5,2,0). The SCLD
	// ($FF/$F4) then overlays extra video modes / DOCK banking. Marks the bus as
	// Timex so WriteIoPort decodes the SCLD ports and the renderer uses the SCLD
	// display modes. `b2068` selects the 2068 feature set (EXROM/DOCK/AY present).
	void MapTimex(bool bB2068);

	// Enable Timex SCLD behaviour WITHOUT remapping memory. For the .sna / .z80
	// load path, where the snapshot has already populated the 48K memory map (and
	// main.cpp has mapped a Spectrum-compatible Home ROM via SetRom48/Map48K); we
	// only need to arm the SCLD port decode + renderer and reset the video state.
	// MapTimex() is the ROM-boot path (.tap/.trd) that also installs the ROM pool.
	void EnableTimex(bool bB2068)
	{
		m_bIsTimex = true;
		m_bIsTimex2068 = bB2068;
		m_ScldMode = 0;
		m_ScldBank = 0;
	}

	// 128K paging port. No-op if paging has been locked (bit 5 latched).
	void WritePort7FFD(uint8_t Value);

	// Direct access to RAM by 16K bank number (used by SNA load/save).
	uint8_t* Ram16KBank(int Bank16K)
	{
		return &m_Ram[Bank16K * 2 * kSlotSize];
	}
	const uint8_t* Ram16KBank(int Bank16K) const
	{
		return &m_Ram[Bank16K * 2 * kSlotSize];
	}

	// Direct access to RAM by 8K page index (0..RAM_PAGE_COUNT-1). Used by
	// the L2 BMP encoder to walk the consecutive pages NR_L2_BANK points at,
	// independent of the slot mapping the dot command happens to be running.
	const uint8_t* Ram8KPage(int Page8K) const
	{
		return &m_Ram[Page8K * kSlotSize];
	}

	uint8_t GetPort7FFD() const
	{
		return m_Port7FFD;
	}
	uint8_t GetPort1FFD() const
	{
		return m_Port1FFD;
	}
	// +2A/+3 second paging port ($1FFD): ROM-select high bit + special all-RAM
	// mode. Set at machine setup; drives MapPlus3.
	void SetIsPlus3(bool bB)
	{
		m_bIsPlus3 = bB;
	}
	bool IsPlus3() const
	{
		return m_bIsPlus3;
	}
	bool HasRomPlus3() const
	{
		return m_bRomPlus3Loaded;
	}
	void MapPlus3(uint8_t Port7FFDValue, uint8_t Port1FFDValue);
	void WritePort1FFD(uint8_t Value);
	void SetRomPlus3(int RomIndex, const uint8_t* Data16K);
	uint64_t GetPort7FFDWrites() const
	{
		return m_Port7FFDWrites;
	}
	uint64_t GetVramPixelWritesBank5() const
	{
		return m_VramPixelWritesBank5;
	}
	uint64_t GetVramAttrWritesBank5() const
	{
		return m_VramAttrWritesBank5;
	}
	uint64_t GetVramPixelWritesBank7() const
	{
		return m_VramPixelWritesBank7;
	}
	uint64_t GetVramAttrWritesBank7() const
	{
		return m_VramAttrWritesBank7;
	}
	uint8_t GetBorderColour() const
	{
		return m_BorderColour;
	}
	void SetBorderColour(uint8_t Value)
	{
		m_BorderColour = uint8_t(Value & 0x07);
	}

	// --- ZX Next state (NextRegs + 8 palettes) ---

	// Mark this MemoryBus as backing a Next-family machine. Enables L2 BMP
	// dispatch in DetectActiveLayer(). Set once by main.cpp.
	void SetIsNext(bool bB)
	{
		m_bIsNext = bB;
	}
	bool IsNext() const
	{
		return m_bIsNext;
	}

	// --- Timex SCLD state ---
	// bIsTimex_ gates the SCLD port decodes (WriteIoPort) + the Timex renderer.
	// ScldMode_ = last OUT to port $FF: the video mode/screen bits (mode&4=hi-res,
	// mode&2=8x1 extended colour, else standard with mode&1 selecting the $6000
	// screen) + hi-res colour in bits 3-5. ScldBank_ = last OUT to $F4 (DOCK/EXROM
	// 8K-chunk enable; 2068 only). Confirmed against local Timex demos.
	bool IsTimex() const
	{
		return m_bIsTimex;
	}
	bool IsTimex2068() const
	{
		return m_bIsTimex2068;
	}
	uint8_t GetScldMode() const
	{
		return m_ScldMode;
	}
	// $FF (the SCLD "DEC" register). Bits 0-2 screen mode, 3-5 hi-res colour,
	// bit 6 INTDISABLE, bit 7 ALTMEMBANK (0 = DOCK cartridge, 1 = EXROM). Only
	// bit 7 moves memory, so the remap is skipped unless it actually changed.
	void SetScldMode(uint8_t V)
	{
		const uint8_t Old = m_ScldMode;
		m_ScldMode = V;
		// The Next exposes port $FF bits 5:0 as NR $69 bits 5:0 (nextreg.txt:
		// "Port 0xFF bits 5:0 alias (Timex display modes)"), so an OUT to $FF
		// has to show up there too. Bits 7:6 of NR $69 are the L2-enable and
		// shadow-screen aliases and are none of $FF's business. On a Timex or
		// ZX-Uno the mirror lands in a register file nothing reads (the dump
		// emits NextRegs only for a Next), so it needs no machine test.
		m_NextRegs[0x69] = uint8_t((m_NextRegs[0x69] & 0xC0) | (V & 0x3F));
		if (((Old ^ V) & 0x80) != 0)
		{
			ApplyTimexBanking();
		}
	}
	// True when a Next has an actual Timex display mode selected. Mode 0 (the
	// plain 8x8 ULA screen) renders identically down the classic path, so the
	// renderers only divert when one of the mode bits is really set.
	bool IsNextTimexDisplay() const
	{
		return m_bIsNext && (m_ScldMode & 0x07) != 0;
	}
	uint8_t GetScldBank() const
	{
		return m_ScldBank;
	}
	// $F4 (the SCLD "HSR" register). One bit per 8K chunk: set = take that
	// chunk from the alternate bank selected by $FF bit 7 instead of Home.
	void SetScldBank(uint8_t V)
	{
		m_ScldBank = V;
		ApplyTimexBanking();
	}
	// SCLD INTDISABLE holds the ULA /INT line off. Real SCLD feature on both
	// Timex models, so it is not gated on the 2068 feature set.
	bool IsTimexIntDisabled() const
	{
		return m_bIsTimex && (m_ScldMode & 0x40) != 0;
	}
	// Recompute the 8K slots from ($F4 HSR, $FF bit 7). 2068 only - the
	// TC2048 has no DOCK/EXROM hardware. Defined in memory.cpp.
	void ApplyTimexBanking();

	// --- ZX Evolution BaseConf (MM_ATM3) ---
	// Pentagon-class 128K boot with the BaseConf extensions armed. Like Map128K
	// but sets bIsZxEvo_ so the extra ports decode and the ATM renderer runs.
	void MapZxEvo(uint8_t Port7FFDValue, bool bUseTrdosRom);
	// Arm BaseConf behaviour WITHOUT remapping memory - the .sna/.z80 load path,
	// where the snapshot already populated the 128K map. Only flips the flag +
	// resets the extension state (analogue of EnableTimex).
	void EnableZxEvo()
	{
		m_bIsZxEvo = true;
		m_Eff7 = 0;
		m_EvoFf77 = 0x03;
		m_EvoPalIdx = 0;
		m_CmosAddr = 0;
		for (int W = 0; W < 4; ++W)
		{
			m_EvoWinPage[W] = -1;
		}
	}
	bool IsZxEvo() const
	{
		return m_bIsZxEvo;
	}
	// $EFF7 system config. Latched value drives the video mode + paging lock
	// + turbo (bit 4, inverted: 1 = 3.5 MHz, 0 = 7 MHz).
	uint8_t GetEff7() const
	{
		return m_Eff7;
	}
	void SetEff7(uint8_t V)
	{
		m_Eff7 = V;
		FireEvoTurbo();
	}
	// $xx77 ATM video-mode select (consumed by the renderer) + turbo bit 3
	// (set = 14 MHz, overriding the $EFF7 speed bit).
	uint8_t GetEvoFf77() const
	{
		return m_EvoFf77;
	}
	void SetEvoFf77(uint8_t V)
	{
		m_EvoFf77 = V;
		FireEvoTurbo();
	}

	// Recompute the Evo effective clock from the $EFF7/$FF77 latches and
	// notify the speed-change callback. Defined in memory.cpp.
	void FireEvoTurbo();
	// ATM hardware palette (16 GRB entries). WriteAtmPal appends via the ATM
	// palette port's auto-advancing index; the renderer reads the array.
	const uint8_t* GetEvoPalette() const
	{
		return m_EvoPal;
	}
	void WriteAtmPal(uint8_t Value);
	// ATM/PentEvo 4MB page-window ports. Port encodes the window (0..3) + source.
	void WriteEvoWindow(uint16_t Port, uint8_t Value);
	// Re-apply any active window overrides on top of the base 128K mapping.
	// Called after MapZxEvo / $7FFD writes so a window RAM override survives a
	// bank switch.
	void ApplyEvoWindows();
	// mc146818 RTC over $DFF7 (addr) / $BFF7 (data). Verification stub.
	void SetCmosAddr(uint8_t V)
	{
		m_CmosAddr = uint8_t(V & 0x3F);
	}
	uint8_t GetCmosAddr() const
	{
		return m_CmosAddr;
	}
	uint8_t ReadCmos() const
	{
		return m_Cmos[m_CmosAddr & 0x3F];
	}
	void WriteCmos(uint8_t V)
	{
		m_Cmos[m_CmosAddr & 0x3F] = V;
	}
	// Dump/resume of the page-window overrides (16K page, or -1 = none).
	int16_t GetEvoWinPage(int W) const
	{
		return m_EvoWinPage[W & 3];
	}
	void SetEvoWinPage(int W, int16_t Page)
	{
		m_EvoWinPage[W & 3] = Page;
	}

	// --- ZX-Uno ---
	// Arm ZX-Uno behaviour on the 128K base: the $FC3B/$FD3B register file, the
	// Timex-style SCLD ($FF) hicolour path, and CoreID detection.
	void EnableZxUno()
	{
		m_bIsZxUno = true;
		m_UnoRegSel = 0;
		m_UnoScanDbl = 0;
		m_UnoCoreIdIdx = 0;
		m_UnoRadasCtrl = 0;
		m_UnoRadasOffset = 0;
		m_UnoRadasPadding = 0;
		m_UnoRadasPalBank = 0;
		m_UnoRadasOffPhase = 0;
		m_UlaPlusReg = 0;
		m_bUlaPlusMode = false;
	}
	bool IsZxUno() const
	{
		return m_bIsZxUno;
	}
	// The ZX-Uno and the ZX Spectrum Next both honour the Timex SCLD $FF port;
	// share the decode + renderer with the real Timex machines. On the Next the
	// WRITE is always decoded (NR $69 bits 5:0 alias it) but the READ is gated
	// on NR $08 bit 2 - see the $FF cases in z80.cpp.
	bool IsScldCapable() const
	{
		return m_bIsTimex || m_bIsZxUno || m_bIsNext;
	}
	// $FC3B register select ($FD3B is the data window). Selecting resets the
	// CoreID read cursor so a fresh read of reg $FF restarts the string.
	void SetUnoRegSel(uint8_t V)
	{
		m_UnoRegSel = V;
		m_UnoCoreIdIdx = 0;
		m_UnoRadasOffPhase = 0;
	}
	uint8_t GetUnoRegSel() const
	{
		return m_UnoRegSel;
	}
	void WriteUnoData(uint8_t V); // write $FD3B: dispatch by selected reg
	uint8_t ReadUnoData(); // read  $FD3B: CoreID string / reg value
	// Dump/resume of the turbo/SCANDBL register (reg $0B).
	uint8_t GetUnoScanDbl() const
	{
		return m_UnoScanDbl;
	}
	void SetUnoScanDbl(uint8_t V)
	{
		m_UnoScanDbl = V;
	}

	// Radastan (128x96 4bpp). Enabled when RADASCTRL bits 1:0 are both set.
	bool IsRadasEnabled() const
	{
		return (m_UnoRadasCtrl & 0x03) == 0x03;
	}
	uint8_t GetUnoRadasCtrl() const
	{
		return m_UnoRadasCtrl;
	}
	uint16_t GetUnoRadasOffset() const
	{
		return m_UnoRadasOffset;
	}
	uint8_t GetUnoRadasPadding() const
	{
		return m_UnoRadasPadding;
	}
	uint8_t GetUnoRadasPalBank() const
	{
		return m_UnoRadasPalBank;
	}
	// Dump/resume setters for the Radastan register state.
	void SetUnoRadasCtrl(uint8_t V)
	{
		m_UnoRadasCtrl = uint8_t(V & 0x03);
	}
	void SetUnoRadasOffset(uint16_t V)
	{
		m_UnoRadasOffset = uint16_t(V & 0x3FFF);
	}
	void SetUnoRadasPadding(uint8_t V)
	{
		m_UnoRadasPadding = V;
	}
	void SetUnoRadasPalBank(uint8_t V)
	{
		m_UnoRadasPalBank = uint8_t(V & 0x03);
	}
	// ULAplus ($BF3B select / $FF3B data). CRAM is the 64-entry GRB 3-3-2 palette.
	void SetUlaPlusReg(uint8_t V)
	{
		m_UlaPlusReg = V;
	}
	uint8_t GetUlaPlusReg() const
	{
		return m_UlaPlusReg;
	}
	void WriteUlaPlusData(uint8_t V);
	const uint8_t* GetUlaPlusCram() const
	{
		return m_UlaPlusCram;
	}
	bool IsUlaPlusMode() const
	{
		return m_bUlaPlusMode;
	}
	// Dump/resume of the ULAplus palette + mode.
	void SetUlaPlusCramEntry(int Idx, uint8_t V)
	{
		m_UlaPlusCram[Idx & 0x3F] = V;
	}
	void SetUlaPlusMode(bool bB)
	{
		m_bUlaPlusMode = bB;
	}

	// Full Next register file (256 indices). Side effects (MMU paging, palette
	// routing, L2 trigger) handled by WriteNextReg below.
	void SetSpriteIndexFromPort(uint8_t Value)
	{
		if (m_NextRegs[0x09] & 0x10)
		{
			m_NextRegs[0x34] = Value & 0x7F;
		}
	}
	uint8_t GetNextReg(uint8_t Index) const
	{
		return m_NextRegs[Index];
	}
	// Port-$253B read: computed registers through their read hook, else the
	// stored value; counted in NextRegReadHist (compatibility sweeps).
	uint8_t ReadNextReg(uint8_t Index);
	// Table registration (modules call these once at setup).
	void RegisterNextRegWrite(uint8_t Index, NextRegWriteFn Fn, uint8_t Flags = 0)
	{
		m_NextRegHooks[Index].m_Write = Fn;
		m_NextRegHooks[Index].m_Flags |= Flags;
	}
	void RegisterNextRegRead(uint8_t Index, NextRegReadFn Fn)
	{
		m_NextRegHooks[Index].m_Read = Fn;
	}
	void SetNextRegFlags(uint8_t Index, uint8_t Flags)
	{
		m_NextRegHooks[Index].m_Flags |= Flags;
	}
	// Live CPU clock (Z80State::Cycles / NextFrameIntCycle / FrameIntPeriod) for
	// registers whose read-back is a function of time (NR $1E/$1F raster line).
	void SetCpuClockPtrs(const uint64_t* Cycles, const uint64_t* NextFrameInt, const uint64_t* FrameIntPeriod)
	{
		m_ClkCycles = Cycles;
		m_ClkNextFrameInt = NextFrameInt;
		m_ClkFrameIntPeriod = FrameIntPeriod;
	}
	// Frame-exact replay (inspeccy/src/replay.*): become a full copy of Src with
	// the slot pointers rebased into this object's own storage and every
	// external tap (hooks, callbacks, coverage, watches, traces, clock
	// pointers) severed. The caller rebinds what the replay needs.
	void AdoptForReplay(const MemoryBus& Src);

	// The base slot table (the mapping the MMU / ROM / model logic set up,
	// before the Layer 2 window is laid over it) as backing store + 8K page,
	// for dump/resume. Re-deriving it from NR $50-$57 on resume is wrong: $FF
	// in slots 0/1 means ROM, a $7FFD write moves slots 6/7, and a Timex dock
	// or out-of-range page maps open bus.
	enum class SlotStore : uint8_t
	{
		kRam,
		kRom,
		kOpenBus,
		kDock,
	};
	struct SlotDesc
	{
		SlotStore m_Store = SlotStore::kOpenBus;
		uint16_t m_Page = 0;
		bool m_bWritable = false;
	};
	SlotDesc DescribeBaseSlot(int SlotIndex) const;
	// The paging latches exactly as dumped, after the Map* call that rebuilt
	// the mapping from them: Map128K masks the bits the Evo keeps, and the
	// Evo's lock needs #EFF7 bit 2 as well as $7FFD bit 5.
	void RestorePagingLatches(uint8_t Port7FFD, uint8_t Port1FFD, bool bLocked)
	{
		m_Port7FFD = Port7FFD;
		m_Port1FFD = Port1FFD;
		m_bPagingLocked = bLocked;
	}

	// Bus state that has no register of its own to restore it through, for
	// dump/resume: the Layer 2 port and window, the clip windows and their
	// write cursors, the Timex mode on the Next, the CPU speed, the Evo / Uno
	// latches and memories, the dock contents and the bus's own statistics.
	struct ResumeExtras
	{
		uint8_t m_Port123B = 0;
		uint8_t m_Layer2Offset = 0;
		uint64_t m_Layer2WindowOpens = 0;
		bool m_bL2StateExplicit = false;
		uint8_t m_ClipWindow[4][4] = {};
		uint8_t m_ClipIndex[4] = {};
		bool m_bClipSet[4] = {};
		uint8_t m_ScldMode = 0;
		uint8_t m_NextCpuSpeed = 0;
		uint8_t m_EvoPal[16] = {};
		uint8_t m_EvoPalIdx = 0;
		uint8_t m_Cmos[64] = {};
		uint8_t m_UnoCoreIdIdx = 0;
		uint8_t m_UnoRadasOffPhase = 0;
		uint8_t m_UlaPlusReg = 0;
		std::vector<uint8_t> m_DockRam;
		std::array<uint8_t, kSlotCount> m_DockKind{};
		uint64_t m_NextRegWriteHist[256] = {};
		uint64_t m_NextRegReadHist[256] = {};
		std::map<uint16_t, uint64_t> m_UnhandledPortReads;
		std::map<uint16_t, uint64_t> m_UnhandledPortWrites;
		uint64_t m_FrameIntPhase = 0; // as last sampled (where the CPU took the interrupt)
		uint64_t m_MmuWriteCycle[8] = {}; // the last NR $50-$57 write per slot
		uint16_t m_MmuWritePc[8] = {};
		uint8_t m_MmuWriteOld[8] = {};
	};
	void ExportResumeExtras(ResumeExtras& Out) const;
	// Raw: no side effects run. Slots are re-applied by ApplyRestoredSlots.
	void ImportResumeExtras(const ResumeExtras& In);
	// False when the page lies outside its store; the slot is left unchanged.
	bool RestoreBaseSlot(int SlotIndex, const SlotDesc& Desc);
	// m_Slots from the restored base table, Layer 2 window applied.
	void ApplyRestoredSlots();
	void RebindContentionClock(const uint64_t* NextFrameInt, const uint64_t* Period)
	{
		m_ContNextPtr = NextFrameInt;
		m_ContPeriodPtr = Period;
	}

	// ---- Classic floating bus (48K / 128K) -------------------------------
	// Enable + geometry: DisplayFetchStart = the in-frame cycle of the first
	// paper byte fetch (= the contention model's "first contended" cycle);
	// TperLine and the 192x256 paper area follow the ULA. Off => reads of an
	// unclaimed port keep returning $FF.
	void ConfigureFloatingBus(bool bOn, uint32_t DisplayFetchStart, uint32_t TperLine)
	{
		m_bFloatingBus = bOn;
		m_FbFetchStart = DisplayFetchStart;
		m_FbTperLine = TperLine;
	}
	bool FloatingBusOn() const
	{
		return m_bFloatingBus;
	}
	uint8_t FloatingBusByte() const;
	// WriterPC / WriterCycle are recorded into LastMmuWrite[] when Index is
	// NR_50..NR_57. Pass 0 for both if you don't care (e.g. boot-time init).
	// Source is a BinaryTrace::NextRegSource - it reaches the .btr so a
	// consumer can tell a copper MOVE from a CPU write. Defaulted so the
	// dozens of boot-time callers stay untouched.
	void WriteNextReg(
		uint8_t Index, uint8_t Value, uint16_t WriterPC = 0, uint64_t WriterCycle = 0, uint8_t Source = 0);

#if INSPECCY_TRACE_SUPPORT
	// Binary-trace hook for display-register writes (.btr v4). This lives on
	// MemoryBus rather than at the CPU's OUT sites because the COPPER writes
	// NextRegs through here and never touches the OUT path - and the copper is
	// exactly what changes video mode mid-frame.
	void SetVideoTrace(TraceWriter* T)
	{
		m_VideoTrace = T;
	}
#endif

	// Port $243B select latch (set by port write, consumed by next $253B write).
	uint8_t GetLastSelectedNextReg() const
	{
		return m_LastSelectedNextReg;
	}
	void SetLastSelectedNextReg(uint8_t Idx)
	{
		m_LastSelectedNextReg = Idx;
	}

	// Total NextReg writes - for inspection counters in the status JSON.
	uint64_t GetNextRegWrites() const
	{
		return m_NextRegWrites;
	}

	// Raw NextReg snapshot used by the dump emitter. Returns a pointer to the
	// 256-entry register file. Read-only; mutations must go through
	// WriteNextReg to keep side effects in sync.
	const uint8_t* GetNextRegsArray() const
	{
		return m_NextRegs;
	}
	uint8_t GetPaletteWriteParity() const
	{
		return m_PaletteWriteParity;
	}
	bool GetAnyL2Touched() const
	{
		return m_bAnyL2Touched;
	}

	// Layer 2 clip window (NR_18), as {X1, X2, Y1, Y2}. The compositor uses
	// this to drop L2 pixels outside the window so lower layers (ULA / border)
	// show through - matching hardware. Default is the 256x192 reset window,
	// which clips 320/640 modes to 192 rows unless the program widens it.
	const uint8_t* GetL2ClipWindow() const
	{
		return m_ClipWindow[0];
	}
	// Clip window for layer L (0=L2,1=sprites,2=ULA,3=tilemap): [X1,X2,Y1,Y2].
	const uint8_t* GetClipWindow(int L) const
	{
		return m_ClipWindow[L & 3];
	}
	bool IsClipSet(int L) const
	{
		return m_bClipSet[L & 3];
	}

	// Dump-load side: restore Next state without re-triggering the side
	// effects in WriteNextReg (paging/L2 flag already reflected by RAM
	// contents + bAnyL2Touched scalar). Caller is expected to call this
	// AFTER LoadMemoryFromDump (which sets bIsNext via SetIsNext).
	void RestoreNextRegsFromDump(const uint8_t* Regs256, uint8_t LastSelected, uint8_t PalWriteParity, bool bL2Touched,
		uint64_t AllocWord0, uint64_t AllocWord1, uint64_t AllocWord2, uint64_t AllocWord3, uint64_t NextRegWriteCount,
		uint64_t PaletteWriteCount)
	{
		for (int IdxI = 0; IdxI < 256; ++IdxI)
		{
			m_NextRegs[IdxI] = Regs256[IdxI];
		}
		m_LastSelectedNextReg = LastSelected;
		m_PaletteWriteParity = PalWriteParity;
		m_bAnyL2Touched = bL2Touched;
		m_AllocatedPages8KWord[0] = AllocWord0;
		m_AllocatedPages8KWord[1] = AllocWord1;
		m_AllocatedPages8KWord[2] = AllocWord2;
		m_AllocatedPages8KWord[3] = AllocWord3;
		m_NextRegWrites = NextRegWriteCount;
		m_PaletteWrites = PaletteWriteCount;
		// MMU8K slot-to-page mapping is HIDDEN state that lives outside
		// NextRegs[$50..$57] - those registers report the LATEST page
		// index written, but the actual paging engine state lives in
		// SetRamSlot's tables. Re-apply each NR_50..NR_57 write so the
		// dump's mapping is in effect after restore. Without this, the
		// demo's code/data/vector tables that live in NextRAM banks
		// become unreachable after resume.
		for (int Slot = 0; Slot < 8; ++Slot)
		{
			const uint8_t RamPage8K = uint8_t(m_NextRegs[0x50 + Slot] & (kRamPageCount - 1));
			SetRamSlot(Slot, RamPage8K);
		}
	}

	// Per-palette pointer for dump emit. Each palette is 256 entries of 2 bytes.
	uint8_t* MutablePalette(int PalIdx)
	{
		return &m_Palettes[PalIdx & 0x07][0][0];
	}

	// CPU-speed change callback. main.cpp registers this so it can rescale
	// the cycle Budget and FrameIntPeriod when the running program flips the
	// machine's speed switch. Fired with the new TARGET clock in MHz by:
	//   - Next  nextreg $07 (values 0..3 = 3.5 / 7 / 14 / 28)
	//   - Uno   reg $0B bits 7:6 (00/01/10/11 = 3.5 / 7 / 14 / 28)
	//   - Evo   #FF77 bit 3 = 14 MHz, else #EFF7 bit 4: 1 = 3.5, 0 = 7
	//           (verified against UnrealSpeccy's ATM3 set_turbo decode)
	using TurboChangeCallback = void (*)(void* User, double NewMhz);
	TurboChangeCallback m_OnTurboChange = nullptr;
	void* m_OnTurboChangeUser = nullptr;
	// Device -> CPU deadline notification. A device register write that arms an
	// event, or moves one EARLIER than the dispatcher's cached event horizon,
	// reports the new absolute cycle here; the CPU side pulls Cpu.NextEventCycle
	// in so the top-of-loop event check sees it on time (the full recompute
	// happens there as usual). Devices: CTC (port write), copper (start), the
	// line interrupt and tape edge sources to come. The bus never sees Z80State.
	using DeadlineCallback = void (*)(void* User, uint64_t Cycle);
	DeadlineCallback m_OnDeadline = nullptr;
	void* m_OnDeadlineUser = nullptr;
	void NotifyDeadline(uint64_t Cycle)
	{
		if (m_OnDeadline != nullptr && Cycle != UINT64_MAX)
		{
			m_OnDeadline(m_OnDeadlineUser, Cycle);
		}
	}

	// NR $22/$23 line-interrupt programming -> CPU. Carries the decoded fields
	// ($22 bit 1 enable, $22 bit 0 | $23 = line 0..511, $22 bit 2 = ULA mask);
	// the CPU side stores them on Z80State and re-arms NextLineIntCycle via
	// CycleOfRasterLine. Same shape as OnTurboChange: the bus never sees Z80State.
	using LineIntCallback = void (*)(void* User, bool bEnabled, uint32_t Line, bool bUlaMasked);
	LineIntCallback m_OnLineIntChange = nullptr;
	void* m_OnLineIntChangeUser = nullptr;

	// Absolute cycle (>= Now) at which video line `Line` next begins, from the
	// frame-interrupt phase - the inverse of NrReadRasterLine. Used by the CPU's
	// OnLineIntChange handler to arm the line-interrupt deadline. UINT64_MAX when
	// the frame clock is disabled (Period 0 / NextFrameInt MAX).
	uint64_t CycleOfRasterLine(uint32_t Line, uint64_t Now, uint64_t NextFrameInt, uint64_t Period) const
	{
		if (Period == 0 || NextFrameInt == UINT64_MAX)
		{
			return UINT64_MAX;
		}
		const uint64_t Lines = m_Copper.m_LinesPerFrame ? m_Copper.m_LinesPerFrame : 312;
		const uint64_t CyclesPerLine = Period / Lines;
		const uint64_t Origin = NextFrameInt - Period; // frame top
		const uint64_t Raw = (uint64_t(Line) + (m_Copper.m_TopBorderLines % Lines)) % Lines;
		uint64_t C = Origin + Raw * CyclesPerLine;
		while (C < Now)
		{
			C += Period; // occurrence at or after Now (line may have passed)
		}
		return C;
	}

	// Physical RAM size in MB. Default = 1 MB (matches the stock Spectrum
	// Next; 2 MB is also factory-configurable). Pages outside the effective
	// range route to OpenBusPage on every read/write.
	void SetRamSizeMb(int Mb)
	{
		m_EffectiveRamPages = (Mb >= 2) ? kRamPageCount2mb : kRamPageCount1mb;
	}
	int GetEffectiveRamPages() const
	{
		return m_EffectiveRamPages;
	}

	// Physical-address write watchpoint. Set by --watch-phys <bank>:<offset>.
	// Fires the callback every time a write resolves to (WatchPage8K * 8K) +
	// WatchOffsetInPage. Stored in 8K-page units so the check is one compare
	// per Wr (the slot mapping already gives us the resolved page index).
	// -1 disables. Both halves of a watched bank: set up two watchpoints.
	int m_WatchPage8K = -1;
	int m_WatchOffsetInPage = -1;
	using PhysicalWatchCallback = void (*)(void* User, int Page8K, int OffsetInPage, uint8_t NewVal, uint8_t OldVal);
	PhysicalWatchCallback m_OnPhysicalWatch = nullptr;
	void* m_OnPhysicalWatchUser = nullptr;

	// Invariants (`readonly` rules). The struct lives in main, we just hold
	// a borrowed pointer to the ranges vector + a callback for violations.
	// Linear scan per Wr is fine for the expected rule counts (1..a few).
	struct InvariantRange8K
	{
		// Inclusive both ends (parser normalised). Page-major byte index.
		int m_Page8KStart;
		uint16_t m_OffsetStart;
		int m_Page8KEnd;
		uint16_t m_OffsetEnd;
	};
	const std::vector<InvariantRange8K>* m_InvariantReadOnly = nullptr;
	using InvariantWriteCallback = void (*)(
		void* User, int Page8K, uint16_t OffsetInPage, uint8_t NewVal, uint8_t OldVal, int SlotIdx);
	InvariantWriteCallback m_OnInvariantWrite = nullptr;
	void* m_OnInvariantWriteUser = nullptr;

	// MMU8K last-write attribution for NR_50..NR_57. Updated unconditionally
	// by WriteNextReg when Index is in $50..$57 - cost is 3 store ops per
	// NR_5X write, and NR_5X writes happen ~handful per frame. Used by the
	// invariants engine's violation event to point at the instruction that
	// set up the wrong mapping (vs. the innocent instruction that wrote into
	// a now-misrouted slot).
	struct MmuLastWrite
	{
		uint64_t m_Cycle = 0;
		uint16_t m_Pc = 0;
		uint8_t m_OldValue = 0;
	};
	MmuLastWrite m_LastMmuWrite[8] = {};

	// IM 2 hardware vector routing (NR_C0). Bit 0 = use hardware-driven
	// vector when accepting an IM 2 interrupt; bits 7:5 = high 3 bits of
	// the vector table's low-byte address (combined with source_index*2
	// for the low 5 bits to pick the correct entry in a 32-byte table).
	bool IsIm2HwVectorEnabled() const
	{
		return (m_NextRegs[0xC0] & 0x01) != 0;
	}
	uint8_t Im2VectorBaseHi3() const
	{
		return uint8_t(m_NextRegs[0xC0] & 0xE0);
	}
	// ULA interrupt mask (NR_C4 bit 0). When NR_C0 bit 0 is set, the ULA
	// frame interrupt only fires if NR_C4 bit 0 is also set.
	bool IsUlaInterruptEnabled() const
	{
		return (m_NextRegs[0xC4] & 0x01) != 0;
	}
	// Line interrupt mask (NR_C4 bit 1) - the HW-vector-mode enable for the
	// NR $22/$23 line interrupt (source index 0). Independent of $22 bit 1,
	// which is the legacy/global enable the CPU tracks in Cpu.bLineIntEnabled.
	bool IsLineInterruptEnabled() const
	{
		return (m_NextRegs[0xC4] & 0x02) != 0;
	}
	// CTC interrupt masks: NR_C5 bit N masks channel N.
	bool IsCtc0InterruptEnabled() const
	{
		return (m_NextRegs[0xC5] & 0x01) != 0;
	}
	bool IsCtcInterruptEnabled(int N) const
	{
		return (m_NextRegs[0xC5] & (1u << (N & 7))) != 0;
	}
	// UART Rx interrupts (NR_C6): bits 0/1 = UART0 (ESP) Rx available / Rx near
	// full, bits 4/5 = UART1 (Pi). Near full overrides available (zxnext.vhd:
	// im2_int_req). Source index 1 (UART0) and 2 (UART1): above the CTC and ULA.
	bool IsUartRxIntEnabled(int Ch) const
	{
		return (m_NextRegs[0xC6] & (Ch ? 0x30 : 0x03)) != 0;
	}
	bool IsUartRxNearFullMode(int Ch) const
	{
		return (m_NextRegs[0xC6] & (Ch ? 0x20 : 0x02)) != 0;
	}
	// NR_CA (INT status 2): a set bit says the device raised an interrupt; the
	// program writes 1s to clear. Kept apart from NextRegs[] so the funnel's
	// store of the written value does not become the status.
	uint8_t m_IntStatus2 = 0;
	void NoteUartRxInt(int Ch)
	{
		m_IntStatus2 |= uint8_t((IsUartRxNearFullMode(Ch) ? 0x02 : 0x01) << (Ch ? 4 : 0));
	}

	// Z80 CTC (lives on MemoryBus so both port writes and dump emit can
	// reach it without a separate singleton).
	// All 8 CTC channels, channel N on port $(18+N)3B. Timer mode counts the
	// prescaled system clock; COUNTER mode (control bit 6) decrements on the
	// PREVIOUS channel's ZC/TO pulse - the Next wires ZC/TO(N) -> CLK/TRG(N+1)
	// for every channel (wiki.specnext.dev/CTC), which is what makes cascaded
	// dividers work. Each channel keeps a pulse GRID (GridAnchor + k *
	// PeriodCycles); the grid stays live when the IRQ output is off (control
	// bit 7 = 0) - a cascade's source is exactly that silent-metronome shape.
	// A counter channel's fire times land ON its source's grid, so mid-ISR
	// reprogramming keeps phase instead of re-anchoring at the write cycle.
	struct CtcChannel
	{
		uint8_t m_ControlWord = 0; // last full control word written
		uint8_t m_TimeConstant = 0; // 0 means 256 in Z80 CTC convention
		bool m_bAwaitingTC = false; // true after a control word with bit 2 set
		bool m_bTcLoaded = false; // a TC has ever been written
		bool m_bCounterMode = false; // control bit 6 at TC-load time
		bool m_bRunning = false; // TC loaded + not reset: the ZC/TO grid is live
		bool m_bEnabled = false; // bRunning AND IRQ output on (control bit 7)
		uint64_t m_GridAnchor = 0; // grid base (pulses at Anchor + k*Period, k >= 1)
		uint64_t m_PeriodCycles = 0; // effective cycles between ZC/TO pulses
		uint64_t m_NextFireCycle = UINT64_MAX; // next IRQ deadline (UINT64_MAX = none)
		uint64_t m_InterruptsFired = 0;
	};
	CtcChannel& Ctc(int N)
	{
		return m_Ctc[N & 7];
	}
	const CtcChannel& Ctc(int N) const
	{
		return m_Ctc[N & 7];
	}
	CtcChannel& Ctc0()
	{
		return m_Ctc[0];
	}
	const CtcChannel& Ctc0() const
	{
		return m_Ctc[0];
	}

	// Port $(18+N)3B write. Decodes "control word vs time constant" via the
	// channel's bAwaitingTC flag. CurrentCycle anchors the grid: a timer's
	// first fire is CurrentCycle + PeriodCycles (the timer arms on the TC
	// write, matching hardware); a counter's first fire is the TC-th source
	// pulse strictly after the write.
	void WriteCtc(int N, uint8_t Value, uint64_t CurrentCycle);

	// Live down-counter value for `in (c)` reads of a channel port,
	// clamped to 0..255.
	uint8_t ReadCtc(int N, uint64_t CurrentCycle) const;

	// Earliest IRQ deadline across all enabled channels - feeds the
	// dispatcher's event pool and the HALT fast-forward.
	uint64_t CtcNextIrqDeadline() const
	{
		uint64_t Min = UINT64_MAX;
		for (const CtcChannel& C : m_Ctc)
		{
			if (C.m_bEnabled && C.m_NextFireCycle < Min)
			{
				Min = C.m_NextFireCycle;
			}
		}
		return Min;
	}

	// ZX Next Copper (NextRegs $60-$63). Lives on MemoryBus so WriteNextReg
	// can program it directly; the dispatcher reaches it via Copper() to step
	// it on its raster deadline. Present only on Next machines (and not
	// --no-copper): when absent it is simply never armed, so NextFireCycle
	// stays UINT64_MAX and it costs nothing. See copper.h.
	CopperState& Copper()
	{
		return m_Copper;
	}
	const CopperState& Copper() const
	{
		return m_Copper;
	}

	// ZX Next sprite engine (non-owning; created in main.cpp alongside IoContext).
	// Set on Next-family machines so WriteNextReg can route the NR $34-$39/$75-$79
	// sprite-attribute mirror registers to sprites.cpp. Null on classic machines
	// (those registers then no-op, as they should). The port path ($303B/$57/$5B)
	// reaches the same SpritesState directly through IoContext.
	void SetSprites(SpritesState* S)
	{
		m_Sprites = S;
	}
	SpritesState* Sprites() const
	{
		return m_Sprites;
	}

	// --pixel-accurate: anchor the copper's / drawer's frame origin to the ACTUAL
	// ULA interrupt phase (z80.cpp sets FrameIntPhase at each accepted interrupt
	// when this is on). Rendering itself is the eager BeamDrawer, wired in main.
	void SetPixelAccurate(bool bB)
	{
		m_bPixelAccurate = bB;
	}
	bool IsPixelAccurate() const
	{
		return m_bPixelAccurate;
	}
	// FrameIntPeriod hint (mirrors Z80State::FrameIntPeriod) so WriteNextReg can
	// detect frame boundaries for the timeline without reaching into the CPU.
	void SetFrameIntPeriodHint(uint64_t P)
	{
		m_FrameIntPeriodHint = P;
	}
	// Frame-interrupt phase: the T-state within a FrameIntPeriod at which the
	// ULA frame interrupt fires (= raster line 0 of a frame). The copper's list
	// restarts on the frame sync and the display's top border sits above it, so
	// aligning the timeline's frame origin to this phase (rather than to the
	// arithmetic Cycles/P grid) puts the interrupt - and the ISR the CPU runs on
	// waking from HALT - in the top border where DISPLAY_OFFSET_LINES hides it,
	// instead of jittering a visible scanline. Default 0 = arithmetic grid.
	void SetFrameIntPhase(uint64_t Phase)
	{
		m_FrameIntPhase = Phase;
	}
	uint64_t FrameIntPhase() const
	{
		return m_FrameIntPhase;
	}
	// Frame origin (absolute cycle of raster line 0) for the frame containing
	// Cycles, aligned to the interrupt phase. Shared by the timeline and copper
	// so their per-line cycle maths agree exactly.
	uint64_t FrameOriginFor(uint64_t Cycles, uint64_t Period) const
	{
		if (!Period)
		{
			return 0;
		}
		const uint64_t Phase = m_FrameIntPhase % Period;
		return (Cycles >= Phase) ? ((Cycles - Phase) / Period) * Period + Phase : 0;
	}

	// --- Beam boundary hook (eager pixel-exact drawer) ---
	// A "boundary" is a write to state the pixel function reads (a display
	// NextReg, palette RAM, displayed VRAM, border). The eager drawer registers
	// a hook here; the write path calls it with the write's cycle BEFORE mutating
	// state, so the drawer can flush all pixels up to that beam position from the
	// pre-write state, then let the write land. An unset hook disables this
	// drawing callback.
	void SetBeamBoundaryHook(void (*Fn)(void*, uint64_t), void* User)
	{
		m_BeamBoundaryHook = Fn;
		m_BeamBoundaryUser = User;
	}
	bool HasBeamBoundaryHook() const
	{
		return m_BeamBoundaryHook != nullptr;
	}
	void FireBeamBoundary(uint64_t Cycle)
	{
		if (m_BeamBoundaryHook)
		{
			// The drawer only draws forward: whatever the copper writes before
			// this cycle must land first. A HALT fast-forward, a DMA burst or any
			// other jump in Cycles can reach a boundary with the copper still
			// behind (it is caught up lazily); flushing first composed those
			// lines from stale registers and the catch-up MOVEs then landed in
			// the past - frames of a copper-split scroll showing the wrong offset.
			// The copper's own MOVEs come back through here, from whoever is
			// stepping it (the dispatcher or this catch-up): never start a
			// second step inside one, or the inner one replays instructions the
			// outer has applied but not yet advanced past.
			if (!m_Copper.m_bStepping && m_Copper.m_NextFireCycle <= Cycle && m_FrameIntPeriodHint != 0)
			{
				StepCopperTo(m_Copper, *this, Cycle, m_FrameIntPeriodHint);
			}
			m_BeamBoundaryHook(m_BeamBoundaryUser, Cycle);
		}
	}
	// Arm cycle: boundaries before this cycle are ignored (the write path's
	// >= BeamArmCycle_ test). We only save the last frame(s), so this is set a
	// couple of frames before the run's end - every earlier frame then costs one
	// comparison per write and never touches the drawer. Default 0 = always armed
	// (used by --seconds-1 tests / --video-out which emit every frame).
	void SetBeamArmCycle(uint64_t Cycle)
	{
		m_BeamArmCycle = Cycle;
	}
	uint64_t GetFrameIntPeriodHint() const
	{
		return m_FrameIntPeriodHint;
	}
	// True when the drawer is present and this cycle is in an armed (to-be-saved)
	// frame. The run loop uses this at the frame boundary to fire a synthetic
	// boundary that finalises the frame even if it had NO display writes (a still
	// screen) - otherwise the eager drawer, which only draws on write-boundaries,
	// would leave such a frame blank.
	bool IsBeamArmed(uint64_t Cycle) const
	{
		return m_BeamBoundaryHook != nullptr && Cycle >= m_BeamArmCycle;
	}
	// A compositor input (a write is a beam boundary): flag in the dispatch table.
	bool IsDisplayReg(uint8_t Idx) const
	{
		return (m_NextRegHooks[Idx].m_Flags & kNrDisplay) != 0;
	}

	// Live cycle counter (points at Z80State::Cycles) so the memory-write path
	// can stamp a boundary without threading a cycle through every Wr() call.
	// Instruction-granular for now (the anti-boxing contract sharpens it later
	// with a ticked/contended core). Null => memory boundaries disabled.
	void SetCyclePtr(const uint64_t* P)
	{
		m_CyclePtr = P;
	}

	// ---- Coarse contention (see ContentionKind/ContentionWindow in z80.h).
	// Kind: 0 = off, 1 = $4000-$7FFF (48K/Timex), 2 = 128K rule (bank 5 +
	// odd bank at $C000), 3 = +2A/+3, 4 = Next (one wait state per memory
	// read at 28 MHz, nothing else). Requires SetCyclePtr and the CPU's
	// frame-interrupt scheduler fields for the beam-window test. The penalty
	// accrues into a mutable counter during Rd/Wr; the dispatcher drains it
	// into Cpu.Cycles once per instruction (coarse by design).
	void ConfigureContention(int Kind, int32_t Line0, uint32_t TperLine, const uint64_t* NextFrameIntCyclePtr,
		const uint64_t* FrameIntPeriodPtr)
	{
		m_ContKind = Kind;
		m_ContLine0 = Line0;
		m_ContTperLine = TperLine;
		m_ContNextPtr = NextFrameIntCyclePtr;
		m_ContPeriodPtr = FrameIntPeriodPtr;
	}
	bool ContentionOn() const
	{
		return m_ContKind != 0;
	}
	// Next CPU speed, from NR $07 and the initial --mhz. Only the 28 MHz
	// read wait state depends on it (ContentionKind::kContentionKindNext).
	void SetNextCpuSpeed(int Speed)
	{
		m_NextCpuSpeed = uint8_t(Speed & 0x03);
		m_bNext28 = (Speed & 0x03) == 0x03;
	}
	bool IsNext28() const
	{
		return m_bNext28;
	}

	// ---- Unsupported-feature histograms (compatibility sweeps). Counted only
	// on the fall-through paths, so the hot paths are untouched.
	void InitNextRegHooks();
	// Built-in write handlers (memory.cpp); modules outside the bus register
	// their own through RegisterNextRegWrite.
	static void NrTurbo(MemoryBus&, uint8_t, uint8_t, uint64_t);
	static void NrIntStatus2(MemoryBus&, uint8_t, uint8_t, uint64_t);
	static uint8_t NrReadIntStatus2(const MemoryBus&, uint8_t);
	static uint8_t NrReadTurbo(const MemoryBus&, uint8_t);
	static void NrMmu(MemoryBus&, uint8_t, uint8_t, uint64_t);
	static void NrDisplayControl(MemoryBus&, uint8_t, uint8_t, uint64_t); // $69
	static void NrLayer2Bank(MemoryBus&, uint8_t, uint8_t, uint64_t); // $12/$13
	static void NrPaletteIndex(MemoryBus&, uint8_t, uint8_t, uint64_t); // $40
	static void NrPalette8(MemoryBus&, uint8_t, uint8_t, uint64_t); // $41
	static void NrPaletteControl(MemoryBus&, uint8_t, uint8_t, uint64_t); // $43
	static void NrPalette9(MemoryBus&, uint8_t, uint8_t, uint64_t); // $44
	static void NrClip(MemoryBus&, uint8_t, uint8_t, uint64_t); // $18-$1B
	static void NrClipControl(MemoryBus&, uint8_t, uint8_t, uint64_t); // $1C
	static uint8_t NrReadClipIndex(const MemoryBus&, uint8_t); // $1C read-back
	static void NrCopper(MemoryBus&, uint8_t, uint8_t, uint64_t); // $60-$63
	static void NrSpriteSlot(MemoryBus&, uint8_t, uint8_t, uint64_t); // $34 (slot select)
	static void NrSpriteAttr(MemoryBus&, uint8_t, uint8_t, uint64_t); // $35-$39 / $75-$79
	static void NrLineInt(MemoryBus&, uint8_t, uint8_t, uint64_t); // $22/$23 line interrupt
	static uint8_t NrReadIdentity(const MemoryBus&, uint8_t); // $00/$01/$0E
	static uint8_t NrReadRasterLine(const MemoryBus&, uint8_t); // $1E/$1F
	const uint64_t* m_ClkCycles = nullptr;
	const uint64_t* m_ClkNextFrameInt = nullptr;
	const uint64_t* m_ClkFrameIntPeriod = nullptr;

	uint64_t m_NextRegWriteHist[256] = {};
	uint64_t m_NextRegReadHist[256] = {}; // port $253B reads, by selected register                     // every NextReg
										  // write, by register (WriteNextReg is not hot)
	std::map<uint16_t, uint64_t> m_UnhandledPortReads; // ReadIoPort  -> 0xFF fallthrough
	std::map<uint16_t, uint64_t> m_UnhandledPortWrites; // WriteIoPort -> ignored fallthrough
	void NoteUnhandledPortRead(uint16_t Port)
	{
		++m_UnhandledPortReads[Port];
	}
	void NoteUnhandledPortWrite(uint16_t Port)
	{
		++m_UnhandledPortWrites[Port];
	}
	// True when NO per-access diagnostic feature is armed (contention is not a
	// diagnostic - it has its own fast mode), so the dispatcher may run a fast
	// specialisation: Access::Fast when contention is off, Access::FastContended
	// when it is on. Evaluated ONCE per run in RunUntil, never per instruction.
	bool IsDiagnosticFree() const
	{
		return m_Coverage == nullptr && m_WatchPage8K < 0 &&
			   (m_InvariantReadOnly == nullptr || m_InvariantReadOnly->empty()) && m_BeamBoundaryHook == nullptr
#if INSPECCY_TRACE_SUPPORT
			   && m_OnWrite == nullptr
#endif
			;
	}
	uint64_t TakeContentionPenalty()
	{
		const uint64_t P = m_ContentionPenalty;
		m_ContentionPenalty = 0;
		return P;
	}

	// Is the 8K physical page currently DISPLAYED (so a write to it can affect a
	// not-yet-scanned pixel and must fire a beam boundary)? Bank 5 (ULA/LoRes)
	// = pages 10/11; Layer 2 = up to 10 pages from the NR $12 base (covers
	// 256/320/640). A superset - over-firing is correct, just slower.
	bool IsWatchedPhysPage(int Page8K) const
	{
		if (Page8K == 10 || Page8K == 11)
		{
			return true;
		}
		const int L2Base = (int(m_NextRegs[0x12]) * 2) & (kRamPageCount - 1);
		return ((Page8K - L2Base) & (kRamPageCount - 1)) < 10;
	}

  private:
	void (*m_BeamBoundaryHook)(void*, uint64_t) = nullptr;
	void* m_BeamBoundaryUser = nullptr;
	const uint64_t* m_CyclePtr = nullptr;

	// ---- Contention state (ConfigureContention). ---------------------------
	int m_ContKind = 0; // ContentionKind: 0 off / 1 Addr4000 / 2 Bank128 / 3 Plus3 / 4 Next
	bool m_bNext28 = false; // Next runs at 28 MHz (NR $07 = 3): every memory read waits one T
	uint8_t m_NextCpuSpeed = 0;
	int32_t m_ContLine0 = 8944; // T-state of ttl==0 (Fuse's line0, from the frame interrupt)
	uint32_t m_ContTperLine = 224;
	const uint64_t* m_ContNextPtr = nullptr; // &Cpu.NextFrameIntCycle
	const uint64_t* m_ContPeriodPtr = nullptr; // &Cpu.FrameIntPeriod
	bool m_bFloatingBus = false;
	uint32_t m_FbFetchStart = 14336;
	uint32_t m_FbTperLine = 224;
	mutable uint64_t m_ContentionPenalty = 0;

	// ULA contention follows Fuse's contend_delay_common pattern.
	// The delay applies only in a contended page's display fetch window.
	// Frame position uses the frame origin modulo the frame period, including
	// under DI. All accesses are charged at instruction start, so their
	// individual M-cycle placement is still approximate.
	static uint32_t ContendPattern(int64_t P, int32_t Line0, uint32_t TperLine, bool bPlus3)
	{
		// Fuse's contend_delay_common uses two ttl & 7 tables:
		// 48K/128K/Timex: {5,4,3,2,1,0,0,6}, offset 1, fetch window [23,151).
		// +2A/+3: {5,4,3,2,1,0,7,6}, offset 4, fetch window [20,148).
		// The +3 line origin is 3 T-states later than the 128K origin.
		static const uint32_t kPat128[8] = {5, 4, 3, 2, 1, 0, 0, 6};
		static const uint32_t kPatP3[8] = {5, 4, 3, 2, 1, 0, 7, 6};
		const int64_t D = P - Line0;
		if (D < 0)
		{
			return 0;
		}
		// `line` is measured from line0; the 192 display lines are 24..215 (the
		// first 24 lines after line0 are top border and are NOT contended).
		const int64_t Line = D / int64_t(TperLine);
		if (Line < 24 || Line >= 24 + 192)
		{
			return 0;
		}
		// ttl = (P - line0 + PHASE) % tperline, PHASE = left_border(24)-16 = 8
		const int64_t Ttl = (D + 8) % int64_t(TperLine);
		const int64_t Lo = bPlus3 ? 20 : 23; // LEFT_BORDER(24) - offset
		if (Ttl < Lo || Ttl >= Lo + 128)
		{
			return 0; // left border / right border+retrace
		}
		return (bPlus3 ? kPatP3 : kPat128)[Ttl & 7];
	}

	void AccrueContention(uint16_t Addr, bool bWrite) const
	{
		if (m_ContKind == 4)
		{
			// Next: +1 T on every memory read at 28 MHz (see ContentionKind).
			// Writes and I/O are not delayed; no beam dependence.
			if (!bWrite && m_bNext28)
			{
				++m_ContentionPenalty;
			}
			return;
		}
		bool bTarget = (Addr & 0xC000) == 0x4000;
		if (m_ContKind == 2 && !bTarget)
		{
			bTarget = ((Addr & 0xC000) == 0xC000) && (m_Port7FFD & 0x01);
		}
		else if (m_ContKind == 3 && !bTarget) // +2A/+3: banks 4-7 contended at $C000
		{
			bTarget = ((Addr & 0xC000) == 0xC000) && (m_Port7FFD & 0x04);
		}
		if (!bTarget || !m_ContNextPtr || !m_ContPeriodPtr || !m_CyclePtr)
		{
			return;
		}
		const uint64_t Period = *m_ContPeriodPtr;
		const uint64_t Next = *m_ContNextPtr;
		if (Period == 0 || Next == UINT64_MAX)
		{
			return; // interrupts disabled => no frame anchor
		}
		const uint64_t Now = *m_CyclePtr;
		const uint64_t LastStart = Next - Period;
		if (Now < LastStart)
		{
			return; // pre-arm window; nothing to anchor on
		}
		const int64_t P = int64_t((Now - LastStart) % Period);
		const uint32_t Delay = ContendPattern(P, m_ContLine0, m_ContTperLine, m_ContKind == 3);
		if (Delay != 0)
		{
			m_ContentionPenalty += Delay;
		}
	}
	uint64_t m_BeamArmCycle = 0; // ignore boundaries before this cycle

	CtcChannel m_Ctc[8];

	// Re-derive counter-mode channels From..7 after a source-grid change
	// (their fire times hang off the previous channel's grid). Stops at the
	// first channel that is not a loaded counter - a timer's grid is
	// independent of its predecessor, so nothing deeper moved.
	void RearmCtcChain(int From, uint64_t CurrentCycle);
	// (Re)arm channel N as a counter on its source's grid.
	void RearmCtcCounter(int N, uint32_t Tc, bool bIrq, uint64_t CurrentCycle);
	CopperState m_Copper;
	SpritesState* m_Sprites = nullptr; // non-owning; see SetSprites
	bool m_bPixelAccurate = false;
	uint64_t m_FrameIntPeriodHint = 0;
	uint64_t m_FrameIntPhase = 0;
	// Append a write to the recording timeline, rolling over to a fresh frame
	// (and publishing the completed one) when the write crosses a frame origin.

  public:
	// 8 palettes indexed by NR_PALETTE_CTRL bits[6:4] directly:
	//   0 = ULA-1     1 = L2-1     2 = Sprites-1   3 = Tilemap-1
	//   4 = ULA-2     5 = L2-2     6 = Sprites-2   7 = Tilemap-2
	// Each entry is 2 bytes (hi=$RRRGGGBB, lo bit 0 = 9th blue bit).
	const uint8_t (*GetPalette(int PalIdx) const)[2]
	{
		return m_Palettes[PalIdx & 0x07];
	}
	uint64_t GetPaletteWrites() const
	{
		return m_PaletteWrites;
	}

	// An explicit NR_69 bit 7 write controls Layer 2 display enable.
	// Otherwise use the register-activity hint for legacy programs.
	bool IsL2DisplayEnabled() const
	{
		return (m_NextRegs[0x69] & 0x80) != 0;
	}

	// Layer 2 access port $123B (the classic way to show/write Layer 2, still
	// used by programs independently of NextReg access). Bit 1 = Layer 2
	// visible, mirrored into NR $69
	// bit 7 (they are the same hardware bit). Bit 0 (write window) and bit 2
	// (read window) lay Layer 2 RAM over slots 0-1 ($0000-$3FFF; the 16K third
	// chosen by bits 7:6) or over slots 0-5 (bits 7:6 = %11: all 48K), above
	// the MMU. Bit 3 maps the shadow Layer 2 (NR $13). A write with bit 4 set
	// is the core-3.0 bank-offset form (bits 2:0) and leaves the rest alone.
	// See ApplyLayer2Window for the mapping; Layer2WindowOpens counts the
	// writes that enabled a window (compatibility sweeps: "draws through the
	// window rather than the MMU").
	uint8_t m_Port123B = 0;
	uint8_t m_Layer2Offset = 0;
	uint64_t m_Layer2WindowOpens = 0;
	void WriteLayer2Port(uint8_t V);
	uint8_t ReadLayer2Port() const
	{
		return m_Port123B;
	}
	void ApplyLayer2Window();

	// NR_15 sprite/layer priority. Bits 4:2 select one of 8 codes; the
	// first 6 are stack orderings (top-to-bottom), the last two specify
	// ULA-blend modes where the ULA layer is mixed with what's below it.
	//
	// Names use 3 letters for the stack order (top first), or *Blend
	// for the additive / 50-50 modes.
	enum LayerPriority
	{
		kLayerPrioritySlu, // %000 sprites, layer2, ULA
		kLayerPriorityLsu, // %001 layer2, sprites, ULA
		kLayerPrioritySul, // %010 sprites, ULA, layer2
		kLayerPriorityLus, // %011 layer2, ULA, sprites
		kLayerPriorityUsl, // %100 ULA, sprites, layer2
		kLayerPriorityUls, // %101 ULA, layer2, sprites
		kLayerPriorityUlaBlend, // %110 ULA additively blended on top of layer2/sprites
		kLayerPriorityUlaHalfBlend, // %111 ULA averaged 50/50 on top of layer2/sprites
	};
	LayerPriority GetLayerPriority() const
	{
		switch ((m_NextRegs[0x15] >> 2) & 0x07)
		{
		case 0x00:
			return LayerPriority::kLayerPrioritySlu;
		case 0x01:
			return LayerPriority::kLayerPriorityLsu;
		case 0x02:
			return LayerPriority::kLayerPrioritySul;
		case 0x03:
			return LayerPriority::kLayerPriorityLus;
		case 0x04:
			return LayerPriority::kLayerPriorityUsl;
		case 0x05:
			return LayerPriority::kLayerPriorityUls;
		case 0x06:
			return LayerPriority::kLayerPriorityUlaBlend;
		case 0x07:
			return LayerPriority::kLayerPriorityUlaHalfBlend;
		default:
			return LayerPriority::kLayerPrioritySlu;
		}
	}

	// Layer detection - drives the per-layer BMP encoder dispatch when the
	// composite encoder isn't in use. Prefers the authoritative NR_69 bit 7
	// signal; falls back on the bAnyL2Touched heuristic for programs that
	// don't write NR_69.
	enum ActiveLayer
	{
		kActiveLayerUla,
		kActiveLayerL2256,
		kActiveLayerL2320,
		kActiveLayerL2640
	};
	ActiveLayer DetectActiveLayer() const
	{
		if (!m_bIsNext)
		{
			return ActiveLayer::kActiveLayerUla;
		}
		if (!IsL2DisplayEnabled() && !m_bAnyL2Touched)
		{
			return ActiveLayer::kActiveLayerUla;
		}
		const uint8_t L2Ctrl = m_NextRegs[0x70];
		switch ((L2Ctrl >> 4) & 0x03)
		{
		case 0x00:
			return ActiveLayer::kActiveLayerL2256;
		case 0x01:
			return ActiveLayer::kActiveLayerL2320;
		case 0x02:
			return ActiveLayer::kActiveLayerL2640;
		default:
			return ActiveLayer::kActiveLayerUla;
		}
	}

	// IDE_BANK allocates descending 8K page IDs within installed RAM.
	// Reserve banks 5 and 2 (pages 10/11 and 4/5), mapped at $4000-$BFFF.
	int AllocBank8K();
	void FreeBank8K(int Page8K);
	bool IsPage8KAllocated(int Page8K) const;

	// Dump emit/load helpers for the 128-bit allocation bitset.
	uint64_t GetAllocatedPages8KWord(int Idx) const
	{
		return m_AllocatedPages8KWord[Idx];
	}

	// Public MMU8K mapping helper. Used by the dot-command loader to swap RAM
	// into slot 1 ($2000-$3FFF) so the binary copy lands in writable memory.
	// Mirrors into NR_5x so coverage attribution and dump-restore's MMU
	// replay see the mapping (same contract as WriteNextReg / WritePort7FFD).
	void MapRamPageToSlot(int SlotIdx, int RamPage8K)
	{
		m_NextRegs[0x50 + SlotIdx] = uint8_t(RamPage8K);
		SetRamSlot(SlotIdx, RamPage8K);
	}

	// Dump-restore convenience: set all the inspection counters + border in
	// one call. Used by LoadMemoryFromDump.
	void RestoreCountersForDump(uint64_t Bank5Pixel, uint64_t Bank5Attr, uint64_t Bank7Pixel, uint64_t Bank7Attr,
		uint64_t Port7FFDW, uint8_t Border)
	{
		m_VramPixelWritesBank5 = Bank5Pixel;
		m_VramAttrWritesBank5 = Bank5Attr;
		m_VramPixelWritesBank7 = Bank7Pixel;
		m_VramAttrWritesBank7 = Bank7Attr;
		m_Port7FFDWrites = Port7FFDW;
		m_BorderColour = uint8_t(Border & 0x07);
	}
	bool IsPagingLocked() const
	{
		return m_bPagingLocked;
	}
	bool Is128KMode() const
	{
		return m_bIs128K;
	}
	bool IsTrdosRomActive() const
	{
		return m_bTrdosRomActive;
	}
	// The 8K ROM page currently selected for slot 0 (slot 1 = +1): TR-DOS when
	// active, else 48K BASIC / 128K editor as $7FFD bit 4 says. Map128K and the
	// Next MMU ($FF = "ROM") both resolve the ROM through this.
	int CurrentRomPage() const
	{
		return m_bTrdosRomActive ? kRomPageTrdos : ((m_Port7FFD & 0x10) ? kRomPage48kBasic : kRomPage128kEditor);
	}
	int CurrentRamBank16K() const
	{
		return m_Port7FFD & 0x07;
	}

	// True when the ROM currently mapped at slots 0/1 (or that *would* be
	// mapped if TR-DOS weren't active) is the 48K BASIC ROM. The Beta Disk
	// page-in trigger fires only when this holds, to avoid hijacking the 128K
	// editor's own $3DXX code.
	inline bool IsCurrentRomLowerHalf48K() const
	{
		if (!m_bIs128K)
		{
			return true;
		}
		return (m_Port7FFD & 0x10) != 0;
	}

	// Pages TR-DOS ROM in (true) or out (false) at slots 0/1, preserving the
	// current Port7FFD selection for the out-of-TR-DOS ROM choice. Cheap;
	// touches only the two slot pointers.
	inline void SetTrdosActive(bool bActive)
	{
		if (bActive == m_bTrdosRomActive)
		{
			return;
		}
		m_bTrdosRomActive = bActive;
		int RomPage;
		if (bActive)
		{
			RomPage = kRomPageTrdos;
		}
		else if (m_bIs128K)
		{
			RomPage = (m_Port7FFD & 0x10) ? kRomPage48kBasic : kRomPage128kEditor;
		}
		else
		{
			RomPage = kRomPage48kBasic;
		}
		SetRomSlot(0, RomPage);
		SetRomSlot(1, RomPage + 1);
	}

	// kFast (the dispatcher's plain-run policy): every per-access diagnostic
	// branch below folds away at compile time. Legal only when
	// IsPlainRunCapable() held when the run started - none of coverage,
	// contention, watchpoints, invariants, beam hook or trace hook is armed.
	template <Access kMode = Access::kAccessFull> inline uint8_t Rd(uint16_t Addr) const
	{
		if constexpr (kMode == Access::kAccessFull)
		{
			if (m_Coverage)
			{
				m_Coverage[CoveragePhysIndex(Addr)] |= 0x01;
			}
			if (m_ContKind)
			{
				AccrueContention(Addr, false);
			}
		}
		else if constexpr (kMode == Access::kAccessFastContended)
		{
			// Contention was ON when the run started, but it can be switched at
			// runtime (Next: NR $08 bit 6, or leaving 3.5 MHz), so keep the
			// cheap ContKind_ test - one predicted branch, no diagnostics.
			if (m_ContKind)
			{
				AccrueContention(Addr, false);
			}
		}
		const MemSlot& S = m_Slots[Addr >> 13];
		return S.m_Ptr[Addr & 0x1FFF];
	}

	template <Access kMode = Access::kAccessFull> inline void Wr(uint16_t Addr, uint8_t Value)
	{
		constexpr bool bKFast = (kMode != Access::kAccessFull); // diagnostics folded away
		if constexpr (kMode == Access::kAccessFull)
		{
			if (m_ContKind)
			{
				AccrueContention(Addr, true);
			}
		}
		else if constexpr (kMode == Access::kAccessFastContended)
		{
			if (m_ContKind)
			{
				AccrueContention(Addr, true);
			}
		}
		const int SlotIdx = Addr >> 13;
		MemSlot& S = m_Slots[SlotIdx];
		const uint16_t SlotOffset = Addr & 0x1FFF;
		// Physical-address watch (--watch-phys). Resolve the destination 8K
		// page through the live MMU mapping; cheap-out via a single compare
		// when the watchpoint is disabled. Captures the OLD value before the
		// write so the callback can report both new and old.
		if constexpr (!bKFast)
		{
			if (m_WatchPage8K >= 0 && S.m_bWritable)
			{
				const int Page8K = int(m_NextRegs[0x50 + SlotIdx]);
				if (Page8K == m_WatchPage8K && int(SlotOffset) == m_WatchOffsetInPage)
				{
					const uint8_t OldVal = S.m_Ptr[SlotOffset];
					if (m_OnPhysicalWatch != nullptr)
					{
						m_OnPhysicalWatch(m_OnPhysicalWatchUser, Page8K, SlotOffset, Value, OldVal);
					}
				}
			}
		}
		// Invariants: check whether this write hits a readonly range.
		if constexpr (!bKFast)
		{
			if (m_InvariantReadOnly != nullptr && S.m_bWritable && !m_InvariantReadOnly->empty())
			{
				const int Page8K = int(m_NextRegs[0x50 + SlotIdx]);
				const int64_t Addr2 = int64_t(Page8K) * 0x2000 + SlotOffset;
				for (const auto& R : *m_InvariantReadOnly)
				{
					const int64_t Lo = int64_t(R.m_Page8KStart) * 0x2000 + R.m_OffsetStart;
					const int64_t Hi = int64_t(R.m_Page8KEnd) * 0x2000 + R.m_OffsetEnd;
					if (Addr2 >= Lo && Addr2 <= Hi)
					{
						if (m_OnInvariantWrite != nullptr)
						{
							m_OnInvariantWrite(
								m_OnInvariantWriteUser, Page8K, SlotOffset, Value, S.m_Ptr[SlotOffset], SlotIdx);
						}
						break;
					}
				}
			}
		}
		// Eager beam drawer: a write INTO displayed memory is a boundary - flush
		// the beam up to this write's cycle from the current bytes before the new
		// byte lands, so chunky-streaming / mid-frame framebuffer edits render as
		// the beam actually saw them (no shadow buffer). The `>= BeamArmCycle_`
		// test comes FIRST (before the expensive watched-page check) so a frame we
		// will NOT save costs only that one comparison per write - the drawer, and
		// this per-write work, run only for the frame(s) actually emitted.
		if constexpr (!bKFast)
		{
			if (m_BeamBoundaryHook && m_CyclePtr && *m_CyclePtr >= m_BeamArmCycle && S.m_bWritable &&
				S.m_WrPtr[SlotOffset] != Value && IsWatchedPhysPage(int((S.m_WrPtr - &m_Ram[0]) / kSlotSize)))
			{
				FireBeamBoundary(*m_CyclePtr);
			}
		}
		if (S.m_bWritable)
		{
			S.m_WrPtr[SlotOffset] = Value;
		}
		// VRAM write counters. Bank 5 lives at $4000-$7FFF on every Spectrum
		// model we support; bank 7 (shadow screen) only when 7FFD's low 3
		// bits select bank 7 and the write hits $C000-area.
		if (Addr >= 0x4000 && Addr < 0x5B00)
		{
			if (Addr < 0x5800)
			{
				++m_VramPixelWritesBank5;
			}
			else
			{
				++m_VramAttrWritesBank5;
			}
		}
		else if (Addr >= 0xC000 && Addr < 0xDB00 && (m_Port7FFD & 0x07) == 7)
		{
			if (Addr < 0xD800)
			{
				++m_VramPixelWritesBank7;
			}
			else
			{
				++m_VramAttrWritesBank7;
			}
		}
#if INSPECCY_TRACE_SUPPORT
		if constexpr (!bKFast)
		{
			if (m_OnWrite != nullptr)
			{
				const Page8KInfo WInfo = Page8KAt(Addr);
				m_OnWrite(m_OnWriteUser, Addr, Value, BinaryTrace::MakePageTag(WInfo.m_bIsRom, WInfo.m_Index));
			}
		}
#endif
	}

	template <Access kMode = Access::kAccessFull> inline uint16_t Rd16(uint16_t Addr) const
	{
		const uint8_t Lo = Rd<kMode>(Addr);
		const uint8_t Hi = Rd<kMode>(uint16_t(Addr + 1));
		return uint16_t(Lo) | (uint16_t(Hi) << 8);
	}

	template <Access kMode = Access::kAccessFull> inline void Wr16(uint16_t Addr, uint16_t Value)
	{
		Wr<kMode>(Addr, uint8_t(Value & 0xFF));
		Wr<kMode>(uint16_t(Addr + 1), uint8_t(Value >> 8));
	}

	// Identifies the 8 KB page currently mapped at the address. Index range:
	// 0..15 for RAM banks; 0..5 for ROM pages (ROM_PAGE_128K_EDITOR{,+1},
	// ROM_PAGE_48K_BASIC{,+1}, ROM_PAGE_TRDOS{,+1}).
	struct Page8KInfo
	{
		bool m_bIsRom;
		uint8_t m_Index;
	};

	inline Page8KInfo Page8KAt(uint16_t Addr) const
	{
		const uint8_t* P = m_Slots[Addr >> 13].m_Ptr;
		Page8KInfo Info{};
		if (P >= m_Ram.data() && P < m_Ram.data() + m_Ram.size())
		{
			Info.m_bIsRom = false;
			Info.m_Index = uint8_t((P - m_Ram.data()) / kSlotSize);
		}
		else if (P >= m_Rom.data() && P < m_Rom.data() + m_Rom.size())
		{
			Info.m_bIsRom = true;
			Info.m_Index = uint8_t((P - m_Rom.data()) / kSlotSize);
		}
		return Info;
	}

  private:
	void SetRamSlot(int SlotIndex, int RamPage8K);
	void SetRomSlot(int SlotIndex, int RomPage8K);

	std::array<MemSlot, kSlotCount> m_Slots{};
	// The mapping the MMU / ROM / model logic asked for, before the Layer 2
	// paging window is laid over slots 0-5. Every base setter writes here
	// first; ApplyLayer2Window derives Slots from it.
	std::array<MemSlot, kSlotCount> m_BaseSlots{};
	std::array<uint8_t, kRamPageCount * kSlotSize> m_Ram{};
	std::array<uint8_t, kRomPageCount * kSlotSize> m_Rom{};
	std::array<bool, 3> m_bRomLoaded{};
	bool m_bRomPlus3Loaded = false;
	bool m_bIsPlus3 = false;
	uint8_t m_Port1FFD = 0;

	// Effective RAM size in 8K pages. Default = 1 MB (128 pages); --ram-mb 2
	// raises it to 256 (= the full Ram[] array). MMU writes targeting a page
	// index >= this value route through `OpenBusPage` (read = $FF, write =
	// dropped) instead of aliasing onto a low physical page.
	int m_EffectiveRamPages = kRamPageCount2mb;
	// Singleton open-bus dummy page. Filled with $FF in the ctor (one-time).
	// Slots that point here have `bWritable = false`, so writes drop on the
	// floor and reads always see $FF - the same behaviour real 1 MB Next hw
	// exhibits when an MMU register selects a non-existent page.
	std::array<uint8_t, kSlotSize> m_OpenBusPage{};
	uint8_t m_Port7FFD = 0;
	bool m_bPagingLocked = false;
	bool m_bIs128K = false;
	bool m_bTrdosRomActive = false;
	bool m_bIsTimex = false; // Timex machine (SCLD ports + renderer)
	bool m_bIsTimex2068 = false; // 2068 feature set (EXROM/DOCK/AY)
#if INSPECCY_TRACE_SUPPORT
	TraceWriter* m_VideoTrace = nullptr;
#endif
	uint8_t m_ScldMode = 0; // last OUT $FF (video mode + hi-res colour)
	uint8_t m_ScldBank = 0; // last OUT $F4 (DOCK/EXROM 8K-chunk enable)

	// --- DOCK cartridge (2068 only), populated from a .dck image ---
	// Eight 8K chunks; chunk N answers N*$2000 when HSR bit N selects the DOCK.
	// Kind: 0 = not populated (stays open bus, which is what a real empty dock
	// does), 1 = RAM (writable), 2 = ROM (writes swallowed). Held as one 64K
	// block so a chunk is just an offset.
	std::vector<uint8_t> m_DockRam;
	std::array<uint8_t, kSlotCount> m_DockKind{};

	// --- ZX Evolution BaseConf (MM_ATM3) state ---
	// bIsZxEvo_ gates the $EFF7/$FFBF/window-reg/RTC decodes + the ATM renderer.
	// Eff7_ is the $EFF7 system-config latch (bit0=4BPP, bit1=512, bit2=LOCKMEM,
	// bit4=turbo/giga, bit5=HWMC, bit6=384, bit7=CMOS). EvoFf77_ is the $xx77
	// video-mode latch (bits0-2: 3=ZX, 0=ATM16, 2=ATM-MC, 6=text, 7=TL). EvoPal_
	// is the 16-entry ATM hardware palette (GRB, consumed by the renderer).
	bool m_bIsZxEvo = false;
	uint8_t m_Eff7 = 0;
	uint8_t m_EvoFf77 = 0x03; // FF77_ZX: standard ZX screen at reset
	uint8_t m_EvoPal[16] = {};
	uint8_t m_EvoPalIdx = 0; // auto-advancing ATM palette write cursor
	// 4MB "always-RAM" window overrides. EvoWinPage_[w] = 8K RAM page base for
	// window w (0..3, each a 16K CPU window); -1 = window follows the standard
	// 128K/Pentagon mapping (the common case; the invite never writes these).
	// Written via the ATM $xFF7 / PentEvo $x7F7 ports. ROM-source windows are a
	// documented follow-on (BaseConf ROM banks aren't modelled yet).
	int16_t m_EvoWinPage[4] = {-1, -1, -1, -1};
	// mc146818 RTC - verification stub: 64 addressable cells, no real timekeeping.
	uint8_t m_CmosAddr = 0;
	uint8_t m_Cmos[64] = {};

	// --- ZX-Uno state ---
	// bIsZxUno_ gates the $FC3B/$FD3B register file + SCLD-via-$FF + Radastan.
	// UnoRegSel_ is the $FC3B register-select latch; UnoScanDbl_ holds reg $0B
	// (SCANDBL/turbo; bit7 = 14 MHz). UnoCoreIdIdx_ is the read cursor for the
	// CoreID string (reg $FF) - reset each time a register is selected.
	bool m_bIsZxUno = false;
	uint8_t m_UnoRegSel = 0;
	uint8_t m_UnoScanDbl = 0;
	uint8_t m_UnoCoreIdIdx = 0;
	// Radastan (128x96 4bpp) registers, from the ZX-Uno core config.vh:
	// RADASCTRL $40 (bits 1:0 = 11 -> enabled), RADASOFFSET $41 (14-bit, low byte
	// then high 6 bits), RADASPADDING $42 (per-row byte stride add), RADASPALBANK
	// $43 (2-bit ULAplus sub-palette). Framebuffer is linear at bank 5 + offset.
	uint8_t m_UnoRadasCtrl = 0;
	uint16_t m_UnoRadasOffset = 0;
	uint8_t m_UnoRadasPadding = 0;
	uint8_t m_UnoRadasPalBank = 0;
	uint8_t m_UnoRadasOffPhase = 0; // RADASOFFSET write toggle (0 = low, 1 = high)
	// ULAplus ($BF3B select / $FF3B data): 64-entry CRAM (GgGRRRBB, GRB 3-3-2) +
	// enable. Radastan colours come from this palette (index = {palbank, nibble}).
	uint8_t m_UlaPlusReg = 0;
	uint8_t m_UlaPlusCram[64] = {};
	bool m_bUlaPlusMode = false;

	// Inspection counters / state. Read at exit for the status JSON.
	uint64_t m_Port7FFDWrites = 0;
	uint64_t m_VramPixelWritesBank5 = 0;
	uint64_t m_VramAttrWritesBank5 = 0;
	uint64_t m_VramPixelWritesBank7 = 0;
	uint64_t m_VramAttrWritesBank7 = 0;
	uint8_t m_BorderColour = 7; // last ULA $FE bits 0..2; reset = white

	// --- Next state ---
	bool m_bIsNext = false;
	bool m_bAnyL2Touched = false;
	bool m_bL2StateExplicit = false; // NR $69 / $123B written: bit 7 is authoritative
	uint8_t m_NextRegs[256] = {};
	uint8_t m_LastSelectedNextReg = 0;
	uint64_t m_NextRegWrites = 0;
	uint64_t m_PaletteWrites = 0;
	uint8_t m_PaletteWriteParity = 0; // 0 = next 9-bit byte is hi; 1 = lo
	uint8_t m_Palettes[8][256][2] = {}; // [PalIdx][Entry][Hi/Lo]
	// Clip windows for the four layers: [0]=L2 ($18), [1]=sprites ($19),
	// [2]=ULA ($1A), [3]=tilemap ($1B). Four sequential writes to a register =
	// X1,X2,Y1,Y2 (inclusive) with a per-layer auto-advancing sub-index reset
	// by NR_1C bits 0/1/2/3. Hardware reset default is the 256x192 window
	// {0,255,0,191} for L2/sprites/ULA (that Y2=191 clips 320x256 L2 to 192
	// rows when a program forgets to widen it); tilemap Y2 default 255.
	uint8_t m_ClipWindow[4][4] = {
		{0, 255, 0, 191}, // L2      $18
		{0, 255, 0, 191}, // sprites $19
		{0, 255, 0, 191}, // ULA     $1A
		{0, 255, 0, 255}, // tilemap $1B
	};
	uint8_t m_ClipIndex[4] = {0, 0, 0, 0};
	// Set once a program writes the layer's clip register ($18-$1B). Lets the
	// compositor tell "reset default" from "deliberately configured": in the
	// 320/640 L2 modes an untouched $18 means the full canvas (a correctly set
	// up program), while a program that narrows $18 gets its sub-rectangle.
	bool m_bClipSet[4] = {false, false, false, false};
	// One bit per 8K page (RAM_PAGE_COUNT bits). Tracks pages handed out by
	// AllocBank8K so FreeBank8K and the descending-allocator can avoid them.
	// 4 x 64 = 256 bits covers up to 2 MB of RAM (256 8K pages). The dump
	// format is 64 hex chars; older 32-hex-char dumps (which only encoded
	// the low 128 bits) load with the high half implicitly zero.
	uint64_t m_AllocatedPages8KWord[4] = {0, 0, 0, 0};
	// Keep the NextReg dispatch table after frequently accessed state
	// so its size does not separate the slots and contention fields.
	NextRegHook m_NextRegHooks[256];
};
static_assert(kRamPageCount <= 256, "AllocatedPages8K bitset only covers up to 256 pages");
