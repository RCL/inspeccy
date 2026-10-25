#pragma once
#include <stdint.h>
#include <vector>

struct MemoryBus;
struct TraceWriter;
struct Trdos;
struct InputState;
struct IoContext;

// Flag bits
constexpr uint8_t kFlagC = 0x01;
constexpr uint8_t kFlagN = 0x02;
constexpr uint8_t kFlagPv = 0x04;
constexpr uint8_t kFlagX = 0x08;
constexpr uint8_t kFlagH = 0x10;
constexpr uint8_t kFlagY = 0x20;
constexpr uint8_t kFlagZ = 0x40;
constexpr uint8_t kFlagS = 0x80;

struct Z80State
{
	uint16_t AF = 0, BC = 0, DE = 0, HL = 0;
	uint16_t AFp = 0, BCp = 0, DEp = 0, HLp = 0;
	uint16_t IX = 0, IY = 0;
	uint16_t SP = 0, PC = 0;
	uint16_t m_MemPtr = 0; // internal WZ register
	uint8_t I = 0, R = 0;
	uint8_t IFF1 = 0, IFF2 = 0, IM = 0;
	bool m_bHalted = false;
	bool m_bEiPending = false; // EI defers IFF set by one instruction
	uint64_t m_HaltStartCycle = 0; // where the current HALT's NOP run began (R counts 4T steps from it)
	uint64_t m_Cycles = 0;

	// Interrupt scheduling. Same cycle space as Cycles.
	uint64_t m_NextFrameIntCycle = UINT64_MAX; // UINT64_MAX = disabled
	uint64_t m_NextEventCycle = UINT64_MAX; // min(Budget, all deadlines)
	uint64_t m_FrameIntPeriod = 0; // 0 = disabled
	uint64_t m_InterruptsAccepted = 0; // diagnostic counter
	// ULA /INT pulse length in T-states from the frame top (IntWindowFor). The
	// frame interrupt can only be TAKEN while Cycles < NextFrameIntCycle +
	// IntWindow; if IFF1 stays 0 (long ISR, DI section, Timex INTDISABLE) for
	// the whole pulse the hardware simply drops that frame - counted below and
	// the request moves to the next frame top, phase kept. 0 = legacy sticky
	// request (taken whenever IFF1 next becomes 1, even mid-frame). The Next's
	// hardware interrupt controller (NR $C0 IM 2 vectoring) latches sources as
	// pending status bits, so the window is not applied while it is enabled.
	uint64_t m_IntWindow = 32;
	uint64_t m_MissedInterrupts = 0; // frame pulses that expired untaken

	// Line interrupt (NR $22/$23). Fires once per frame at the programmed video
	// line while enabled; shares the /INT wire with the frame interrupt, and in
	// IM2 HW-vector mode is source index 0 (above every CTC channel and the ULA).
	// $22 bit 2 masks the ULA (frame) interrupt without disabling the line one.
	// UINT64_MAX = disabled -> costs one compare inside the UNLIKELY event block.
	uint64_t m_NextLineIntCycle = UINT64_MAX; // deadline; MAX = disabled
	uint32_t m_LineIntLine = 0; // 0..511 as programmed ($22 bit0 = bit8)
	bool m_bLineIntEnabled = false; // $22 bit 1
	bool m_bUlaIntMasked = false; // $22 bit 2
	uint64_t m_LineIntsAccepted = 0; // diagnostic counter
	uint64_t m_LineIntsMissed = 0; // line pulses that expired untaken

	// UART Rx interrupts (NR $C6). The Rx FIFO fills on the wire's clock whether
	// or not the program polls, so while a link exists the UART model is advanced
	// every kUartPollCycles and the request levels are sampled there.
	// UINT64_MAX = no UART attached.
	uint64_t m_NextUartPollCycle = UINT64_MAX;

	// PSG frame-tick scheduling. Independent of IM2 interrupts; fires whether
	// IFF1 is set or not. UINT64_MAX = disabled.
	uint64_t m_NextPsgTickCycle = UINT64_MAX;
	uint64_t m_PsgTickPeriod = 0;

	// DI-adjacency for halt classification. Set in DI handler to Cpu.Cycles
	// BEFORE the +4 bump; UINT64_MAX = no DI ever executed in this run.
	uint64_t m_LastDiCycles = UINT64_MAX;

	// CPU usage tracking. HaltCycles accumulates every fast-forward jump in
	// the bHalted block; consumed by the status writer.
	uint64_t m_HaltCycles = 0;

