#pragma once
#include <stdint.h>
#include <string>

struct Z80State;
struct MemoryBus;

enum SnaKind
{
	kSnaKindK48,
	kSnaKindK128,
};

struct SnaLoadResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	SnaKind m_Kind = SnaKind::kSnaKindK48;
	bool m_bWantsTrdosRom = false;
	uint8_t m_Border = 0;
	uint8_t m_Port7FFD = 0;
};

// Loads .sna into Cpu + Mem. Requires Mem to already have the appropriate ROMs
// loaded (caller checks this against Kind / bWantsTrdosRom from a prior probe is
// not necessary; the simpler flow is: load, then map ROMs based on Kind + flag).
//
// Mapping is performed inside LoadSna for SnaKind::K48 (Map48K) and SnaKind::K128
// (Map128K with the SNA's Port7FFD + TR-DOS flag).
SnaLoadResult LoadSna(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem);

// Probes file size without mutating CPU/memory. Useful so the caller can validate
// that the right ROMs are available before commencing the load.
struct SnaProbeResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	SnaKind m_Kind = SnaKind::kSnaKindK48;
};
SnaProbeResult ProbeSna(const char* Path);

// Saves current Cpu+Mem state to Path in the dimension dictated by Kind.
// For 48K Kind: PC is pushed onto the emulator stack (SP -= 2 in CPU state and
// the two bytes are written via MemoryBus before dumping the RAM image).
bool SaveSna(const char* Path, SnaKind Kind, Z80State& InOutCpu, MemoryBus& InOutMem, std::string& OutError);
