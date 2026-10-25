#include "psg.h"

#include <algorithm>
#include <stdio.h>
#include <utility>

PsgCapture::PsgCapture(std::string OutPath, int RateHz) : m_Path(std::move(OutPath)), m_Rate(RateHz)
{
	m_Body.reserve(4096);
}

void PsgCapture::OnAyWrite(uint8_t Reg, uint8_t Value)
{
	const uint8_t R = uint8_t(Reg & 0x0F);
	m_DirtyMask = uint16_t(m_DirtyMask | (1u << R));
	m_FrameValues[R] = Value;
	++m_Writes;
}

void PsgCapture::FlushSilentRun()
{
	uint32_t Remaining = m_SilentFrames;
	while (Remaining >= 4)
	{
		const uint32_t Quartets = std::min<uint32_t>(Remaining / 4, 255);
		m_Body.push_back(0xFE);
		m_Body.push_back(uint8_t(Quartets));
		Remaining -= Quartets * 4;
	}
	while (Remaining > 0)
	{
		m_Body.push_back(0xFF);
		--Remaining;
	}
	m_SilentFrames = 0;
}

void PsgCapture::OnFrameTick()
{
	if (m_DirtyMask == 0)
	{
		++m_SilentFrames;
		++m_Frames;
		return;
	}

	FlushSilentRun();

	for (uint8_t Reg = 0; Reg < 16; ++Reg)
	{
		if (m_DirtyMask & uint16_t(1u << Reg))
		{
			m_Body.push_back(Reg);
			m_Body.push_back(m_FrameValues[Reg]);
		}
	}
	m_Body.push_back(0xFF);
	m_DirtyMask = 0;
	++m_Frames;
}

bool PsgCapture::Finalise(std::string& OutError)
{
	// The file is the capture so far plus its ending, built on a copy: the
	// capture itself stays as the run left it, which is what a dump written
	// afterwards must record (a resume would otherwise start one frame ahead,
	// its body already ended).
	PsgCapture Out = *this;
	// If there were writes after the last tick, emit them as a trailing frame.
	if (Out.m_DirtyMask != 0)
	{
		Out.FlushSilentRun();
		for (uint8_t Reg = 0; Reg < 16; ++Reg)
		{
			if (Out.m_DirtyMask & uint16_t(1u << Reg))
			{
				Out.m_Body.push_back(Reg);
				Out.m_Body.push_back(Out.m_FrameValues[Reg]);
			}
		}
		Out.m_Body.push_back(0xFF);
	}
	else
	{
		Out.FlushSilentRun();
	}

	Out.m_Body.push_back(0xFD); // end of song

	FILE* F = fopen(m_Path.c_str(), "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open psg output: ") + m_Path;
		return false;
	}

	uint8_t Header[16] = {};
	Header[0] = 'P';
	Header[1] = 'S';
	Header[2] = 'G';
	Header[3] = 0x1A; // Ctrl-Z / EOF terminator
	Header[4] = 0; // version
	Header[5] = uint8_t(m_Rate == 50 ? 0 : m_Rate);
	// Header[6..15] reserved, zero.

	if (fwrite(Header, 1, 16, F) != 16 || fwrite(Out.m_Body.data(), 1, Out.m_Body.size(), F) != Out.m_Body.size())
	{
		fclose(F);
		OutError = std::string("short write to psg output: ") + m_Path;
		return false;
	}
	fclose(F);
	return true;
}