	// Per-frame stats collection tick. Period = FrameIntPeriod (or machine
	// default if interrupts are disabled). On each fire, snapshot
	// (HaltCycles - LastStatsSnapshotHalt) into PerFrameHalt.
	uint64_t m_NextStatsTickCycle = UINT64_MAX;
	uint64_t m_StatsTickPeriod = 0;
	uint64_t m_LastStatsSnapshotHalt = 0;
	std::vector<uint32_t> m_PerFrameHalt;

	// DMA-preemption tracking. While a zxnDMA transfer runs the CPU is
	// bus-locked, but interrupts can preempt (per NR_CC / NR_CD). When an
	// ISR returns to depth 0 mid-DMA, exactly one main-program instruction
	// executes before DMA resumes (zxnext.vhd:2007 + im2_control.vhd:238).
	static constexpr int kIsrNestingMax = 8;
	uint8_t m_IsrDepth = 0;
	uint8_t m_BDmaResumePendingOneInstr = 0; // 1 = one main instr allowed
	uint16_t m_IsrReturnSP[kIsrNestingMax] = {}; // SP BEFORE the int's push
	uint16_t m_IsrReturnPC[kIsrNestingMax] = {}; // PC that was pushed

	inline uint8_t A() const
	{
		return uint8_t(AF >> 8);
	}
	inline uint8_t F() const
	{
		return uint8_t(AF & 0xFF);
	}
	inline void SetA(uint8_t V)
	{
		AF = uint16_t((uint16_t(V) << 8) | (AF & 0xFF));
	}
	inline void SetF(uint8_t V)
	{
		AF = uint16_t((AF & 0xFF00) | V);
	}
};

// Machine timing model. The frame interrupt period depends on this and, for
// Next, on the user-selected CPU clock (turbo).
enum MachineModel : int
{
	kMachineModelSpectrum48K,
	kMachineModelSpectrum128K,
	kMachineModelPentagon128,
	kMachineModelNext50,
	kMachineModelNext60,
	// Timex machines (SCLD video via port $FF). TC2048/TC2068 are PAL (312-line,
	// Spectrum-class timing); TS2068 is NTSC (262-line). Clocks differ slightly
	// from 3.5 MHz on real hardware [CONFIRM] but Spectrum-class T-counts are used
	// here - exact enough for headless render; refine if --seconds accuracy needs.
	kMachineModelTs2068, // NTSC, Home 16K + EXROM 8K, DOCK, AY
	kMachineModelTc2068, // PAL,  Home 16K + EXROM 8K, DOCK, AY
	kMachineModelTc2048, // PAL,  single Spectrum-compatible 16K ROM, Kempston, no AY/DOCK
	// Post-Spectrum clones. ZxEvo (ZX Evolution BaseConf) is Pentagon-derived: 71680 T,
	// 320 lines, 4 MB, $EFF7/$?FF7 paging + ATM video. ZxUno is a Spectrum clone
	// with the $FC3B/$FD3B register file; timing here is 128K-class [CONFIRM -
	// core-dependent], adequate for headless render since the invite doesn't
	// measure ZX-Uno frame length.
	kMachineModelZxEvo, // ZX Evolution BaseConf (Pentagon-class, 4 MB, ATM video)
	kMachineModelZxUno, // ZX-Uno (128K-class, $FC3B/$FD3B regs, Radastan + Timex hicolour)
	// Amstrad +2A / +3: 128K-class timing, four 16K ROMs, the second paging port
	// $1FFD (ROM-select high bit + special all-RAM mode), banks 4-7 contended.
	// They share memory/paging; the +3 additionally has the uPD765 disk (stubbed).
	kMachineModelSpectrumPlus2A,
	kMachineModelSpectrumPlus3,
};

// Nominal CPU clock in MHz - the speed the real machine's Z80 runs at.
// Emulated seconds are wall seconds at this clock (or at the effective turbo
// clock on machines with a speed switch). Frame T-counts stay per-machine
// constants; only the wall-time mapping uses the clock.
inline double NominalMhzFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
		return 3.5;
	case MachineModel::kMachineModelSpectrum128K:
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
		return 3.54690;
	case MachineModel::kMachineModelPentagon128:
		return 3.5;
	case MachineModel::kMachineModelNext50:
		return 3.5; // base; nextreg $07 switches at runtime
	case MachineModel::kMachineModelNext60:
		return 3.5;
	case MachineModel::kMachineModelTc2048:
		return 3.5; // PAL Spectrum-class [VERIFY]
	case MachineModel::kMachineModelTc2068:
		return 3.5; // PAL [VERIFY 3.5 vs 3.528]
	case MachineModel::kMachineModelTs2068:
		return 3.5280; // NTSC: 14.112 MHz master / 4
	case MachineModel::kMachineModelZxEvo:
		return 3.5; // base; #EFF7 bit4 / #FF77 bit3 switch (boots turbo=7)
	case MachineModel::kMachineModelZxUno:
		return 3.5; // base; Uno reg $0B bits 7:6 switch
	}
	return 3.5;
}

