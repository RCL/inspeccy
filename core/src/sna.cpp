#include "sna.h"

#include "memory.h"
#include "z80.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{

constexpr size_t kHeaderSize = 27;
constexpr size_t kBankSize = 16384;
constexpr size_t kSize48k = kHeaderSize + 3 * kBankSize;
constexpr size_t kSize128k5banks = kSize48k + 4 + 5 * kBankSize;
constexpr size_t kSize128k6banks = kSize48k + 4 + 6 * kBankSize;

inline uint16_t ReadU16LE(const uint8_t* P)
{
	return uint16_t(P[0]) | (uint16_t(P[1]) << 8);
}

inline void WriteU16LE(uint8_t* P, uint16_t V)
{
	P[0] = uint8_t(V & 0xFF);
	P[1] = uint8_t(V >> 8);
}

bool ReadEntireFile(const char* Path, std::vector<uint8_t>& OutData, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open file: ") + Path;
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
	OutData.resize(size_t(Size));
	const size_t Got = fread(OutData.data(), 1, size_t(Size), F);
	fclose(F);
	if (Got != size_t(Size))
	{
		OutError = "short read";
		return false;
	}
	return true;
}

bool WriteEntireFile(const char* Path, const std::vector<uint8_t>& Data, std::string& OutError)
{
	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open file for writing: ") + Path;
		return false;
	}
	const size_t Wrote = fwrite(Data.data(), 1, Data.size(), F);
	fclose(F);
	if (Wrote != Data.size())
	{
		OutError = "short write";
		return false;
	}
	return true;
}

void ReadHeaderInto(const uint8_t* H, Z80State& InOutCpu, uint8_t& OutBorder)
{
	InOutCpu.I = H[0];
	InOutCpu.HLp = ReadU16LE(H + 1);
	InOutCpu.DEp = ReadU16LE(H + 3);
	InOutCpu.BCp = ReadU16LE(H + 5);
	InOutCpu.AFp = ReadU16LE(H + 7);
	InOutCpu.HL = ReadU16LE(H + 9);
	InOutCpu.DE = ReadU16LE(H + 11);
	InOutCpu.BC = ReadU16LE(H + 13);
	InOutCpu.IY = ReadU16LE(H + 15);
	InOutCpu.IX = ReadU16LE(H + 17);
	const uint8_t IffByte = H[19];
	InOutCpu.IFF2 = (IffByte & 0x04) ? 1 : 0;
	InOutCpu.IFF1 = InOutCpu.IFF2;
	InOutCpu.R = H[20];
	InOutCpu.AF = ReadU16LE(H + 21);
	InOutCpu.SP = ReadU16LE(H + 23);
	InOutCpu.IM = H[25];
	OutBorder = H[26];
	InOutCpu.m_bHalted = false;
	InOutCpu.m_bEiPending = false;
	InOutCpu.m_Cycles = 0;

	InOutCpu.m_NextFrameIntCycle = UINT64_MAX;
	InOutCpu.m_NextEventCycle = UINT64_MAX;
	InOutCpu.m_FrameIntPeriod = 0;
	InOutCpu.m_InterruptsAccepted = 0;
}

void WriteHeaderFrom(uint8_t* H, const Z80State& Cpu, uint8_t Border)
{
	H[0] = Cpu.I;
	WriteU16LE(H + 1, Cpu.HLp);
	WriteU16LE(H + 3, Cpu.DEp);
	WriteU16LE(H + 5, Cpu.BCp);
	WriteU16LE(H + 7, Cpu.AFp);
	WriteU16LE(H + 9, Cpu.HL);
	WriteU16LE(H + 11, Cpu.DE);
	WriteU16LE(H + 13, Cpu.BC);
	WriteU16LE(H + 15, Cpu.IY);
	WriteU16LE(H + 17, Cpu.IX);
	H[19] = uint8_t((Cpu.IFF2 ? 0x04 : 0x00));
	H[20] = Cpu.R;
	WriteU16LE(H + 21, Cpu.AF);
	WriteU16LE(H + 23, Cpu.SP);
	H[25] = Cpu.IM;
	H[26] = Border;
}

} // namespace

SnaProbeResult ProbeSna(const char* Path)
{
	SnaProbeResult R;
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		R.m_ErrorMessage = std::string("cannot open SNA: ") + Path;
		return R;
	}
	fseek(F, 0, SEEK_END);
	const long Size = ftell(F);
	fclose(F);

	if (Size == long(kSize48k))
	{
		R.m_Kind = SnaKind::kSnaKindK48;
		R.m_bOk = true;
	}
	else if (Size == long(kSize128k5banks) || Size == long(kSize128k6banks))
	{
		R.m_Kind = SnaKind::kSnaKindK128;
		R.m_bOk = true;
	}
	else
	{
		char Buf[128];
		snprintf(Buf, sizeof(Buf), "unrecognised SNA size: %ld bytes", Size);
		R.m_ErrorMessage = Buf;
	}
	return R;
}

