// Regression coverage for live AY and DAC audio playback.
#include <stdio.h>
#include <vector>
#include "../../inspeccy-emul/src/ay_synth.h"
#include "../../inspeccy-emul/src/dac_stream.h"
int main()
{
	AyState Chip;
	Chip.m_Registers[0] = 111;
	Chip.m_Registers[7] = 0x3e;
	Chip.m_Registers[8] = 15;
	AySynth Synth;
	Synth.Init(48000);
	std::vector<int16_t> Samples(96000);
	Synth.Render(Chip, Samples.data(), 48000, false, false);
	int Rises = 0;
	for (int IdxSample = 1; IdxSample < 48000; ++IdxSample)
	{
		if (Samples[2 * IdxSample] && !Samples[2 * IdxSample - 2])
		{
			++Rises;
		}
		if (Samples[2 * IdxSample + 1])
		{
			return 1; // A is left only
		}
	}
	// 1773400 / (16 * 111) = 998.5 Hz, not an octave below.
	if (Rises < 997 || Rises > 1000)
	{
		return 2;
	}
	Chip.m_Registers[8] = 0;
	Chip.m_Registers[9] = 15;
	Chip.m_Registers[7] = 63;
	Samples.assign(20, 0);
	Synth.Render(Chip, Samples.data(), 10, false, false);
	if (!Samples[0] || Samples[0] != Samples[1])
	{
		return 3; // B centred in ABC
	}
	Samples.assign(20, 0);
	Synth.Render(Chip, Samples.data(), 10, true, false);
	if (Samples[0] || !Samples[1])
	{
		return 4; // B right in ACB
	}
	Samples.assign(20, 0);
	Synth.Render(Chip, Samples.data(), 10, true, true);
	if (!Samples[0] || Samples[0] != Samples[1])
	{
		return 5; // mono override
	}
	Synth.m_EnvPos = 7;
	++Chip.m_EnvelopeWrites;
	Synth.Render(Chip, Samples.data(), 0, false, false);
	if (Synth.m_EnvPos != 0)
	{
		return 6; // unchanged R13 still restarts
	}

	DacStream Dac;
	Dac.Write(9, 255, 25); // $DF: A and D
	Dac.Write(9, 0, 75);
	Samples.assign(8, 0);
	Dac.Mix(Samples, 4, 100);
	if (Samples != std::vector<int16_t>{0, 0, 4064, 4064, 4064, 4064, -4096, -4096})
	{
		return 7;
	}
	if (!Dac.m_Events.empty())
	{
		return 8;
	}
	// A write exactly at the preceding frame's end becomes the new hold.
	Dac.Write(1, 255, 100); // DAC A alone: the left side moves
	Samples.assign(4, 0);
	Dac.Mix(Samples, 2, 150);
	if (Samples != std::vector<int16_t>{4064, -4096, 4064, -4096})
	{
		return 9;
	}
	Dac.Write(9, 255, 150);
	Samples.assign(2, 32000);
	Dac.Mix(Samples, 1, 200);
	if (Samples[0] != 32767 || Samples[1] != 32767)
	{
		return 10;
	}
	puts("PASS: AY pitch, ABC/ACB/mono stereo and repeated envelope triggers");
	puts("PASS: timestamped DAC samples, stereo mix, held values and saturation");
}
