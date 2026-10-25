#include "ay.h"
#include "io.h"
#include "memory.h"
#include "z80.h"
#include <cstdio>
#include <memory>

// Execute actual ED opcodes, not the flag helper. The oracle uses the block-I/O
// equations and the between-iteration changes measured by David Banks:
// https://github.com/hoglet67/Z80Decoder/wiki/Undocumented-Flags
// The input source is a real AY register (or the idle bus / Next Timex latch).
static bool EvenParity(unsigned Value)
{
	unsigned Ones = 0;
	for (; Value; Value >>= 1) Ones += Value & 1;
	return (Ones & 1) == 0;
}

static uint8_t ExpectedFlags(uint8_t Value, unsigned Sum, uint8_t B, bool Repeat, uint16_t Pc)
{
	bool H = Sum > 255;
	const bool C = H;
	const bool N = (Value & 128) != 0;
	bool P = EvenParity((Sum % 8) ^ B);
	if (Repeat)
	{
		if (C)
		{
			P = P ^ EvenParity(unsigned(B + (N ? -1 : 1)) & 7) ^ true;
			H = (B % 16) == (N ? 0 : 15);
		}
		else P = P ^ EvenParity(B % 8) ^ true;
	}
	const unsigned XY = Repeat ? (Pc >> 8) : B;
	return uint8_t((B & 128) | (B == 0 ? 64 : 0) | (XY & 40) |
		(H ? 16 : 0) | (P ? 4 : 0) | (N ? 2 : 0) | (C ? 1 : 0));
}

struct Fixture
{
	std::unique_ptr<MemoryBus> Mem = std::make_unique<MemoryBus>();
	AyState Ay;
	IoContext Io{*Mem, nullptr, nullptr, Ay, nullptr, nullptr, nullptr, nullptr,
		nullptr, nullptr, true, false, false};
	unsigned Cases = 0, Failures = 0;

	void Check(uint8_t Op, uint8_t Value, uint8_t L, uint8_t BeforeB, uint8_t C, uint16_t Pc)
	{
		const bool Output = (Op & 1) != 0;
		const int Direction = (Op & 8) ? -1 : 1;
		const uint8_t B = uint8_t(BeforeB - 1);
		const bool Repeat = (Op & 16) && B != 0;
		const uint16_t Hl = uint16_t(0x9000 + L);
		const uint16_t NewHl = uint16_t(Hl + Direction);
		const uint16_t Bc = uint16_t((unsigned(BeforeB) << 8) | C);
		const uint16_t NewBc = uint16_t((unsigned(B) << 8) | C);
		Mem->Wr(Pc, 0xED);
		Mem->Wr(uint16_t(Pc + 1), Op);
		Mem->Wr(Hl, Output ? Value : uint8_t(~Value));
		// Read before B decrements. Unclaimed ports return the idle $FF bus.
		Ay.m_Latch = 0;
		Ay.m_Registers[0] = Value;
		if (!Output)
		{
			if (Mem->IsNext() && C == 0xFF) Mem->SetScldMode(Value);
			else if ((Bc & 0xC002) != 0xC000) Value = 0xFF;
		}
		const unsigned Sum = Value + (Output ? uint8_t(NewHl) : uint8_t(C + Direction));
		const uint8_t Flags = ExpectedFlags(Value, Sum, B, Repeat, Pc);
		const uint16_t Wz = Repeat ? uint16_t(Pc + 1) :
			uint16_t((Output ? NewBc : Bc) + Direction);
		Z80State Cpu;
		Cpu.AF = uint16_t(0x5500 | ((Cases & 1) ? 0xFF : 0));
		Cpu.BC = Bc; Cpu.HL = Hl; Cpu.PC = Pc;
		uint64_t Budget = Repeat ? 21 : 16;
		const auto Result = RunUntil(Cpu, Io, Budget);
		++Cases;
		if (Result.m_Result != kRunResultBudgetReached || Cpu.F() != Flags || Cpu.A() != 0x55 ||
			Cpu.BC != NewBc || Cpu.HL != NewHl || Cpu.PC != (Repeat ? Pc : uint16_t(Pc + 2)) ||
			Cpu.m_MemPtr != Wz || Cpu.m_Cycles != Budget || Cpu.R != 2 ||
			(!Output && Mem->Rd(Hl) != Value))
		{
			if (Failures++ < 8)
				std::fprintf(stderr, "ED %02X data=%02X L=%02X B=%02X C=%02X PC=%04X "
					"F=%02X expected=%02X WZ=%04X expected=%04X\n",
					Op, Value, L, BeforeB, C, Pc, Cpu.F(), Flags, Cpu.m_MemPtr, Wz);
		}
	}

	void CheckOutputPort(uint8_t Op)
	{
		// The I/O address must be $BFFD (AY data), not $C0FD (AY select).
		Ay = {};
		Check(Op, 0x34, 1, 0xC0, 0xFD, 0x8000);
		if (Ay.m_Registers[0] != 0x34 || Ay.m_WritesObserved != 1)
		{
			++Failures;
			std::fprintf(stderr, "ED %02X used B before decrement for OUT\n", Op);
		}
	}

