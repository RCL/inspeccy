#include "video.h"

#include "bmp.h"
#include "frame_path.h"
#include "output_paths.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <filesystem>
#include <vector>

bool EmitVideoFrame(VideoCapture& InOutV, const MemoryBus& Mem, const SpritesState* Sprites, const BeamDrawer* Beam,
	std::string& OutError)
{
	if (InOutV.m_OnFrame)
	{
		InOutV.m_OnFrame(InOutV.m_OnFrameUser, Mem, Sprites, Beam);
		++InOutV.m_FrameCounter;
		InOutV.m_NextEmitCycle += InOutV.m_PeriodCycles;
		InOutV.m_bAny = true;
		return true;
	}
	std::string Path;
	if (!FormatFramePath(InOutV.m_Pattern, InOutV.m_FrameCounter, Path))
	{
		OutError = "video-out needs one bounded integer filename placeholder";
		return false;
	}

	if (InOutV.m_OutputPaths && !InOutV.m_OutputPaths->CheckFrame(Path, InOutV.m_Pattern, OutError))
	{
		++InOutV.m_FrameCounter;
		InOutV.m_NextEmitCycle += InOutV.m_PeriodCycles;
		return false;
	}

	// Auto-create the parent directory on the first frame so the user
	// doesn't have to mkdir beforehand. fopen() returns nullptr if the
	// dir is missing - and if we don't advance NextEmitCycle on failure
	// we'd then retry fopen on every Z80 instruction, looking like a hang.
	if (InOutV.m_FrameCounter == 0)
	{
		std::error_code Ec;
		const std::filesystem::path Parent = std::filesystem::path(Path).parent_path();
		if (!Parent.empty())
		{
			std::filesystem::create_directories(Parent, Ec);
		}
	}

	// Beam-accurate frame if the eager drawer has one; else a still.
	//
	// The still goes through WriteVisibleScreenBmp, the SAME machine-aware
	// dispatcher --bmp-out uses. Calling WriteCompositeBmp directly rendered
	// every machine on the Next composite canvas (320x256 RGB888), so a Timex
	// or 48K video frame came out 320x256 with a 32-pixel border while the
	// same machine's --bmp-out gave 352x288 with 48. Nothing errored; the
	// frames just did not line up with the stills, and anything measured off
	// them by cropping at the stills' border was silently 16 pixels out.
	const bool bOk = (Beam != nullptr && Beam->HasFrame())
						 ? Beam->WriteBmp(Path.c_str(), OutError)
						 : WriteVisibleScreenBmp(Mem, Sprites, Path.c_str(), OutError);
	// Advance the deadline regardless of success so a persistent write
	// error doesn't trap the emulator in a per-instruction retry loop.
	++InOutV.m_FrameCounter;
	InOutV.m_NextEmitCycle += InOutV.m_PeriodCycles;
	if (!bOk)
	{
		return false;
	}
	InOutV.m_bAny = true;
	return true;
}
