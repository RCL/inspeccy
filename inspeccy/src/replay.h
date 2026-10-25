#pragma once
// Frame-exact --bmp-out (default on, --bmp-exact off to disable).
//
// The plain exit screenshot is a snapshot of live state at the exit cycle -
// coherent for double-buffered programs, misleading for anything beam-timed,
// and possibly mid-draw when the budget lands inside a frame. This module makes
// the default screenshot a frame a real display actually showed, without paying
// the eager drawer's per-frame cost for the whole run:
//
//   * every N frames (the per-frame stats tick, which fires regardless of IFF1)
//     the WHOLE machine is checkpointed into one of two alternating in-memory
//     slots - CPU, MemoryBus (RAM, NextRegs, copper, CTC, contention), sprites,
//     DMA, AY, tape cursor, input state, TR-DOS, and a VFS clone whose FILE*s
//     are reopened at the live files' positions;
//   * at exit - for ANY reason: budget, --until-pc, RestartTrap, a halt - the
//     older checkpoint is replayed in that scratch copy up to the exit cycle
//     with the eager BeamDrawer armed, and the drawer's last COMPLETED frame is
//     written. Determinism makes the replay exact; a self-check compares the
//     replayed CPU state against the live exit state and falls back to the
//     plain snapshot (with a stderr note) on any divergence.
//
// The replay universe has null output sinks (no trace, no audio, no video, no
// status), so nothing double-emits and the live run's counters are untouched.

#include <stdint.h>
#include <memory>
#include <string>

#include "ay.h"
#include "input.h"
#include "memory.h"
#include "nextzxos.h"
#include "sprites.h"
#include "tape.h"
#include "trdos.h"
#include "z80.h"

struct IoContext;
struct ZxnDmaState;

struct FrameExactCheckpoint
{
	bool m_bValid = false;
	uint64_t m_Cycles = 0; // Cpu.Cycles at save time
	std::unique_ptr<MemoryBus> m_Mem; // allocated on first save (2 MB+)
	Z80State m_Cpu;
	SpritesState m_Sprites;
	AyState m_Ay;
	AyState m_Ay2;
	TapePlayer m_Tape;
	InputState m_Input;
	Trdos m_Trd;
	NextZxosVfs m_Vfs; // clone with its own read-only FILE*s
	// ZxnDmaState is defined in dma.h; stored via pointer to keep this header light.
	std::unique_ptr<ZxnDmaState> m_Dma;
	uint8_t m_AyActive = 0;
	double m_CurrentMhz = 3.5;
};

struct FrameExact
{
  public:
	struct LiveRefs
	{
		MemoryBus* m_Mem = nullptr;
		Z80State* m_Cpu = nullptr;
		SpritesState* m_Sprites = nullptr; // nullable
		ZxnDmaState* m_Dma = nullptr; // nullable
		AyState* m_Ay = nullptr;
		AyState* m_Ay2 = nullptr; // nullable (--ts only)
		TapePlayer* m_Tape = nullptr; // nullable
		InputState* m_Input = nullptr; // nullable
		Trdos* m_Trd = nullptr; // nullable
		NextZxosVfs* m_Vfs = nullptr; // the DotVfs object (may be unmounted)
		const IoContext* m_Io = nullptr; // for flags: bAyEnabled, bResetTrap, ...
		uint16_t m_LinesPerFrame = 312;
		uint16_t m_TopBorderLines = 64;
		int m_EveryNFrames = 8; // checkpoint cadence (frames)
	};

	void Configure(const LiveRefs& R)
	{
		m_Live = R;
	}

	// Called from the dispatcher's per-frame stats tick (IoContext::OnFrameTick).
	void OnFrameTick();

	// Replay the older checkpoint up to the live exit cycle and write the
	// drawer's last completed frame to Path. Returns false with OutReason set
	// when there is no usable checkpoint, the replay diverged from the live
	// run, or no frame completed inside the replay window.
	bool ReplayAndWriteBmp(const char* Path, std::string& OutReason);

  private:
	void Save(FrameExactCheckpoint& InOutC);

	LiveRefs m_Live{};
	FrameExactCheckpoint m_Ck[2];
	int m_Next = 0;
	uint64_t m_Ticks = 0;
};
