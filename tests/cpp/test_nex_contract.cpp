#include "memory.h"
#include "nex.h"
#include "z80.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Original fixtures: screen bytes differ from every bank byte, so an incorrect
// palette skip cannot accidentally pass. Expectations follow the NEX header spec.
int main(int argc, char** argv)
{
	if (argc != 2) return 1;
	int failures = 0;
	for (const auto screen : {0, 1, 2, 4, 8, 16, 0x81, 0x84})
	{
		const int pixels = (screen & 1) ? 49152 : (screen & 2) ? 6912 : screen ? 12288 : 0;
		const int palette = (screen & 5) && !(screen & 0x80) ? 512 : 0;
		std::vector<uint8_t> file(512 + palette + pixels + 16384 + 1, 0);
		const char* magic = "NextV1.2";
		for (int i = 0; i < 8; ++i) file[i] = uint8_t(magic[i]);
		file[9] = 1; file[10] = uint8_t(screen);
		file[13] = 0xBF; file[15] = 0xC0;
		file[18 + 40] = 1; file[139] = 40;
		file[140] = 1; // handle in BC: loader must retain it for the caller
		for (int i = 0; i < 16384; ++i) file[512 + palette + pixels + i] = uint8_t(i * 7 + 0x35);
		file.back() = 0xD9; // appended payload, handle must point here
		FILE* fixture = fopen(argv[1], "wb");
		if (!fixture) return 2;
		const bool written = fwrite(file.data(), 1, file.size(), fixture) == file.size();
		fclose(fixture);
		if (!written) return 2;
		auto mem = std::make_unique<MemoryBus>();
		Z80State cpu{};
		std::string error;
		auto result = LoadNex(argv[1], cpu, *mem, error);
		bool ok = result.m_bOk && result.m_BanksOffset == unsigned(512 + palette + pixels)
			&& mem->Rd(0xC000) == 0x35 && mem->Rd(0xDFFF) == uint8_t(8191 * 7 + 0x35)
			&& mem->Rd(0xE000) == uint8_t(8192 * 7 + 0x35)
			&& mem->ReadNextReg(0x56) == 80 && mem->ReadNextReg(0x57) == 81;
		if (result.m_OpenFile) { ok = ok && fgetc(result.m_OpenFile) == 0xD9; fclose(result.m_OpenFile); }
		else ok = false;
		if (!ok) { fprintf(stderr, "NEX contract failed screen=%02X: %s\n", screen, error.c_str()); ++failures; }
	}
	auto mem = std::make_unique<MemoryBus>();
	for (uint8_t speed = 0; speed < 4; ++speed)
	{
		mem->WriteNextReg(7, speed, 0, 0, 0);
		if (mem->ReadNextReg(7) != uint8_t(speed * 17)) ++failures;
	}
	mem->SetNextCpuSpeed(1);
	if (mem->ReadNextReg(7) != 0x13) ++failures; // actual != programmed
	return failures ? 1 : 0;
}
