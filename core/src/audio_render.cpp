#include "audio_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

// Measured 16-level AY output DAC, 16-bit full scale - the table used across
// Spectrum emulators (x128 lineage). Index = volume level 0..15.
constexpr uint16_t kAyVolume[16] = {
	0x0000,
	0x0340,
	0x04C0,
	0x06F2,
	0x0A44,
	0x0F13,
	0x1510,
	0x227E,
	0x289F,
	0x414E,
	0x5B21,
	0x7258,
	0x905E,
	0xB550,
	0xD7A0,
	0xFFFF,
};

// One AY chip, stepped at ChipClock/8 with integer state only.
//  - tone toggles every max(TP,1) steps        -> square at clock/(16*TP)
//  - noise LFSR advances every 2*max(NP,1)     -> clocked at clock/(16*NP)
//  - envelope advances every 2*max(EP,1) steps -> stepped at clock/(16*EP)
struct AyChip
{
	uint8_t m_Regs[16] = {};
	uint32_t m_ToneCnt[3] = {0, 0, 0};
	uint8_t m_ToneOut[3] = {0, 0, 0};
	uint32_t m_NoiseCnt = 0;
	uint32_t m_Lfsr = 1; // 17-bit, nonzero seed
	uint8_t m_NoiseOut = 0;
	uint32_t m_EnvCnt = 0;
	int32_t m_EnvLevel = 0;
	int32_t m_EnvDir = -1;
	bool m_bEnvHold = true;
	bool m_bAnyWrite = false;

	uint32_t TonePeriod(int C) const
	{
		const uint32_t P = uint32_t(m_Regs[C * 2]) | (uint32_t(m_Regs[C * 2 + 1] & 0x0F) << 8);
		return P ? P : 1;
	}
	uint32_t NoisePeriod() const
	{
		const uint32_t P = m_Regs[6] & 0x1F;
		return P ? P : 1;
	}
	uint32_t EnvPeriod() const
	{
		const uint32_t P = uint32_t(m_Regs[11]) | (uint32_t(m_Regs[12]) << 8);
		return P ? P : 1;
	}

	void Write(uint8_t Reg, uint8_t Value)
	{
		m_Regs[Reg & 0x0F] = Value;
		m_bAnyWrite = true;
		if ((Reg & 0x0F) == 13)
		{
			// Envelope shape write RETRIGGERS, even with an unchanged value.
			const uint8_t S = Value & 0x0F;
			m_EnvCnt = 0;
			m_EnvDir = (S & 0x04) ? +1 : -1; // ATTACK
			m_EnvLevel = (S & 0x04) ? 0 : 15;
			m_bEnvHold = false;
		}
	}

	void EnvStep()
	{
		if (m_bEnvHold)
		{
			return;
		}
		m_EnvLevel += m_EnvDir;
		if (m_EnvLevel >= 0 && m_EnvLevel <= 15)
		{
			return;
		}
		const uint8_t S = m_Regs[13] & 0x0F;
		if (!(S & 0x08))
		{
			// Shapes 0..7: one ramp, then silence forever.
			m_EnvLevel = 0;
			m_bEnvHold = true;
			return;
		}
		if (S & 0x01)
		{
			// HOLD: park at this end (ALTERNATE picks the opposite end).
			const bool bAlt = (S & 0x02) != 0;
			m_EnvLevel = ((m_EnvDir > 0) != bAlt) ? 15 : 0;
			m_bEnvHold = true;
			return;
		}
		if (S & 0x02)
		{
			// ALTERNATE: triangle - reverse and step back inside the range.
			m_EnvDir = -m_EnvDir;
			m_EnvLevel += 2 * m_EnvDir;
		}
		else
		{
			// Sawtooth: wrap to the far end.
			m_EnvLevel = (m_EnvDir > 0) ? 0 : 15;
		}
	}

