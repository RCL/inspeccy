#include "tape.h"

#include "memory.h"
#include "z80.h"

#include <stdio.h>
#include <vector>

bool TapePlayer::Load(const char* Path, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open .tap: ") + Path;
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
	const size_t Sz = size_t(Size);
	std::vector<uint8_t> D(Sz);
	const size_t Got = fread(D.data(), 1, Sz, F);
	fclose(F);
	if (Got != size_t(Size))
	{
		OutError = "short read";
		return false;
	}

	m_Blocks.clear();
	m_Cursor = 0;
	size_t IdxI = 0;
	while (IdxI + 2 <= D.size())
	{
		const size_t Len = size_t(D[IdxI]) | (size_t(D[IdxI + 1]) << 8);
		IdxI += 2;
		if (Len == 0)
		{
			continue; // empty block - skip
		}
		if (IdxI + Len > D.size())
		{
			OutError = "truncated .tap block";
			return false;
		}
		m_Blocks.emplace_back(D.begin() + long(IdxI), D.begin() + long(IdxI + Len));
		IdxI += Len;
	}
	if (m_Blocks.empty())
	{
		OutError = "no blocks in .tap";
		return false;
	}
	return true;
}

void TapePlayer::HandleLdBytes(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// Simulate LD-BYTES return: pop the caller's return address into PC, then
	// set the carry flag to the load result. Do this up front so every path
	// (including the error paths) returns from the trapped call.
	const uint16_t Ret = uint16_t(InOutMem.Rd(InOutCpu.SP) | (InOutMem.Rd(uint16_t(InOutCpu.SP + 1)) << 8));
	InOutCpu.SP = uint16_t(InOutCpu.SP + 2);
	InOutCpu.PC = Ret;

	auto SetCarry = [&](bool bOk)
	{
		uint8_t Fl = InOutCpu.F();
		if (bOk)
		{
			Fl |= kFlagC;
		}
		else
		{
			Fl = uint8_t(Fl & ~kFlagC);
		}
		InOutCpu.SetF(Fl);
	};

	if (m_Cursor >= m_Blocks.size())
	{
		SetCarry(false); // tape exhausted -> loading error
		return;
	}

	const std::vector<uint8_t>& Blk = m_Blocks[m_Cursor];
	const uint8_t ExpectedFlag = InOutCpu.A();
	// Blk = [flag] [data...] [checksum]; the ROM asks for DE data bytes at IX.
	if (Blk.size() < 2 || Blk[0] != ExpectedFlag)
	{
		// Flag mismatch: the ROM wanted a header but sees data (or vice versa).
		// Skip this block so a stray call can't spin, and report an error.
		++m_Cursor;
		SetCarry(false);
		return;
	}

	const size_t Avail = Blk.size() - 2; // data bytes (excl flag + checksum)
	uint16_t Want = InOutCpu.DE;
	size_t N = Want;
	if (N > Avail)
	{
		N = Avail; // never read past the block
	}

	uint16_t Ix = InOutCpu.IX;
	for (size_t K = 0; K < N; ++K)
	{
		InOutMem.Wr(uint16_t(Ix + K), Blk[1 + K]);
	}
	InOutCpu.IX = uint16_t(Ix + N);
	InOutCpu.DE = uint16_t(Want - N); // 0 on a full block load

	++m_Cursor;
	++m_BlocksDelivered;
	m_BytesDelivered += N;
	SetCarry(true);
}