// --mhz selects the INITIAL speed only on machines whose real hardware has a
// CPU-speed switch the flag can stand in for. Everything else runs at its
// nominal clock - we cannot (and do not want to) run a TS2068 faster than the
// real one. ZxEvo has a switch, but it is the #EFF7/#FF77 config latch, not
// --mhz; it boots at its hardware reset state instead.
inline bool MachineHonoursMhzFlag(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelNext50:
	case MachineModel::kMachineModelNext60:
	case MachineModel::kMachineModelZxUno:
		return true;
	default:
		return false;
	}
}

// Frame interrupt period in T-states. The interrupt RATE belongs to the display,
// not to the CPU: an accelerated machine of any model still sees its display
// rate and simply gets proportionally more T-states to spend inside the frame,
// so the period scales with the effective clock. At EffectiveMhz ==
// NominalMhzFor(M) the period is exactly the machine's stock T-count.
// Length of the ULA's /INT pulse at the frame top, in T-states at the base
// clock (Fuse/libspectrum interrupt_length): 32 on the 48K class, Timex,
// Pentagon and Next; 36 on the 128K/+2 class. A Z80 whose IFF1 is 0 for the
// whole pulse never sees that frame's interrupt.
inline uint64_t IntWindowFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum128K:
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
	case MachineModel::kMachineModelZxUno:
		return 36; // [VERIFY +2A/+3 int length]
	default:
		return 32;
	}
}

inline uint64_t FrameIntPeriodFor(MachineModel M, double EffectiveMhz)
{
	double Base = 69888.0;
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
		Base = 69888.0;
		break;
	case MachineModel::kMachineModelSpectrum128K:
		Base = 70908.0;
		break;
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
		Base = 70908.0;
		break; // 128K-class
	case MachineModel::kMachineModelPentagon128:
		Base = 71680.0;
		break;
	case MachineModel::kMachineModelNext50:
		Base = 69888.0;
		break;
	case MachineModel::kMachineModelNext60:
		Base = 58688.0;
		break;
	case MachineModel::kMachineModelTc2048:
	case MachineModel::kMachineModelTc2068:
		Base = 69888.0;
		break; // PAL, Spectrum-class
	case MachineModel::kMachineModelTs2068:
		Base = 58688.0;
		break; // NTSC 60 Hz
	case MachineModel::kMachineModelZxEvo:
		Base = 71680.0;
		break; // Pentagon-class (BaseConf)
	case MachineModel::kMachineModelZxUno:
		Base = 70908.0;
		break; // 128K-class [CONFIRM]
	}
	return uint64_t(Base * EffectiveMhz / NominalMhzFor(M) + 0.5);
}

// ---- Coarse contention model ------------------------------------------------
// "ULA on a display line and inside the fetch window -> contended-page access
// costs X extra cycles." Flat X (default 4 ~= the real 48K 6,5,4,3,2,1,0,0
// pattern's fetch-window average), no per-T pattern - deliberately coarse.

// Which pages contend on this machine.
enum ContentionKind
{
	kContentionKindNone = 0, // Pentagon / ZxEvo (that is the point of a Pentagon) / ZxUno [VERIFY]
	kContentionKindAddr4000 = 1, // 48K + Timex home RAM: $4000-$7FFF
	kContentionKindBank128 = 2, // 128K: bank 5 at $4000 + odd bank paged at $C000 ($7FFD bit 0)
	kContentionKindPlus3 = 3, // +2A/+3: bank 5 at $4000 + banks 4-7 at $C000 ($7FFD bit 2)
	// Next: no ULA contention modelled (the core does contend bank 5/7 at
	// 3.5 MHz unless NR $08 bit 6; not done here), but at 28 MHz the FPGA
	// inserts one wait state on EVERY memory read - opcode fetch, operand,
	// data, stack, DMA source - and none on writes or I/O. zxnext.vhd,
	// "wait states on memory read cycles at 28MHz": the SRAM cycle takes the
	// whole 28 MHz period and the read data is registered at its end, one
	// clock after the T80 samples it, so sram_wait_n holds T2 for one clock.
	kContentionKindNext = 4,
};

