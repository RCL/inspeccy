#include "z80snap.h"

#include "memory.h"
#include "z80.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// .z80 snapshot loader (Z80 / v1 / v2 / v3), covering 48K, 128K, Pentagon and
// the Timex machines (TC2048/TC2068/TS2068). Format reference: the World of
// Spectrum .z80 spec. sjasmplus cannot WRITE .z80, so this is a load-only path;
// the .sna round-trip remains the save format.

namespace
{

constexpr size_t kV1Header = 30;
constexpr size_t kBankSize = 16384;

inline uint16_t U16(const uint8_t* P)
{
	return uint16_t(P[0]) | (uint16_t(P[1]) << 8);
}

bool ReadEntireFile(const char* Path, std::vector<uint8_t>& Out, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open .z80: ") + Path;
		return false;
	}
	fseek(F, 0, SEEK_END);
	const long Size = ftell(F);
	fseek(F, 0, SEEK_SET);
	if (Size < 0)
	{
		fclose(F);
		OutError = "ftell failed";
		return false;
	}
	Out.resize(size_t(Size));
	const size_t Got = fread(Out.data(), 1, size_t(Size), F);
	fclose(F);
	if (Got != size_t(Size))
	{
		OutError = "short read";
		return false;
	}
	return true;
}

// Expand a .z80 RLE stream: `ED ED count value` -> valuexcount; a lone ED (or an
// ED at the tail) is a literal. Consumes up to InLen input bytes, produces up to
// OutMax output bytes, and returns the number produced.
size_t DecodeRle(const uint8_t* In, size_t InLen, uint8_t* Out, size_t OutMax)
{
	size_t IdxI = 0, O = 0;
	while (IdxI < InLen && O < OutMax)
	{
		if (In[IdxI] == 0xED && IdxI + 1 < InLen && In[IdxI + 1] == 0xED)
		{
			if (IdxI + 3 >= InLen)
			{
				break; // need count + value
			}
			const uint8_t Count = In[IdxI + 2];
			const uint8_t Value = In[IdxI + 3];
			IdxI += 4;
			for (uint8_t K = 0; K < Count && O < OutMax; ++K)
			{
				Out[O++] = Value;
			}
		}
		else
		{
			Out[O++] = In[IdxI++];
		}
	}
	return O;
}

// Map a v2/v3 page number to a 16K RAM bank index, or -1 for ROM / ignored pages.
// 48K-class (incl. Timex) uses pages 4/5/8; 128K-class uses pages 3..10 = banks 0..7.
int PageToBank(int Page, bool bB128)
{
	if (bB128)
	{
		return (Page >= 3 && Page <= 10) ? (Page - 3) : -1;
	}
	switch (Page)
	{
	case 8:
		return 5; // $4000-$7FFF
	case 4:
		return 2; // $8000-$BFFF
	case 5:
		return 0; // $C000-$FFFF
	default:
		return -1;
	}
}

// Decode the common register fields (present identically in v1 and v2/v3
// headers). PC is set by the caller (differs between versions).
void ReadRegisters(const uint8_t* H, Z80State& InOutCpu, uint8_t& OutBorder)
{
	InOutCpu.AF = uint16_t((uint16_t(H[0]) << 8) | H[1]);
	InOutCpu.BC = U16(H + 2);
	InOutCpu.HL = U16(H + 4);
	InOutCpu.SP = U16(H + 8);
	InOutCpu.I = H[10];
	InOutCpu.R = uint8_t((H[11] & 0x7F) | ((H[12] & 0x01) << 7));
	OutBorder = uint8_t((H[12] >> 1) & 0x07);
	InOutCpu.DE = U16(H + 13);
	InOutCpu.BCp = U16(H + 15);
	InOutCpu.DEp = U16(H + 17);
	InOutCpu.HLp = U16(H + 19);
	InOutCpu.AFp = uint16_t((uint16_t(H[21]) << 8) | H[22]);
	InOutCpu.IY = U16(H + 23);
	InOutCpu.IX = U16(H + 25);
	InOutCpu.IFF1 = H[27] ? 1 : 0;
	InOutCpu.IFF2 = H[28] ? 1 : 0;
	InOutCpu.IM = uint8_t(H[29] & 0x03);

	InOutCpu.m_bHalted = false;
	InOutCpu.m_bEiPending = false;
	InOutCpu.m_Cycles = 0;
	InOutCpu.m_NextFrameIntCycle = UINT64_MAX;
	InOutCpu.m_NextEventCycle = UINT64_MAX;
	InOutCpu.m_FrameIntPeriod = 0;
	InOutCpu.m_InterruptsAccepted = 0;
}

// Classify the v2/v3 hardware-mode byte. bV3 selects the (larger) v3 table where
// the numbering diverges from v2. Fills machine + Timex flags; returns whether it
// is a 128K-class (banked) memory layout.
bool ClassifyHardware(uint8_t Mode, bool bV3, Z80SnapLoadResult& InOutR)
{
	// Timex modes share numbering across v2/v3.
	if (Mode == 14)
	{
		InOutR.m_Machine = MachineModel::kMachineModelTc2048;
		InOutR.m_bIsTimex = true;
		InOutR.m_bIs2068 = false;
		return false;
	}
	if (Mode == 15)
	{
		InOutR.m_Machine = MachineModel::kMachineModelTc2068;
		InOutR.m_bIsTimex = true;
		InOutR.m_bIs2068 = true;
		return false;
	}
	if (Mode == 128)
	{
		InOutR.m_Machine = MachineModel::kMachineModelTs2068;
		InOutR.m_bIsTimex = true;
		InOutR.m_bIs2068 = true;
		return false;
	}

	if (!bV3)
	{
		// v2: 0/1=48K, 2=SamRam(->48K), 3/4=128K.
		switch (Mode)
		{
		case 0:
		case 1:
		case 2:
			InOutR.m_Machine = MachineModel::kMachineModelSpectrum48K;
			return false;
		case 3:
		case 4:
			InOutR.m_Machine = MachineModel::kMachineModelSpectrum128K;
			return true;
		default:
			InOutR.m_Machine = MachineModel::kMachineModelSpectrum48K;
			return false;
		}
	}
	// v3: 0..3=48K-class, 4..6=128K, 7/8/13=+3(->128K), 9=Pentagon, 12=+2.
	switch (Mode)
	{
	case 0:
	case 1:
	case 2:
	case 3:
		InOutR.m_Machine = MachineModel::kMachineModelSpectrum48K;
		return false;
	case 9:
		InOutR.m_Machine = MachineModel::kMachineModelPentagon128;
		return true;
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
	case 12:
	case 13:
	default:
		InOutR.m_Machine = MachineModel::kMachineModelSpectrum128K;
		return true;
	}
}

} // namespace

