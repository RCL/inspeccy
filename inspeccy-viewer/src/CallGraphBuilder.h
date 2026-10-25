#pragma once
#include <QString>
#include <QVector>

#include <stdint.h>

struct TraceModel;

// A single "stack frame" on the reconstructed call graph. End-open frames
// (still on the runtime stack when the trace stopped) get EndCycle = the
// final cycle from the trace header (or last walked record).
struct CallFrame
{
	enum Kind : uint8_t
	{
		kKindZ80Call = 0, // CD opcode or taken conditional CALL
		kKindZ80Rst = 1, // RST $00..$38
		kKindInterrupt = 2, // IM 1 / IM 2 / CTC vector accepted
		kKindTrappedCall = 3, // NextZXOS / esxDOS API intercept (Phase 4 trace)
	};
	uint64_t m_StartCycle = 0;
	uint64_t m_EndCycle = 0;
	uint64_t m_StartByteOffset = 0; // points at the record that opened this frame
	uint32_t m_Depth = 0;
	int32_t m_Parent = -1; // index of the frame this one was called
						   // from, or -1 when it opened at depth 0.
						   // Recorded at push time from the open
						   // stack, so it is exact - deriving it
						   // afterwards from Depth + cycle ranges
						   // would have to re-guess the nesting.
	uint16_t m_TargetPC = 0; // address being called into
	uint16_t m_CallerPC = 0; // PC of the originating CALL/RST/INT
	uint16_t m_SpAfterPush = 0; // running SP right after the push that
								// opened this frame. Frame closes when
								// runtime SP rises above this - covers
								// both real RETs and `pop hl; jp (hl)`
								// style indirect returns.
	uint8_t m_TargetPageTag = 0; // page tag at the callee's entry PC:
								 // low nibble = 8K page, bit 7 = ROM.
								 // Captured from the first record executed
								 // inside the frame (the caller's own page
								 // differs for cross-page calls). Drives
								 // SLD symbol lookup for the icicle, the
								 // same way FormatRow uses PrePageTag for
								 // the instruction list.
	Kind m_KindV = kKindZ80Call;

	// Highlight-class flags computed during build. "InInterrupt" cascades
	// to descendants - code called from inside an ISR is still interrupt
	// code. "InRom" applies only when this frame's TargetPC lands in ROM
	// (does NOT cascade because a ROM routine that calls back into user
	// code leaves ROM execution).
	bool m_bIsInInterrupt = false;
	bool m_bIsInRom = false;
	uint8_t m_InterruptSource = 0; // CpuSource id; meaningful for interrupt frames
	bool m_bComplete = false; // observed stack return, not clipped at trace end
};

// Cycle range during which the CPU was executing in ROM (including TR-DOS
// auto-page-in regions, RST trap vectors, BASIC ROM, etc.). Built alongside
// the call graph from per-record PageTag bits, so it covers ROM entry via
// JP / JR (which don't create call frames) as well as CALL.
struct RomInterval
{
	uint64_t m_StartCycle = 0;
	uint64_t m_EndCycle = 0;
};

// Walks all records once at file open to reconstruct the call graph from
// CALL / RST / Interrupt / RET / RETI / RETN events. Interrupts and unmatched
// returns are handled as separate edges.
struct CallGraphBuilder
{
  public:
	void Build(const TraceModel& Model);

	const QVector<CallFrame>& Frames() const
	{
		return m_Frames;
	}
	const QVector<RomInterval>& RomIntervals() const
	{
		return m_RomIntervals;
	}

	// Returns the index of the deepest open frame at the given cycle, or -1
	// if no frame is open there. O(n) linear scan - fine while the icicle
	// hasn't built its spatial index yet.
	int FrameAtCycle(uint64_t Cycle) const;

	// Returns true if `cycle` lies within any RomInterval. Binary search on
	// intervals sorted by StartCycle. Used by the icicle's ROM highlight.
	bool IsCycleInRom(uint64_t Cycle) const;

  private:
	QVector<CallFrame> m_Frames;
	QVector<RomInterval> m_RomIntervals;
};
