#pragma once
// Text-format rendering of trace records. Shared between the emulator's
// writer thread (it formats live TraceFrames as they leave the dispatcher)
// and the binary->text decoder (it reconstructs TraceFrames from the .btr
// stream and feeds them here). Single source of truth for the text output;
// the round-trip test (`tests/python/test_btr_roundtrip.py`) asserts byte-identity
// between the two paths.

#include <stdint.h>
#include <stdio.h>

struct SldSymbolMap;
struct TraceFrame; // defined in emulator/src/trace.h, but as a
				   // forward decl here so the trace/ library can
				   // expose the API without depending on the
				   // emulator-side header at compile time.
struct TraceFormatOptions; // same

void FormatTextLine(const TraceFrame& Frame, const TraceFormatOptions& Options, const SldSymbolMap* Symbols, FILE* Out,
	uint64_t& InOutLastEmittedFrame);
