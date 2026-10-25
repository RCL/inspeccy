#include "copper.h"
#include "memory.h"

#include <cstdio>
#include <memory>

// A copper WAIT must match on its LINE, not on raw cycles. The copper's frame
// origin is re-derived from the frame-interrupt phase on every call, and that
// phase is sampled where the CPU accepts the interrupt - a busy CPU accepts it
// at the end of an instruction, so the phase jitters by a few T-states from
// frame to frame. The line-0 WAIT is scheduled under a phase 5 T-states late,
// then serviced under the true phase, so its target computes 5 T-states behind
// the beam the copper already reached. A strict cycle comparison called that
// "past" and parked the WAIT for the whole frame - the line-0 MOVEs (the
// scrolled tilemap offset) were skipped and the frame drew at offset 0.
static bool TestWaitJitter()
{
	auto Mem = std::make_unique<MemoryBus>();
	Mem->SetIsNext(true);
	CopperState& Cop = Mem->Copper();
	Cop.m_bPresent = true;
	Cop.m_LinesPerFrame = 312;
	Cop.m_TopBorderLines = 64;
	constexpr uint64_t Period = 559104; // Next 50 Hz at 28 MHz
	constexpr uint64_t Frame = 10 * Period;

	// Synthetic split with tilemap X
	// offset 0 above display line 0, 306 for lines 0..191, 0 again below.
	const uint8_t List[] = {0x2f, 0, 0x30, 0, 0x80, 0, 0x2f, 1, 0x30, 0x32, 0x80, 192, 0x2f, 0, 0x30, 0, 0xff, 0xff};
	Mem->WriteNextReg(0x62, 0x00);
	Mem->WriteNextReg(0x61, 0x00);
	for (uint8_t B : List)
	{
		Mem->WriteNextReg(0x60, B);
	}

	// The frame starts with the phase sampled 5 T-states late.
	Mem->SetFrameIntPhase(1005);
	const uint64_t Start = Frame + 1010;
	Mem->WriteNextReg(0x62, 0xc0, 0, Start, 0); // run, restart every frame
	StepCopperTo(Cop, *Mem, Start + 50, Period);
	const uint64_t Scheduled = Cop.m_NextFireCycle;
	if (Cop.m_Cpc != 2 || Scheduled == UINT64_MAX)
	{
		std::fprintf(stderr, "setup: expected the line-0 WAIT scheduled (cpc %u, next %llu)\n", Cop.m_Cpc,
			(unsigned long long)Scheduled);
		return false;
	}

	// The true phase is back by the time the WAIT comes due.
	// Serviced as the dispatcher does: only once its deadline is reached, so a
	// WAIT parked for the next frame is not visited again within this one.
	Mem->SetFrameIntPhase(1000);
	for (int Step = 0; Step < 8 && Cop.m_NextFireCycle < Scheduled + 64; ++Step)
	{
		StepCopperTo(Cop, *Mem, Cop.m_NextFireCycle, Period);
	}
	const unsigned Hi = Mem->GetNextReg(0x2f), Lo = Mem->GetNextReg(0x30);
	if (Hi != 1 || Lo != 0x32)
	{
		std::fprintf(stderr, "line-0 MOVEs skipped: NR $2F/$30 = %u/$%02X, want 1/$32 (cpc %u)\n", Hi, Lo,
			Cop.m_Cpc);
		return false;
	}
	return true;
}

int main()
{
	if (!TestWaitJitter())
	{
		return 1;
	}
	std::printf("copper wait jitter: ok\n");
	return 0;
}
