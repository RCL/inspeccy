#pragma once
// Per-frame motion and brightness measurements of the 256x192 paper area.
// Block averaging supports comparison of dithered and greyscale frames.
// Optional paper BMPs contain the downsampled image; cadence matches video.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

struct OutputPaths;
struct MemoryBus;
struct SpritesState;

struct MetricsCapture
{
	const OutputPaths* m_OutputPaths = nullptr;
	std::string m_MotionPath; // --motion-out; empty = not writing motion
	std::string m_HistPath; // --histogram-out; empty = not writing histogram
	std::string m_PaperPattern; // --paper-out <printf-pattern>; one downsampled BMP per sample
	int m_Block = 1; // --metrics-block N: average NxN before measuring

	uint64_t m_PeriodCycles = 0; // cycles between samples
	uint64_t m_NextEmitCycle = 0;
	uint64_t m_FrameCounter = 0;
	bool m_bWriteFailed = false;
	bool m_bAny = false;

	// Ring of the last 16 downsampled frames, for correlation at lag 1/4/16.
	std::vector<std::vector<uint8_t>> m_History;
	FILE* m_MotionFile = nullptr;
	FILE* m_HistFile = nullptr;
	std::vector<uint8_t> m_Luma; // scratch: 256*192 paper luminance
	std::vector<uint8_t> m_Small; // scratch: the downsampled frame
};

// Open the output files and write their headers. Returns false + OutError if a
// file cannot be created.
bool OpenMetrics(MetricsCapture& InOutM, std::string& OutError);

// Sample one frame. Called by the dispatcher when Cpu.Cycles >= NextEmitCycle;
// advances FrameCounter and NextEmitCycle whether or not the write succeeded.
bool EmitMetricsFrame(MetricsCapture& InOutM, const MemoryBus& Mem, const SpritesState* Sprites, std::string& OutError);

void CloseMetrics(MetricsCapture& InOutM);
