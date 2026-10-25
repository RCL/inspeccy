#include "replay.h"

#include <stdio.h>
#include <type_traits>

#include "bmp.h"
#include "dma.h"
#include "io.h"

// The checkpoint copies device objects by value; if any of them grows a member
// that cannot be copied (a FILE*, a unique_ptr), this is the compile-time tap
// on the shoulder to extend the checkpoint instead of silently slicing state.
static_assert(std::is_copy_assignable<Z80State>::value, "checkpoint copies Z80State");
static_assert(std::is_copy_assignable<SpritesState>::value, "checkpoint copies SpritesState");
static_assert(std::is_copy_assignable<ZxnDmaState>::value, "checkpoint copies ZxnDmaState");
static_assert(std::is_copy_assignable<AyState>::value, "checkpoint copies AyState");
static_assert(std::is_copy_assignable<TapePlayer>::value, "checkpoint copies TapePlayer");
static_assert(std::is_copy_assignable<InputState>::value, "checkpoint copies InputState");
static_assert(std::is_copy_assignable<Trdos>::value, "checkpoint copies Trdos");

namespace
{
struct ReplayBeamCtx
{
	BeamDrawer* m_Drawer = nullptr;
	MemoryBus* m_Mem = nullptr;
	SpritesState* m_Sprites = nullptr;
};
void ReplayBeamTrampoline(void* User, uint64_t Cycle)
{
	ReplayBeamCtx* C = static_cast<ReplayBeamCtx*>(User);
	C->m_Drawer->FlushTo(*C->m_Mem, C->m_Sprites, Cycle);
}
} // namespace

void FrameExact::OnFrameTick()
{
	++m_Ticks;
	if (m_Live.m_EveryNFrames > 1 && (m_Ticks % uint64_t(m_Live.m_EveryNFrames)) != 0)
	{
		return;
	}
	Save(m_Ck[m_Next]);
	m_Next ^= 1;
}

void FrameExact::Save(FrameExactCheckpoint& InOutC)
{
	if (!InOutC.m_Mem)
	{
		InOutC.m_Mem = std::make_unique<MemoryBus>();
	}
	if (!InOutC.m_Dma)
	{
		InOutC.m_Dma = std::make_unique<ZxnDmaState>();
	}
	InOutC.m_Cycles = m_Live.m_Cpu->m_Cycles;
	InOutC.m_Mem->AdoptForReplay(*m_Live.m_Mem);
	InOutC.m_Cpu = *m_Live.m_Cpu;
	if (m_Live.m_Sprites)
	{
		InOutC.m_Sprites = *m_Live.m_Sprites;
	}
	if (m_Live.m_Dma)
	{
		*InOutC.m_Dma = *m_Live.m_Dma;
	}
	if (m_Live.m_Ay)
	{
		InOutC.m_Ay = *m_Live.m_Ay;
	}
	if (m_Live.m_Ay2)
	{
		InOutC.m_Ay2 = *m_Live.m_Ay2;
	}
	if (m_Live.m_Tape)
	{
		InOutC.m_Tape = *m_Live.m_Tape;
	}
	if (m_Live.m_Input)
	{
		InOutC.m_Input = *m_Live.m_Input;
	}
	if (m_Live.m_Trd)
	{
		InOutC.m_Trd = *m_Live.m_Trd;
	}
	if (m_Live.m_Vfs)
	{
		InOutC.m_Vfs.CloneForReplay(*m_Live.m_Vfs);
	}
	InOutC.m_AyActive = m_Live.m_Io->m_AyActive;
	InOutC.m_CurrentMhz = m_Live.m_Io->m_CurrentMhz;
	InOutC.m_bValid = true;
}

