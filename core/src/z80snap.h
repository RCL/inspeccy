#pragma once
#include <stdint.h>
#include <string>

#include "z80.h" // MachineModel

struct Z80State;
struct MemoryBus;

// Result of loading a .z80 snapshot (v1 / v2 / v3). The machine is detected from
// the header's hardware-mode byte (v2/v3) or defaulted to 48K (v1). Memory is
// mapped inside LoadZ80 (Map48K / Map128K); Timex machines use the 48K memory
// layout plus the restored SCLD state, which the caller re-applies after arming
// the Timex mode (EnableTimex resets it, so it must be set afterwards).
struct Z80SnapLoadResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	MachineModel m_Machine = MachineModel::kMachineModelSpectrum48K;
	uint8_t m_Border = 0;
	uint8_t m_Port7FFD = 0; // 128K paging latch (128K/Pentagon sources)
	bool m_bIsTimex = false; // machine is TC2048/TC2068/TS2068
	bool m_bIs2068 = false; // 2068 feature set (EXROM/DOCK/AY)
	uint8_t m_ScldMode = 0; // Timex: last OUT to port $FF (video mode)
	uint8_t m_ScldBank = 0; // Timex: last OUT to port $F4 (DOCK/EXROM bank)
};

// Loads a .z80 snapshot into Cpu + Mem. Requires the appropriate ROMs to already
// be mapped by the caller (same contract as LoadSna). RLE decompression (ED ED
// count value) is handled for both the v1 single-block form and the v2/v3
// per-page blocks. Unknown/unsupported hardware modes fall back to 48K.
Z80SnapLoadResult LoadZ80(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem);

// Probes the file for its detected MachineModel without mutating CPU/memory, so
// the caller can pick ROMs / validate before the full load.
struct Z80SnapProbeResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	MachineModel m_Machine = MachineModel::kMachineModelSpectrum48K;
	bool m_bIsTimex = false;
};
Z80SnapProbeResult ProbeZ80(const char* Path);
