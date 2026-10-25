#pragma once
#include "memory.h"
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

struct Z80State;
struct IoContext;

// Numeric, bounded observations only. No symbol lookup, JSON or guest writes.
struct IrqDiagnostics
{
	enum Source
	{
		Frame,
		Line
	};
	enum Kind
	{
		FramePulseExpired,
		LinePulseExpired,
		GuardBusyAtAccept
	};
	struct Guard
	{
		std::string Name;
		Source Interrupt = Frame;
		unsigned Page = 0, Offset = 0, Width = 1, Idle = 0;
	};
	struct Event
	{
		Kind Type = FramePulseExpired;
		Source Interrupt = Frame;
		uint64_t Cycles = 0, Nanos = 0, Scheduled = 0, Deadline = 0;
		int64_t BatchStep = -1;
		uint16_t PC = 0, SP = 0, GuardValue = 0;
		uint8_t IFF1 = 0, IFF2 = 0, IM = 0;
		bool EiPending = false, Halted = false;
		DebugMapping PcMapping, SpMapping;
		int GuardIndex = -1;
	};
	struct Bucket
	{
		Event First;
		uint64_t Count = 0;
	};
	using Key = std::tuple<int, int, int, int, int>;
	std::array<bool, 2> Sources{{true, true}};
	std::vector<Guard> Guards;
	unsigned MaxEvents = 256;
	uint64_t InitialCycle = 0;
	int64_t BatchStep = -1;
	std::array<uint64_t, 3> Counts{};
	uint64_t OmittedEvents = 0, OverflowLocations = 0;
	std::vector<Event> Events;
	std::map<Key, Bucket> Locations;

	void Expired(Source Interrupt, const Z80State& Cpu, const IoContext& Io, uint64_t Scheduled, uint64_t Deadline);
	void BeforeAccept(Source Interrupt, const Z80State& Cpu, const IoContext& Io);
	void Record(Event E, const Z80State& Cpu, const IoContext& Io);
};
