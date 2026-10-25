#include "nex.h"

#include "memory.h"
#include "z80.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{

constexpr size_t kHeaderSize = 512;
constexpr int kMaxBank = 112;
// sjasmplus save order for banks 0..7 (matches nexBankOrder[] in
// sjasm/io_nex.cpp:32). Banks 8..111 follow numerically after these eight.
constexpr int kSaveOrder[8] = {5, 2, 0, 1, 3, 4, 6, 7};

// Loading-screen flag bits from the header.
constexpr uint8_t kScrLayer2 = 0x01;
constexpr uint8_t kScrUla = 0x02;
constexpr uint8_t kScrLores = 0x04;
constexpr uint8_t kScrHires = 0x08;
constexpr uint8_t kScrHicol = 0x10;
constexpr uint8_t kScrExt2 = 0x40;
constexpr uint8_t kScrNopal = 0x80;

uint16_t ReadU16LE(const uint8_t* P)
{
	return uint16_t(P[0] | (uint16_t(P[1]) << 8));
}
uint32_t ReadU32LE(const uint8_t* P)
{
	return uint32_t(P[0]) | (uint32_t(P[1]) << 8) | (uint32_t(P[2]) << 16) | (uint32_t(P[3]) << 24);
}

// Bytes occupied by optional loading screens and their palette.
size_t LoadingScreenBytes(uint8_t Screen)
{
	if (Screen == 0)
	{
		return 0;
	}
	// ULA and Timex screens have no palette block, even without bit 7.
	// https://wiki.specnext.dev/NEX_file_format (header offset 10)
	const bool bHasPalette = (Screen & (kScrLayer2 | kScrLores | kScrExt2)) != 0 && (Screen & kScrNopal) == 0;
	size_t Total = bHasPalette ? 512 : 0;
	if (Screen & kScrLayer2)
	{
		// The 320x256 8-bit and 640x256 4-bit modes both use 81920 bytes.
		Total += (Screen & kScrExt2) ? 81920 : 49152;
	}
	if (Screen & kScrUla)
	{
		Total += 6912;
	}
	if (Screen & kScrLores)
	{
		Total += 12288;
	}
	if (Screen & kScrHires)
	{
		Total += 12288;
	}
	if (Screen & kScrHicol)
	{
		Total += 12288;
	}
	return Total;
}

// Apply the NEX loading screen so a title that relies on the loader's picture
// (as its title, or drawn over) shows it instead of a black screen. Only the
// Layer 2 screen is applied
// (256x192, or the V1.3 320/640 SCR_EXT2 variants; with or without palette).
// ULA/LoRes/HiRes/HiCol loading screens are a separate follow-up. `Buf` is the
// screen block that starts at file offset 512 (palette, then screens in flag
// order). Everything goes through the hardware-path writers so the compositor /
// timeline see a consistent register file. Returns true if an L2 screen landed.
bool ApplyLoadingScreen(const uint8_t* Buf, size_t Len, uint8_t Screen, uint8_t Screen2, MemoryBus& InOutMem)
{
	if (!(Screen & kScrLayer2))
	{
		return false; // only Layer 2 handled for now
	}

	size_t Off = 0;
	const uint8_t* Palette = nullptr;
	if ((Screen & kScrNopal) == 0 && Off + 512 <= Len)
	{
		Palette = Buf;
		Off = 512;
	}

	const bool bExt2 = (Screen & kScrExt2) != 0; // V1.3 320/640
	const size_t L2Len = bExt2 ? 81920 : 49152; // 5 banks vs 3
	if (Off + L2Len > Len)
	{
		return false; // truncated file - leave black
	}

	// Pixels: consecutive 16K banks from bank 9 (the bank the loader targets;
	// NR $12 = 9 below). 3 banks for 256x192, 5 for 320/640.
	const int Banks = int(L2Len / 16384);
	for (int B = 0; B < Banks; ++B)
	{
		memcpy(InOutMem.Ram16KBank(9 + B), Buf + Off + size_t(B) * 16384, 16384);
	}

	// Palette -> the first Layer 2 palette (index 1, which the compositor reads
	// by default; NR $43 bit 2 clear). 256 x 9-bit LE words == the [256][2]
	// {Hi=RRRGGGBB, Lo=9th bit} layout of MutablePalette, so a straight blit.
	if (Palette)
	{
		memcpy(InOutMem.MutablePalette(1), Palette, 512);
	}

	// Registers: NR $70 mode (0=256, $10=320, $20=640), NR $12 = bank 9, L2 on.
	const uint8_t Mode = bExt2 ? uint8_t(((Screen2 == 2) ? 2u : 1u) << 4) : 0u;
	InOutMem.WriteNextReg(0x70, Mode, 0, 0, 0);
	InOutMem.WriteNextReg(0x12, 9, 0, 0, 0);
	InOutMem.WriteLayer2Port(0x02); // visible (mirrors NR $69 bit 7)
	return true;
}

} // namespace

