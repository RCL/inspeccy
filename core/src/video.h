#pragma once
// Per-frame composite BMP capture for video recording.
//
// Each frame is one composite BMP, named per `Pattern` (printf-style, must
// contain exactly one %d / %u / %X / %06d-style integer placeholder for the
// frame counter). After the run, ffmpeg consumes the directory with e.g.:
//
//     ffmpeg -framerate 50 -i frame_%06d.bmp -c:v libx264 -pix_fmt yuv420p out.mp4
//
// Cadence is purely cycle-based: every PeriodCycles cycles emulated we
// emit one frame. PeriodCycles = round(BaseMhz * 1e6 / Fps), computed once
// at startup. Turbo (NR_07) changes are intentionally NOT tracked: the
// output is always Fps frames per *base-clock* emulated second. For a demo
// that turbos throughout, that gives faithful 50/60 fps playback.
//
// Per-frame BMP write is inline (~1 ms each at 320x256 RGB888). For a 30 s
// 50-fps recording that's ~1.5 s of total write time on top of emulation.

#include <stdint.h>
#include <string>

struct OutputPaths;
struct MemoryBus;
struct SpritesState;
struct BeamDrawer;

struct VideoCapture
{
	const OutputPaths* m_OutputPaths = nullptr;
	std::string m_Pattern; // printf pattern, e.g. "frames/frame_%06d.bmp"
	uint64_t m_PeriodCycles = 0; // cycles between frame emits
	uint64_t m_NextEmitCycle = 0; // next cycle count at which to emit
	uint64_t m_FrameCounter = 0; // emitted-so-far count
	bool m_bWriteFailed = false;
	bool m_bAny = false; // true when at least one frame written
	/** Optional live frontend sink, on exactly the recording cadence. No
	 * filesystem output is produced when this is installed. */
	void (*m_OnFrame)(void*, const MemoryBus&, const SpritesState*, const BeamDrawer*) = nullptr;
	void* m_OnFrameUser = nullptr;
};

// Emit a frame BMP (composite). Called by the dispatcher when
// Cpu.Cycles >= V.NextEmitCycle. Advances FrameCounter + NextEmitCycle.
// Returns false (and sets OutError) on file-write failure.
// When Beam != nullptr and it has a completed frame, the emitted BMP is the
// drawer's last beam-scanned frame (pixel-exact incl. memory writes); otherwise
// it's the end-of-frame composite snapshot.
bool EmitVideoFrame(VideoCapture& InOutV, const MemoryBus& Mem, const SpritesState* Sprites, const BeamDrawer* Beam,
	std::string& OutError);
