// Regression coverage for black transparency fallback and full-ink borders.
#include <stdio.h>
#include <memory>
#include <vector>
#include "bmp.h"
#include "memory.h"

static bool Check(bool bWide, bool bLoRes, bool bFullInk, unsigned char Fallback)
{
	auto Mem = std::make_unique<MemoryBus>();
	Mem->SetIsNext(true);
	Mem->SetBorderColour(0);
	Mem->WriteNextReg(0x43, 1);
	Mem->WriteNextReg(0x40, 128);
	Mem->WriteNextReg(0x41, 0x80); // an unmistakable red border palette entry
	Mem->WriteNextReg(0x42, bFullInk ? 255 : 7);
	Mem->WriteNextReg(0x14, 0); // black pixels in both layers are transparent
	Mem->WriteNextReg(0x4A, Fallback);
	Mem->WriteNextReg(0x12, 16);
	Mem->WriteNextReg(0x69, 0x80);
	Mem->WriteNextReg(0x70, bWide ? 0x10 : 0);
	Mem->WriteNextReg(0x15, bLoRes ? 0x80 : 0);
	Mem->WriteNextReg(0x6A, 0x20);
	Mem->WriteNextReg(0x68, bLoRes ? 0 : 0x80);

	std::vector<unsigned char> Frame(320 * 256 * 3);
	RenderCompositeBgr(*Mem, nullptr, Frame.data(), 960);
	for (int IdxRow = 0; IdxRow < 256; ++IdxRow)
	{
		for (int IdxCol = 0; IdxCol < 320; ++IdxCol)
		{
			const bool bPaper = bWide || (IdxCol >= 32 && IdxCol < 288 && IdxRow >= 32 && IdxRow < 224);
			const bool bFallback = bPaper || bFullInk;
			const unsigned char Red = bFallback ? 0 : 146;
			const unsigned char Blue = bFallback && Fallback == 3 ? 255 : 0;
			const size_t IdxPixel = size_t(IdxRow * 320 + IdxCol) * 3;
			if (Frame[IdxPixel] != Blue || Frame[IdxPixel + 1] != 0 || Frame[IdxPixel + 2] != Red)
			{
				fprintf(stderr, "fallback mismatch: wide=%d lores=%d fullink=%d fallback=%u pixel=%d,%d\n", bWide,
					bLoRes, bFullInk, Fallback, IdxCol, IdxRow);
				return false;
			}
		}
	}

	constexpr unsigned long long kPeriod = 312 * 224;
	Mem->SetFrameIntPeriodHint(kPeriod);
	BeamDrawer Beam;
	Beam.Configure(kPeriod, 312, 64);
	Beam.FlushTo(*Mem, nullptr, 0);
	Beam.FlushTo(*Mem, nullptr, kPeriod);
	std::vector<unsigned char> Captured(Frame.size());
	if (!Beam.CopyCompletedBgr(Captured.data(), 960) || Captured != Frame)
	{
		fprintf(stderr, "fallback beam/snapshot mismatch\n");
		return false;
	}
	return true;
}

int main()
{
	for (bool bWide : {false, true})
	{
		for (bool bLoRes : {false, true})
		{
			for (bool bFullInk : {false, true})
			{
				for (unsigned char Fallback : {0, 3})
				{
					if (!Check(bWide, bLoRes, bFullInk, Fallback))
					{
						return 1;
					}
				}
			}
		}
	}
	puts("PASS: zero/nonzero fallback, full-ink borders, wide/narrow L2, LoRes, snapshot and beam");
	return 0;
}