inline ContentionKind ContentionKindFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
	case MachineModel::kMachineModelTc2048:
	case MachineModel::kMachineModelTc2068:
	case MachineModel::kMachineModelTs2068:
		return ContentionKind::kContentionKindAddr4000;
	case MachineModel::kMachineModelSpectrum128K:
		return ContentionKind::kContentionKindBank128;
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
		return ContentionKind::kContentionKindPlus3;
	case MachineModel::kMachineModelNext50:
	case MachineModel::kMachineModelNext60:
		return ContentionKind::kContentionKindNext;
	default:
		return ContentionKind::kContentionKindNone;
	}
}

// Beam-window constants, measured from the frame interrupt (line 0 = first
// line after INT). TFetch = contended fetch window at each display line start.
struct ContentionWindow
{
	uint32_t m_TperLine = 224;
	uint32_t m_FirstDisplayLine = 64;
	uint32_t m_TFetch = 128;
};

inline ContentionWindow ContentionWindowFor(MachineModel M)
{
	ContentionWindow W;
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum128K:
		W.m_TperLine = 228;
		W.m_FirstDisplayLine = 63;
		break; // 311 lines x 228 T
	case MachineModel::kMachineModelTs2068:
		W.m_TperLine = 224;
		W.m_FirstDisplayLine = 40;
		break; // NTSC 262 x 224 [calibrate on hw]
	default:
		W.m_TperLine = 224;
		W.m_FirstDisplayLine = 64;
		break; // 48K-class 312 x 224
	}
	return W;
}

// Contention parameters for the Fuse-style delay tables. line0 is the
// frame-relative origin and tperline the scanline length. Delays are charged
// per instruction; individual M-cycle placement is approximate.
// Pentagon and Next have no contended RAM in this model.
struct ContentionModel
{
	int32_t m_Line0 = 8944; // T-state of ttl==0 (frame origin = interrupt)
	uint32_t m_TperLine = 224;
	bool m_bPlus3 = false; // +2A/+3 use the 76543210 pattern (offset 4); selected via ContentionKind::Plus3
};

inline ContentionModel ContentionModelFor(MachineModel M)
{
	ContentionModel C;
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum128K:
		C.m_Line0 = 8874;
		C.m_TperLine = 228;
		break; // first contended 14361
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3: // first contended 14361, but 76543210 pattern
		C.m_Line0 = 8877;
		C.m_TperLine = 228;
		C.m_bPlus3 = true;
		break; // Fuse-validated (3113754624)
	case MachineModel::kMachineModelTs2068:
		C.m_Line0 = 3777;
		C.m_TperLine = 224;
		break; // first contended 9168
	case MachineModel::kMachineModelTc2048:
	case MachineModel::kMachineModelTc2068:
		C.m_Line0 = 8929;
		C.m_TperLine = 224;
		break; // first contended 14320
	default:
		C.m_Line0 = 8944;
		C.m_TperLine = 224;
		break; // 48K: first contended 14335
	}
	return C;
}

// Total raster lines per frame. Used to map a Copper WAIT(line) onto a T-state
// (cycles-per-line = FrameIntPeriod / LinesPerFrame, which turbo-scales for
// free because FrameIntPeriod already does). Only the Next values are load-
// bearing today - the copper is a Next-only device - but the classic machines
// return their real line counts for completeness.
inline uint16_t LinesPerFrameFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
		return 312; // 69888 / 224
	case MachineModel::kMachineModelSpectrum128K:
		return 311; // 70908 / 228
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
		return 311;
	case MachineModel::kMachineModelPentagon128:
		return 320; // 71680 / 224
	case MachineModel::kMachineModelNext50:
		return 312;
	case MachineModel::kMachineModelNext60:
		return 262; // 58688 / 224
	case MachineModel::kMachineModelTc2048:
	case MachineModel::kMachineModelTc2068:
		return 312;
	case MachineModel::kMachineModelTs2068:
		return 262;
	case MachineModel::kMachineModelZxEvo:
		return 320; // 71680 / 224 (Pentagon)
	case MachineModel::kMachineModelZxUno:
		return 311; // 70908 / 228 (128K)
	}
	return 312;
}