NexLoadResult LoadNex(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem, std::string& OutError)
{
	NexLoadResult R;

	FILE* F = fopen(Path, "rb");
	if (!F)
	{
		OutError = std::string("cannot open NEX file: ") + Path;
		return R;
	}

	uint8_t H[kHeaderSize];
	if (fread(H, 1, kHeaderSize, F) != kHeaderSize)
	{
		fclose(F);
		OutError = "short read of NEX header";
		return R;
	}

	// Magic: "NextV1.2" or "NextV1.3".
	// Magic: "NextV1.0" .. "NextV1.3" (case-insensitive on "Next": at least one
	// published game writes "NEXTv1.0"). V1.0/V1.1 share the 512-byte layout;
	// the fields V1.2/V1.3 added (core version, entry bank, file handle) are
	// zero in older files, which is exactly their "not used" value.
	// Accept V1.1 files as well as later supported NEX versions.
	auto Lower = [](unsigned char C)
	{
		return (C >= 'A' && C <= 'Z') ? char(C + 32) : char(C);
	};
	const bool bMagic = Lower(H[0]) == 'n' && Lower(H[1]) == 'e' && Lower(H[2]) == 'x' && Lower(H[3]) == 't' &&
						Lower(H[4]) == 'v' && H[5] == '1' && H[6] == '.' && H[7] >= '0' && H[7] <= '3';
	if (!bMagic)
	{
		OutError = "NEX magic mismatch (expected NextV1.0 .. NextV1.3)";
		return R;
	}
	R.m_Version = std::string("V1.") + char(H[7]);

	R.m_RamReq = H[8];
	R.m_NumBanks = H[9];
	R.m_ScreenFlags = H[10];
	R.m_Border = H[11];
	R.m_InitialSP = ReadU16LE(H + 12);
	R.m_InitialPC = ReadU16LE(H + 14);

	// banks[0..111] presence flags at offset 18.
	R.m_BanksPresent.reserve(R.m_NumBanks);
	for (int B = 0; B < kMaxBank; ++B)
	{
		if (H[18 + B] != 0)
		{
			R.m_BanksPresent.push_back(B);
			if (B >= int(kRamPageCount / 2))
			{
				fclose(F);
				OutError = "NEX needs a bank outside our RAM model (current pool is " +
						   std::to_string(kRamPageCount / 2) + " 16K banks)";
				return R;
			}
		}
	}
	if (int(R.m_BanksPresent.size()) != int(R.m_NumBanks))
	{
		// Tolerate a count mismatch - sjasmplus's numBanks is informational,
		// the presence array is authoritative.
		R.m_NumBanks = uint8_t(R.m_BanksPresent.size());
	}

	// Tail of the V1.2 header (loadbar UX, core version, entry bank, file
	// handle pass-through).
	R.m_CoreVersion[0] = H[135];
	R.m_CoreVersion[1] = H[136];
	R.m_CoreVersion[2] = H[137];
	R.m_EntryBank = H[139];
	R.m_FileHandleCfg = ReadU16LE(H + 140);
	if (R.m_EntryBank >= kMaxBank)
	{
		fclose(F);
		OutError = "NEX entry bank is outside 0..111";
		return R;
	}

	R.m_BanksOffset = kHeaderSize;
	uint8_t Screen2 = 0;
	bool bCopper = false;
	if (R.m_Version == "V1.3")
	{
		// V1.3 has a banksOffset field that overrides the default.
		const uint32_t kBO = ReadU32LE(H + 144);
		if (kBO != 0)
		{
			R.m_BanksOffset = kBO;
		}
		Screen2 = H[152];
		bCopper = (H[153] != 0);
		R.m_bHasCopperCode = bCopper;
	}
	R.m_bHasLoadingScreen = (R.m_ScreenFlags != 0);

	// If the header doesn't pin BanksOffset (V1.2 always defaults; some V1.3
	// tools leave the field zero), advance past the loading screen + copper
	// ourselves so the banks read from the right offset.
	if (R.m_BanksOffset == kHeaderSize)
	{
		R.m_BanksOffset = kHeaderSize + uint32_t(LoadingScreenBytes(R.m_ScreenFlags));
		if (bCopper)
		{
			R.m_BanksOffset += 2048;
		}
	}

	// Apply the loading screen BEFORE the banks: on hardware the loader writes
	// the screen first and the file's banks after, so a bank 9-13 present in the
	// file legitimately overwrites the picture. Read the screen block (starts at
	// offset 512) and blit it through the hardware-path writers.
	R.m_bLoadingScreenApplied = false;
	if (R.m_ScreenFlags != 0)
	{
		const size_t ScrBytes = LoadingScreenBytes(R.m_ScreenFlags);
		if (ScrBytes > 0 && fseek(F, long(kHeaderSize), SEEK_SET) == 0)
		{
			std::vector<uint8_t> ScrBuf(ScrBytes);
			const size_t Got = fread(ScrBuf.data(), 1, ScrBytes, F);
			R.m_bLoadingScreenApplied = ApplyLoadingScreen(ScrBuf.data(), Got, R.m_ScreenFlags, Screen2, InOutMem);
		}
	}

	// Load each present bank in sjasmplus save order. File index advances
	// by one per bank actually read.
	int FileIdx = 0;
	auto LoadOne = [&](int Bank) -> bool
	{
		if (H[18 + Bank] == 0)
		{
			return true; // not present
		}
		const long Off = long(R.m_BanksOffset) + long(FileIdx) * 16384L;
		if (fseek(F, Off, SEEK_SET) != 0)
		{
			OutError = "NEX seek failed for bank " + std::to_string(Bank);
			return false;
		}
		uint8_t* Dst = InOutMem.Ram16KBank(Bank);
		if (fread(Dst, 1, 16384, F) != 16384)
		{
			OutError = "NEX short read for bank " + std::to_string(Bank);
			return false;
		}
		++FileIdx;
		return true;
	};
	for (int IdxI = 0; IdxI < 8; ++IdxI)
	{
		if (!LoadOne(kSaveOrder[IdxI]))
		{
			fclose(F);
			return R;
		}
	}
	for (int B = 8; B < kMaxBank; ++B)
	{
		if (!LoadOne(B))
		{
			fclose(F);
			return R;
		}
	}
	// Position past the last bank - that's where the appended payload starts.
	R.m_PayloadOffset = long(R.m_BanksOffset) + long(FileIdx) * 16384L;
	if (fseek(F, 0, SEEK_END) == 0)
	{
		const long End = ftell(F);
		R.m_PayloadSize = (End > R.m_PayloadOffset) ? (End - R.m_PayloadOffset) : 0;
	}
	fseek(F, R.m_PayloadOffset, SEEK_SET);
	// Hand ownership of the open file to the caller when streaming was
	// requested (fileHandleCfg points into RAM); otherwise close it now.
	if (R.m_FileHandleCfg != 0)
	{
		R.m_OpenFile = F;
	}
	else
	{
		fclose(F);
	}

	// CPU register setup. Per the V1.2 spec the NEX loader hands control
	// with IFF1=IFF2=0, IM=1, I=R=0; the program does its own EI / im 2 /
	// ld i,a setup. PC may be 0 (no auto-start) - caller decides what to
	// do with that.
	InOutCpu.PC = R.m_InitialPC;
	InOutCpu.SP = R.m_InitialSP;
	InOutCpu.AF = 0;
	InOutCpu.BC = 0;
	InOutCpu.DE = 0;
	InOutCpu.HL = 0;
	InOutCpu.IX = 0;
	InOutCpu.IY = 0;
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

	// Default Spectrum 128K layout: slot 0/1 ROM, slot 2/3 = bank 5,
	// slot 4/5 = bank 2, slot 6/7 = entry bank (defaults to bank 0).
	// sjasmplus's NEX runtime conventionally maps entryBank into $C000;
	// we do the same so PCs in that window land in the right code.
	// ROM 1 (48K BASIC, = NextZXOS ROM 3 for our purposes) selected via
	// $7FFD bit 4, as NextZXOS leaves it when it starts a NEX: programs call
	// 48K ROM routines (KEY-SCAN, the IM 1 handler's keyboard scan, the
	// FLAGS-bit-5 key wait) and the 128K editor ROM's handler would instead
	// jump into an editor that was never booted.
	InOutMem.Map128K(uint8_t(0x10 | (R.m_EntryBank & 0x07)), false);
	// The NEX entry bank is an extended 16K bank, not a three-bit 7FFD bank.
	InOutMem.WriteNextReg(0x56, uint8_t(R.m_EntryBank * 2), 0, 0, 0);
	InOutMem.WriteNextReg(0x57, uint8_t(R.m_EntryBank * 2 + 1), 0, 0, 0);

	// Header SP = 0 means "keep the OS stack": on hardware the program runs
	// on NextZXOS's stack near the top of RAM and a final RET returns to the
	// loader. Give it that: SP just below a return frame whose target is a
	// DI; HALT stub at $FFFE, so a program that quits ends the run as
	// "halted at $FFFF" instead of popping ROM bytes and derailing.
	if (R.m_InitialSP == 0)
	{
		InOutCpu.SP = 0xFFFC;
		InOutMem.Wr(0xFFFC, 0xFE);
		InOutMem.Wr(0xFFFD, 0xFF);
		InOutMem.Wr(0xFFFE, 0xF3); // di
		InOutMem.Wr(0xFFFF, 0x76); // halt
	}

	InOutMem.SetBorderColour(R.m_Border);
	InOutMem.SetIsNext(true);

	R.m_bOk = true;
	OutError.clear();
	return R;
}
