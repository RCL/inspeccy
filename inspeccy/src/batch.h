#pragma once
#include <stdint.h>
#include <string>
#include "inspection.h"
struct SldSymbolMap;
struct Z80State;
struct IoContext;
struct RunSummary;
struct TraceWriter;

// A finite, replayable experiment on one loaded machine. No server or
// intermediate dumps; normal CLI final outputs still apply. Returns 0 on
// success, 3 on I/O error, 4 on invalid input, 9 on a failed run/assertion.
int RunBatch(const std::string& Path, const std::string& ReportPath, Z80State& Cpu, IoContext& Io, uint64_t& Budget,
	TraceWriter* Trace, RunSummary& Summary, const SldSymbolMap& Symbols, bool BoundSymbols,
	const InspectionIdentity& Identity = {});