// Top-border height in raster lines = lines between the frame interrupt (raster
// line 0) and display row 0 (first paper line). From the Next FPGA video timing
// (zxula_timing.vhd: c_min_vactive - c_int_v; 48K-class 50 Hz = 64 - 0, 60 Hz =
// 40 - 0). The copper's vertical WAIT counter is paper-relative, so this bridges
// a WAIT(line D) to display row D and keeps the interrupt ISR up in the border.
inline uint16_t TopBorderLinesFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
		return 64;
	case MachineModel::kMachineModelSpectrum128K:
		return 63; // c_min_vactive 64 - c_int_v 1
	case MachineModel::kMachineModelSpectrumPlus2A:
	case MachineModel::kMachineModelSpectrumPlus3:
		return 63;
	case MachineModel::kMachineModelPentagon128:
		return 80;
	case MachineModel::kMachineModelNext50:
		return 64;
	case MachineModel::kMachineModelNext60:
		return 40;
	case MachineModel::kMachineModelTc2048:
	case MachineModel::kMachineModelTc2068:
		return 64;
	case MachineModel::kMachineModelTs2068:
		return 40;
	case MachineModel::kMachineModelZxEvo:
		return 80; // Pentagon top border
	case MachineModel::kMachineModelZxUno:
		return 63; // 128K-class
	}
	return 64;
}

// Frame-interrupt control. EnableFrameInterrupts arms the first interrupt at
// (Cpu.Cycles + FrameIntPeriodTStates). DisableFrameInterrupts removes the
// interrupt source.
void EnableFrameInterrupts(Z80State& InOutCpu, uint64_t FrameIntPeriodTStates);
void DisableFrameInterrupts(Z80State& InOutCpu);

// PSG capture tick scheduling. Period in T-states (= Mhz * 1'000'000 / Rate).
void EnablePsgTicks(Z80State& InOutCpu, uint64_t PsgTickPeriodTStates);
void DisablePsgTicks(Z80State& InOutCpu);

// Per-frame stats tick. Period typically = frame interrupt period.
void EnableStatsTicks(Z80State& InOutCpu, uint64_t StatsTickPeriodTStates);
void DisableStatsTicks(Z80State& InOutCpu);

enum RunResult
{
	kRunResultBudgetReached,
	kRunResultInvalidOpcode,
	kRunResultHaltedInterruptsDisabled, // HALT with IFF1=0; DI was NOT the immediately prior instruction
	kRunResultHaltedDeliberateStop, // strict DI; HALT sequence (DI directly before HALT)
	kRunResultRestartTrap, // PC reached $0000 with ROM mapped at slot 0 (Spectrum reset)
	kRunResultReachedPc, // --until-pc: PC (and, if given, its 8K page) reached the target
};

struct RunSummary
{
	RunResult m_Result;
	uint64_t m_CyclesExecuted;
	uint64_t m_Budget;
	uint16_t m_LastPC;
	uint8_t m_LastOpcode;
	uint64_t m_InterruptsAccepted = 0;
	uint64_t m_MissedInterrupts = 0; // frame interrupts the hardware would have dropped
	uint64_t m_LineIntsAccepted = 0; // NR $22/$23 line interrupts taken
	uint64_t m_LineIntsMissed = 0; // line interrupt pulses that expired untaken
	uint64_t m_AyWritesObserved = 0;
	uint64_t m_PsgFramesWritten = 0;
	uint64_t m_DiToHaltCycles = UINT64_MAX; // cycles between last DI and HALT detection; MAX = no DI
	uint64_t m_HaltCycles = 0; // total cycles spent in HALT fast-forward
};

// Runs until cumulative Cycles >= BudgetCycles is observed at a branch boundary,
// or until the CPU halts with interrupts disabled, or an invalid opcode is decoded.
//
// When Trace is non-null and INSPECCY_TRACE_SUPPORT is compiled in, each executed
// instruction (including LDIR-class iterations and the final InvalidOpcode if
// triggered) is emitted to the writer. The trace-off path is the templated
// instantiation `RunUntilImpl<false>` and has zero trace-related code.
// BudgetCycles is taken by reference so callers (main.cpp) can update it
// from a NextReg side-effect callback (turbo / NR_07 rescaling). The
// dispatcher re-reads it on every iteration.
RunSummary RunUntil(Z80State& InOutCpu, IoContext& InOutIo, uint64_t& InOutBudgetCycles, TraceWriter* Trace = nullptr);