	// One chip step (8 chip clocks). Returns the three channel amplitudes.
	void Step(uint16_t OutAmp[3])
	{
		for (int C = 0; C < 3; ++C)
		{
			if (++m_ToneCnt[C] >= TonePeriod(C))
			{
				m_ToneCnt[C] = 0;
				m_ToneOut[C] ^= 1;
			}
		}
		if (++m_NoiseCnt >= 2 * NoisePeriod())
		{
			m_NoiseCnt = 0;
			// 17-bit LFSR, feedback bit0 ^ bit3 into bit16 (shift right).
			const uint32_t Fb = ((m_Lfsr ^ (m_Lfsr >> 3)) & 1u);
			m_Lfsr = (m_Lfsr >> 1) | (Fb << 16);
			m_NoiseOut = uint8_t(m_Lfsr & 1u);
		}
		if (++m_EnvCnt >= 2 * EnvPeriod())
		{
			m_EnvCnt = 0;
			EnvStep();
		}
		const uint8_t Mixer = m_Regs[7];
		for (int C = 0; C < 3; ++C)
		{
			// R7 enables are ACTIVE LOW: a set bit disables the source,
			// which for the AND-mix means "force 1".
			const uint8_t T = (Mixer & (1u << C)) ? 1 : m_ToneOut[C];
			const uint8_t N = (Mixer & (1u << (C + 3))) ? 1 : m_NoiseOut;
			const uint8_t On = T & N;
			const uint8_t VolReg = m_Regs[8 + C];
			const int32_t Level = (VolReg & 0x10) ? m_EnvLevel : (VolReg & 0x0F);
			OutAmp[C] = On ? kAyVolume[Level & 0x0F] : 0;
		}
	}
};

// Exact integer idx * 1e9 / Hz without 128-bit math:
// split into (idx / Hz) whole seconds + remainder scaled.
inline uint64_t IdxToNanos(uint64_t Idx, uint32_t Hz)
{
	return (Idx / Hz) * 1'000'000'000ULL + (Idx % Hz) * 1'000'000'000ULL / Hz;
}

void Append4(std::vector<uint8_t>& InOutV, const char* S)
{
	InOutV.insert(InOutV.end(), S, S + 4);
}
void WriteU32LE(std::vector<uint8_t>& InOutV, uint32_t X)
{
	InOutV.push_back(uint8_t(X));
	InOutV.push_back(uint8_t(X >> 8));
	InOutV.push_back(uint8_t(X >> 16));
	InOutV.push_back(uint8_t(X >> 24));
}
void WriteU16LE(std::vector<uint8_t>& InOutV, uint16_t X)
{
	InOutV.push_back(uint8_t(X));
	InOutV.push_back(uint8_t(X >> 8));
}

inline int16_t ClampS16(int32_t X)
{
	if (X > 32767)
	{
		return 32767;
	}
	if (X < -32768)
	{
		return -32768;
	}
	return int16_t(X);
}

} // namespace

AudioRenderer::AudioRenderer(std::string OutPath, uint32_t RateHz, StereoMode StereoIn)
	: m_Path(std::move(OutPath)), m_Rate(RateHz ? RateHz : 44100), m_Mode(StereoIn)
{
	m_Events.reserve(1u << 16);
}