bool FrameExact::ReplayAndWriteBmp(const char* Path, std::string& OutReason)
{
	const uint64_t Exit = m_Live.m_Cpu->m_Cycles;

	// Oldest usable checkpoint: the replay needs at least one WHOLE frame
	// between its start and the exit for the drawer to complete something.
	FrameExactCheckpoint* Ck = nullptr;
	for (FrameExactCheckpoint& C : m_Ck)
	{
		if (!C.m_bValid || C.m_Cycles >= Exit)
		{
			continue;
		}
		if (Ck == nullptr || C.m_Cycles < Ck->m_Cycles)
		{
			Ck = &C;
		}
	}
	if (Ck == nullptr)
	{
		OutReason = "no checkpoint (run shorter than the checkpoint cadence)";
		return false;
	}

	// ---- Build the scratch universe around the checkpoint's own objects. ----
	MemoryBus& RMem = *Ck->m_Mem;
	Z80State& RCpu = Ck->m_Cpu;
	RMem.SetCyclePtr(&RCpu.m_Cycles);
	RMem.SetCpuClockPtrs(&RCpu.m_Cycles, &RCpu.m_NextFrameIntCycle, &RCpu.m_FrameIntPeriod);
	RMem.RebindContentionClock(&RCpu.m_NextFrameIntCycle, &RCpu.m_FrameIntPeriod);

	// The eager drawer paints the NEXT composite; classic machines instead get
	// a snapshot at the last frame-interrupt boundary before the exit, written
	// through their own encoder (their compositors have no mid-frame model, so
	// the boundary snapshot IS the completed display frame).
	const bool bNextComposite = RMem.IsNext();

	BeamDrawer Drawer;
	ReplayBeamCtx BeamCtx{&Drawer, &RMem, m_Live.m_Sprites ? &Ck->m_Sprites : nullptr};
	if (bNextComposite)
	{
		Drawer.Configure(RCpu.m_FrameIntPeriod ? RCpu.m_FrameIntPeriod : RMem.GetFrameIntPeriodHint(),
			m_Live.m_LinesPerFrame, m_Live.m_TopBorderLines);
		RMem.SetBeamBoundaryHook(ReplayBeamTrampoline, &BeamCtx);
		RMem.SetBeamArmCycle(0);
	}
	RMem.SetFrameIntPeriodHint(RCpu.m_FrameIntPeriod);

	const IoContext& L = *m_Live.m_Io;
	IoContext RIo{RMem, m_Live.m_Trd ? &Ck->m_Trd : nullptr, m_Live.m_Input ? &Ck->m_Input : nullptr, Ck->m_Ay,
		nullptr /* PsgCapture */, (L.m_DotCmd != nullptr) ? &Ck->m_Vfs : nullptr,
		m_Live.m_Dma ? Ck->m_Dma.get() : nullptr, m_Live.m_Sprites ? &Ck->m_Sprites : nullptr, nullptr /* Video */,
		nullptr /* Beam: hook-driven, not emitted */, L.m_bAyEnabled, false /* input log */, L.m_bResetTrap,
		L.m_bLenientUndocEd, false /* trace drip */};
	RIo.m_Cpu = &RCpu;
	RIo.m_Tape = m_Live.m_Tape ? &Ck->m_Tape : nullptr;
	RIo.m_Ay2 = m_Live.m_Ay2 ? &Ck->m_Ay2 : nullptr;
	RIo.m_AyActive = Ck->m_AyActive;
	RIo.m_CurrentMhz = Ck->m_CurrentMhz;
	uint64_t RBudget = Exit;
	RIo.m_BudgetPtr = &RBudget;

	// Turbo inside the replay window: replicate the live callback's CPU-side
	// scaling (period, deadline re-anchor, INT window, hint). The budget stays
	// the absolute exit cycle - the live run's budget arithmetic already
	// resolved to it.
	RMem.m_OnTurboChangeUser = &RIo;
	RMem.m_OnTurboChange = [](void* User, double NewMhz)
	{
		IoContext& Io = *static_cast<IoContext*>(User);
		const double OldMhz = Io.m_CurrentMhz;
		if (NewMhz == OldMhz)
		{
			return;
		}
		Io.m_CurrentMhz = NewMhz;
		if (Io.m_Cpu->m_FrameIntPeriod > 0)
		{
			const uint64_t NewPeriod = uint64_t(double(Io.m_Cpu->m_FrameIntPeriod) * NewMhz / OldMhz + 0.5);
			Io.m_Cpu->m_FrameIntPeriod = NewPeriod;
			Io.m_Cpu->m_NextFrameIntCycle = Io.m_Cpu->m_Cycles + NewPeriod;
			Io.m_Cpu->m_IntWindow = uint64_t(double(Io.m_Cpu->m_IntWindow) * NewMhz / OldMhz + 0.5);
			Io.m_Mem.SetFrameIntPeriodHint(NewPeriod);
		}
	};
	RMem.m_OnDeadlineUser = &RIo;
	RMem.m_OnDeadline = [](void* User, uint64_t Cycle)
	{
		IoContext& Io = *static_cast<IoContext*>(User);
		if (Io.m_Cpu != nullptr && Cycle < Io.m_Cpu->m_NextEventCycle)
		{
			Io.m_Cpu->m_NextEventCycle = Cycle;
		}
	};

	bool bWrote = false;
	if (!bNextComposite)
	{
		// Stage 1: run to the last frame boundary at or before the exit, write
		// the machine's own encoder there, then finish the replay so the
		// self-check still compares against the live exit state.
		if (RCpu.m_FrameIntPeriod == 0 || RCpu.m_NextFrameIntCycle == UINT64_MAX || RCpu.m_NextFrameIntCycle > Exit)
		{
			OutReason = "no frame boundary inside the replay window";
			return false;
		}
		uint64_t Boundary = RCpu.m_NextFrameIntCycle +
							((Exit - RCpu.m_NextFrameIntCycle) / RCpu.m_FrameIntPeriod) * RCpu.m_FrameIntPeriod;
		RunUntil(RCpu, RIo, Boundary, nullptr);
		std::string Err;
		if (!WriteVisibleScreenBmp(RMem, RIo.m_Sprites, Path, Err))
		{
			OutReason = Err;
			return false;
		}
		bWrote = true; // on a later self-check failure main overwrites with the snapshot
		RBudget = Exit;
	}
	RunUntil(RCpu, RIo, RBudget, nullptr);

	// ---- Self-check: the replay must land exactly on the live exit state. ----
	const Z80State& Lv = *m_Live.m_Cpu;
	auto Mismatch = [&](const char* What, uint64_t A, uint64_t B)
	{
		char Buf[128];
		snprintf(Buf, sizeof(Buf), "replay diverged (%s: replay %llu vs live %llu)", What, (unsigned long long)A,
			(unsigned long long)B);
		OutReason = Buf;
		return false;
	};
	if (RCpu.m_Cycles != Lv.m_Cycles)
	{
		return Mismatch("cycles", RCpu.m_Cycles, Lv.m_Cycles);
	}
	if (RCpu.PC != Lv.PC)
	{
		return Mismatch("pc", RCpu.PC, Lv.PC);
	}
	if (RCpu.SP != Lv.SP)
	{
		return Mismatch("sp", RCpu.SP, Lv.SP);
	}
	if (RCpu.AF != Lv.AF)
	{
		return Mismatch("af", RCpu.AF, Lv.AF);
	}
	if (RCpu.BC != Lv.BC)
	{
		return Mismatch("bc", RCpu.BC, Lv.BC);
	}
	if (RCpu.DE != Lv.DE)
	{
		return Mismatch("de", RCpu.DE, Lv.DE);
	}
	if (RCpu.HL != Lv.HL)
	{
		return Mismatch("hl", RCpu.HL, Lv.HL);
	}
	if (RCpu.IX != Lv.IX)
	{
		return Mismatch("ix", RCpu.IX, Lv.IX);
	}
	if (RCpu.IY != Lv.IY)
	{
		return Mismatch("iy", RCpu.IY, Lv.IY);
	}
	if (RCpu.R != Lv.R)
	{
		return Mismatch("r", RCpu.R, Lv.R);
	}

	if (bWrote)
	{
		return true; // classic path already wrote at the frame boundary
	}
	// Complete the in-progress frame from the exit-cycle live state, UNLESS the
	// copper is running. A program that draws and RETs/halts mid-frame (a
	// fill-once demo, a dot command) leaves the drawer's beam stopped where its
	// last write flushed it - the rows the beam had not yet reached are still
	// blank, though final memory holds their pixels. Finish() paints that tail
	// from live state = the frame the display would have shown. When the copper
	// is running it drives those remaining scanlines with per-line register
	// values (NR $33/$32 scaling), which a single live snapshot would collapse -
	// so there we keep the last fully-scanned frame instead (the copper composed
	// it correctly, and it is the coprocessor's own last complete frame).
	if (!RMem.Copper().m_bRunning)
	{
		Drawer.Finish(RMem, RIo.m_Sprites);
	}
	if (!Drawer.HasCompletedFrame())
	{
		OutReason = "no frame completed inside the replay window";
		return false;
	}
	std::string Err;
	if (!Drawer.WriteBmp(Path, Err))
	{
		OutReason = Err;
		return false;
	}
	return true;
}