Z80SnapLoadResult LoadZ80(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem)
{
	Z80SnapLoadResult R;
	std::vector<uint8_t> B;
	if (!ReadEntireFile(Path, B, R.m_ErrorMessage))
	{
		return R;
	}
	if (B.size() < kV1Header + 4)
	{
		R.m_ErrorMessage = "truncated .z80 file";
		return R;
	}

	const uint8_t* H = B.data();
	ReadRegisters(H, InOutCpu, R.m_Border);

	const uint16_t PcV1 = U16(H + 6);
	if (PcV1 != 0)
	{
		// --- Version 1: 48K, single memory block ($4000-$FFFF). --------------
		InOutCpu.PC = PcV1;
		R.m_Machine = MachineModel::kMachineModelSpectrum48K;

		std::vector<uint8_t> Ram(3 * kBankSize, 0);
		const uint8_t* Data = H + kV1Header;
		const size_t DataLen = B.size() - kV1Header;
		const bool bCompressed = (H[12] & 0x20) != 0;
		if (bCompressed)
		{
			DecodeRle(Data, DataLen, Ram.data(), Ram.size());
		}
		else
		{
			memcpy(Ram.data(), Data, DataLen < Ram.size() ? DataLen : Ram.size());
		}
		memcpy(InOutMem.Ram16KBank(5), Ram.data() + 0 * kBankSize, kBankSize);
		memcpy(InOutMem.Ram16KBank(2), Ram.data() + 1 * kBankSize, kBankSize);
		memcpy(InOutMem.Ram16KBank(0), Ram.data() + 2 * kBankSize, kBankSize);
		InOutMem.Map48K();
		R.m_bOk = true;
		return R;
	}

	// --- Version 2 / 3: extended header + numbered memory pages. -------------
	const uint16_t ExtLen = U16(H + 30);
	const size_t PagesAt = 32 + ExtLen;
	if (PagesAt > B.size())
	{
		R.m_ErrorMessage = "corrupt .z80 extended header length";
		return R;
	}
	InOutCpu.PC = U16(H + 32);
	const uint8_t HwMode = H[34];
	const bool bV3 = (ExtLen >= 54);
	const bool bB128 = ClassifyHardware(HwMode, bV3, R);

	if (R.m_bIsTimex)
	{
		// Timex: byte 35 = last OUT $F4 (DOCK/EXROM bank), byte 36 = last OUT $FF
		// (SCLD video mode). The 128K paging latch is unused here.
		R.m_ScldBank = H[35];
		R.m_ScldMode = H[36];
	}
	else if (bB128)
	{
		R.m_Port7FFD = H[35];
	}

	// Walk the page blocks: [len_lo len_hi page] then len bytes (0xFFFF = 16384 raw).
	size_t P = PagesAt;
	while (P + 3 <= B.size())
	{
		const uint16_t Len = U16(B.data() + P);
		const uint8_t Page = B[P + 2];
		P += 3;
		const bool bRaw = (Len == 0xFFFF);
		const size_t Take = bRaw ? kBankSize : Len;
		if (P + Take > B.size())
		{
			break;
		}

		const int Bank = PageToBank(Page, bB128);
		if (Bank >= 0)
		{
			if (bRaw)
			{
				memcpy(InOutMem.Ram16KBank(Bank), B.data() + P, kBankSize);
			}
			else
			{
				DecodeRle(B.data() + P, Take, InOutMem.Ram16KBank(Bank), kBankSize);
			}
		}
		P += Take;
	}

	if (bB128)
	{
		InOutMem.Map128K(R.m_Port7FFD, /*bUseTrdosRom=*/false);
	}
	else
	{
		// 48K memory layout (also used by the Timex machines).
		InOutMem.Map48K();
	}
	R.m_bOk = true;
	return R;
}

Z80SnapProbeResult ProbeZ80(const char* Path)
{
	Z80SnapProbeResult R;
	std::vector<uint8_t> B;
	if (!ReadEntireFile(Path, B, R.m_ErrorMessage))
	{
		return R;
	}
	if (B.size() < kV1Header + 4)
	{
		R.m_ErrorMessage = "truncated .z80 file";
		return R;
	}

	const uint8_t* H = B.data();
	if (U16(H + 6) != 0)
	{
		R.m_Machine = MachineModel::kMachineModelSpectrum48K;
		R.m_bOk = true;
		return R;
	}
	const uint16_t ExtLen = U16(H + 30);
	Z80SnapLoadResult Tmp;
	ClassifyHardware(H[34], ExtLen >= 54, Tmp);
	R.m_Machine = Tmp.m_Machine;
	R.m_bIsTimex = Tmp.m_bIsTimex;
	R.m_bOk = true;
	return R;
}