uint32_t AudioRenderer::DacDerivedRateHz() const
{
	if (m_DacWrites < 2 || m_LastDacNanos <= m_FirstDacNanos)
	{
		return 0;
	}
	const uint64_t SpanNanos = m_LastDacNanos - m_FirstDacNanos;
	// (writes - 1) intervals over the span.
	return uint32_t(((m_DacWrites - 1) * 1'000'000'000ULL + SpanNanos / 2) / SpanNanos);
}

std::vector<uint8_t> AudioRenderer::PackEvents() const
{
	std::vector<uint8_t> Out;
	Out.reserve(m_Events.size() * 11);
	for (const Event& E : m_Events)
	{
		for (int B = 0; B < 8; ++B)
		{
			Out.push_back(uint8_t(E.m_Nanos >> (B * 8)));
		}
		Out.push_back(E.m_Src);
		Out.push_back(E.m_A);
		Out.push_back(E.m_B);
	}
	return Out;
}

bool AudioRenderer::RestoreFromDump(const std::vector<uint8_t>& PackedEvents, uint64_t AyWrites0, uint64_t AyWrites1,
	uint64_t DacWrites, uint64_t BeeperEdges, uint64_t FirstDacNanos, uint64_t LastDacNanos,
	uint64_t SamplesAlreadyWritten)
{
	if (PackedEvents.size() % 11 != 0)
	{
		return false;
	}
	m_Events.clear();
	m_Events.reserve(PackedEvents.size() / 11);
	for (size_t I = 0; I < PackedEvents.size(); I += 11)
	{
		Event E{};
		for (int B = 0; B < 8; ++B)
		{
			E.m_Nanos |= uint64_t(PackedEvents[I + B]) << (B * 8);
		}
		E.m_Src = PackedEvents[I + 8];
		E.m_A = PackedEvents[I + 9];
		E.m_B = PackedEvents[I + 10];
		m_Events.push_back(E);
	}
	m_AyWrites[0] = AyWrites0;
	m_AyWrites[1] = AyWrites1;
	m_DacWrites = DacWrites;
	m_BeeperEdges = BeeperEdges;
	m_FirstDacNanos = FirstDacNanos;
	m_LastDacNanos = LastDacNanos;
	m_Samples = SamplesAlreadyWritten;
	m_SkipSamples = SamplesAlreadyWritten;
	return true;
}

bool AudioRenderer::Finalise(uint64_t TotalNanos, uint32_t AyClockHz, std::string& OutError)
{
	OutError.clear();
	if (m_Events.empty())
	{
		return true; // nothing audible ever happened; no file (observable via counters)
	}

	if (getenv("INSPECCY_AUDIO_DEBUG") != nullptr)
	{
		fprintf(stderr, "audio: %zu events, TotalNanos=%llu, AyClockHz=%u\n", m_Events.size(),
			(unsigned long long)TotalNanos, AyClockHz);
		for (size_t I = 0; I < m_Events.size() && I < 8; ++I)
		{
			fprintf(stderr, "  ev[%zu] nanos=%llu src=%d a=%u b=%u\n", I, (unsigned long long)m_Events[I].m_Nanos,
				int(m_Events[I].m_Src), m_Events[I].m_A, m_Events[I].m_B);
		}
	}
	// Diagnostic: dump every AY register write (nanos chip reg val) to a file,
	// for comparing the actual emulated register stream against a reference.
	if (const char* DumpPath = getenv("INSPECCY_AY_DUMP"))
	{
		if (FILE* DF = fopen(DumpPath, "wb"))
		{
			for (const Event& E : m_Events)
			{
				if (E.m_Src == kSrcAy0Reg || E.m_Src == kSrcAy1Reg)
				{
					fprintf(DF, "%llu %d %u %u\n", (unsigned long long)E.m_Nanos, E.m_Src == kSrcAy1Reg ? 1 : 0, E.m_A,
						E.m_B);
				}
			}
			fclose(DF);
		}
	}

	// Per-channel stereo weights, x256. ABC: A left, B centre, C right.
	int32_t WL[3] = {256, 128, 0};
	int32_t WR[3] = {0, 128, 256};
	if (m_Mode == kStereoAcb)
	{
		WL[1] = 0;
		WL[2] = 128;
		WR[1] = 256;
		WR[2] = 128;
	}
	else if (m_Mode == kStereoMono)
	{
		WL[0] = WL[1] = WL[2] = 128;
		WR[0] = WR[1] = WR[2] = 128;
	}
	const bool bStereo = (m_Mode != kStereoMono);
	const int Channels = bStereo ? 2 : 1;

	// Fixed output scaling (documented, never auto-gain):
	//   AY: per-side weighted sum max = 1.5 * 65535 per chip; >> 4 with two
	//       chips gives <= 12287 each, 24575 total.
	//   DAC: (byte - 128) << 5 per DAC -> +-4096 each, +-8192 a side
	//        (A + B left, C + D right).
	//   Beeper: +-2048.
	// Worst case ~34.8k clips (documented); realistic content never does.
	const uint32_t StepHz = AyClockHz / 8;

	const uint64_t NumSamples =
		(TotalNanos / 1'000'000'000ULL) * m_Rate + (TotalNanos % 1'000'000'000ULL) * m_Rate / 1'000'000'000ULL;
	if (NumSamples == 0)
	{
		return true;
	}

	AyChip Chips[2];
	int32_t DacHold[4] = {0, 0, 0, 0}; // A..D, centred (-128..127) after first write
	auto HoldDacs = [&DacHold](uint8_t Mask, uint8_t Sample)
	{
		for (int IdxDac = 0; IdxDac < 4; ++IdxDac)
		{
			if (Mask & (1 << IdxDac))
			{
				DacHold[IdxDac] = int32_t(Sample) - 128;
			}
		}
	};
	int32_t Beeper = 0; // 0 / 1

	std::vector<int16_t> Pcm;
	Pcm.reserve(size_t(NumSamples > m_SkipSamples ? NumSamples - m_SkipSamples : 0) * Channels);

	size_t Ev = 0;
	uint64_t StepIdx = 0;
	// Rolling per-sample AY accumulation.
	int64_t LastAyL = 0, LastAyR = 0;

	// AY DC blocker. The AY DAC is unipolar (kAyVolume is 0..full-scale), so a
	// steady tone sits on a large positive offset that varies with the overall
	// level. Real hardware AC-couples the output, and reference renderers (e.g.
	// Ayumi) remove this DC; without it the offset dominates the sub-30 Hz band,
	// wrecks spectral fidelity and steals headroom. One-pole high-pass at ~5 Hz,
	// per side, applied to the AY component only (the DAC and beeper paths are
	// already centred).
	const double DcPole = 1.0 - (2.0 * 3.14159265358979323846 * 5.0 / double(m_Rate));
	double DcInL = 0.0, DcOutL = 0.0, DcInR = 0.0, DcOutR = 0.0;

	for (uint64_t S = 0; S < NumSamples; ++S)
	{
		const uint64_t SampleEnd = IdxToNanos(S + 1, m_Rate);
		int64_t AccL = 0, AccR = 0;
		uint32_t StepsRun = 0;

		while (IdxToNanos(StepIdx, StepHz) < SampleEnd)
		{
			const uint64_t StepNanos = IdxToNanos(StepIdx, StepHz);
			while (Ev < m_Events.size() && m_Events[Ev].m_Nanos <= StepNanos)
			{
				const Event& E = m_Events[Ev++];
				switch (E.m_Src)
				{
				case kSrcAy0Reg:
					Chips[0].Write(E.m_A, E.m_B);
					break;
				case kSrcAy1Reg:
					Chips[1].Write(E.m_A, E.m_B);
					break;
				case kSrcDacDf:
					HoldDacs(9, E.m_A);
					break;
				case kSrcDac1f:
					HoldDacs(1, E.m_A);
					break;
				case kSrcDacMask:
					HoldDacs(E.m_B, E.m_A);
					break;
				case kSrcBeeper:
					Beeper = E.m_A & 1;
					break;
				}
			}
			for (AyChip& Chip : Chips)
			{
				uint16_t Amp[3];
				Chip.Step(Amp);
				for (int C = 0; C < 3; ++C)
				{
					AccL += int64_t(Amp[C]) * WL[C];
					AccR += int64_t(Amp[C]) * WR[C];
				}
			}
			++StepIdx;
			++StepsRun;
		}
		// Late events inside the tail of the sample window (sub-step) apply
		// before the next sample's first step; nothing to do here.

		if (StepsRun > 0)
		{
			LastAyL = (AccL / StepsRun) >> (8 + 4); // /256 weight scale, /16 headroom
			LastAyR = (AccR / StepsRun) >> (8 + 4);
		}
		// AC-couple the AY: y[n] = x[n] - x[n-1] + pole*y[n-1].
		const double AyAcL = double(LastAyL) - DcInL + DcPole * DcOutL;
		DcInL = double(LastAyL);
		DcOutL = AyAcL;
		const double AyAcR = double(LastAyR) - DcInR + DcPole * DcOutR;
		DcInR = double(LastAyR);
		DcOutR = AyAcR;

		int32_t DacL = (DacHold[0] + DacHold[1]) << 5;
		int32_t DacR = (DacHold[2] + DacHold[3]) << 5;
		if (!bStereo)
		{
			DacL = (DacL + DacR) / 2; // mono writes L alone
		}
		const int32_t Beep = Beeper ? 2048 : 0;
		const int32_t L = int32_t(AyAcL) + DacL + Beep;
		const int32_t R = int32_t(AyAcR) + DacR + Beep;
		if (S < m_SkipSamples)
		{
			continue; // the resumed-from run wrote it
		}
		Pcm.push_back(ClampS16(L));
		if (bStereo)
		{
			Pcm.push_back(ClampS16(R));
		}
	}

	// WAV container (16-bit PCM).
	const uint32_t DataBytes = uint32_t(Pcm.size() * 2);
	std::vector<uint8_t> Wav;
	Wav.reserve(44 + DataBytes);
	Append4(Wav, "RIFF");
	WriteU32LE(Wav, 36 + DataBytes);
	Append4(Wav, "WAVE");
	Append4(Wav, "fmt ");
	WriteU32LE(Wav, 16);
	WriteU16LE(Wav, 1); // PCM
	WriteU16LE(Wav, uint16_t(Channels));
	WriteU32LE(Wav, m_Rate);
	WriteU32LE(Wav, m_Rate * Channels * 2); // byte rate
	WriteU16LE(Wav, uint16_t(Channels * 2)); // block align
	WriteU16LE(Wav, 16); // bits per sample
	Append4(Wav, "data");
	WriteU32LE(Wav, DataBytes);
	const uint8_t* P = reinterpret_cast<const uint8_t*>(Pcm.data());
	Wav.insert(Wav.end(), P, P + DataBytes);

	FILE* F = fopen(m_Path.c_str(), "wb");
	if (F == nullptr)
	{
		OutError = "cannot open audio output file: " + m_Path;
		return false;
	}
	const size_t Written = fwrite(Wav.data(), 1, Wav.size(), F);
	fclose(F);
	if (Written != Wav.size())
	{
		OutError = "short write on audio output file: " + m_Path;
		return false;
	}
	m_Samples = NumSamples;
	m_bWrote = true;
	return true;
}
