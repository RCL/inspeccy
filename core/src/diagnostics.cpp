#include "diagnostics.h"
#include "io.h"
#include "z80.h"

void IrqDiagnostics::Record(Event E, const Z80State& C, const IoContext& Io)
{
	E.Cycles = C.m_Cycles;
	E.Nanos = Io.WallNanosNow(C.m_Cycles);
	E.PC = C.PC;
	E.SP = C.SP;
	E.IFF1 = C.IFF1;
	E.IFF2 = C.IFF2;
	E.IM = C.IM;
	E.EiPending = C.m_bEiPending;
	E.Halted = C.m_bHalted;
	E.PcMapping = Io.m_Mem.InspectMapping(C.PC);
	E.SpMapping = Io.m_Mem.InspectMapping(C.SP);
	E.BatchStep = BatchStep;
	++Counts[E.Type];
	const Key K{int(E.Type), int(E.Interrupt), int(E.PcMapping.m_Kind), E.PcMapping.m_Page, E.PcMapping.m_Offset};
	auto It = Locations.find(K);
	if (It != Locations.end())
	{
		++It->second.Count;
	}
	else if (Locations.size() < 256)
	{
		Locations.emplace(K, Bucket{E, 1});
	}
	else
	{
		++OverflowLocations;
	}
	if (Events.size() < MaxEvents)
	{
		Events.push_back(E);
	}
	else
	{
		++OmittedEvents;
	}
}

void IrqDiagnostics::Expired(
	Source Interrupt, const Z80State& C, const IoContext& Io, uint64_t Scheduled, uint64_t Deadline)
{
	if (!Sources[Interrupt])
	{
		return;
	}
	Event E;
	E.Type = Interrupt == Frame ? FramePulseExpired : LinePulseExpired;
	E.Interrupt = Interrupt;
	E.Scheduled = Scheduled;
	E.Deadline = Deadline;
	Record(E, C, Io);
}

void IrqDiagnostics::BeforeAccept(Source Interrupt, const Z80State& C, const IoContext& Io)
{
	if (!Sources[Interrupt])
	{
		return;
	}
	for (size_t I = 0; I < Guards.size(); ++I)
	{
		const auto& G = Guards[I];
		if (G.Interrupt != Interrupt)
		{
			continue;
		}
		const auto* P = Io.m_Mem.Ram16KBank(int(G.Page / 2)) + (G.Page % 2) * 8192 + G.Offset;
		const unsigned Value = P[0] | (G.Width == 2 ? unsigned(P[1]) << 8 : 0);
		if (Value == G.Idle)
		{
			continue;
		}
		Event E;
		E.Type = GuardBusyAtAccept;
		E.Interrupt = Interrupt;
		E.GuardIndex = int(I);
		E.GuardValue = uint16_t(Value);
		Record(E, C, Io);
	}
}
