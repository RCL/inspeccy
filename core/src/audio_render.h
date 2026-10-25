#pragma once
// Unified audio render (--audio-out): every sound source the machine has -
// both AY chips, the Next's four DACs (A and B left, C and D right; $DF
// always live, the other ports behind NR_08 bit 3),
// the beeper - logged as wall-nanos events during the run and rendered
// offline into ONE WAV at Finalise().
//
// Design rules (see doc plan):
//  - Events are timestamped in emulated WALL nanos (the IoContext Q20
//    integer clock), so turbo flips never bend pitch.
//  - The AY synth is integer-only and event-application is chip-step
//    granular (~4.5 us at 1.77 MHz/8) => renders are byte-identical across
//    runs and platforms, so tests can pin hashes.
//  - Fixed, documented output scaling - never auto-gain - so two renders of
//    related runs are directly comparable.

#include <stdint.h>
#include <string>
#include <vector>

/** Which of the Next's DACs (bit 0..3 = A..D) a write to this low port byte
 *  reaches; 0 = none. ports.txt: A and B are heard on the left, C and D on
 *  the right. $DF answers whatever NR_08 says, as it always has here. */
inline uint8_t NextDacMask(uint8_t PortLo, bool bDacsEnabled)
{
	constexpr uint8_t A = 1, B = 2, C = 4, D = 8;
	if (PortLo == 0xDF)
	{
		return A | D; // Specdrum
	}
	if (!bDacsEnabled)
	{
		return 0;
	}
	switch (PortLo)
	{
	case 0x1F: // Soundrive 1
	case 0xF1: // Soundrive 2
	case 0x3F: // Covox
		return A;
	case 0x0F:
	case 0xF3:
		return B;
	case 0x4F:
	case 0xF9:
		return C;
	case 0x5F:
		return D;
	case 0xB3: // GS Covox
		return B | C;
	case 0xFB: // Pentagon/ATM
		return A | D;
	default:
		return 0;
	}
}

struct AudioRenderer final
{
  public:
	enum Source : uint8_t
	{
		kSrcAy0Reg = 0, // A = register 0..15, B = value (post write-mask)
		kSrcAy1Reg = 1, // second TurboSound chip
		kSrcDacDf = 2, // A = sample byte, DACs A and D (port $DF)
		kSrcDac1f = 3, // A = sample byte, DAC A (port $1F)
		kSrcBeeper = 4, // A = EAR bit (0/1), from $FE bit 4
		kSrcDacMask = 5, // A = sample byte, B = the DACs (bit 0..3 = A..D)
	};

	enum StereoMode : uint8_t
	{
		kStereoAbc = 0, // A left, B centre, C right (default)
		kStereoAcb = 1, // A left, C centre, B right
		kStereoMono = 2,
	};

	AudioRenderer(std::string OutPath, uint32_t RateHz, StereoMode Mode);

	// Hot-path appends (called from the I/O dispatcher; renderer non-null
	// only when --audio-out was given).
	void OnAyWrite(int Chip, uint8_t Reg, uint8_t Value, uint64_t WallNanos)
	{
		m_Events.push_back({WallNanos, Chip ? kSrcAy1Reg : kSrcAy0Reg, Reg, Value});
		++m_AyWrites[Chip ? 1 : 0];
	}
	void OnDacWrite(uint8_t Mask, uint8_t Sample, uint64_t WallNanos)
	{
		// the two old sources keep their codes, so older dumps still resume
		if (Mask == 9)
		{
			m_Events.push_back({WallNanos, kSrcDacDf, Sample, 0});
		}
		else if (Mask == 1)
		{
			m_Events.push_back({WallNanos, kSrcDac1f, Sample, 0});
		}
		else
		{
			m_Events.push_back({WallNanos, kSrcDacMask, Sample, Mask});
		}
		++m_DacWrites;
		if (m_FirstDacNanos == UINT64_MAX)
		{
			m_FirstDacNanos = WallNanos;
		}
		m_LastDacNanos = WallNanos;
	}
	void OnBeeperEdge(uint8_t EarBit, uint64_t WallNanos)
	{
		m_Events.push_back({WallNanos, kSrcBeeper, EarBit, 0});
		++m_BeeperEdges;
	}

	// Render + write the WAV. TotalNanos = emulated wall length of the run;
	// AyClockHz = the machine's AY chip clock. Produces no file when nothing
	// audible was ever logged (absence observable via the counters).
	bool Finalise(uint64_t TotalNanos, uint32_t AyClockHz, std::string& OutError);

	// Status / dump accessors.
	const std::string& OutPath() const
	{
		return m_Path;
	}
	uint32_t RateHz() const
	{
		return m_Rate;
	}
	StereoMode Stereo() const
	{
		return m_Mode;
	}
	uint64_t AyWritesLogged(int C) const
	{
		return m_AyWrites[C & 1];
	}
	uint64_t DacWritesLogged() const
	{
		return m_DacWrites;
	}
	uint64_t BeeperEdgesLogged() const
	{
		return m_BeeperEdges;
	}
	uint64_t SamplesWritten() const
	{
		return m_Samples;
	}
	// Derived DAC input rate (writes / span between first and last write),
	// the diagnostic the old specdrum capture reported.
	uint32_t DacDerivedRateHz() const;
	bool BFileWritten() const
	{
		return m_bWrote;
	}

	// Dump/resume. The synth is rendered from the whole event log at
	// Finalise, so a resumed run carries the log of the run before it and
	// writes only the samples after the ones that run already wrote: the two
	// files then join into what one run would have written.
	std::vector<uint8_t> PackEvents() const; // 11 bytes an event, little-endian
	uint64_t FirstDacNanos() const
	{
		return m_FirstDacNanos;
	}
	uint64_t LastDacNanos() const
	{
		return m_LastDacNanos;
	}
	bool RestoreFromDump(const std::vector<uint8_t>& PackedEvents, uint64_t AyWrites0, uint64_t AyWrites1,
		uint64_t DacWrites, uint64_t BeeperEdges, uint64_t FirstDacNanos, uint64_t LastDacNanos,
		uint64_t SamplesAlreadyWritten);

  private:
	struct Event
	{
		uint64_t m_Nanos;
		uint8_t m_Src;
		uint8_t m_A;
		uint8_t m_B;
	};

	std::string m_Path;
	uint32_t m_Rate;
	StereoMode m_Mode;
	std::vector<Event> m_Events;
	uint64_t m_AyWrites[2] = {0, 0};
	uint64_t m_DacWrites = 0;
	uint64_t m_BeeperEdges = 0;
	uint64_t m_FirstDacNanos = UINT64_MAX;
	uint64_t m_LastDacNanos = 0;
	uint64_t m_Samples = 0;
	uint64_t m_SkipSamples = 0; // written by the run this one resumed
	bool m_bWrote = false;
};
