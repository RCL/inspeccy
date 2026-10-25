#pragma once
#include <stdint.h>

// I/O dispatch context. Bundles every piece of mutable state that the Z80
// dispatcher used to receive as a separate parameter to RunUntil. Built once
// in main.cpp, passed by reference through to the dispatcher and its I/O
// helpers (WriteIoPort / ReadIoPort in z80.cpp).
//
// Phase 1 (PR A1): bundles existing I/O sinks only - no behaviour change.
// Optional devices are supplied through IoContext.

struct MemoryBus;
struct Trdos;
struct InputState;
struct AyState;
struct PsgCapture;
struct NextZxosVfs;
struct Z80State;
struct ZxnDmaState;
struct AudioRenderer;
struct TraceWriter;
struct SpritesState;
struct VideoCapture;
struct MetricsCapture;
struct BeamDrawer;
struct TapePlayer;
struct IrqDiagnostics;
struct NextUart;

struct IoContext
{
	MemoryBus& m_Mem; // memory + $7FFD paging sink
	Trdos* m_Trd; // nullable; FDC + TR-DOS ROM routing
	InputState* m_Input; // nullable; keyboard + Kempston injection
	AyState& m_Ay; // always present; bAyEnabled gates decode
	PsgCapture* m_Capture; // nullable; non-null only when --psg-out
	NextZxosVfs* m_DotCmd; // nullable; non-null only when --dot or --nex (streaming)
	ZxnDmaState* m_Dma = nullptr; // nullable; non-null when Mem.IsNext() - port $6B
	SpritesState* m_Sprites = nullptr; // nullable; non-null on Next-family machines
	VideoCapture* m_Video = nullptr; // nullable; non-null only when --video-out
	BeamDrawer* m_Beam = nullptr; // nullable; eager pixel-exact drawer (video emit source)
	bool m_bAyEnabled; // default true; flipped off by --no-ay
	bool m_bInputLog; // emit "# input" lines to the trace
	bool m_bResetTrap; // PC reaching $0000 with ROM mapped traps run
	bool m_bLenientUndocEd = false; // --lenient-undoc-ed: treat undocumented ED nn as 8-cycle NOP
	bool m_bTraceDrip = false; // --trace-drip: log each drip-instr that runs during a DMA window
	// NOTE: everything from here on must stay AFTER the positional brace-init
	// in main.cpp / replay.cpp, or the initialiser silently shifts by one.
	MetricsCapture* m_Metrics = nullptr; // nullable; non-null only when --motion-out / --histogram-out
	/** Beam cadence belongs to the machine, not one RunUntil call. A live
	 * frontend splits execution at every frame; reseeding from the CPU's
	 * interrupt deadline changes the phase after turbo/interrupt setup. */
	uint64_t m_NextBeamFrameCycle = 0; // zero: seed on the first run

	// --until-pc <addr>: stop the moment the CPU is about to execute UntilPc
	// (state as of that instant, before the instruction runs). UntilPcPage >= 0
	// additionally requires the 8K physical RAM page mapped at PC's slot to
	// equal it - a banked/24-bit breakpoint, so the same $C000 in a different
	// Next bank does not false-fire. -1 = match on the 16-bit PC alone. When
	// bUntilPc is false the loop pays one predicted-not-taken branch per
	// instruction (the check is compiled in but never taken).
	bool m_bUntilPc = false;
	uint16_t m_UntilPc = 0;
	int32_t m_UntilPcPage = -1;

	// --dispatch full: refuse the plain-run dispatcher specialisation and run the
	// general one even when nothing is armed. For equivalence testing only - the
	// two must be byte-identical in behaviour; the plain one is just faster.
	bool m_bForceFullDispatch = false;

	// TurboSound (--ts): second AY + NedoPC/CHRV chip select. A write of
	// $F8..$FF to the select port ($FFFD family) switches the active chip:
	// bit0=1 -> chip 0 (the primary, also the reset default), bit0=0 ->
	// chip 1. Register selects 0..15 and data writes then apply to the
	// active chip. All nullptr / 0 when --ts is off, in which case the
	// dispatch is byte-identical to the single-AY path. (Defaulted members
	// sit AFTER everything covered by main.cpp's positional brace-init.)
	// .tap tape player (--tap). Non-null enables the LD-BYTES ($0556) ROM trap
	// that serves tape blocks instantly. nullptr = no tape (byte-identical path).
	TapePlayer* m_Tape = nullptr;

