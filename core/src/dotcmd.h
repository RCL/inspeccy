#pragma once
#include <stdint.h>
#include <string>

struct MemoryBus;
struct Z80State;

// NextZXOS dot-command launch convention:
//   code at $2000, maximum 40 KB; PC=$2000, SP=$FFF0, IY=$5C3A.
//   HL=0 without arguments, otherwise points to the CR-terminated argument
//   text at $5CDB. BC points to the entire command line without the leading
//   dot, including the command name. BC must not be assumed to be zero.
//   IFF1=IFF2=0; AF, DE and IX start at zero as a loader choice, not a
//   documented firmware guarantee. Unspecified system variables remain zero.
// RST $08 services are trapped by the NextZXOS dispatcher.

constexpr uint16_t kDotcmdLoadAddr = 0x2000;
constexpr uint16_t kDotcmdStackTop = 0xFFF0;
constexpr uint16_t kDotcmdSysvarsBase = 0x5C3A;
constexpr uint16_t kDotcmdArgBuffer = 0x5CDB;
constexpr uint16_t kDotcmdLineFloor = 0x5CB6; // do not lay the name below this
constexpr size_t kDotcmdMaxSize = 0xA000; // 40 KB ($2000..$BFFF)

struct DotCmdLoadResult
{
	bool m_bOk = false;
	size_t m_BinarySize = 0;
	int m_Code8KPage = -1; // 8K RAM page mapped to slot 1 ($2000)
	std::string m_ErrorMessage;
};

// Read the dot-command binary at Path, copy into Mem at DOTCMD_LOAD_ADDR,
// set CPU registers per the convention above. Place ArgString + 0x0D at
// DOTCMD_ARG_BUFFER (when non-empty) and set HL accordingly.
DotCmdLoadResult LoadDotCmd(const char* Path, const std::string& ArgString, Z80State& InOutCpu, MemoryBus& InOutMem);

// Install the 18-byte IM 1 forwarder stub at $5B00. Replaces the 128K
// editor ROM's RAM-resident handler that would normally be installed by
// the editor's boot sequence. Required by anything that uses IM 1 + halts
// in RAM (dot commands, NEX programs that briefly run IM 1 during init).
// Safe no-op if the program switches to IM 2 - the stub just sits unused.
void InstallNextZxosIm1Stub(MemoryBus& InOutMem);
