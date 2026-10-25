#pragma once
// State-invariants engine. Reads `.inv` files describing memory protection
// rules ("these physical bytes must not be written"), watches every memory
// write (CPU + DMA), and emits JSONL violation events.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

struct Z80State;
struct ZxnDmaState;
struct MemoryBus;

struct InvariantRange
{
	// Physical byte range. Inclusive both ends after parse normalisation
	// (open-bracket endpoints get adjusted by +/-1).
	int m_Page8KStart = 0;
	uint16_t m_OffsetStart = 0; // 0..$1FFF
	int m_Page8KEnd = 0;
	uint16_t m_OffsetEnd = 0;
	std::string m_SourceText; // verbatim original line, for the event
};

struct InvariantsState
{
	std::vector<InvariantRange> m_ReadOnly;

	FILE* m_ViolationsOut = nullptr; // defaults to stderr
	bool m_bOwnViolationsFh = false; // close on destruction?
	uint64_t m_MaxViolations = 1000;
	uint64_t m_ViolationsEmitted = 0;
	bool m_bExitOnViolation = false;
	bool m_bAnyViolation = false;

	// Set by the DMA-step branch in z80.cpp so violation events can attribute
	// CPU vs DMA. The dispatcher sets this to true around the StepZxnDma /
	// inline IO-DMA loops and clears it after.
	bool m_bInDmaStep = false;
	const ZxnDmaState* m_DmaActive = nullptr; // non-null during DMA step

	~InvariantsState()
	{
		if (m_bOwnViolationsFh && m_ViolationsOut != nullptr && m_ViolationsOut != stderr)
		{
			fclose(m_ViolationsOut);
		}
	}
};

// Parse one .inv file, append rules to State. Returns true on success;
// false + sets OutError if the file is malformed or unreadable.
bool LoadInvariantsFile(const char* Path, InvariantsState& InOutState, std::string& OutError);

// Called from memory.h's Wr hook when a write hits a readonly range.
// Emits one JSONL event, increments counters, sets bAnyViolation.
void EmitInvariantViolation(InvariantsState& InOutState, int Page8K, uint16_t OffsetInPage, uint8_t OldVal,
	uint8_t NewVal, int SlotIdx, const Z80State& Cpu, const MemoryBus& Mem);