	AyState* m_Ay2 = nullptr; // nullable; non-null only when --ts
	PsgCapture* m_Capture2 = nullptr; // nullable; chip-1 capture (--ts + --psg-out)
	uint8_t m_AyActive = 0; // 0 = primary chip (reset default), 1 = second
	// Optional third Next AY. Classic two-chip TurboSound keeps its original
	// select semantics when this is null; the SDL frontend supplies all three.
	AyState* m_Ay3 = nullptr;
	AyState& ActiveAy() const
	{
		return m_AyActive == 2 && m_Ay3 ? *m_Ay3 :
			(m_AyActive == 1 && m_Ay2 ? *m_Ay2 : m_Ay);
	}

	// Turbo (Next NR_07) tracking. The dot command can change the effective
	// CPU clock mid-run; we honour the user's wall-clock --seconds by
	// recomputing Budget and FrameIntPeriod every time NR_07 changes. Set
	// by main.cpp before RunUntil; MemoryBus invokes the callback below
	// from WriteNextReg($07) which updates these in place.
	Z80State* m_Cpu = nullptr; // for cycle reads inside the turbo cb
	uint64_t* m_BudgetPtr = nullptr; // mutable budget; RunUntil reads each iter
	double m_UserSeconds = 0.0; // CLI --seconds (constant)
	double m_BaseMhz = 3.5; // CLI --mhz (the initial clock)
	double m_CurrentMhz = 3.5; // updated on NR_07 writes
	uint64_t m_LastTurboCheckpointCycle = 0; // cycle count at last turbo change
	double m_AccumulatedSeconds = 0.0; // wall time consumed up to checkpoint

	// Integer-math cache for per-cycle wall-time conversion. Recomputed only
	// when the turbo callback fires (rare); read on every traced instruction
	// (TraceSnapshotPre). Lets us turn the per-record wall-time fill from
	// a 3-float-op + cvttsd2si chain (showed 12% of total in perf annotate)
	// into a single u64 multiply + add + shift.
	//
	//   AccumulatedNanosAtCheckpoint: emulated wall-clock nanos consumed up
	//       to LastTurboCheckpointCycle. Tracks AccumulatedSeconds x 1e9.
	//   NanosPerCycleQ20: nanos-per-cycle at CurrentMhz, in Q20 fixed point
	//       (= 1e9 / (CurrentMhz x 1e6) x 2^20). At 3.5 MHz: ~299593 (real
	//       value 285714.286 x 1.0486 ~ ... actually computed in main.cpp).
	//       Per-cycle nanos at 28 MHz: ~35.7, at 3.5 MHz: ~285.7. Q20 keeps
	//       sub-cycle precision even at 28 MHz turbo.
	uint64_t m_AccumulatedNanosAtCheckpoint = 0;
	uint64_t m_NanosPerCycleQ20 = 0; // set by main + turbo callback

	// --cycles users want exactly N cycles regardless of clock; tell the
	// turbo callback to leave the budget alone (FrameIntPeriod / video
	// cadence are still rescaled - that's about emulated wall time, not
	// about how long we run for).
	bool m_bBudgetInCycles = false;

	// Unified audio render (--audio-out); nullable. Appended after the
	// positionally-initialised members so the aggregate init in main stays
	// untouched - main assigns it explicitly.
	AudioRenderer* m_Audio = nullptr;
	/** Live frontend DAC sink, using the same turbo-independent wall clock.
	 *  The mask names the Next's DACs the write reached: bit 0..3 = A..D. */
	void (*m_OnDacWrite)(void*, uint8_t, uint8_t, uint64_t) = nullptr;
	void* m_OnDacWriteUser = nullptr;
	// Per-frame tick, fired from the dispatcher's stats tick (IFF1-independent).
	// The frame-exact --bmp-out checkpoints hang off this; null = no cost.
	void (*m_OnFrameTick)(void*) = nullptr;
	void* m_OnFrameTickUser = nullptr;

	// Binary-trace hook for video-state port writes (.btr v4); nullable.
	// Ports are recorded from WriteIoPort's single choke point rather than
	// from each machine's branch, because those branches return early and a
	// newly added one would silently stop being recorded.
	TraceWriter* m_VideoTrace = nullptr;

	// Emulated wall-clock nanos at the given cycle count - the audio event
	// timestamp. Integer Q20 math, exact across turbo flips (same bank/
	// checkpoint scheme the budget rescale uses).
	uint64_t WallNanosNow(uint64_t Cycles) const
	{
		return m_AccumulatedNanosAtCheckpoint + (((Cycles - m_LastTurboCheckpointCycle) * m_NanosPerCycleQ20) >> 20);
	}
	// Primary execution only; replay contexts deliberately leave this null.
	IrqDiagnostics* m_IrqDiagnostics = nullptr;
	// Next UART at $133B..$163B (--uart-pi / --uart-esp); nullptr = ports float.
	NextUart* m_Uart = nullptr;
};
