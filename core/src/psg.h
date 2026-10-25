#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// .psg file capture of AY-3-8910 register writes. De facto Sinclair / Vortex
// Tracker format: 16-byte header (P S G 0x1A, version, int-freq, reserved)
// then a stream of frames where each frame is { <reg> <val> }* terminated by
// 0xFF, with 0xFE <nn> compressing nn*4 silent frames, and 0xFD ending the song.
//
// OnAyWrite() is called from the I/O dispatcher on every captured write to
// the AY data port. OnFrameTick() is called by the scheduler at the per-frame
// rate (50 Hz / 60 Hz). Last-write-wins per register per frame.
//
// Finalise() flushes any pending frame, appends 0xFD, and writes the file.

struct PsgCapture final
{
  public:
	PsgCapture(std::string OutPath, int Rate);

	void OnAyWrite(uint8_t Reg, uint8_t Value);
	void OnFrameTick();
	bool Finalise(std::string& OutError);

	uint64_t FramesWritten() const
	{
		return m_Frames;
	}
	uint64_t WritesCaptured() const
	{
		return m_Writes;
	}

	// Inspection / dump accessors - read-only views of in-progress state.
	const std::string& OutPath() const
	{
		return m_Path;
	}
	int RateHz() const
	{
		return m_Rate;
	}
	uint16_t GetDirtyMask() const
	{
		return m_DirtyMask;
	}
	const uint8_t* GetFrameValues() const
	{
		return m_FrameValues;
	}
	uint32_t SilentFramesPending() const
	{
		return m_SilentFrames;
	}
	const std::vector<uint8_t>& BodySoFar() const
	{
		return m_Body;
	}

	// Resume support - pre-seed in-progress capture state from a dump.
	// Subsequent OnAyWrite/OnFrameTick/Finalise calls then produce
	// byte-identical output to a non-interrupted run.
	void LoadStream(const std::vector<uint8_t>& BodySoFar, uint16_t Dirty, const uint8_t Values[16],
		uint32_t SilentFramesPending, uint64_t FramesAlready, uint64_t WritesAlready)
	{
		m_Body = BodySoFar;
		m_DirtyMask = Dirty;
		for (int IdxI = 0; IdxI < 16; ++IdxI)
		{
			m_FrameValues[IdxI] = Values[IdxI];
		}
		m_SilentFrames = SilentFramesPending;
		m_Frames = FramesAlready;
		m_Writes = WritesAlready;
	}

  private:
	void FlushSilentRun();

	std::string m_Path;
	int m_Rate;
	std::vector<uint8_t> m_Body;
	uint16_t m_DirtyMask = 0;
	uint8_t m_FrameValues[16] = {};
	uint32_t m_SilentFrames = 0;
	uint64_t m_Frames = 0;
	uint64_t m_Writes = 0;
};
