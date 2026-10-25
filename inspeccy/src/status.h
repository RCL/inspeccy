#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

struct MemoryBus;
struct AyState;
struct RunSummary;

// Snapshot of everything the status writer needs from the rest of the program.
// Built in main.cpp after RunUntil returns.
struct StatusReport
{
	// Unsupported-feature histograms (see MemoryBus / NextZxosVfs). Empty maps
	// mean "nothing the program asked for was missing".
	uint64_t m_Rst08Calls = 0, m_Rst08Unknown = 0;
	std::map<uint8_t, uint64_t> m_Rst08ByCode;
	std::map<uint8_t, uint64_t> m_Rst08UnknownByCode;
	std::map<uint16_t, uint64_t> m_NextRegWrites; // index -> writes (all registers)
	std::map<uint16_t, uint64_t> m_NextRegReads; // index -> port $253B reads
	std::map<uint16_t, uint64_t> m_PortsUnhandledReads;
	std::map<uint16_t, uint64_t> m_PortsUnhandledWrites;
	uint8_t m_Layer2Port = 0; // last $123B value written (bits: 0 wr-window,1 visible,2 rd-window,3 shadow,6-7 third)
	uint64_t m_Layer2WindowOpens = 0; // $123B writes that enabled the read/write paging window

	// Exit
	int m_ExitCode = 0;
	const char* m_ResultName = ""; // e.g. "BudgetReached"
	uint64_t m_DiToHaltCycles = 0; // UINT64_MAX = no DI ever ran
	uint16_t m_LastPC = 0;
	uint8_t m_LastOpcode = 0;

	// Cycles
	uint64_t m_CyclesTotal = 0;
	uint64_t m_Budget = 0;
	int64_t m_Overshoot = 0;
	uint64_t m_HaltCycles = 0;
	// Per-frame halt distribution
	const std::vector<uint32_t>* m_PerFrameHalt = nullptr;
	uint64_t m_StatsTickPeriod = 0; // frame T-state count for busy-pct math

	// Interrupts
	uint64_t m_InterruptsAccepted = 0;
	uint64_t m_MissedInterrupts =
		0; // frame interrupts the hardware would have dropped (ISR overrun / DI over the pulse)
	uint64_t m_LineIntsAccepted = 0; // NR $22/$23 line interrupts taken
	uint64_t m_LineIntsMissed = 0; // line interrupt pulses that expired untaken
	uint32_t m_LineIntLine = 0; // programmed line ($22 bit0|$23)
	bool m_bLineIntEnabled = false;
	uint64_t m_FrameIntPeriod = 0;
	std::string m_IrqDiagnosticsJson; // already escaped, produced by the diagnostics writer

	// AY
	uint64_t m_AyWrites = 0;
	uint16_t m_RegistersTouched = 0;
	uint8_t m_AyFinalLatch = 0;

	// PSG
	uint64_t m_PsgFramesWritten = 0;
	std::string m_PsgOutPath;

	// VRAM analysis (post-run scan)
	int m_DisplayedBank = 5;
	uint64_t m_PixelWritesBank5 = 0;
	uint64_t m_AttrWritesBank5 = 0;
	uint64_t m_PixelWritesBank7 = 0;
	uint64_t m_AttrWritesBank7 = 0;
	bool m_bPixelsAllZero = false;
	bool m_bPixelsAllEqual = false;
	uint8_t m_FirstPixelByte = 0;
	bool m_bAttrsAllEqual = false;
	uint8_t m_FirstAttrByte = 0;
	int m_DistinctAttrs = 0;
	bool m_bUniformScreen = false;
	uint8_t m_BorderColour = 7;

	// Paging
	uint8_t m_CurrentPort7FFD = 0;
	uint64_t m_WritesTo7FFD = 0;

	// Outputs (paths; empty string for "not produced")
	std::string m_OutputSna;
	std::string m_OutputBmp;
	std::string m_OutputTrace;
	std::string m_OutputStatus;

	// Wall clock
	double m_WallMs = 0.0;
	double m_Mhz = 3.5; // effective clock at run end
	double m_NominalMhz = 3.5; // the machine's real stock clock
	uint32_t m_ContentionX = 0; // 0 = contention off
};

// Fill the VRAM-analysis fields of Out from the displayed bank. Cheap; touches
// 16 KB.
void FillVramAnalysis(const MemoryBus& Mem, StatusReport& Out);

// Write Out as flat JSON to Path. Returns false and sets OutError on failure.
bool WriteStatusJson(const StatusReport& Out, const char* Path, std::string& OutError);
