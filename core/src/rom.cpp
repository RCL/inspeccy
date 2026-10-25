#include "rom.h"

#include <stdio.h>
#include <string.h>
#include <vector>

RomLoadResult LoadRom16K(const char* Path, bool bUseBackHalfIfCombined)
{
	RomLoadResult Result;

	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		Result.m_ErrorMessage = std::string("cannot open ROM file: ") + Path;
		return Result;
	}

	fseek(F, 0, SEEK_END);
	const long Size = ftell(F);
	fseek(F, 0, SEEK_SET);

	if (Size != 16384 && Size != 32768)
	{
		fclose(F);
		Result.m_ErrorMessage = std::string("ROM must be 16384 or 32768 bytes: ") + Path;
		return Result;
	}

	std::vector<uint8_t> Whole;
	Whole.resize(size_t(Size));
	const size_t Got = fread(Whole.data(), 1, size_t(Size), F);
	fclose(F);

	if (Got != size_t(Size))
	{
		Result.m_ErrorMessage = std::string("short read on ROM file: ") + Path;
		return Result;
	}

	Result.m_Data.resize(16384);
	if (Size == 16384)
	{
		memcpy(Result.m_Data.data(), Whole.data(), 16384);
	}
	else
	{
		const size_t Off = bUseBackHalfIfCombined ? 16384 : 0;
		memcpy(Result.m_Data.data(), Whole.data() + Off, 16384);
	}

	Result.m_bOk = true;
	return Result;
}
