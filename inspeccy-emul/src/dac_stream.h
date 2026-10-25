#pragma once
// Timestamped DAC playback for the live frontend.
#include <assert.h>
#include <stdint.h>
#include <algorithm>
#include <vector>

/** Mix one frame of unsigned 8-bit writes to the Next's four DACs (A and B
 *  left, C and D right), preserving the held voltages across frames. */
struct DacStream
{
	struct Event
	{
		uint64_t m_Ns;
		uint8_t m_Mask; // bit 0..3 = DAC A..D
		uint8_t m_Value;
	};
	std::vector<Event> m_Events;
	int m_Hold[4] = {0, 0, 0, 0};
	uint64_t m_RenderedUntil = 0;

	/** Queue a register write on the turbo-independent wall clock. */
	void Write(uint8_t Mask, uint8_t Value, uint64_t Ns)
	{
		m_Events.push_back({Ns, Mask, Value});
	}

	/** Add the queued voltages to an existing interleaved stereo PCM buffer. */
	void Mix(std::vector<int16_t>& InOutStereo, int Samples, uint64_t Until)
	{
		assert(Samples >= 0 && InOutStereo.size() >= size_t(Samples) * 2);
		size_t IdxEvent = 0;
		for (int IdxSample = 0; IdxSample < Samples; ++IdxSample)
		{
			const uint64_t Ns = m_RenderedUntil + (Until - m_RenderedUntil) * uint64_t(IdxSample) / uint64_t(Samples);
			while (IdxEvent < m_Events.size() && m_Events[IdxEvent].m_Ns <= Ns)
			{
				Hold(m_Events[IdxEvent]);
				++IdxEvent;
			}
			// same fixed DAC gain as the native WAV renderer
			const int Side[2] = {(m_Hold[0] + m_Hold[1]) * 32, (m_Hold[2] + m_Hold[3]) * 32};
			for (int IdxChannel = 0; IdxChannel < 2; ++IdxChannel)
			{
				InOutStereo[IdxSample * 2 + IdxChannel] =
					int16_t(std::clamp(int(InOutStereo[IdxSample * 2 + IdxChannel]) + Side[IdxChannel], -32768, 32767));
			}
		}
		// retain writes after the final sample for the next frame's hold
		while (IdxEvent < m_Events.size())
		{
			Hold(m_Events[IdxEvent]);
			++IdxEvent;
		}
		m_Events.clear();
		m_RenderedUntil = Until;
	}

  private:
	void Hold(const Event& E)
	{
		for (int IdxDac = 0; IdxDac < 4; ++IdxDac)
		{
			if (E.m_Mask & (1 << IdxDac))
			{
				m_Hold[IdxDac] = int(E.m_Value) - 128;
			}
		}
	}
};
