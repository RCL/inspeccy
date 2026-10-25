#pragma once
#include "diagnostics.h"
#include "inspection.h"

void LoadIrqDiagnostics(
	const std::string& Path, const MemoryBus& Mem, const SldSymbolMap& Symbols, bool BoundSymbols, IrqDiagnostics& Out);
std::string IrqSummaryJson(const IrqDiagnostics& D);
std::string IrqReportJson(
	const IrqDiagnostics& D, const SldSymbolMap& Symbols, bool BoundSymbols, const InspectionIdentity& Identity);
