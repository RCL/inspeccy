#include "dotcmd.h"

#include "memory.h"
#include "z80.h"

#include <stdio.h>
#include <filesystem>
#include <string>
#include <vector>

DotCmdLoadResult LoadDotCmd(const char* Path, const std::string& ArgString, Z80State& InOutCpu, MemoryBus& InOutMem)
{
	DotCmdLoadResult R;

	FILE* F = fopen(Path, "rb");
	if (!F)
	{
		R.m_ErrorMessage = std::string("cannot open dot-command binary: ") + Path;
		return R;
	}
	fseek(F, 0, SEEK_END);
	const long Sz = ftell(F);
	fseek(F, 0, SEEK_SET);
	if (Sz < 0)
	{
		fclose(F);
		R.m_ErrorMessage = "ftell failed";
		return R;
	}
	if (size_t(Sz) > kDotcmdMaxSize)
	{
		fclose(F);
		R.m_ErrorMessage = "dot-command binary too large (max 40 KB)";
		return R;
	}
	std::vector<uint8_t> Bytes;
	Bytes.resize(size_t(Sz));
	const size_t Got = (Sz > 0) ? fread(Bytes.data(), 1, size_t(Sz), F) : 0;
	fclose(F);
	if (Got != size_t(Sz))
	{
		R.m_ErrorMessage = "short read";
		return R;
	}

	// Map RAM into slot 1 ($2000-$3FFF) before loading the dot command.
	// Allocate its 8K page through IDE_BANK; keep slot 0 mapped to ROM
	// so a return to $0000 still reaches the reset trap.
	const int Page8K = InOutMem.AllocBank8K();
	if (Page8K < 0)
	{
		R.m_ErrorMessage = "no free 8K page for dot-command code";
		return R;
	}
	InOutMem.MapRamPageToSlot(1, Page8K);
	R.m_Code8KPage = Page8K;

	// Copy into emulator RAM at $2000.
	for (size_t IdxI = 0; IdxI < Bytes.size(); ++IdxI)
	{
		InOutMem.Wr(uint16_t(kDotcmdLoadAddr + IdxI), Bytes[IdxI]);
	}

	// Set registers per the dot-command launch convention.
	InOutCpu.PC = kDotcmdLoadAddr;
	InOutCpu.SP = kDotcmdStackTop;
	InOutCpu.AF = 0;
	InOutCpu.DE = 0; // BC and HL are set from the command line, further down
	InOutCpu.IX = 0;
	InOutCpu.IY = kDotcmdSysvarsBase;
	InOutCpu.AFp = InOutCpu.BCp = InOutCpu.DEp = InOutCpu.HLp = 0;
	InOutCpu.m_MemPtr = 0;
	InOutCpu.I = 0;
	InOutCpu.R = 0;
	InOutCpu.IFF1 = 0;
	InOutCpu.IFF2 = 0;
	InOutCpu.IM = 1;
	InOutCpu.m_bHalted = false;
	InOutCpu.m_bEiPending = false;
	InOutCpu.m_Cycles = 0;
	InOutCpu.m_NextFrameIntCycle = UINT64_MAX;
	InOutCpu.m_NextPsgTickCycle = UINT64_MAX;
	InOutCpu.m_NextStatsTickCycle = UINT64_MAX;
	InOutCpu.m_NextEventCycle = UINT64_MAX;
	InOutCpu.m_FrameIntPeriod = 0;
	InOutCpu.m_PsgTickPeriod = 0;
	InOutCpu.m_StatsTickPeriod = 0;
	InOutCpu.m_InterruptsAccepted = 0;
	InOutCpu.m_HaltCycles = 0;
	InOutCpu.m_LastDiCycles = UINT64_MAX;
	InOutCpu.m_LastStatsSnapshotHalt = 0;
	InOutCpu.m_PerFrameHalt.clear();

	// Lay out the command line the way NextZXOS documents it: the command name
	// (no leading ".") followed by a space and the arguments, $0D-terminated,
	// positioned so the argument text lands at $5CDB -- the address a
	// diagnostic dot command reported HL holding on real hardware. HL points at
	// the arguments (0 when there are none); BC points at the start of the
	// whole line and is never 0.
	std::string CmdName = std::filesystem::path(Path).stem().string();
	const size_t NameRoom = size_t(kDotcmdArgBuffer - kDotcmdLineFloor) - 1;
	if (CmdName.empty())
	{
		CmdName = "dot";
	}
	if (CmdName.size() > NameRoom)
	{
		CmdName.resize(NameRoom);
	}

	const uint16_t LineStart = uint16_t(kDotcmdArgBuffer - (CmdName.size() + 1));
	for (size_t IdxI = 0; IdxI < CmdName.size(); ++IdxI)
	{
		InOutMem.Wr(uint16_t(LineStart + IdxI), uint8_t(CmdName[IdxI]));
	}
	InOutMem.Wr(uint16_t(kDotcmdArgBuffer - 1), ' ');
	for (size_t IdxI = 0; IdxI < ArgString.size(); ++IdxI)
	{
		InOutMem.Wr(uint16_t(kDotcmdArgBuffer + IdxI), uint8_t(ArgString[IdxI]));
	}
	InOutMem.Wr(uint16_t(kDotcmdArgBuffer + ArgString.size()), 0x0D);

	InOutCpu.BC = LineStart;
	InOutCpu.HL = ArgString.empty() ? 0 : kDotcmdArgBuffer;

	InstallNextZxosIm1Stub(InOutMem);

	R.m_bOk = true;
	R.m_BinarySize = Bytes.size();
	return R;
}

void InstallNextZxosIm1Stub(MemoryBus& InOutMem)
{
	// The 128K editor's IM 1 handler jumps to RAM code at $5B00 after
	// pushing four words. Supply the missing RAM handler without booting
	// the editor, and preserve HL while updating the FRAMES counter.
	constexpr uint16_t kIntStub = 0x5B00;
	constexpr uint16_t kFRAMES = 0x5C78;
	const uint8_t Stub[] = {
		0x33, 0x33, 0x33, 0x33, 0x33, 0x33, // inc sp * 6  - skip $0038/$5B00/$0048
		0xE1, // pop hl       - pull editor's saved origHL
		0xE5, // push hl      - keep it for after the FRAMES bump
		0x2A, uint8_t(kFRAMES & 0xFF), uint8_t(kFRAMES >> 8), // ld hl,(FRAMES)
		0x23, // inc hl
		0x22, uint8_t(kFRAMES & 0xFF), uint8_t(kFRAMES >> 8), // ld (FRAMES), hl
		0xE1, // pop hl       - restore origHL
		0xFB, // ei
		0xC9 // ret
	};
	for (size_t IdxI = 0; IdxI < sizeof(Stub); ++IdxI)
	{
		InOutMem.Wr(uint16_t(kIntStub + IdxI), Stub[IdxI]);
	}
}
