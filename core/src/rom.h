#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct RomLoadResult
{
	bool m_bOk = false;
	std::string m_ErrorMessage;
	std::vector<uint8_t> m_Data;
};

// Loads a 16 KB ROM. Accepts both 16384-byte standalone files and 32768-byte
// "combined" ROM files (front 16 KB = 128K editor / shadow ROM; back 16 KB =
// 48K BASIC ROM). When the file is 32 KB, bUseBackHalf selects which half to
// return - pass true to extract the 48K BASIC half from a combined image,
// false to extract the 128K editor half (or for any 16 KB file).
RomLoadResult LoadRom16K(const char* Path, bool bUseBackHalfIfCombined = false);
