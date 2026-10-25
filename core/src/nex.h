#pragma once
// Loader for sjasmplus's .NEX (ZX Spectrum Next executable) format.
// 512-byte header documents the present 16K banks, initial PC/SP/border,
// optional loading screen + palette + copper, and entry-bank for the
// $C000-$FFFF window. Bank data appears in sjasmplus save order
// (5, 2, 0, 1, 3, 4, 6, 7, then 8..111 numerically). V1.2 and V1.3 both
// supported; we parse V1.3 extras enough to skip past them.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

struct MemoryBus;
struct Z80State;

struct NexLoadResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	// Parsed header summary, surfaced for the dump JSON / status.
	std::string m_Version; // "V1.2" or "V1.3"
	uint8_t m_RamReq = 0; // 0 = 768 KB, 1 = 1792 KB
	uint8_t m_NumBanks = 0;
	uint8_t m_ScreenFlags = 0;
	uint8_t m_Border = 0;
	uint8_t m_EntryBank = 0;
	uint16_t m_InitialSP = 0;
	uint16_t m_InitialPC = 0;
	uint8_t m_CoreVersion[3] = {0, 0, 0};
	bool m_bHasCopperCode = false;
	bool m_bHasLoadingScreen = false;
	bool m_bLoadingScreenApplied = false; // Layer 2 loading screen blitted + enabled
	uint32_t m_BanksOffset = 512;
	std::vector<int> m_BanksPresent; // bank indices in numerical order

	// fileHandleCfg from header bytes 140-141. 0 = close file after load.
	// 1..0x3FFF = pass the open handle in BC.
	// >= 0x4000 = address where NextZXOS writes the open-file handle byte
	// before passing control to the demo. The demo then uses that handle
	// for F_READ to stream data from the appended payload (e.g. WAV samples
	// for Specdrum playback).
	uint16_t m_FileHandleCfg = 0;
	// Open file handle, positioned at end-of-banks (= start of appended
	// payload). Non-null only when FileHandleCfg requests streaming. Caller
	// owns the handle and must close it (we transfer ownership to
	// NextZxosVfs at the call site by registering it as a pre-opened
	// handle).
	FILE* m_OpenFile = nullptr;
	long m_PayloadOffset = 0; // file position immediately after last bank
	long m_PayloadSize = 0; // bytes from PayloadOffset to EOF
};

// Read the NEX file at Path, populate Mem with the banks, set Cpu registers
// per the header. ROM at slots 0/1 is left as-is - the NEX entry point
// always runs from RAM and the user-provided 128K ROM stays mapped so
// safety traps and any RST routing still works.
NexLoadResult LoadNex(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem, std::string& OutError);
