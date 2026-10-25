#include "copper.h"

#include <trace/trace_binary.h>

#include "memory.h"
#include "z80.h"

// Copper control-register indices.
namespace
{
constexpr uint8_t kNrCopperData = 0x60; // 8-bit data write, auto-inc
constexpr uint8_t kNrCopperCtrlLo = 0x61; // write-addr low 8 bits
constexpr uint8_t kNrCopperCtrlHi = 0x62; // write-addr high 3 bits + mode[7:6]
constexpr uint8_t kNrCopperData16b = 0x63; // 16-bit data write (hi then lo), auto-inc
} // namespace

void WriteCopper(CopperState& InOutCop, uint8_t RegIndex, uint8_t Value, uint64_t CurrentCycle)
{
	switch (RegIndex)
	{
	case kNrCopperData:
		// 8-bit path: store one byte at the write cursor, auto-increment.
		InOutCop.m_Mem2K[InOutCop.m_WriteAddr & 0x07FF] = Value;
		InOutCop.m_WriteAddr = uint16_t((InOutCop.m_WriteAddr + 1) & 0x07FF);
		return;

	case kNrCopperCtrlLo:
		// Low 8 bits of the 11-bit write address; also clears the 16-bit
		// assembly latch (a fresh address starts a fresh instruction).
		InOutCop.m_WriteAddr = uint16_t((InOutCop.m_WriteAddr & 0x0700) | Value);
		InOutCop.m_bData16HaveHi = false;
		return;

	case kNrCopperCtrlHi:
	{
		// High 3 bits of the write address + copper mode in bits [7:6].
		InOutCop.m_WriteAddr = uint16_t((InOutCop.m_WriteAddr & 0x00FF) | (uint16_t(Value & 0x07) << 8));
		InOutCop.m_bData16HaveHi = false;
		const uint8_t NewMode = uint8_t((Value >> 6) & 0x03);
		InOutCop.m_Mode = NewMode;
		if (NewMode == 0)
		{
			// Stop. Keep instruction memory intact; just park the runner.
			InOutCop.m_bRunning = false;
			InOutCop.m_bRestartEachFrame = false;
			InOutCop.m_NextFireCycle = UINT64_MAX;
			InOutCop.m_FrameOrigin = UINT64_MAX;
			return;
		}
		// Run. Modes 1 and 3 reset the program counter; mode 2 resumes.
		InOutCop.m_bRunning = true;
		InOutCop.m_bRestartEachFrame = (NewMode == 3);
		if (NewMode == 1 || NewMode == 3)
		{
			InOutCop.m_Cpc = 0;
		}
		// Schedule promptly. The real first WAIT cycle is computed on the
		// first StepCopper; seeding NextFireCycle = now makes the dispatcher
		// pick the copper up at the next RecomputeNextEvent (<= one frame).
		InOutCop.m_FrameOrigin = UINT64_MAX; // force re-derivation in StepCopper
		InOutCop.m_NextFireCycle = CurrentCycle;
		return;
	}

	case kNrCopperData16b:
		// 16-bit path: buffer the high byte, commit both on the low byte,
		// advancing the cursor by one instruction (2 bytes). This is the
		// upload path demos use ("waits for full 16b before write").
		if (!InOutCop.m_bData16HaveHi)
		{
			InOutCop.m_Data16Hi = Value;
			InOutCop.m_bData16HaveHi = true;
		}
		else
		{
			InOutCop.m_Mem2K[InOutCop.m_WriteAddr & 0x07FF] = InOutCop.m_Data16Hi;
			InOutCop.m_Mem2K[(InOutCop.m_WriteAddr + 1) & 0x07FF] = Value;
			InOutCop.m_WriteAddr = uint16_t((InOutCop.m_WriteAddr + 2) & 0x07FF);
			InOutCop.m_bData16HaveHi = false;
		}
		return;

	default:
		return;
	}
}

