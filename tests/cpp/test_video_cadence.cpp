// Regression coverage for live and recorded video capture cadence.
#include <stdio.h>
#include <memory>
#include <vector>
#include "ay.h"
#include "bmp.h"
#include "io.h"
#include "memory.h"
#include "video.h"
#include "z80.h"

// HALT must not jump over a capture deadline. A silent display must be
// flushed before the sink sees it. Splitting RunUntil must keep both facts.
static bool Check(bool bSegmented)
{
	auto Mem = std::make_unique<MemoryBus>();
	Mem->SetIsNext(true);
	Mem->WriteNextReg(0x54, 32);
	Mem->WriteNextReg(0x68, 0x80);
	Mem->WriteNextReg(0x4a, 0xe0);
	Mem->Wr(0x8000, 0x76); // HALT with an interrupt later than all captures
	Z80State Cpu;
	Cpu.PC = 0x8000;
	Cpu.IFF1 = Cpu.IFF2 = 1;
	EnableFrameInterrupts(Cpu, 1000);
	Mem->SetFrameIntPeriodHint(100);
	Mem->SetBeamArmCycle(0);
	BeamDrawer Beam;
	Beam.Configure(100, 10, 2);
	struct Hook
	{
		MemoryBus& m_Mem;
		BeamDrawer& m_Beam;
	} BoundaryHook{*Mem, Beam};
	Mem->SetBeamBoundaryHook(
		[](void* InOutUser, uint64_t Cycle)
		{
			auto& Boundary = *static_cast<Hook*>(InOutUser);
			Boundary.m_Beam.FlushTo(Boundary.m_Mem, nullptr, Cycle);
		},
		&BoundaryHook);
	Beam.FlushTo(*Mem, nullptr, 0);
	AyState Ay;
	VideoCapture Video;
	Video.m_PeriodCycles = 100;
	Video.m_NextEmitCycle = 100;
	struct Sink
	{
		Z80State& m_Cpu;
		unsigned int m_Seen = 0;
		bool m_bOk = true;
	} CaptureSink{Cpu};
	Video.m_OnFrameUser = &CaptureSink;
	Video.m_OnFrame = [](void* InOutUser, const MemoryBus&, const SpritesState*, const BeamDrawer* InBeam)
	{
		auto& Capture = *static_cast<Sink*>(InOutUser);
		++Capture.m_Seen;
		Capture.m_bOk &= Capture.m_Cpu.m_Cycles == Capture.m_Seen * 100 && InBeam->HasCompletedFrame();
		std::vector<uint8_t> Bgr(320 * 256 * 3);
		Capture.m_bOk &= InBeam->CopyCaptureBgr(Bgr.data(), 960);
		const int Centre = (128 * 320 + 160) * 3;
		Capture.m_bOk &= Bgr[Centre] == 0 && Bgr[Centre + 1] == 0 && Bgr[Centre + 2] == 255;
	};
	IoContext Io{*Mem, nullptr, nullptr, Ay, nullptr, nullptr, nullptr, nullptr, &Video, &Beam, true, false, false};
	Io.m_Cpu = &Cpu;
	if (bSegmented)
	{
		for (uint64_t End = 101; End <= 801; End += 100)
		{
			uint64_t Budget = End;
			RunUntil(Cpu, Io, Budget, nullptr);
		}
	}
	uint64_t Budget = 850;
	RunUntil(Cpu, Io, Budget, nullptr);
	if (!CaptureSink.m_bOk || CaptureSink.m_Seen != 8 || Video.m_FrameCounter != 8)
	{
		fprintf(
			stderr, "cadence failed: segmented=%d seen=%u ok=%d\n", bSegmented, CaptureSink.m_Seen, CaptureSink.m_bOk);
		return false;
	}
	return true;
}

int main()
{
	if (!Check(false) || !Check(true))
	{
		return 1;
	}
	puts("PASS: HALT capture deadlines, quiet beam flush, contiguous/split runs");
}
