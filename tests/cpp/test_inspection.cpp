#include "memory.h"
#include "diagnostics.h"
#include "io.h"
#include "ay.h"
#include "z80.h"
#include <cstdio>
#include <memory>

int main()
{
	auto M = std::make_unique<MemoryBus>();
	M->SetIsNext(true);
	M->WriteNextReg(0x50, 255, 0, 0); // ROM
	if (M->InspectMapping(0).m_Kind != DebugMapping::Rom)
	{
		return 1;
	}
	M->WriteNextReg(0x54, 230, 0, 0);
	if (M->InspectMapping(0x8123).m_Page != 230 || M->InspectMapping(0x8123).m_Offset != 0x123)
	{
		return 2;
	}
	M->SetRamSizeMb(1);
	M->WriteNextReg(0x54, 230, 0, 0);
	uint8_t V = 0;
	if (M->InspectMapping(0x8000).m_Kind != DebugMapping::OpenBus || !M->DebugPeek(0x8000, V) || V != 255)
	{
		return 3;
	}
	M->WriteNextReg(0x12, 9, 0, 0);
	M->WriteLayer2Port(1); // write-only L2 overlay leaves ROM reads intact
	if (M->InspectMapping(0).m_Kind != DebugMapping::Rom || M->InspectMapping(0).m_Writable ||
		M->InspectMapping(0, true).m_Page != 18 || !M->InspectMapping(0, true).m_Writable)
	{
		return 4;
	}
	M->WriteLayer2Port(5); // read + write
	if (M->InspectMapping(0).m_Page != 18 || M->InspectMapping(8192).m_Page != 19)
	{
		return 5;
	}
	std::vector<uint8_t> Coverage(kRamPageCount * kSlotSize);
	M->m_Coverage = Coverage.data();
	M->m_CoverageSize = Coverage.size();
	M->Ram16KBank(9)[0] = 73;
	if (!M->DebugPeek(0, V) || V != 73)
	{
		return 6;
	}
	for (auto B : Coverage)
	{
		if (B)
		{
			return 7;
		}
	}

	AyState Ay;
	IoContext Io{*M, nullptr, nullptr, Ay, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, true, false, false};
	Io.m_NanosPerCycleQ20 = (1000000000ull << 20) / 3500000;
	Z80State C;
	IrqDiagnostics D;
	D.MaxEvents = 3;
	for (unsigned I = 0; I < 300; ++I)
	{
		C.PC = uint16_t(I);
		C.m_Cycles = 1000 + I;
		D.Expired(IrqDiagnostics::Frame, C, Io, I, I + 32);
	}
	if (D.Events.size() != 3 || D.OmittedEvents != 297 || D.Locations.size() != 256 || D.OverflowLocations != 44 ||
		D.Counts[0] != 300)
	{
		return 8;
	}
	D.Guards.push_back({"guard", IrqDiagnostics::Frame, 18, 0, 1, 0});
	C.IFF1 = 1;
	C.SP = 0x9000;
	D.BeforeAccept(IrqDiagnostics::Frame, C, Io);
	if (D.Counts[2] != 1 || C.IFF1 != 1 || C.SP != 0x9000 || C.m_Cycles != 1299)
	{
		return 9;
	}
	puts("PASS: debug mappings, side-effect-free peek and bounded numeric IRQ capture");
}
