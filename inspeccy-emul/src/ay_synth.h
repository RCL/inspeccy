#pragma once
#include "ay.h"
#include <algorithm>
#include <cstdint>

// Frame-sampled AY synthesis for the SDL frontend.
struct AySynth
{
	uint32_t m_Rate = 44100;
	// counters run in AY clock / 8 units, 16.16 fixed point per sample
	uint32_t m_StepQ16 = 0;
	uint32_t m_ToneAcc[3] = {};
	uint32_t m_ToneCount[3] = {};
	bool m_ToneOut[3] = {};
	uint32_t m_NoiseAcc = 0;
	uint32_t m_NoiseCount = 0;
	uint32_t m_NoiseLfsr = 1;
	bool m_NoiseOut = false;
	uint32_t m_EnvCount = 0;
	int m_EnvPos = 0; // 0..15 within a run
	int m_EnvRun = 0; // runs completed: the alternate shapes flip on odd ones
	bool m_EnvHold = false;
	int m_EnvHoldLevel = 0;
	uint64_t m_LastEnvelopeWrite = UINT64_MAX;

	void Init(uint32_t Rate)
	{
		m_Rate = Rate;
		m_StepQ16 = uint32_t((uint64_t(1'773'400 / 8) << 16) / Rate);
	}

	static int Volume(int Level) // the 16 steps, roughly 1.5 dB apart, out of 4095
	{
		static const int Tab[16] = {0, 42, 61, 87, 127, 187, 260, 415, 543, 830, 1125, 1477, 1953, 2557, 3225, 4095};
		return Tab[Level & 15];
	}

	void Envelope(const AyState& Chip)
	{
		if (Chip.m_EnvelopeWrites != m_LastEnvelopeWrite) // a write to R13 restarts the envelope
		{
			m_LastEnvelopeWrite = Chip.m_EnvelopeWrites;
			m_EnvPos = 0;
			m_EnvRun = 0;
			m_EnvHold = false;
			m_EnvCount = 0;
		}
	}

	void EnvStep(uint8_t Shape)
	{
		if (m_EnvHold)
		{
			return;
		}
		if (++m_EnvPos <= 15)
		{
			return;
		}
		m_EnvPos = 0;
		++m_EnvRun;
		const bool bCont = Shape & 8, bAtt = Shape & 4, bAlt = Shape & 2, bHold = Shape & 1;
		if (!bCont)
		{
			m_EnvHold = true; // one run, then silence
			m_EnvHoldLevel = 0;
		}
		else if (bHold)
		{
			m_EnvHold = true; // one run, then the end level, flipped by ALT
			m_EnvHoldLevel = (bAtt != bAlt) ? 15 : 0;
		}
	}

	int EnvLevel(uint8_t Shape) const
	{
		if (m_EnvHold)
		{
			return m_EnvHoldLevel;
		}
		const bool bAtt = Shape & 4, bAlt = Shape & 2;
		const bool bUp = bAtt != (bAlt && (m_EnvRun & 1));
		return bUp ? m_EnvPos : 15 - m_EnvPos;
	}

	void Render(const AyState& Chip, int16_t* Out, int Frames, bool Acb, bool Mono)
	{
		const uint8_t* R = Chip.m_Registers;
		Envelope(Chip);
		const uint32_t Period[3] = {uint32_t(R[0] | (R[1] & 15) << 8), uint32_t(R[2] | (R[3] & 15) << 8),
			uint32_t(R[4] | (R[5] & 15) << 8)};
		const uint32_t NoisePeriod = R[6] & 31;
		const uint32_t EnvPeriod = uint32_t(R[11] | R[12] << 8);
		const uint8_t Mixer = R[7];
		const uint8_t Shape = R[13];
		for (int I = 0; I < Frames; ++I)
		{
			// tones and noise advance in clock/8 steps
			m_ToneAcc[0] += m_StepQ16;
			const uint32_t Ticks = m_ToneAcc[0] >> 16;
			m_ToneAcc[0] &= 0xFFFF;
			for (uint32_t T = 0; T < Ticks; ++T)
			{
				for (int Ch = 0; Ch < 3; ++Ch)
				{
					if (++m_ToneCount[Ch] >= std::max<uint32_t>(Period[Ch], 1))
					{
						m_ToneCount[Ch] = 0;
						m_ToneOut[Ch] = !m_ToneOut[Ch];
					}
				}
				if (++m_NoiseCount >= std::max<uint32_t>(NoisePeriod, 1) * 2)
				{
					m_NoiseCount = 0;
					const uint32_t Bit = (m_NoiseLfsr ^ (m_NoiseLfsr >> 3)) & 1;
					m_NoiseLfsr = (m_NoiseLfsr >> 1) | (Bit << 16);
					m_NoiseOut = m_NoiseLfsr & 1;
				}
				// the envelope steps every period x 2 clock/8 ticks
				if (++m_EnvCount >= std::max<uint32_t>(EnvPeriod, 1) * 2)
				{
					m_EnvCount = 0;
					EnvStep(Shape);
				}
			}
			int Left = 0, Right = 0;
			for (int Ch = 0; Ch < 3; ++Ch)
			{
				const bool bToneOn = !(Mixer & (1 << Ch)) ? m_ToneOut[Ch] : true;
				const bool bNoiseOn = !(Mixer & (8 << Ch)) ? m_NoiseOut : true;
				if (bToneOn && bNoiseOn)
				{
					const uint8_t Vol = R[8 + Ch];
					const int Level = Volume((Vol & 16) ? EnvLevel(Shape) : (Vol & 15));
					// Next ABC: A+B left, B+C right. ACB swaps B and C;
					// NR09 can instead route each whole chip to both outputs.
					if (Mono || Ch == 0 || Ch == (Acb ? 2 : 1)) Left += Level;
					if (Mono || Ch != 0) Right += Level;
				}
			}
			// Headroom for nine simultaneous full-volume channels, even mono.
			Out[I * 2 + 0] = int16_t(std::clamp(Out[I * 2 + 0] + Left * 3 / 4, -32768, 32767));
			Out[I * 2 + 1] = int16_t(std::clamp(Out[I * 2 + 1] + Right * 3 / 4, -32768, 32767));
		}
	}
};