void StepCopper(CopperState& InOutCop, MemoryBus& InOutMem, Z80State& InOutCpu)
{
	StepCopperTo(InOutCop, InOutMem, InOutCpu.m_Cycles, InOutCpu.m_FrameIntPeriod);
}

void StepCopperTo(CopperState& InOutCop, MemoryBus& InOutMem, uint64_t Now, uint64_t InFrameIntPeriod)
{
	// Marked for its whole run, every return included (see m_bStepping).
	struct SteppingScope
	{
		bool& m_bFlag;
		~SteppingScope()
		{
			m_bFlag = false;
		}
	} Scope{InOutCop.m_bStepping};
	InOutCop.m_bStepping = true;

	if (!InOutCop.m_bRunning || !InOutCop.m_bPresent)
	{
		InOutCop.m_NextFireCycle = UINT64_MAX;
		return;
	}
	const uint64_t FrameIntPeriod = InFrameIntPeriod;
	if (FrameIntPeriod == 0)
	{
		// No frame timing to map raster lines onto - cannot schedule.
		InOutCop.m_NextFireCycle = UINT64_MAX;
		return;
	}
	const uint16_t Lines = InOutCop.m_LinesPerFrame ? InOutCop.m_LinesPerFrame : 312;
	const uint64_t CyclesPerLine = FrameIntPeriod / Lines;
	const uint64_t PixelCycles = CyclesPerLine / 56; // 6-bit H spans a line
	// Per-instruction execution time. On hardware the copper is a coprocessor
	// that executes one instruction every few pixel clocks whether or not a WAIT
	// is satisfied - so a WAIT-less list (modes %10/%11) does not park, it runs
	// its MOVEs back-to-back forever, wrapping at the list end. A MOVE is applied
	// at the CURRENT BeamCycle (so a single MOVE after a WAIT still lands exactly
	// at the WAIT position - the oracles stay byte-identical), then BeamCycle
	// advances by this; when it passes Cpu.Cycles the copper has caught up to now
	// and yields. Sub-H-unit (PixelCycles/8) so several MOVEs after one WAIT stay
	// within its scanline. Was: MOVEs took no time -> a WAIT-less list piled every
	// MOVE at FrameOrigin and tripped the 4096-step guard (a WAIT-less list).
	const uint64_t CopInstrCycles = (PixelCycles / 8) ? (PixelCycles / 8) : 1;

	// Anchor the execution frame origin - the T-state at which the frame the
	// copper is *currently executing* began. Aligned to the frame-interrupt
	// phase (Mem.FrameIntPhase) so the copper's line 0 coincides with the ULA
	// frame interrupt (the real frame sync mode-3 restarts on) and the timeline
	// samples off the exact same origin. Phase defaults to 0 (arithmetic grid)
	// unless --pixel-accurate set it, so execution timing is unchanged outside
	// that mode. On (re)arm FrameOrigin is UINT64_MAX; seed it to the frame the
	// current cycle sits in.
	// Otherwise RE-ALIGN FrameOrigin to the live phase grid: the ULA interrupt
	// phase drifts through the dot/demo setup before it stabilises, and the
	// write-timeline slices frames by that SAME live grid (FrameOriginFor). If we
	// let FrameOrigin accumulate a fixed +FrameIntPeriod from a stale seed it
	// falls behind the grid, and every MOVE then lands at the wrong in-frame
	// offset (the whole mid-scanline structure collapses -- dup renders as one
	// half). Snapping via +Period/2 keeps us inside the frame we're executing
	// before FrameOriginFor rounds down to its grid origin.
	if (InOutCop.m_FrameOrigin == UINT64_MAX)
	{
		InOutCop.m_FrameOrigin = InOutMem.FrameOriginFor(Now, FrameIntPeriod);
	}
	else
	{
		InOutCop.m_FrameOrigin = InOutMem.FrameOriginFor(InOutCop.m_FrameOrigin + FrameIntPeriod / 2, FrameIntPeriod);
	}

	// Catch-up: replay every instruction whose beam time is <= Cpu.Cycles, ACROSS
	// frame restarts, in a single call. This is what lets the dispatcher
	// fast-forward a halted CPU straight to its next wake (typically the frame
	// interrupt) WITHOUT stopping at every copper line - the copper reconstructs
	// each line here instead, recording each MOVE at the cycle its WAIT released
	// (BeamCycle), so the pixel-accurate timeline stays exact while a program that
	// HALTs every frame costs one StepCopper call per frame rather than
	// ~192. The guard caps a pathological WAIT-less list.
	//
	// BeamCycle = the execution clock: the T-state at which the current
	// instruction runs (set to a WAIT's target when one releases; advanced by
	// CopInstrCycles per MOVE otherwise). RESUME it from the last yield
	// (NextFireCycle), not the frame origin: a WAIT-less list serviced many
	// times within a busy frame must continue from where it left off, or each
	// call re-replays the whole frame from the origin, repeatedly writing NR $00
	// (the zero-fill NOOP). A satisfied WAIT
	// re-syncs it to the raster anyway, so persistence is safe for those.
	uint64_t BeamCycle =
		(InOutCop.m_NextFireCycle != UINT64_MAX && InOutCop.m_NextFireCycle >= InOutCop.m_FrameOrigin &&
			InOutCop.m_NextFireCycle <= Now)
			? InOutCop.m_NextFireCycle
			: InOutCop.m_FrameOrigin;
	// Per-servicing MOVE cap. A WAIT-anchored list yields at its next WAIT long
	// before this (a frame holds ~192 WAITs), so the cap only bites a WAIT-less
	// runaway - which is PERIODIC (period = list scan). One full 1024-scan sets
	// every register it touches to the steady repeating state; beyond that the
	// MOVEs just repeat. So cap here, and if we hit it while still behind
	// Cpu.Cycles (a HALT fast-forward replaying a whole frame of MOVEs, which no
	// running CPU observed), coalesce below instead of writing millions of regs.
	const int GuardMax = 2048;
	for (int Guard = 0; Guard < GuardMax; ++Guard)
	{
		const uint32_t Base = uint32_t(InOutCop.m_Cpc & 0x03FF) * 2u;
		const uint8_t Hi = InOutCop.m_Mem2K[Base];
		const uint8_t Lo = InOutCop.m_Mem2K[Base + 1];

		if ((Hi & 0x80) == 0)
		{
			// MOVE: nextreg (Hi & 0x7F) = Lo, applied at the beam cycle of the
			// preceding WAIT (NOT Cpu.Cycles - that would collapse a whole batched
			// frame onto the fast-forward instant). Reuse WriteNextReg so the
			// copper composes with every register side-effect (palette select,
			// MMU, L2 flags, Level-B write-timeline). WriterPC 0xFFFF marks a
			// copper-sourced write.
			// NR $00 is the machine-id register (read-only); a MOVE to it - the
			// copper's zero-fill NOOP - does nothing on hardware, so skip the
			// write but still spend the instruction time.
			if ((Hi & 0x7F) != 0)
			{
				InOutMem.WriteNextReg(uint8_t(Hi & 0x7F), Lo, 0xFFFF, BeamCycle, BinaryTrace::kNRSource_Copper);
			}
			++InOutCop.m_Moves;
			InOutCop.m_Cpc = uint16_t((InOutCop.m_Cpc + 1) & 0x03FF);
			if (!InOutCop.m_bRunning) // a MOVE to NR $62 may have stopped us
			{
				InOutCop.m_NextFireCycle = UINT64_MAX;
				return;
			}
			BeamCycle += CopInstrCycles; // the instruction took time
			if (BeamCycle > Now)
			{
				// Caught up to the present: every MOVE due by now has been
				// applied. Yield; the dispatcher re-services at the next
				// instruction's cycle. This is what paces a WAIT-less list
				// instead of parking it (or spinning the guard).
				InOutCop.m_NextFireCycle = BeamCycle;
				return;
			}
			continue;
		}

		// WAIT: line = 9-bit, h = 6-bit.
		const uint16_t Line = uint16_t((uint16_t(Hi & 0x01) << 8) | Lo);
		const uint8_t H = uint8_t((Hi >> 1) & 0x3F);
		if (Line >= Lines)
		{
			// Off-screen WAIT (e.g. terminal $FFFF) - the list has parked. In
			// restart mode, advance to the next frame; if that frame has already
			// begun (the halted CPU jumped past it) restart inline and keep
			// catching up, else schedule the restart and yield.
			if (!InOutCop.m_bRestartEachFrame)
			{
				InOutCop.m_NextFireCycle = UINT64_MAX;
				return;
			}
			// Next frame's grid origin (phase-aligned, like the entry re-align).
			const uint64_t NextFrame =
				InOutMem.FrameOriginFor(InOutCop.m_FrameOrigin + FrameIntPeriod + FrameIntPeriod / 2, FrameIntPeriod);
			if (NextFrame <= Now)
			{
				InOutCop.m_FrameOrigin = NextFrame;
				InOutCop.m_Cpc = 0;
				BeamCycle = InOutCop.m_FrameOrigin;
				++InOutCop.m_Restarts;
				continue;
			}
			InOutCop.m_NextFireCycle = NextFrame;
			return;
		}
		// The WAIT line field is display-row-relative on hardware (copper vcount
		// resets to paper line 0), while FrameOrigin is the frame interrupt a top
		// border above it - so add the border to land WAIT(line D) on display row
		// D and keep the interrupt/ISR up in the border. H granularity is 8 raw
		// hcount pixels + 12 into the line (see copper.h); PixelCycles = line/56
		// ~ 8 hcount, the +12 sub-pixel offset is not modelled here.
		const uint64_t Target = InOutCop.m_FrameOrigin + uint64_t(InOutCop.m_TopBorderLines + Line) * CyclesPerLine +
								uint64_t(H) * PixelCycles;
		if (Target > Now)
		{
			// This WAIT is in the future for the frame we're executing. But if the
			// CPU cycle has itself moved into a LATER frame (a halted CPU jumped a
			// whole frame past a WAIT-terminated, park-less list), mode 3 must
			// restart into that frame rather than stall on a stale deadline -
			// otherwise NextFireCycle lands behind Cpu.Cycles and the dispatcher
			// re-services us every instruction.
			if (InOutCop.m_bRestartEachFrame)
			{
				const uint64_t NextFrame = InOutMem.FrameOriginFor(
					InOutCop.m_FrameOrigin + FrameIntPeriod + FrameIntPeriod / 2, FrameIntPeriod);
				if (NextFrame <= Now)
				{
					InOutCop.m_FrameOrigin = NextFrame;
					InOutCop.m_Cpc = 0;
					BeamCycle = InOutCop.m_FrameOrigin;
					++InOutCop.m_Restarts;
					continue;
				}
			}
			// Future WAIT within the current frame - schedule and yield, leaving
			// Cpc on this WAIT.
			InOutCop.m_NextFireCycle = Target;
			return;
		}
		// Past means a LATER LINE: hardware matches vcount == line and hcount >= h,
		// so a beam already on the target line, past h, satisfies the WAIT at
		// once. Comparing raw cycles instead parked such a WAIT for a whole frame
		// whenever FrameOrigin moved by a few T-states between calls - it is
		// re-derived from the interrupt phase, sampled where a busy CPU accepts
		// the interrupt, so it jitters by up to an instruction - and a mode-3
		// list then skipped its line-0 MOVEs every few frames (a copper-split
		// tilemap scroll flashing to offset 0).
		const uint64_t TargetLineStart = Target - uint64_t(H) * PixelCycles;
		if (BeamCycle >= TargetLineStart + CyclesPerLine)
		{
			// The beam is already PAST this position in the copper's own
			// timeline (a WAIT for an earlier line/column than the last one
			// released). Hardware compares the counters for equality, so the
			// copper sits until the position comes round again NEXT frame. This
			// used to be consumed for free (and pulled BeamCycle backwards), so
			// a non-ascending list spun through the 4096-step guard on every
			// dispatcher visit - billions of copper MOVEs per emulated second
			// and a severe slowdown.
			const uint64_t NextFrame =
				InOutMem.FrameOriginFor(InOutCop.m_FrameOrigin + FrameIntPeriod + FrameIntPeriod / 2, FrameIntPeriod);
			if (InOutCop.m_bRestartEachFrame)
			{
				// Mode 3 restarts the list at the frame top anyway: park there,
				// or restart inline if that frame has already begun.
				if (NextFrame <= Now)
				{
					InOutCop.m_FrameOrigin = NextFrame;
					InOutCop.m_Cpc = 0;
					BeamCycle = InOutCop.m_FrameOrigin;
					++InOutCop.m_Restarts;
					continue;
				}
				InOutCop.m_NextFireCycle = NextFrame;
				return;
			}
			// Mode 2: the same WAIT, evaluated against the next frame's origin.
			InOutCop.m_FrameOrigin = NextFrame;
			BeamCycle = InOutCop.m_FrameOrigin;
			continue;
		}
		// Reached (or the copper is being caught up past it) - consume the WAIT,
		// advance the beam (never backwards: on its line past h it runs now),
		// and continue.
		++InOutCop.m_Waits;
		if (Target > BeamCycle)
		{
			BeamCycle = Target;
		}
		InOutCop.m_Cpc = uint16_t((InOutCop.m_Cpc + 1) & 0x03FF);
	}

	// Cap hit: a WAIT-less runaway still behind Cpu.Cycles (the GuardMax MOVEs
	// above already applied one-plus full scans, so every register it writes is
	// at its steady repeating value). Coalesce the remaining whole scans: jump
	// the copper to the modular position it would hold at Cpu.Cycles and apply
	// only that final partial scan, so the register values are current when the
	// CPU next reads them. Bounded at one list scan; a HALT fast-forward that
	// would otherwise replay a frame of MOVEs (which no running CPU observed)
	// becomes ~one scan. If a WAIT turns up in the partial scan the list was not
	// truly WAIT-less after all: fall through to the normal deadline logic.
	const uint64_t TotalInstr = (Now > InOutCop.m_FrameOrigin)
									? (Now - InOutCop.m_FrameOrigin) / CopInstrCycles
									: 0;
	const uint16_t TargetCpc = uint16_t(TotalInstr & 0x03FF);
	for (int Step = 0; Step < 1024 && InOutCop.m_Cpc != TargetCpc; ++Step)
	{
		const uint32_t B = uint32_t(InOutCop.m_Cpc & 0x03FF) * 2u;
		if (InOutCop.m_Mem2K[B] & 0x80)
		{
			break; // a WAIT: not WAIT-less; stop coalescing
		}
		if ((InOutCop.m_Mem2K[B] & 0x7F) != 0) // skip the NR $00 NOOP
		{
			InOutMem.WriteNextReg(uint8_t(InOutCop.m_Mem2K[B] & 0x7F), InOutCop.m_Mem2K[B + 1], 0xFFFF,
				Now, BinaryTrace::kNRSource_Copper);
		}
		++InOutCop.m_Moves;
		InOutCop.m_Cpc = uint16_t((InOutCop.m_Cpc + 1) & 0x03FF);
		if (!InOutCop.m_bRunning)
		{
			InOutCop.m_NextFireCycle = UINT64_MAX;
			return;
		}
	}
	// Resume one instruction-time ahead; a spinning CPU re-services promptly and
	// keeps seeing the register change, a halted one wakes on its own deadline.
	InOutCop.m_NextFireCycle = Now + CopInstrCycles;
}
