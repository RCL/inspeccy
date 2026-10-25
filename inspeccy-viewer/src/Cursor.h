#pragma once
#include <stdint.h>

// Shared cursor model. Every widget that depends on cursor position
// subscribes to MainWindow's `cursorMoved` signal carrying one of these.
//
// Cycle is canonical; Frame and ByteOffset are derived (computed by
// TraceModel::seekToCycle from the .fidx index). All three are valid only
// after a TraceModel is loaded.

struct Cursor
{
	uint64_t m_Cycle = 0;
	uint64_t m_Frame = 0;
	uint64_t m_ByteOffset = 0; // record offset in the .btr stream
	uint64_t m_TimeNanos = 0; // derived from index entry's StartTimeNanos
};