	void CheckLoop(uint8_t Op, uint8_t BeforeB, bool Interrupt)
	{
		const bool Output = (Op & 1) != 0;
		const int Direction = (Op & 8) ? -1 : 1;
		constexpr uint16_t Pc = 0xA7FF;
		constexpr uint16_t Hl = 0x9100;
		Mem->Wr(Pc, 0xED); Mem->Wr(Pc + 1, Op);
		for (int I = 0; I < 256; ++I) Mem->Wr(uint16_t(Hl + Direction * I), 0x80);
		Z80State Cpu;
		Cpu.PC = Pc; Cpu.HL = Hl; Cpu.BC = uint16_t((unsigned(BeforeB) << 8) | 3);
		Cpu.AF = 0x55FF; Cpu.SP = 0xB000;
		if (Interrupt)
		{
			Cpu.IFF1 = Cpu.IFF2 = 1; Cpu.IM = 1;
			Cpu.m_NextFrameIntCycle = 21; Cpu.m_FrameIntPeriod = 69888;
		}
		const unsigned Count = BeforeB ? BeforeB : 256;
		uint64_t Budget = Interrupt ? 34 : 21 * (Count - 1) + 16;
		// Stop at ISR entry before the ROM's first instruction can change state.
		Io.m_bUntilPc = Interrupt;
		Io.m_UntilPc = 0x38;
		const auto Result = RunUntil(Cpu, Io, Budget);
		Io.m_bUntilPc = false;
		const uint16_t NewHl = uint16_t(Hl + Direction * int(Interrupt ? 1 : Count));
		const uint8_t B = Interrupt ? uint8_t(BeforeB - 1) : 0;
		const uint8_t Value = Output ? 0x80 : 0xFF;
		const unsigned Sum = Value + (Output ? uint8_t(NewHl) : uint8_t(3 + Direction));
		const uint8_t Flags = ExpectedFlags(Value, Sum, B, Interrupt, Pc);
		const uint16_t Wz = Interrupt ? 0x38 : uint16_t((Output ? 3 : 0x103) + Direction);
		++Cases;
		if (Result.m_Result != (Interrupt ? kRunResultReachedPc : kRunResultBudgetReached) ||
			Cpu.F() != Flags || Cpu.A() != 0x55 ||
			Cpu.BC != uint16_t((unsigned(B) << 8) | 3) || Cpu.HL != NewHl ||
			Cpu.PC != (Interrupt ? 0x38 : Pc + 2) || Cpu.m_Cycles != Budget || Cpu.m_MemPtr != Wz ||
			(Interrupt && (Cpu.m_InterruptsAccepted != 1 || Mem->Rd16(Cpu.SP) != Pc)))
		{
			++Failures;
			std::fprintf(stderr, "ED %02X complete/interrupt=%d B=%02X F=%02X expected=%02X "
				"PC=%04X WZ=%04X HL=%04X BC=%04X cycles=%llu budget=%llu result=%d "
				"IRQs=%llu stack=%04X\n", Op, Interrupt, BeforeB, Cpu.F(), Flags,
				Cpu.PC, Cpu.m_MemPtr, Cpu.HL, Cpu.BC,
				static_cast<unsigned long long>(Cpu.m_Cycles), static_cast<unsigned long long>(Budget),
				int(Result.m_Result), static_cast<unsigned long long>(Cpu.m_InterruptsAccepted), Mem->Rd16(Cpu.SP));
		}
		if (!Output)
			for (unsigned I = 0; I < (Interrupt ? 1 : Count); ++I)
				if (Mem->Rd(uint16_t(Hl + Direction * int(I))) != Value) ++Failures;
	}
};

int main()
{
	constexpr uint8_t Ops[] = {0xA2, 0xA3, 0xAA, 0xAB, 0xB2, 0xB3, 0xBA, 0xBB};
	constexpr uint8_t Values[] = {0, 1, 7, 8, 0x7F, 0x80, 0xFE, 0xFF};
	constexpr uint8_t Counters[] = {0, 1, 2, 8, 0x0F, 0x10, 0x11, 0x7F, 0x80, 0x81, 0xC0, 0xC1, 0xFF};
	constexpr uint16_t Pcs[] = {0x8000, 0xA7FF};
	unsigned Cases = 0, Failures = 0;
	for (bool Next : {false, true})
		for (bool Full : {false, true})
		{
			Fixture F;
			F.Mem->Map48K();
			F.Mem->SetIsNext(Next);
			F.Io.m_bForceFullDispatch = Full;
			for (uint8_t Op : Ops)
			{
				for (uint8_t Value : Values)
					for (int Low = 0; Low < 256; ++Low)
						for (uint8_t B : Counters)
							for (uint16_t Pc : Pcs)
								F.Check(Op, Value, (Op & 1) ? uint8_t(Low) : uint8_t(~Low), B,
									(Op & 1) ? 3 : uint8_t(Low), Pc);
				if (Op & 1) F.CheckOutputPort(Op);
				if (Op & 16)
				{
					F.CheckLoop(Op, 2, false);
					F.CheckLoop(Op, 0, false);
					F.CheckLoop(Op, 0x11, true);
				}
			}
			std::printf("block I/O: next=%d full=%d, %u cases, %u failures\n", Next, Full, F.Cases, F.Failures);
			Cases += F.Cases; Failures += F.Failures;
		}
	std::printf("block I/O total: %u cases, %u failures\n", Cases, Failures);
	return Failures ? 1 : 0;
}
