#include "memory.h"
#include "nextzxos.h"
#include "z80.h"

#include <memory>
#include <stdio.h>

int main(int Argc, char** Argv)
{
	if (Argc != 2)
	{
		return 1;
	}
	NextZxosVfs Vfs;
	std::string Error;
	if (!Vfs.Mount(Argv[1], 'C', Error))
	{
		fprintf(stderr, "%s\n", Error.c_str());
		return 1;
	}
	auto Mem = std::make_unique<MemoryBus>();
	Mem->Map48K();
	const auto Open = [&](const char* Name, uint8_t Mode) -> bool
	{
		for (size_t Idx = 0;; ++Idx)
		{
			Mem->Wr(uint16_t(0x8000 + Idx), uint8_t(Name[Idx]));
			if (!Name[Idx])
			{
				break;
			}
		}
		Z80State Cpu{};
		Cpu.HL = 0x8000;
		Cpu.BC = uint16_t(Mode) << 8;
		Vfs.HandleRst08(0x9A, Cpu, *Mem);
		const bool bOpened = (Cpu.F() & kFlagC) == 0;
		if (bOpened)
		{
			Vfs.HandleRst08(0x9B, Cpu, *Mem);
		}
		return bOpened;
	};
	if (!Open("inside.txt", 1) || !Open("new.txt", 0x0E) || !Open("allowed/inside.txt", 1))
	{
		fprintf(stderr, "ordinary paths or an internal link were rejected\n");
		return 1;
	}
	for (const char* Name : {"../outside/sentinel.txt", "escape/sentinel.txt", "escape/new.txt", "inside.txt:stream",
			 "C:/escape/sentinel.txt", "escape\\sentinel.txt"})
	{
		if (Open(Name, 1) || Open(Name, 0x0E))
		{
			fprintf(stderr, "path escaped mount: %s\n", Name);
			return 1;
		}
	}
	return 0;
}