SnaLoadResult LoadSna(const char* Path, Z80State& InOutCpu, MemoryBus& InOutMem)
{
	SnaLoadResult Out;

	std::vector<uint8_t> Bytes;
	if (!ReadEntireFile(Path, Bytes, Out.m_ErrorMessage))
	{
		return Out;
	}

	if (Bytes.size() == kSize48k)
	{
		Out.m_Kind = SnaKind::kSnaKindK48;
		ReadHeaderInto(Bytes.data(), InOutCpu, Out.m_Border);

		// 48K RAM: bytes 27..49178 -> bank 5, then bank 2, then bank 0.
		memcpy(InOutMem.Ram16KBank(5), Bytes.data() + kHeaderSize, kBankSize);
		memcpy(InOutMem.Ram16KBank(2), Bytes.data() + kHeaderSize + kBankSize, kBankSize);
		memcpy(InOutMem.Ram16KBank(0), Bytes.data() + kHeaderSize + 2 * kBankSize, kBankSize);

		InOutMem.Map48K();

		// PC is on the stack - pop it.
		const uint16_t Lo = InOutMem.Rd(InOutCpu.SP);
		const uint16_t Hi = InOutMem.Rd(uint16_t(InOutCpu.SP + 1));
		InOutCpu.PC = uint16_t(Lo | (Hi << 8));
		InOutCpu.SP = uint16_t(InOutCpu.SP + 2);

		Out.m_Port7FFD = 0;
		Out.m_bWantsTrdosRom = false;
		Out.m_bOk = true;
		return Out;
	}

	if (Bytes.size() == kSize128k5banks || Bytes.size() == kSize128k6banks)
	{
		Out.m_Kind = SnaKind::kSnaKindK128;
		ReadHeaderInto(Bytes.data(), InOutCpu, Out.m_Border);

		const uint8_t* P = Bytes.data() + kHeaderSize;
		memcpy(InOutMem.Ram16KBank(5), P, kBankSize);
		P += kBankSize;
		memcpy(InOutMem.Ram16KBank(2), P, kBankSize);
		P += kBankSize;
		const uint8_t* PagedBlock = P;
		P += kBankSize;

		InOutCpu.PC = ReadU16LE(P);
		P += 2;
		const uint8_t Port7FFD = *P++;
		const uint8_t TrdosFlag = *P++;
		Out.m_Port7FFD = Port7FFD;
		Out.m_bWantsTrdosRom = TrdosFlag != 0;

		const int CurrentBank = Port7FFD & 0x07;
		memcpy(InOutMem.Ram16KBank(CurrentBank), PagedBlock, kBankSize);

		// Remaining banks in ascending order, skipping 5, 2, and the currently paged.
		for (int B = 0; B < 8; ++B)
		{
			if (B == 5 || B == 2 || B == CurrentBank)
			{
				continue;
			}
			memcpy(InOutMem.Ram16KBank(B), P, kBankSize);
			P += kBankSize;
		}

		InOutMem.Map128K(Port7FFD, Out.m_bWantsTrdosRom);
		Out.m_bOk = true;
		return Out;
	}

	char Buf[128];
	snprintf(Buf, sizeof(Buf), "unrecognised SNA size: %zu bytes", Bytes.size());
	Out.m_ErrorMessage = Buf;
	return Out;
}

bool SaveSna(const char* Path, SnaKind Kind, Z80State& InOutCpu, MemoryBus& InOutMem, std::string& OutError)
{
	constexpr uint8_t kBorderDefault = 7;

	if (Kind == SnaKind::kSnaKindK48)
	{
		// Push PC onto the emulator stack.
		InOutCpu.SP = uint16_t(InOutCpu.SP - 2);
		InOutMem.Wr(InOutCpu.SP, uint8_t(InOutCpu.PC & 0xFF));
		InOutMem.Wr(uint16_t(InOutCpu.SP + 1), uint8_t(InOutCpu.PC >> 8));

		std::vector<uint8_t> Out(kSize48k, 0);
		WriteHeaderFrom(Out.data(), InOutCpu, kBorderDefault);
		memcpy(Out.data() + kHeaderSize, InOutMem.Ram16KBank(5), kBankSize);
		memcpy(Out.data() + kHeaderSize + kBankSize, InOutMem.Ram16KBank(2), kBankSize);
		memcpy(Out.data() + kHeaderSize + 2 * kBankSize, InOutMem.Ram16KBank(0), kBankSize);
		return WriteEntireFile(Path, Out, OutError);
	}

	// 128K save
	const uint8_t Port7FFD = InOutMem.GetPort7FFD();
	const int CurrentBank = Port7FFD & 0x07;
	const bool bTrdos = InOutMem.IsTrdosRomActive();

	// Count extra banks (those in 0..7 excluding 5, 2, CurrentBank).
	int ExtraCount = 0;
	for (int B = 0; B < 8; ++B)
	{
		if (B == 5 || B == 2 || B == CurrentBank)
		{
			continue;
		}
		++ExtraCount;
	}
	const size_t TotalSize = kSize48k + 4 + size_t(ExtraCount) * kBankSize;

	std::vector<uint8_t> Out(TotalSize, 0);
	WriteHeaderFrom(Out.data(), InOutCpu, kBorderDefault);

	uint8_t* P = Out.data() + kHeaderSize;
	memcpy(P, InOutMem.Ram16KBank(5), kBankSize);
	P += kBankSize;
	memcpy(P, InOutMem.Ram16KBank(2), kBankSize);
	P += kBankSize;
	memcpy(P, InOutMem.Ram16KBank(CurrentBank), kBankSize);
	P += kBankSize;

	WriteU16LE(P, InOutCpu.PC);
	P += 2;
	*P++ = Port7FFD;
	*P++ = bTrdos ? 0x01 : 0x00;

	for (int B = 0; B < 8; ++B)
	{
		if (B == 5 || B == 2 || B == CurrentBank)
		{
			continue;
		}
		memcpy(P, InOutMem.Ram16KBank(B), kBankSize);
		P += kBankSize;
	}

	return WriteEntireFile(Path, Out, OutError);
}
