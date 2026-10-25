#include "z80.h"
#include "diagnostics.h"

#include "ay.h"
#include "dma.h"
#include "input.h"
#include "io.h"
#include "tape.h"
#include "memory.h"
#include "nextzxos.h"
#include "psg.h"
#include "audio_render.h"
#include "sprites.h"
#include "uart.h"
#include "video.h"
#include "metrics.h"
#include "trdos.h"
#include "trace/trace_binary.h"
#if INSPECCY_TRACE_SUPPORT
#include "trace.h"
#endif

#include <array>
#include <stdint.h>
#include <string.h> // memset / memcpy

#if defined(__GNUC__) || defined(__clang__)
#define UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define LIKELY(x) (__builtin_expect(!!(x), 1))
#else
#define UNLIKELY(x) (x)
#define LIKELY(x) (x)
#endif

namespace
{

constexpr std::array<uint8_t, 256> MakeParityTable()
{
	std::array<uint8_t, 256> T{};
	for (int IdxI = 0; IdxI < 256; ++IdxI)
	{
		uint8_t V = uint8_t(IdxI);
		V ^= uint8_t(V >> 4);
		V ^= uint8_t(V >> 2);
		V ^= uint8_t(V >> 1);
		T[IdxI] = (V & 1) ? 0 : kFlagPv;
	}
	return T;
}

constexpr std::array<uint8_t, 256> MakeSZ53Table()
{
	std::array<uint8_t, 256> T{};
	for (int IdxI = 0; IdxI < 256; ++IdxI)
	{
		uint8_t V = uint8_t(IdxI);
		uint8_t F = V & (kFlagS | kFlagY | kFlagX);
		if (V == 0)
		{
			F |= kFlagZ;
		}
		T[IdxI] = F;
	}
	return T;
}

constexpr auto kParityTable = MakeParityTable();
constexpr auto kSZ53Table = MakeSZ53Table();

inline uint8_t SZ53P(uint8_t V)
{
	return uint8_t(kSZ53Table[V] | kParityTable[V]);
}

// Value is the transferred byte; Sum uses the updated L for output or
// wrapped C +/- 1 for input. B has already decremented in both cases.
inline void FinishBlockIo(Z80State& InOutC, uint8_t Value, unsigned Sum, bool bRepeat)
{
	const uint8_t B = uint8_t(InOutC.BC >> 8);
	uint8_t F = uint8_t(kSZ53Table[B] | ((Value & 0x80) ? kFlagN : 0) |
		kParityTable[uint8_t((Sum & 7) ^ B)]);
	if (Sum > 0xFF)
	{
		F |= kFlagH | kFlagC;
	}
	if (bRepeat && B != 0)
	{
		InOutC.PC = uint16_t(InOutC.PC - 2);
		// The extra five T-states change flags visible at an interrupt boundary.
		// https://github.com/hoglet67/Z80Decoder/wiki/Undocumented-Flags
		F = uint8_t((F & ~(kFlagX | kFlagY)) | ((InOutC.PC >> 8) & (kFlagX | kFlagY)));
		uint8_t ParityAdjust = B;
		if (F & kFlagC)
		{
			const bool bNegative = (Value & 0x80) != 0;
			ParityAdjust = uint8_t(B + (bNegative ? -1 : 1));
			F = uint8_t(F & ~kFlagH);
			if ((B & 0x0F) == (bNegative ? 0 : 0x0F))
			{
				F |= kFlagH;
			}
		}
		F ^= uint8_t(kParityTable[ParityAdjust & 7] ^ kFlagPv);
		InOutC.m_MemPtr = uint16_t(InOutC.PC + 1);
		InOutC.m_Cycles += 21;
	}
	else
	{
		InOutC.m_Cycles += 16;
	}
	InOutC.SetF(F);
}

inline void IncR(Z80State& InOutC)
{
	InOutC.R = uint8_t((InOutC.R & 0x80) | ((InOutC.R + 1) & 0x7F));
}

// The Z80N extension opcodes (second byte after ED), per the Next's
// extended-instruction table: $23-$2C, $30-$36, $8A, $90-$95, $98, $A4, $A5,
// $AC, $B4, $B7, $BC. Must match the case labels in the ED dispatcher.
inline bool IsZ80NOpcode(uint8_t Op2)
{
	switch (Op2)
	{
	case 0x23:
	case 0x24:
	case 0x27:
	case 0x28:
	case 0x29:
	case 0x2A:
	case 0x2B:
	case 0x2C:
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x8A:
	case 0x90:
	case 0x91:
	case 0x92:
	case 0x93:
	case 0x94:
	case 0x95:
	case 0x98:
	case 0xA4:
	case 0xA5:
	case 0xAC:
	case 0xB4:
	case 0xB7:
	case 0xBC:
		return true;
	default:
		return false;
	}
}

// Optional GCC dispatch experiment: with the
// accessors fully inlined in the Fast dispatcher, gcc also inlines these small
// helpers everywhere and the hot loop grows ~24%. INSPECCY_HELPER_NOINLINE keeps
// them out-of-line so the size/i-cache trade-off can be measured per compiler.
#if defined(INSPECCY_HELPER_NOINLINE) && (defined(__GNUC__) || defined(__clang__))
#define INSPECCY_HELPER_ATTR __attribute__((noinline))
#else
#define INSPECCY_HELPER_ATTR
#endif

template <class Pol> INSPECCY_HELPER_ATTR inline uint8_t Fetch8(Z80State& InOutC, MemoryBus& InOutM)
{
	InOutM.MarkExec<Pol::Mem>(InOutC.PC);
	const uint8_t V = InOutM.Rd<Pol::Mem>(InOutC.PC);
	InOutC.PC = uint16_t(InOutC.PC + 1);
	return V;
}

template <class Pol> INSPECCY_HELPER_ATTR inline uint16_t Fetch16(Z80State& InOutC, MemoryBus& InOutM)
{
	const uint16_t Lo = Fetch8<Pol>(InOutC, InOutM);
	const uint16_t Hi = Fetch8<Pol>(InOutC, InOutM);
	return uint16_t(Lo | (Hi << 8));
}

template <class Pol> INSPECCY_HELPER_ATTR inline void Push16(Z80State& InOutC, MemoryBus& InOutM, uint16_t V)
{
	InOutC.SP = uint16_t(InOutC.SP - 1);
	InOutM.Wr<Pol::Mem>(InOutC.SP, uint8_t(V >> 8));
	InOutC.SP = uint16_t(InOutC.SP - 1);
	InOutM.Wr<Pol::Mem>(InOutC.SP, uint8_t(V & 0xFF));
}

template <class Pol> INSPECCY_HELPER_ATTR inline uint16_t Pop16(Z80State& InOutC, MemoryBus& InOutM)
{
	const uint16_t Lo = InOutM.Rd<Pol::Mem>(InOutC.SP);
	InOutC.SP = uint16_t(InOutC.SP + 1);
	const uint16_t Hi = InOutM.Rd<Pol::Mem>(InOutC.SP);
	InOutC.SP = uint16_t(InOutC.SP + 1);
	return uint16_t(Lo | (Hi << 8));
}

inline void AluAdd(Z80State& InOutC, uint8_t V)
{
	const uint8_t A = InOutC.A();
	const uint16_t R = uint16_t(A) + uint16_t(V);
	const uint8_t Rb = uint8_t(R);
	uint8_t F = uint8_t(kSZ53Table[Rb]);
	F |= uint8_t((A ^ V ^ Rb) & kFlagH);
	if (((A ^ V) & 0x80) == 0 && ((A ^ Rb) & 0x80) != 0)
	{
		F |= kFlagPv;
	}
	if (R & 0x100)
	{
		F |= kFlagC;
	}
	InOutC.SetA(Rb);
	InOutC.SetF(F);
}

inline void AluAdc(Z80State& InOutC, uint8_t V)
{
	const uint8_t A = InOutC.A();
	const uint8_t Cy = (InOutC.F() & kFlagC) ? 1 : 0;
	const uint16_t R = uint16_t(A) + uint16_t(V) + uint16_t(Cy);
	const uint8_t Rb = uint8_t(R);
	uint8_t F = uint8_t(kSZ53Table[Rb]);
	F |= uint8_t((A ^ V ^ Rb) & kFlagH);
	if (((A ^ V) & 0x80) == 0 && ((A ^ Rb) & 0x80) != 0)
	{
		F |= kFlagPv;
	}
	if (R & 0x100)
	{
		F |= kFlagC;
	}
	InOutC.SetA(Rb);
	InOutC.SetF(F);
}

inline void AluSub(Z80State& InOutC, uint8_t V)
{
	const uint8_t A = InOutC.A();
	const uint16_t R = uint16_t(A) - uint16_t(V);
	const uint8_t Rb = uint8_t(R);
	uint8_t F = uint8_t(kSZ53Table[Rb] | kFlagN);
	F |= uint8_t((A ^ V ^ Rb) & kFlagH);
	if (((A ^ V) & 0x80) != 0 && ((A ^ Rb) & 0x80) != 0)
	{
		F |= kFlagPv;
	}
	if (R & 0x100)
	{
		F |= kFlagC;
	}
	InOutC.SetA(Rb);
	InOutC.SetF(F);
}

inline void AluSbc(Z80State& InOutC, uint8_t V)
{
	const uint8_t A = InOutC.A();
	const uint8_t Cy = (InOutC.F() & kFlagC) ? 1 : 0;
	const uint16_t R = uint16_t(A) - uint16_t(V) - uint16_t(Cy);
	const uint8_t Rb = uint8_t(R);
	uint8_t F = uint8_t(kSZ53Table[Rb] | kFlagN);
	F |= uint8_t((A ^ V ^ Rb) & kFlagH);
	if (((A ^ V) & 0x80) != 0 && ((A ^ Rb) & 0x80) != 0)
	{
		F |= kFlagPv;
	}
	if (R & 0x100)
	{
		F |= kFlagC;
	}
	InOutC.SetA(Rb);
	InOutC.SetF(F);
}

inline void AluAnd(Z80State& InOutC, uint8_t V)
{
	const uint8_t R = uint8_t(InOutC.A() & V);
	InOutC.SetA(R);
	InOutC.SetF(uint8_t(SZ53P(R) | kFlagH));
}

inline void AluOr(Z80State& InOutC, uint8_t V)
{
	const uint8_t R = uint8_t(InOutC.A() | V);
	InOutC.SetA(R);
	InOutC.SetF(SZ53P(R));
}

inline void AluXor(Z80State& InOutC, uint8_t V)
{
	const uint8_t R = uint8_t(InOutC.A() ^ V);
	InOutC.SetA(R);
	InOutC.SetF(SZ53P(R));
}

inline void AluCp(Z80State& InOutC, uint8_t V)
{
	const uint8_t A = InOutC.A();
	const uint16_t R = uint16_t(A) - uint16_t(V);
	const uint8_t Rb = uint8_t(R);
	uint8_t F = uint8_t((kSZ53Table[Rb] & kFlagS) | kFlagN);
	if (Rb == 0)
	{
		F |= kFlagZ;
	}
	F |= uint8_t(V & (kFlagY | kFlagX));
	F |= uint8_t((A ^ V ^ Rb) & kFlagH);
	if (((A ^ V) & 0x80) != 0 && ((A ^ Rb) & 0x80) != 0)
	{
		F |= kFlagPv;
	}
	if (R & 0x100)
	{
		F |= kFlagC;
	}
	InOutC.SetF(F);
}

inline uint8_t AluInc(Z80State& InOutC, uint8_t V)
{
	const uint8_t R = uint8_t(V + 1);
	uint8_t F = uint8_t((InOutC.F() & kFlagC) | kSZ53Table[R]);
	if ((R & 0x0F) == 0)
	{
		F |= kFlagH;
	}
	if (R == 0x80)
	{
		F |= kFlagPv;
	}
	InOutC.SetF(F);
	return R;
}

inline uint8_t AluDec(Z80State& InOutC, uint8_t V)
{
	const uint8_t R = uint8_t(V - 1);
	uint8_t F = uint8_t((InOutC.F() & kFlagC) | kSZ53Table[R] | kFlagN);
	if ((R & 0x0F) == 0x0F)
	{
		F |= kFlagH;
	}
	if (R == 0x7F)
	{
		F |= kFlagPv;
	}
	InOutC.SetF(F);
	return R;
}

inline uint16_t AluAdd16(Z80State& InOutC, uint16_t Dst, uint16_t Src)
{
	const uint32_t R = uint32_t(Dst) + uint32_t(Src);
	uint8_t F = uint8_t(InOutC.F() & (kFlagS | kFlagZ | kFlagPv));
	if (((Dst & 0x0FFF) + (Src & 0x0FFF)) & 0x1000)
	{
		F |= kFlagH;
	}
	if (R & 0x10000)
	{
		F |= kFlagC;
	}
	F |= uint8_t((R >> 8) & (kFlagY | kFlagX));
	InOutC.SetF(F);
	return uint16_t(R);
}

inline uint16_t AluAdc16(Z80State& InOutC, uint16_t Dst, uint16_t Src)
{
	const uint32_t Cy = (InOutC.F() & kFlagC) ? 1u : 0u;
	const uint32_t R = uint32_t(Dst) + uint32_t(Src) + Cy;
	const uint16_t Ru = uint16_t(R);
	uint8_t F = 0;
	if (Ru & 0x8000)
	{
		F |= kFlagS;
	}
	if (Ru == 0)
	{
		F |= kFlagZ;
	}
	if (((Dst & 0x0FFF) + (Src & 0x0FFF) + Cy) & 0x1000)
	{
		F |= kFlagH;
	}
	if (R & 0x10000)
	{
		F |= kFlagC;
	}
	if (((Dst ^ Src) & 0x8000) == 0 && ((Dst ^ Ru) & 0x8000) != 0)
	{
		F |= kFlagPv;
	}
	F |= uint8_t((Ru >> 8) & (kFlagY | kFlagX));
	InOutC.SetF(F);
	return Ru;
}

inline uint16_t AluSbc16(Z80State& InOutC, uint16_t Dst, uint16_t Src)
{
	const uint32_t Cy = (InOutC.F() & kFlagC) ? 1u : 0u;
	const int32_t R = int32_t(Dst) - int32_t(Src) - int32_t(Cy);
	const uint16_t Ru = uint16_t(R);
	uint8_t F = kFlagN;
	if (Ru & 0x8000)
	{
		F |= kFlagS;
	}
	if (Ru == 0)
	{
		F |= kFlagZ;
	}
	const int32_t kHC = int32_t(Dst & 0x0FFF) - int32_t(Src & 0x0FFF) - int32_t(Cy);
	if (kHC & 0x1000)
	{
		F |= kFlagH;
	}
	if (R < 0)
	{
		F |= kFlagC;
	}
	if (((Dst ^ Src) & 0x8000) != 0 && ((Dst ^ Ru) & 0x8000) != 0)
	{
		F |= kFlagPv;
	}
	F |= uint8_t((Ru >> 8) & (kFlagY | kFlagX));
	InOutC.SetF(F);
	return Ru;
}

inline uint8_t Rlc8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V >> 7);
	const uint8_t R = uint8_t((V << 1) | Cy);
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Rrc8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V & 1);
	const uint8_t R = uint8_t((V >> 1) | (Cy << 7));
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Rl8(Z80State& InOutC, uint8_t V)
{
	const uint8_t InC = (InOutC.F() & kFlagC) ? 1 : 0;
	const uint8_t Cy = uint8_t(V >> 7);
	const uint8_t R = uint8_t((V << 1) | InC);
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Rr8(Z80State& InOutC, uint8_t V)
{
	const uint8_t InC = (InOutC.F() & kFlagC) ? 1 : 0;
	const uint8_t Cy = uint8_t(V & 1);
	const uint8_t R = uint8_t((V >> 1) | (InC << 7));
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Sla8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V >> 7);
	const uint8_t R = uint8_t(V << 1);
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Sra8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V & 1);
	const uint8_t R = uint8_t((V >> 1) | (V & 0x80));
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Sll8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V >> 7);
	const uint8_t R = uint8_t((V << 1) | 1);
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}
inline uint8_t Srl8(Z80State& InOutC, uint8_t V)
{
	const uint8_t Cy = uint8_t(V & 1);
	const uint8_t R = uint8_t(V >> 1);
	InOutC.SetF(uint8_t(SZ53P(R) | Cy));
	return R;
}

inline void BitN(Z80State& InOutC, int N, uint8_t V)
{
	uint8_t F = uint8_t(InOutC.F() & kFlagC);
	const uint8_t Tested = uint8_t(V & (1u << N));
	F |= kFlagH;
	if (Tested == 0)
	{
		F |= (kFlagZ | kFlagPv);
	}
	if (N == 7 && Tested)
	{
		F |= kFlagS;
	}
	F |= uint8_t(V & (kFlagY | kFlagX));
	InOutC.SetF(F);
}

inline void Daa(Z80State& InOutC)
{
	const uint8_t A = InOutC.A();
	const uint8_t kF0 = InOutC.F();
	uint8_t Add = 0;
	uint8_t Cy = (kF0 & kFlagC) ? kFlagC : 0;
	if ((kF0 & kFlagH) || (A & 0x0F) > 0x09)
	{
		Add |= 0x06;
	}
	if ((kF0 & kFlagC) || A > 0x99)
	{
		Add |= 0x60;
		Cy = kFlagC;
	}
	const uint8_t R = (kF0 & kFlagN) ? uint8_t(A - Add) : uint8_t(A + Add);
	uint8_t F = uint8_t((kF0 & kFlagN) | Cy | SZ53P(R));
	F |= uint8_t((A ^ R) & kFlagH);
	InOutC.SetA(R);
	InOutC.SetF(F);
}

// 8-bit register accessors keyed by r-code (0..7) with index-prefix substitution.
// bForceHL means "this op pairs r with (HL); even under DD/FD prefix, H stays H and L stays L".
template <class Pol>
inline uint8_t Read8Reg(
	Z80State& InOutC, MemoryBus& InOutM, int RegCode, int Idx, bool bForceHL, bool& OutbDisplaced, int8_t& OutDisp)
{
	OutbDisplaced = false;
	OutDisp = 0;
	switch (RegCode)
	{
	case 0:
		return uint8_t(InOutC.BC >> 8);
	case 1:
		return uint8_t(InOutC.BC & 0xFF);
	case 2:
		return uint8_t(InOutC.DE >> 8);
	case 3:
		return uint8_t(InOutC.DE & 0xFF);
	case 4:
		if (Idx == 0 || bForceHL)
		{
			return uint8_t(InOutC.HL >> 8);
		}
		return uint8_t((Idx == 1 ? InOutC.IX : InOutC.IY) >> 8);
	case 5:
		if (Idx == 0 || bForceHL)
		{
			return uint8_t(InOutC.HL & 0xFF);
		}
		return uint8_t((Idx == 1 ? InOutC.IX : InOutC.IY) & 0xFF);
	case 6:
	{
		if (Idx == 0)
		{
			return InOutM.Rd<Pol::Mem>(InOutC.HL);
		}
		OutDisp = int8_t(Fetch8<Pol>(InOutC, InOutM));
		OutbDisplaced = true;
		const uint16_t Base = (Idx == 1) ? InOutC.IX : InOutC.IY;
		return InOutM.Rd<Pol::Mem>(uint16_t(Base + OutDisp));
	}
	case 7:
		return uint8_t(InOutC.AF >> 8);
	default:
		return 0;
	}
}

template <class Pol>
inline void Write8Reg(
	Z80State& InOutC, MemoryBus& InOutM, int RegCode, int Idx, bool bForceHL, int8_t Disp, bool bUseDisp, uint8_t Value)
{
	switch (RegCode)
	{
	case 0:
		InOutC.BC = uint16_t((InOutC.BC & 0x00FF) | (uint16_t(Value) << 8));
		break;
	case 1:
		InOutC.BC = uint16_t((InOutC.BC & 0xFF00) | Value);
		break;
	case 2:
		InOutC.DE = uint16_t((InOutC.DE & 0x00FF) | (uint16_t(Value) << 8));
		break;
	case 3:
		InOutC.DE = uint16_t((InOutC.DE & 0xFF00) | Value);
		break;
	case 4:
		if (Idx == 0 || bForceHL)
		{
			InOutC.HL = uint16_t((InOutC.HL & 0x00FF) | (uint16_t(Value) << 8));
		}
		else if (Idx == 1)
		{
			InOutC.IX = uint16_t((InOutC.IX & 0x00FF) | (uint16_t(Value) << 8));
		}
		else
		{
			InOutC.IY = uint16_t((InOutC.IY & 0x00FF) | (uint16_t(Value) << 8));
		}
		break;
	case 5:
		if (Idx == 0 || bForceHL)
		{
			InOutC.HL = uint16_t((InOutC.HL & 0xFF00) | Value);
		}
		else if (Idx == 1)
		{
			InOutC.IX = uint16_t((InOutC.IX & 0xFF00) | Value);
		}
		else
		{
			InOutC.IY = uint16_t((InOutC.IY & 0xFF00) | Value);
		}
		break;
	case 6:
	{
		if (Idx == 0)
		{
			InOutM.Wr<Pol::Mem>(InOutC.HL, Value);
		}
		else
		{
			if (!bUseDisp)
			{
				Disp = int8_t(Fetch8<Pol>(InOutC, InOutM));
			}
			const uint16_t Base = (Idx == 1) ? InOutC.IX : InOutC.IY;
			InOutM.Wr<Pol::Mem>(uint16_t(Base + Disp), Value);
		}
		break;
	}
	case 7:
		InOutC.AF = uint16_t((InOutC.AF & 0x00FF) | (uint16_t(Value) << 8));
		break;
	default:
		break;
	}
}

inline uint16_t Read16RegPair(Z80State& InOutC, int Pair, int Idx)
{
	// Pair encoding: 0=BC, 1=DE, 2=HL/IX/IY, 3=SP
	switch (Pair)
	{
	case 0:
		return InOutC.BC;
	case 1:
		return InOutC.DE;
	case 2:
		return (Idx == 0) ? InOutC.HL : (Idx == 1 ? InOutC.IX : InOutC.IY);
	case 3:
		return InOutC.SP;
	default:
		return 0;
	}
}

inline void Write16RegPair(Z80State& InOutC, int Pair, int Idx, uint16_t Value)
{
	switch (Pair)
	{
	case 0:
		InOutC.BC = Value;
		break;
	case 1:
		InOutC.DE = Value;
		break;
	case 2:
		if (Idx == 0)
		{
			InOutC.HL = Value;
		}
		else if (Idx == 1)
		{
			InOutC.IX = Value;
		}
		else
		{
			InOutC.IY = Value;
		}
		break;
	case 3:
		InOutC.SP = Value;
		break;
	default:
		break;
	}
}

// PUSH/POP pair encoding: 0=BC, 1=DE, 2=HL/IX/IY, 3=AF
inline uint16_t ReadPushPopPair(Z80State& InOutC, int Pair, int Idx)
{
	switch (Pair)
	{
	case 0:
		return InOutC.BC;
	case 1:
		return InOutC.DE;
	case 2:
		return (Idx == 0) ? InOutC.HL : (Idx == 1 ? InOutC.IX : InOutC.IY);
	case 3:
		return InOutC.AF;
	default:
		return 0;
	}
}
inline void WritePushPopPair(Z80State& InOutC, int Pair, int Idx, uint16_t Value)
{
	switch (Pair)
	{
	case 0:
		InOutC.BC = Value;
		break;
	case 1:
		InOutC.DE = Value;
		break;
	case 2:
		if (Idx == 0)
		{
			InOutC.HL = Value;
		}
		else if (Idx == 1)
		{
			InOutC.IX = Value;
		}
		else
		{
			InOutC.IY = Value;
		}
		break;
	case 3:
		InOutC.AF = Value;
		break;
	default:
		break;
	}
}

// Tests condition code 0..7 (NZ, Z, NC, C, PO, PE, P, M).
inline bool TestCond(Z80State& InOutC, int CC)
{
	const uint8_t F = InOutC.F();
	switch (CC)
	{
	case 0:
		return (F & kFlagZ) == 0;
	case 1:
		return (F & kFlagZ) != 0;
	case 2:
		return (F & kFlagC) == 0;
	case 3:
		return (F & kFlagC) != 0;
	case 4:
		return (F & kFlagPv) == 0;
	case 5:
		return (F & kFlagPv) != 0;
	case 6:
		return (F & kFlagS) == 0;
	case 7:
		return (F & kFlagS) != 0;
	default:
		return false;
	}
}

template <class Pol> inline void AcceptInterrupt(Z80State& InOutC, MemoryBus& InOutM, uint8_t VectorByte)
{
	// Wake from HALT. PC was already advanced past 0x76 during the HALT fetch,
	// so the pushed return address points at the instruction AFTER halt.
	InOutC.m_bHalted = false;
	InOutC.IFF1 = 0;
	InOutC.IFF2 = 0;

	// Two M1 cycles during interrupt acknowledge -> two R increments.
	IncR(InOutC);
	IncR(InOutC);

	// Stash the SP-before-push and the PC about to be pushed so the
	// matching RET/RETI/RETN can identify "back to main". Pure RETI-only tracking would
	// miss RET-returning demos; pure SP tracking would misfire on
	// ISRs that swing SP via `ld sp, table; pop bc; ...`.
	if (InOutC.m_IsrDepth < Z80State::kIsrNestingMax)
	{
		InOutC.m_IsrReturnSP[InOutC.m_IsrDepth] = InOutC.SP;
		InOutC.m_IsrReturnPC[InOutC.m_IsrDepth] = InOutC.PC;
		++InOutC.m_IsrDepth;
	}

	Push16<Pol>(InOutC, InOutM, InOutC.PC);

	switch (InOutC.IM)
	{
	case 0: // IM0 with no device driving the bus: act as RST 38h
	case 1: // IM1: fixed vector
		InOutC.PC = 0x0038;
		InOutC.m_MemPtr = InOutC.PC;
		InOutC.m_Cycles += 13;
		break;
	case 2:
	default:
	{
		const uint16_t Addr = uint16_t((uint16_t(InOutC.I) << 8) | VectorByte);
		InOutC.PC = InOutM.Rd16<Pol::Mem>(Addr);
		InOutC.m_MemPtr = InOutC.PC;
		InOutC.m_Cycles += 19;
		break;
	}
	}

	++InOutC.m_InterruptsAccepted;
}

// On RET/RETI/RETN: decrement ISR depth when SP+PC match the saved pair
// from AcceptInterrupt. NO drip is armed here: per the VHDL the SRL hold
// (im2_control.vhd:238) ends during RETI's M3, before RETI's M4 completes,
// so the DMA reasserts BUSREQ before the CPU starts the post-RETI M1 - no
// main-program instruction executes between consecutive ISRs during a
// DMA window. The drip-instr arms once per DMA window when CMD_ENABLE is
// written (see WriteIoPort path for port $6B).
inline void NotifyRetMaybeIsrReturn(Z80State& InOutC, uint16_t PoppedPc, uint16_t SpAfterPop, bool bDmaActive,
	bool bTraceDrip = false, MemoryBus* TraceMem = nullptr)
{
	(void)bDmaActive;
	if (InOutC.m_IsrDepth == 0)
	{
		return;
	}
	const uint8_t Top = uint8_t(InOutC.m_IsrDepth - 1);
	if (SpAfterPop == InOutC.m_IsrReturnSP[Top] && PoppedPc == InOutC.m_IsrReturnPC[Top])
	{
		--InOutC.m_IsrDepth;
		if (bTraceDrip && TraceMem && InOutC.m_IsrDepth == 0)
		{
			uint8_t Op[4] = {0};
			for (int IdxI = 0; IdxI < 4; ++IdxI)
			{
				Op[IdxI] = TraceMem->Rd(uint16_t(PoppedPc + IdxI));
			}
			fprintf(stderr, "DRIP-SKIPPED-AFTER-RETI: cyc=%llu  PC=$%04X  next-instr-bytes=[%02X %02X %02X %02X]\n",
				static_cast<unsigned long long>(InOutC.m_Cycles), PoppedPc, Op[0], Op[1], Op[2], Op[3]);
		}
	}
}

// UART Rx FIFO sampling grid for the NR $C6 interrupts: 2800 cycles = 100 us at
// 28 MHz, 20 bytes at 2 Mbaud (the 384-byte near-full level is seen within that).
constexpr uint64_t kUartPollCycles = 2800;

inline void RecomputeNextEvent(
	Z80State& InOutC, uint64_t Budget, uint64_t Ctc0NextFire = UINT64_MAX, uint64_t CopperNextFire = UINT64_MAX)
{
	uint64_t E = Budget;
	if (InOutC.m_NextFrameIntCycle < E)
	{
		E = InOutC.m_NextFrameIntCycle;
	}
	if (InOutC.m_NextPsgTickCycle < E)
	{
		E = InOutC.m_NextPsgTickCycle;
	}
	if (InOutC.m_NextStatsTickCycle < E)
	{
		E = InOutC.m_NextStatsTickCycle;
	}
	if (Ctc0NextFire < E)
	{
		E = Ctc0NextFire;
	}
	if (CopperNextFire < E)
	{
		E = CopperNextFire;
	}
	if (InOutC.m_NextLineIntCycle < E)
	{
		E = InOutC.m_NextLineIntCycle;
	}
	if (InOutC.m_NextUartPollCycle < E)
	{
		E = InOutC.m_NextUartPollCycle;
	}
	InOutC.m_NextEventCycle = E;
}

#if INSPECCY_TRACE_SUPPORT
// Record writes to ports that decide what the display shows (.btr v4). Decodes
// mirror the real handlers further down this function exactly - the ULA claims
// any even port, the SCLD only on machines that have one, and so on - so the
// trace says a write happened iff the machine acted on it.
inline void NoteVideoPortWrite(IoContext& InOutIo, uint16_t Port, uint8_t Value)
{
	using namespace BinaryTrace;
	const uint8_t Lo = uint8_t(Port & 0xFF);
	uint16_t Which = 0;
	if (InOutIo.m_Mem.IsScldCapable() && Lo == 0xFF)
	{
		Which = kVideoPort_Scld;
	}
	else if ((Port & 0xC002) == 0x4000)
	{
		Which = kVideoPort_7FFD;
	}
	else if (InOutIo.m_Mem.IsZxEvo() && Lo == 0x77)
	{
		Which = kVideoPort_EvoFF77;
	}
	else if (InOutIo.m_Mem.IsZxUno() && Port == 0xFC3B)
	{
		Which = kVideoPort_UnoSel;
	}
	else if (InOutIo.m_Mem.IsZxUno() && Port == 0xFD3B)
	{
		Which = kVideoPort_UnoData;
	}
	else if ((Port & 0x0001) == 0)
	{
		Which = kVideoPort_UlaFE;
	}
	else
	{
		return;
	}

	const uint64_t Cyc = InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0;
	const uint64_t Frm =
		(InOutIo.m_Cpu != nullptr && InOutIo.m_Cpu->m_FrameIntPeriod > 0) ? (Cyc / InOutIo.m_Cpu->m_FrameIntPeriod) : 0;
	InOutIo.m_VideoTrace->NoteStateWrite(1 /*I/O port*/, Which, Value, 0, Cyc, Frm);
}
#endif

// Centralised port dispatch. Order on writes: TR-DOS -> AY -> $7FFD -> drop.
// Order on reads: Input -> TR-DOS -> AY-readback -> $FF.
inline void WriteIoPort(IoContext& InOutIo, uint16_t Port, uint8_t Value)
{
#if INSPECCY_TRACE_SUPPORT
	// Before any branch below, all of which can return early.
	if (UNLIKELY(InOutIo.m_VideoTrace != nullptr))
	{
		NoteVideoPortWrite(InOutIo, Port, Value);
	}
#endif
	// FD1793 ports are decoded (and replace the traditional ports) only while the
	// TR-DOS ROM window is paged in - same rule as the read path above.
	if (InOutIo.m_Trd && InOutIo.m_Mem.IsTrdosRomActive() && InOutIo.m_Trd->MaybeWritePort(Port, Value))
	{
		return;
	}

	// Timex SCLD ports (decoded on the low byte). $FF = video mode/screen/hi-res
	// colour; $F4 = DOCK/EXROM 8K-chunk enable (2068). AY-3-8912 at $F5 (register
	// select) / $F6 (data) on the 2068. $FE (border) still falls through below.
	// The ZX-Uno and the ZX Spectrum Next honour the same $FF SCLD port
	// (OUT 255,2 = 8x1 hicolour), so this is gated on IsScldCapable() = Timex,
	// ZX-Uno or Next. On the Next the write is never gated: NR $08 bit 2 gates
	// only the READ, and NR $69 bits 5:0 are a documented alias of this port.
	if (UNLIKELY(InOutIo.m_Mem.IsScldCapable()))
	{
		const uint8_t Lo = uint8_t(Port & 0xFF);
		if (Lo == 0xFF)
		{
			InOutIo.m_Mem.SetScldMode(Value);
			return;
		}
		// $F4 is the 2068's DOCK/EXROM chunk enable and exists on no other
		// machine here, so only a 2068 may swallow it - a TC2048, a ZX-Uno or
		// a Next must leave $F4 alone rather than latch a bank mask nothing
		// reads (and echo it back on the read path).
		if (Lo == 0xF4 && InOutIo.m_Mem.IsTimex2068())
		{
			InOutIo.m_Mem.SetScldBank(Value);
			return;
		}
		if (InOutIo.m_bAyEnabled && InOutIo.m_Mem.IsTimex2068())
		{
			if (Lo == 0xF5)
			{
				InOutIo.m_Ay.m_Latch = uint8_t(Value & 0x0F);
				return;
			}
			if (Lo == 0xF6)
			{
				const uint8_t R = uint8_t(InOutIo.m_Ay.m_Latch & 0x0F);
				const uint8_t V = uint8_t(Value & kAyWriteMask[R]);
				InOutIo.m_Ay.m_Registers[R] = V;
				++InOutIo.m_Ay.m_WritesObserved;
				InOutIo.m_Ay.m_RegistersTouched = uint16_t(InOutIo.m_Ay.m_RegistersTouched | (1u << R));
				if (UNLIKELY(InOutIo.m_Capture != nullptr))
				{
					InOutIo.m_Capture->OnAyWrite(R, V);
				}
				if (UNLIKELY(InOutIo.m_Audio != nullptr))
				{
					InOutIo.m_Audio->OnAyWrite(
						0, R, V, InOutIo.WallNanosNow(InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0));
				}
				return;
			}
		}
	}

	// ZX Evolution BaseConf ports (decoded only on ZX-Evo). Ordered so the
	// specific ports win over the broad page-window pattern (low 12 bits = 0xFF7),
	// which $EFF7 / $DFF7 / $BFF7 all alias - PentEvo decodes only the low byte,
	// so the CMOS + system-config ports must be caught first.
	if (UNLIKELY(InOutIo.m_Mem.IsZxEvo()))
	{
		if (Port == 0xEFF7)
		{
			InOutIo.m_Mem.SetEff7(Value);
			return;
		}
		// mc146818 RTC ($DFF7 addr / $BFF7 data), gated by EFF7 CMOS bit (bit7).
		// $BFF7 doubles as ATM window-2; the CMOS gate disambiguates.
		if (InOutIo.m_Mem.GetEff7() & 0x80)
		{
			if (Port == 0xDFF7)
			{
				InOutIo.m_Mem.SetCmosAddr(Value);
				return;
			}
			if (Port == 0xBFF7)
			{
				InOutIo.m_Mem.WriteCmos(Value);
				return;
			}
		}
		// ATM video-mode select ($xx77).
		if ((Port & 0xFF) == 0x77)
		{
			InOutIo.m_Mem.SetEvoFf77(Value);
			return;
		}
		// Page-window managers: $x7F7 (PentEvo 4 MB) and $xFF7 (ATM) - low 12
		// bits 0x7F7 vs 0xFF7 tell them apart.
		if ((Port & 0x3FFF) == 0x37F7 || (Port & 0x0FFF) == 0x0FF7)
		{
			InOutIo.m_Mem.WriteEvoWindow(Port, Value);
			return;
		}
	}

	// ZX-Uno register file: $FC3B selects a register, $FD3B writes its data.
	// ULAplus lives on its own port pair ($BF3B select / $FF3B data) and supplies
	// the Radastan palette.
	if (UNLIKELY(InOutIo.m_Mem.IsZxUno()))
	{
		if (Port == 0xFC3B)
		{
			InOutIo.m_Mem.SetUnoRegSel(Value);
			return;
		}
		if (Port == 0xFD3B)
		{
			InOutIo.m_Mem.WriteUnoData(Value);
			return;
		}
		if (Port == 0xBF3B)
		{
			InOutIo.m_Mem.SetUlaPlusReg(Value);
			return;
		}
		if (Port == 0xFF3B)
		{
			InOutIo.m_Mem.WriteUlaPlusData(Value);
			return;
		}
	}

	if (LIKELY(InOutIo.m_bAyEnabled))
	{
		if (UNLIKELY((Port & 0xC002) == 0xC000))
		{
			// TurboSound (NedoPC/CHRV, cf. UnrealSpeccy io.cpp): $F8..$FF on
			// the select port switches the active chip (bit0=1 -> primary).
			// Only engages under --ts; plain AY code never writes $F8+ here.
			//
			// On the ZX Next the 2nd/3rd AY is FROZEN unless NR_08 bit 1
			// (Enable TurboSound) is set. While disabled the $F8..$FF chip-
			// select is a no-op: the active chip stays put, so a chip-B data
			// write falls through to the still-selected primary chip - exactly
			// the hardware failure a demo hits if it plays a TurboSound tune
			// but forgets to enable TurboSound in NR_08. We model it so that
			// bug reproduces in the tracer instead of only on real hardware.
			// Non-Next machines (real NedoPC/CHRV TurboSound) have no NR_08
			// gate, so they always switch.
			if (UNLIKELY(InOutIo.m_Ay2 != nullptr) && (Value & 0xF8) == 0xF8)
			{
				const bool bTsEnabled = !InOutIo.m_Mem.IsNext() || (InOutIo.m_Mem.GetNextReg(0x08) & 0x02) != 0;
				if (bTsEnabled)
				{
					if (InOutIo.m_Ay3 && InOutIo.m_Mem.IsNext())
					{
						// Next: $FF/$FE/$FD select three independent chips.
						if (Value >= 0xFD) InOutIo.m_AyActive = uint8_t(0xFF - Value);
					}
					else InOutIo.m_AyActive = uint8_t((Value & 1) ^ 1);
				}
			}
			AyState& Chip = InOutIo.ActiveAy();
			Chip.m_Latch = uint8_t(Value & 0x0F);
			return;
		}
		if (UNLIKELY((Port & 0xC002) == 0x8000))
		{
			AyState& Chip = InOutIo.ActiveAy();
			const uint8_t R = uint8_t(Chip.m_Latch & 0x0F);
			const uint8_t V = uint8_t(Value & kAyWriteMask[R]);
			Chip.m_Registers[R] = V;
			++Chip.m_WritesObserved;
			if (R == 13) ++Chip.m_EnvelopeWrites;
			Chip.m_RegistersTouched = uint16_t(Chip.m_RegistersTouched | (1u << R));
			PsgCapture* const Cap = InOutIo.m_AyActive == 2 ? nullptr :
				(InOutIo.m_AyActive == 1 ? InOutIo.m_Capture2 : InOutIo.m_Capture);
			if (UNLIKELY(Cap != nullptr))
			{
				Cap->OnAyWrite(R, V);
			}
			if (UNLIKELY(InOutIo.m_Audio != nullptr) && InOutIo.m_AyActive < 2)
			{
				InOutIo.m_Audio->OnAyWrite(InOutIo.m_AyActive != 0 ? 1 : 0, R, V,
					InOutIo.WallNanosNow(InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0));
			}
			return;
		}
	}

	if ((Port & 0xC002) == 0x4000)
	{
		InOutIo.m_Mem.WritePort7FFD(Value);
		return;
	}
	// +2A/+3 second paging port $1FFD (A15=A14=A13=0, A12=1, A1=0).
	if (InOutIo.m_Mem.IsPlus3() && (Port & 0xF002) == 0x1000)
	{
		InOutIo.m_Mem.WritePort1FFD(Value);
		return;
	}

	// ZX Next register port. $243B selects the NextReg index; $253B writes
	// the data byte to the selected register. Only on Next machines: real
	// classic hardware ignores these addresses, and letting them through
	// would give a 48K/128K program the power to remap MMU slots or
	// palettes that its machine does not have.
	if (InOutIo.m_Mem.IsNext())
	{
		if (InOutIo.m_Uart && NextUart::IsUartPort(Port))
		{
			const uint64_t Cycles = InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0;
			InOutIo.m_Uart->WritePort(Port, Value, InOutIo.WallNanosNow(Cycles), (InOutIo.m_Mem.GetNextReg(0xA0) & 0x10) != 0);
			return;
		}
		if (Port == 0x243B)
		{
			InOutIo.m_Mem.SetLastSelectedNextReg(Value);
			return;
		}
		if (Port == 0x253B)
		{
			InOutIo.m_Mem.WriteNextReg(InOutIo.m_Mem.GetLastSelectedNextReg(), Value,
				InOutIo.m_Cpu ? InOutIo.m_Cpu->PC : 0, InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0,
				BinaryTrace::kNRSource_Port253B);
			return;
		}
	}

	// Z80 CTC channels 0..7 at $183B..$1F3B (channel N at $(18+N)3B).
	// Model timer mode and counter-mode cascades from ZC/TO(N) to CLK/TRG(N+1).
	if ((Port & 0x00FF) == 0x3B && (Port >> 8) >= 0x18 && (Port >> 8) <= 0x1F)
	{
		InOutIo.m_Mem.WriteCtc(int(Port >> 8) - 0x18, Value, InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0);
		// A CTC write can move an IRQ deadline EARLIER than the dispatcher's
		// cached event horizon (a player re-arming a short count from inside
		// its own ISR does exactly that): report it (MemoryBus::OnDeadline pulls
		// Cpu.NextEventCycle in); the full recompute happens at the event gate.
		InOutIo.m_Mem.NotifyDeadline(InOutIo.m_Mem.CtcNextIrqDeadline());
		return;
	}

	// zxnDMA (port $6B). Demo DMA routines upload their program with
	// `ld c, $6B; outi` - OUTI puts the byte-counter B on A8..A15 so the
	// port walks $206B, $1F6B, $1E6B... The zxnDMA spec only decodes
	// A0..A7, so match on the low byte only. Skip cleanly when not on a
	// Next machine.
	if ((Port & 0xFF) == 0x6B && InOutIo.m_Dma)
	{
		// CMD_ENABLE arms a transfer (BytesRemaining set); the dispatcher
		// steps bytes via StepZxnDma in chunks bounded by the next event.
		//
		// If a fresh transfer was just armed (0 -> >0 transition on
		// BytesRemaining), arm the per-DMA-window drip: real hw's SRL hold
		// means exactly one main-program instruction executes between
		// "CMD_ENABLE arms" and "DMA grabs the bus and bus-locks the CPU"
		// (per the FPGA core cycle trace).
		const uint32_t BytesBefore = InOutIo.m_Dma->m_BytesRemaining;
		WriteZxnDma(*InOutIo.m_Dma, Value, InOutIo.m_Mem, InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0);
		if (BytesBefore == 0 && InOutIo.m_Dma->m_BytesRemaining > 0 && InOutIo.m_Cpu)
		{
			InOutIo.m_Cpu->m_BDmaResumePendingOneInstr = 1;
		}
		return;
	}

	// The Next's four 8-bit DACs: A and B are heard on the left, C and D on
	// the right. Ports are decoded on the low byte (ports.txt); all but $DF
	// need NR_08 bit 3, and $DF stays always live as it has been here.
	// See https://wiki.specnext.dev/Peripheral_3_Register.
	if (InOutIo.m_Audio || InOutIo.m_OnDacWrite)
	{
		const uint8_t Mask = NextDacMask(uint8_t(Port & 0xFF), (InOutIo.m_Mem.GetNextReg(0x08) & 0x08) != 0);
		if (Mask != 0)
		{
			const uint64_t Ns = InOutIo.WallNanosNow(InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0);
			if (InOutIo.m_Audio)
			{
				InOutIo.m_Audio->OnDacWrite(Mask, Value, Ns);
			}
			if (InOutIo.m_OnDacWrite)
			{
				InOutIo.m_OnDacWrite(InOutIo.m_OnDacWriteUser, Mask, Value, Ns);
			}
			// Don't return - these ports may be shared with other devices.
		}
	}

	// ZX Next sprite engine. Three ports:
	//   $303B = slot select (full 16-bit decoded - distinguishes from $7B etc.)
	//   $57   = attribute upload (low-byte decoded; demos use OTIR which
	//           walks the high byte through B)
	//   $5B   = pattern upload   (low-byte decoded for the same reason)
	if (InOutIo.m_Sprites)
	{
		if (Port == 0x303B)
		{
			WriteSpriteSlotSelect(*InOutIo.m_Sprites, Value, InOutIo.m_Mem);
			return;
		}
		if (Port == 0x123B)
		{
			InOutIo.m_Mem.WriteLayer2Port(Value);
			return;
		} // Layer 2 access port
		if ((Port & 0xFF) == 0x57)
		{
			WriteSpriteAttr(*InOutIo.m_Sprites, Value, InOutIo.m_Mem);
			return;
		}
		if ((Port & 0xFF) == 0x5B)
		{
			WriteSpritePattern(*InOutIo.m_Sprites, Value);
			return;
		}
	}

	// ULA: any even-low-byte port. Bits 0..2 are border colour; bit 4 (EAR)
	// is the beeper level - an audio-render event when --audio-out is on
	// (bit 3 MIC stays unmodelled).
	if ((Port & 0x0001) == 0)
	{
		if (UNLIKELY(InOutIo.m_Audio != nullptr))
		{
			InOutIo.m_Audio->OnBeeperEdge(
				uint8_t((Value >> 4) & 1), InOutIo.WallNanosNow(InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0));
		}
		InOutIo.m_Mem.SetBorderColour(Value);
		return;
	}
	// Other ports: floating bus, ignored - counted for compatibility sweeps.
	InOutIo.m_Mem.NoteUnhandledPortWrite(Port);
}

inline uint8_t ReadIoPort(IoContext& InOutIo, uint16_t Port)
{
	// While the TR-DOS ROM window is paged in, the Beta-128 FD1793 ports
	// ($1F/$3F/$5F/$7F/$FF) completely REPLACE the traditional ZX ports at those
	// addresses (and are reachable only then). So they must be decoded before the
	// input layer - e.g. the FDC status register at $1F otherwise collides with
	// the Kempston joystick. The FDC does not claim $FE, so keyboard reads (needed
	// for the TR-DOS A> prompt) still fall through to Input; and when the ROM is
	// paged out, this is skipped entirely and Kempston at $1F works normally.
	if (InOutIo.m_Trd && InOutIo.m_Mem.IsTrdosRomActive())
	{
		uint8_t V;
		if (InOutIo.m_Trd->MaybeReadPort(Port, V))
		{
			return V;
		}
	}
	if (InOutIo.m_Input && InOutIo.m_Input->HasScript())
	{
		uint8_t V;
		if (InOutIo.m_Input->MaybeReadPort(Port, V))
		{
			return V;
		}
	}
	// Timex SCLD readback. Both SCLD registers read back the value last written
	// ($FF = DEC, $F4 = HSR), matching Fuse's scld_dec_read/scld_hsr_read.
	//
	// This is not a nicety. The 2068's RAM-resident bank pager does read-modify-
	// write on both ports - `in a,($f4)` / mask / `out ($f4),a` - so if the reads
	// float to $FF it computes a garbage chunk mask and pages the EXROM over
	// almost the whole address space, taking itself out with it. That is exactly
	// how a 2068 tape boot died here before this existed.
	//
	// $FF is also the floating-bus port on a plain Spectrum, but a Timex decodes
	// it as the SCLD, so this must come before any floating-bus fallback. It is
	// gated on IsScldCapable() so non-Timex machines keep the old behaviour.
	//
	// The Next is the exception: it decodes $FF WRITES as the Timex mode but
	// answers reads only when NR $08 bit 2 is set ("Enable port 0xff Timex
	// video mode read (hides floating bus on 0xff)", hard reset = 0). With the
	// bit clear $FF must keep floating, because that is what Next code doing
	// floating-bus tricks expects - so fall through to the bus below instead.
	if (UNLIKELY(InOutIo.m_Mem.IsScldCapable()))
	{
		const uint8_t Lo = uint8_t(Port & 0xFF);
		if (Lo == 0xFF && (!InOutIo.m_Mem.IsNext() || (InOutIo.m_Mem.GetNextReg(0x08) & 0x04) != 0))
		{
			return InOutIo.m_Mem.GetScldMode();
		}
		if (Lo == 0xF4 && InOutIo.m_Mem.IsTimex2068())
		{
			return InOutIo.m_Mem.GetScldBank();
		}
	}
	// ZX Evolution BaseConf reads. $FFBF is the config-read port: bits 7:6 are
	// hardwired 0 and actively driven, so it reads a stable value (not floating
	// $FF). The invite's DetectEvo demands bits 7:6 == 0 + 16 identical reads -
	// returning 0x00 passes it, while non-ZX-Evo machines return floating $FF and
	// fail. $BFF7 reads the RTC data cell when CMOS is enabled.
	if (UNLIKELY(InOutIo.m_Mem.IsZxEvo()))
	{
		if (Port == 0xFFBF)
		{
			return 0x00;
		}
		if ((InOutIo.m_Mem.GetEff7() & 0x80) && Port == 0xBFF7)
		{
			return InOutIo.m_Mem.ReadCmos();
		}
	}
	// ZX-Uno data port ($FD3B) returns the selected register: CoreID string
	// bytes (reg $FF, for detection) or the SCANDBL/turbo latch (reg $0B).
	if (UNLIKELY(InOutIo.m_Mem.IsZxUno()) && Port == 0xFD3B)
	{
		return InOutIo.m_Mem.ReadUnoData();
	}
	// Timex 2068 AY data port ($F6, decoded on the low byte like the write path)
	// is bidirectional on real hardware: reading it returns the latched register,
	// same semantics as the 128K $FFFD read.
	if (UNLIKELY(InOutIo.m_Mem.IsTimex()) && InOutIo.m_bAyEnabled && InOutIo.m_Mem.IsTimex2068() &&
		(Port & 0xFF) == 0xF6)
	{
		return InOutIo.m_Ay.m_Registers[InOutIo.m_Ay.m_Latch & 0x0F];
	}
	// Timex SCLD port $FF latches the last written value and reads it back on
	// TC2048/TS2068/TC2068 - SCLD detection code relies on this echo.
	if (UNLIKELY(InOutIo.m_Mem.IsScldCapable()) && (Port & 0xFF) == 0xFF)
	{
		return InOutIo.m_Mem.GetScldMode();
	}
	if (LIKELY(InOutIo.m_bAyEnabled) && UNLIKELY((Port & 0xC002) == 0xC000))
	{
		const AyState& Chip = InOutIo.ActiveAy();
		return Chip.m_Registers[Chip.m_Latch & 0x0F];
	}
	// ZX Next register port reads (Next machines only; classic hardware
	// leaves these floating).
	if (InOutIo.m_Mem.IsNext() && InOutIo.m_Uart && NextUart::IsUartPort(Port))
	{
		const uint64_t Cycles = InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0;
		return InOutIo.m_Uart->ReadPort(Port, InOutIo.WallNanosNow(Cycles), (InOutIo.m_Mem.GetNextReg(0xA0) & 0x10) != 0);
	}
	if (InOutIo.m_Mem.IsNext() && Port == 0x243B)
	{
		return InOutIo.m_Mem.GetLastSelectedNextReg();
	}
	if (InOutIo.m_Mem.IsNext() && Port == 0x253B)
	{
		// Computed registers (identity $00/$01/$0E, raster line $1E/$1F) come
		// from the bus's read hooks; everything else reads back as stored.
		return InOutIo.m_Mem.ReadNextReg(InOutIo.m_Mem.GetLastSelectedNextReg());
	}
	if ((Port & 0x00FF) == 0x3B && (Port >> 8) >= 0x18 && (Port >> 8) <= 0x1F)
	{
		// CTC channel N at $(18+N)3B - live down-counter read.
		return InOutIo.m_Mem.ReadCtc(int(Port >> 8) - 0x18, InOutIo.m_Cpu ? InOutIo.m_Cpu->m_Cycles : 0);
	}
	if (Port == 0x123B) // Layer 2 access port read-back
	{
		return InOutIo.m_Mem.ReadLayer2Port();
	}
	// zxnDMA status (port partial-decoded on low byte, same as write at L796).
	if ((Port & 0xFF) == 0x6B && InOutIo.m_Dma)
	{
		return ReadZxnDma(*InOutIo.m_Dma);
	}
	// +2A/+3 quirk (Marta, 2026-09): a read of port $0FFD returns the floating
	// bus with bit 0 forced to 1 - a raster-sync source that was "not known
	// BITD". Only on the +2A/+3.
	if (InOutIo.m_Mem.IsPlus3() && Port == 0x0FFD)
	{
		return uint8_t(InOutIo.m_Mem.FloatingBusByte() | 0x01);
	}
	InOutIo.m_Mem.NoteUnhandledPortRead(Port); // compatibility sweeps: what did the program poll?
	// Classic floating bus (48K / 128K / +2A / +3): the ULA drives the data bus
	// during the paper fetch, so an idle-bus read returns the byte under the
	// beam. Off (=> $FF) on every other machine. See MemoryBus::FloatingBusByte.
	return InOutIo.m_Mem.FloatingBusOn() ? InOutIo.m_Mem.FloatingBusByte() : 0xFF;
}

} // namespace

void EnableFrameInterrupts(Z80State& InOutCpu, uint64_t FrameIntPeriodTStates)
{
	InOutCpu.m_FrameIntPeriod = FrameIntPeriodTStates;
	InOutCpu.m_NextFrameIntCycle = InOutCpu.m_Cycles + FrameIntPeriodTStates;
}

void DisableFrameInterrupts(Z80State& InOutCpu)
{
	InOutCpu.m_FrameIntPeriod = 0;
	InOutCpu.m_NextFrameIntCycle = UINT64_MAX;
}

void EnablePsgTicks(Z80State& InOutCpu, uint64_t PsgTickPeriodTStates)
{
	InOutCpu.m_PsgTickPeriod = PsgTickPeriodTStates;
	InOutCpu.m_NextPsgTickCycle = InOutCpu.m_Cycles + PsgTickPeriodTStates;
}

void DisablePsgTicks(Z80State& InOutCpu)
{
	InOutCpu.m_PsgTickPeriod = 0;
	InOutCpu.m_NextPsgTickCycle = UINT64_MAX;
}

void EnableStatsTicks(Z80State& InOutCpu, uint64_t StatsTickPeriodTStates)
{
	InOutCpu.m_StatsTickPeriod = StatsTickPeriodTStates;
	InOutCpu.m_NextStatsTickCycle = InOutCpu.m_Cycles + StatsTickPeriodTStates;
	InOutCpu.m_LastStatsSnapshotHalt = InOutCpu.m_HaltCycles;
	InOutCpu.m_PerFrameHalt.clear();
}

void DisableStatsTicks(Z80State& InOutCpu)
{
	InOutCpu.m_StatsTickPeriod = 0;
	InOutCpu.m_NextStatsTickCycle = UINT64_MAX;
}

// Instantiate the dispatcher per machine, tracing, breakpoint and memory
// policy. if constexpr removes inactive features from the instruction loop.
template <bool kNext, bool kTrace, bool kBreak, MemoryBus::Access kMem> struct DispatchPolicy
{
	static constexpr bool bNext = kNext; // Z80N decoded, NEXTREG/MMU reachable
	static constexpr bool bTrace = kTrace; // per-instruction trace frames
	static constexpr bool bBreak = kBreak; // --until-pc check compiled into the loop (else it vanishes)
	// Memory-access mode (see MemoryBus::Access). Fast / FastContended are the
	// "plain run": no coverage / watchpoint / invariant / beam / trace hook /
	// video-out armed, so every per-access diagnostic branch in Rd/Wr/Fetch and
	// the per-instruction feature checks fold away; FastContended keeps ONLY the
	// ULA contention accrual (48K/128K/Timex default). Decided once in RunUntil.
	static constexpr MemoryBus::Access Mem = kMem;
	static constexpr bool bPlain = (kMem != MemoryBus::Access::kAccessFull); // diagnostics folded
	static constexpr bool bCont = (kMem != MemoryBus::Access::kAccessFast); // contention drain present
};

template <class Pol>
static RunSummary RunUntilImpl(Z80State& InOutCpu, IoContext& InOutIo, uint64_t& InOutBudget, TraceWriter* Trace)
{
	constexpr bool bTrace = Pol::bTrace;

	MemoryBus& Mem = InOutIo.m_Mem;
	Trdos* Trd = InOutIo.m_Trd;
	InputState* Input = InOutIo.m_Input;
	const bool bResetTrapEnabled = InOutIo.m_bResetTrap;
	const bool bInputLog = InOutIo.m_bInputLog;
	[[maybe_unused]] const uint16_t UntilPcTarget = InOutIo.m_UntilPc; // used only when Pol::bBreak (--until-pc)
	[[maybe_unused]] const int32_t UntilPcPage = InOutIo.m_UntilPcPage;

	RunResult Result = RunResult::kRunResultBudgetReached;
	// Contention banked at the top of this iteration (0 when none is armed);
	// the HALT DI-adjacency check adds it to DI's nominal 4 T.
	[[maybe_unused]] uint64_t Drained = 0;
	uint16_t LastPC = InOutCpu.PC;
	uint8_t LastOp = 0;

#if INSPECCY_TRACE_SUPPORT
	TraceFrame* PendingFrame = nullptr;
	if constexpr (bTrace)
	{
		Mem.m_OnWrite = &TraceWriter::MemWriteHook;
		Mem.m_OnWriteUser = Trace;
	}
#else
	(void)Trace;
#endif

	bool bEiClearOnNext = false;
	InOutCpu.m_NextUartPollCycle = (InOutIo.m_Uart != nullptr) ? InOutCpu.m_Cycles : UINT64_MAX;
	auto ScheduleNext = [&]()
	{
		RecomputeNextEvent(InOutCpu, InOutBudget, Mem.CtcNextIrqDeadline(), Mem.Copper().m_NextFireCycle);
		if constexpr (!Pol::bPlain)
		{
			if (InOutIo.m_Video && InOutIo.m_Video->m_NextEmitCycle < InOutCpu.m_NextEventCycle)
			{
				InOutCpu.m_NextEventCycle = InOutIo.m_Video->m_NextEmitCycle;
			}
		}
	};
	ScheduleNext();

	const bool bTrdosSupportActive = (Trd != nullptr) && Trd->IsSupportActive();
	const bool bInputActive = (Input != nullptr) && Input->HasScript();

	// Beam-frame boundary deadline for the eager drawer (see the flush below).
	// Tracked independently of interrupt acceptance (IFF1) so a DI demo still
	// finalises a frame per 50 Hz beam frame. Seeded to the first frame interrupt.
	uint64_t& NextBeamFrameCycle = InOutIo.m_NextBeamFrameCycle;
	if (NextBeamFrameCycle == 0)
	{
		NextBeamFrameCycle = InOutCpu.m_NextFrameIntCycle;
	}

	while (true)
	{
#if INSPECCY_TRACE_SUPPORT
		if constexpr (bTrace)
		{
			if (PendingFrame != nullptr)
			{
				TraceSnapshotPost(InOutCpu, *PendingFrame);
				Trace->CommitFrame();
				PendingFrame = nullptr;
			}
		}
#endif

		// Video-frame capture (--video-out). Cycle-cadence emit - once per
		// PeriodCycles we write a composite BMP for the current state. The
		// common case (no recording) is a single nullptr check; the slow
		// path runs at most fps times per emulated second.
		if constexpr (!Pol::bPlain)
		{
			if (UNLIKELY(InOutIo.m_Video != nullptr && InOutCpu.m_Cycles >= InOutIo.m_Video->m_NextEmitCycle))
			{
				// finish scanning up to the capture boundary before selecting
				// the completed image. Otherwise a quiet frame repeats the
				// preceding image until the next display write flushes the beam.
				if (Mem.HasBeamBoundaryHook() && Mem.IsBeamArmed(InOutCpu.m_Cycles))
				{
					Mem.FireBeamBoundary(InOutCpu.m_Cycles);
				}
				std::string Error;
				if (!EmitVideoFrame(*InOutIo.m_Video, Mem, InOutIo.m_Sprites, InOutIo.m_Beam, Error))
				{
					if (!InOutIo.m_Video->m_bWriteFailed && !Error.empty())
					{
						fprintf(stderr, "video: %s\n", Error.c_str());
					}
					InOutIo.m_Video->m_bWriteFailed = true;
				}
				// The caller reports capture failures through the process exit code.
			}
		}

		// Motion / brightness metrics (--motion-out, --histogram-out). Same
		// cycle cadence as the video capture, and the same reasoning: one
		// nullptr check on the common path.
		if constexpr (!Pol::bPlain)
		{
			if (UNLIKELY(InOutIo.m_Metrics != nullptr && InOutCpu.m_Cycles >= InOutIo.m_Metrics->m_NextEmitCycle))
			{
				std::string Error;
				if (!EmitMetricsFrame(*InOutIo.m_Metrics, Mem, InOutIo.m_Sprites, Error))
				{
					if (!InOutIo.m_Metrics->m_bWriteFailed && !Error.empty())
					{
						fprintf(stderr, "metrics: %s\n", Error.c_str());
					}
					InOutIo.m_Metrics->m_bWriteFailed = true;
				}
			}
		}

		// EI-defer two-step latch: an interrupt pending after EI must not be
		// accepted until one further instruction has executed. We snapshot at
		// top-of-iteration and clear at the START of the iteration after the
		// one where bEiPending was observed. This avoids touching every
		// `continue` in the dispatcher.
		if (bEiClearOnNext)
		{
			InOutCpu.m_bEiPending = false;
			bEiClearOnNext = false;
		}
		const bool bEiAtStart = InOutCpu.m_bEiPending;
		if (bEiAtStart)
		{
			bEiClearOnNext = true;
		}

		// Coarse contention: bank the penalty accrued by the previous
		// instruction's contended memory accesses BEFORE the event checks
		// read Cpu.Cycles. Off = one predictable not-taken branch.
		if constexpr (Pol::bCont)
		{
			Drained = 0;
			if (UNLIKELY(Mem.ContentionOn()))
			{
				Drained = Mem.TakeContentionPenalty();
				InOutCpu.m_Cycles += Drained;
			}
		}

		// Top-of-loop event check. Owns BOTH budget exit and interrupt accept.
		if (UNLIKELY(InOutCpu.m_Cycles >= InOutCpu.m_NextEventCycle))
		{
			if (InOutCpu.m_Cycles >= InOutBudget)
			{
				goto done;
			}

			// --- Line interrupt (NR $22/$23) - source index 0, HIGHEST priority.
			// Placed before the CTC scan and the ULA gate: AcceptInterrupt clears
			// IFF1, so both short-circuit and the line int wins when it coincides
			// with a CTC channel (3..10) or the ULA (11). Legacy /INT: $0038 (IM1)
			// or $FF bus (IM2). HW-vector: vector = base | (0<<1) = base. Same
			// bounded /INT pulse as the frame int (a DI program misses it, counted);
			// latched (no window) under HW-vector mode like the ULA.
			if (InOutCpu.m_bLineIntEnabled && InOutCpu.m_NextLineIntCycle != UINT64_MAX &&
				InOutCpu.m_FrameIntPeriod != 0)
			{
				if (InOutCpu.m_IntWindow != 0 && !Mem.IsIm2HwVectorEnabled())
				{
					while (InOutCpu.m_Cycles >= InOutCpu.m_NextLineIntCycle + InOutCpu.m_IntWindow)
					{
						++InOutCpu.m_LineIntsMissed;
						if (InOutIo.m_IrqDiagnostics)
						{
							InOutIo.m_IrqDiagnostics->Expired(IrqDiagnostics::Line, InOutCpu, InOutIo,
								InOutCpu.m_NextLineIntCycle, InOutCpu.m_NextLineIntCycle + InOutCpu.m_IntWindow);
						}
						InOutCpu.m_NextLineIntCycle += InOutCpu.m_FrameIntPeriod;
					}
				}
				if (!bEiAtStart && InOutCpu.IFF1 && InOutCpu.m_NextLineIntCycle != UINT64_MAX &&
					InOutCpu.m_Cycles >= InOutCpu.m_NextLineIntCycle &&
					(!Mem.IsIm2HwVectorEnabled() || Mem.IsLineInterruptEnabled()))
				{
					const uint8_t BusValue =
						Mem.IsIm2HwVectorEnabled()
							? uint8_t(Mem.Im2VectorBaseHi3() | uint8_t(0x00 << 1)) // source index 0
							: uint8_t(0xFF);
#if INSPECCY_TRACE_SUPPORT
					TraceFrame* LineFrame = nullptr;
					if constexpr (bTrace)
					{
						LineFrame = Trace->BeginFrame();
						LineFrame->m_Flags = TraceFrame::kFlagInterrupt;
						LineFrame->m_StartCycles = InOutCpu.m_Cycles;
						LineFrame->m_Frame =
							(InOutCpu.m_FrameIntPeriod > 0) ? (InOutCpu.m_Cycles / InOutCpu.m_FrameIntPeriod) : 0;
						LineFrame->m_PrePC = InOutCpu.PC;
						LineFrame->m_PreAF = InOutCpu.AF;
						LineFrame->m_PreBC = InOutCpu.BC;
						LineFrame->m_PreDE = InOutCpu.DE;
						LineFrame->m_PreHL = InOutCpu.HL;
						LineFrame->m_PreIX = InOutCpu.IX;
						LineFrame->m_PreIY = InOutCpu.IY;
						LineFrame->m_PreSP = InOutCpu.SP;
						LineFrame->m_PreAFp = InOutCpu.AFp;
						LineFrame->m_PreBCp = InOutCpu.BCp;
						LineFrame->m_PreDEp = InOutCpu.DEp;
						LineFrame->m_PreHLp = InOutCpu.HLp;
						LineFrame->m_PreMemPtr = InOutCpu.m_MemPtr;
						LineFrame->m_PreI = InOutCpu.I;
						LineFrame->m_PreR = InOutCpu.R;
						LineFrame->m_PreIFF1 = InOutCpu.IFF1;
						LineFrame->m_PreIFF2 = InOutCpu.IFF2;
						LineFrame->m_PreHalted = InOutCpu.m_bHalted ? 1 : 0;
						const MemoryBus::Page8KInfo P = Mem.Page8KAt(InOutCpu.PC);
						LineFrame->m_PrePageTag = BinaryTrace::MakePageTag(P.m_bIsRom, P.m_Index);
						LineFrame->m_InterruptVectorByte = BusValue;
						LineFrame->m_OpcodeBytes[0] = BusValue;
						LineFrame->m_OpcodeBytes[1] = BinaryTrace::kIntAckMarker;
					}
#endif
					if (InOutIo.m_IrqDiagnostics)
					{
						InOutIo.m_IrqDiagnostics->BeforeAccept(IrqDiagnostics::Line, InOutCpu, InOutIo);
					}
					AcceptInterrupt<Pol>(InOutCpu, Mem, BusValue);
#if INSPECCY_TRACE_SUPPORT
					if constexpr (bTrace)
					{
						TraceSnapshotPost(InOutCpu, *LineFrame);
						Trace->CommitFrame();
					}
#endif
					++InOutCpu.m_LineIntsAccepted;
					InOutCpu.m_NextLineIntCycle += InOutCpu.m_FrameIntPeriod; // re-arm next frame
				}
			}

			// UART Rx interrupts (NR $C6) - source index 1 (UART0) and 2 (UART1),
			// above the CTC channels and the ULA. Level requests from the Rx FIFO:
			// near full (>= 384 of 512, $C6 bit 1/5) or any byte ($C6 bit 0/4).
			// The FIFO is advanced here on a fixed grid (kUartPollCycles, ~100 us
			// at 28 MHz) so a program that stopped polling still sees the level
			// rise on time; the status bit in NR $CA is set when the vector is
			// taken. Only under IM 2 hardware vectoring (NR $C0 bit 0).
			if (InOutIo.m_Uart != nullptr && InOutCpu.m_Cycles >= InOutCpu.m_NextUartPollCycle)
			{
				InOutCpu.m_NextUartPollCycle = InOutCpu.m_Cycles + kUartPollCycles;
				if (Mem.IsIm2HwVectorEnabled() && (Mem.IsUartRxIntEnabled(0) || Mem.IsUartRxIntEnabled(1)))
				{
					InOutIo.m_Uart->Advance(InOutIo.WallNanosNow(InOutCpu.m_Cycles), (Mem.GetNextReg(0xA0) & 0x10) != 0);
				}
			}
			if (!bEiAtStart && InOutCpu.IFF1 && InOutIo.m_Uart != nullptr && Mem.IsIm2HwVectorEnabled())
			{
				for (int Ch = 0; Ch < 2; ++Ch)
				{
					if (Mem.IsUartRxIntEnabled(Ch) && InOutIo.m_Uart->RxIntRequest(Ch, Mem.IsUartRxNearFullMode(Ch)))
					{
						Mem.NoteUartRxInt(Ch);
						++InOutIo.m_Uart->m_Ch[Ch].m_RxInts;
						AcceptInterrupt<Pol>(InOutCpu, Mem, uint8_t(Mem.Im2VectorBaseHi3() | uint8_t((1 + Ch) << 1)));
						break;
					}
				}
			}

			// Z80 CTC channels - higher priority than ULA per Next source
			// index order (CTC0 = 3 .. CTC7 = $A, ULA = $B; lower wins, so
			// scan channels in index order). Only fires when IM 2 hardware
			// vector routing is enabled (NR_C0 bit 0) and the channel's mask
			// bit is set (NR_C5 bit N). Bus value low 5 bits = source index
			// (3 + N) * 2, i.e. channel N vectors to IM 2 table entry N + 3.
			int CtcN = -1;
			if (!bEiAtStart && InOutCpu.IFF1 && Mem.IsIm2HwVectorEnabled())
			{
				for (int I = 0; I < 8; ++I)
				{
					const MemoryBus::CtcChannel& Ch = Mem.Ctc(I);
					if (Ch.m_bEnabled && Ch.m_NextFireCycle != UINT64_MAX && InOutCpu.m_Cycles >= Ch.m_NextFireCycle &&
						Mem.IsCtcInterruptEnabled(I))
					{
						CtcN = I;
						break;
					}
				}
			}
			if (CtcN >= 0)
			{
				const uint8_t BusValue = uint8_t(Mem.Im2VectorBaseHi3() | uint8_t((3 + CtcN) << 1));
#if INSPECCY_TRACE_SUPPORT
				TraceFrame* CtcFrame = nullptr;
				if constexpr (bTrace)
				{
					CtcFrame = Trace->BeginFrame();
					CtcFrame->m_Flags = TraceFrame::kFlagInterrupt;
					CtcFrame->m_StartCycles = InOutCpu.m_Cycles;
					CtcFrame->m_Frame =
						(InOutCpu.m_FrameIntPeriod > 0) ? (InOutCpu.m_Cycles / InOutCpu.m_FrameIntPeriod) : 0;
					CtcFrame->m_PrePC = InOutCpu.PC;
					CtcFrame->m_PreAF = InOutCpu.AF;
					CtcFrame->m_PreBC = InOutCpu.BC;
					CtcFrame->m_PreDE = InOutCpu.DE;
					CtcFrame->m_PreHL = InOutCpu.HL;
					CtcFrame->m_PreIX = InOutCpu.IX;
					CtcFrame->m_PreIY = InOutCpu.IY;
					CtcFrame->m_PreSP = InOutCpu.SP;
					CtcFrame->m_PreAFp = InOutCpu.AFp;
					CtcFrame->m_PreBCp = InOutCpu.BCp;
					CtcFrame->m_PreDEp = InOutCpu.DEp;
					CtcFrame->m_PreHLp = InOutCpu.HLp;
					CtcFrame->m_PreMemPtr = InOutCpu.m_MemPtr;
					CtcFrame->m_PreI = InOutCpu.I;
					CtcFrame->m_PreR = InOutCpu.R;
					CtcFrame->m_PreIFF1 = InOutCpu.IFF1;
					CtcFrame->m_PreIFF2 = InOutCpu.IFF2;
					CtcFrame->m_PreHalted = InOutCpu.m_bHalted ? 1 : 0;
					const MemoryBus::Page8KInfo P = Mem.Page8KAt(InOutCpu.PC);
					CtcFrame->m_PrePageTag = BinaryTrace::MakePageTag(P.m_bIsRom, P.m_Index);
					CtcFrame->m_InterruptVectorByte = BusValue;
					// The binary record has no dedicated vector field; its
					// (unused) opcode bytes carry the acknowledge bus value so
					// consumers can attribute CPU time to the firing device.
					CtcFrame->m_OpcodeBytes[0] = BusValue;
					CtcFrame->m_OpcodeBytes[1] = BinaryTrace::kIntAckMarker;
				}
#endif
				AcceptInterrupt<Pol>(InOutCpu, Mem, BusValue);
#if INSPECCY_TRACE_SUPPORT
				if constexpr (bTrace)
				{
					TraceSnapshotPost(InOutCpu, *CtcFrame);
					Trace->CommitFrame();
				}
#endif
				MemoryBus::CtcChannel& Fired = Mem.Ctc(CtcN);
				++Fired.m_InterruptsFired;
				// Advance by the effective period - grid-aligned, since the
				// initial fire was on the grid (auto-reload on zero: a late
				// reprogram gets the OLD interval, never a stall).
				Fired.m_NextFireCycle += Fired.m_PeriodCycles;
				if (Fired.m_NextFireCycle < InOutCpu.m_Cycles)
				{
					Fired.m_NextFireCycle = InOutCpu.m_Cycles + Fired.m_PeriodCycles;
				}
				// After accepting CTC0, fall through to the ULA gate - but
				// since AcceptInterrupt already cleared IFF1, the next
				// check below short-circuits. The frame int will fire on
				// the NEXT pass through the loop.
			}

			// SCLD INTDISABLE ($FF bit 6) holds the ULA /INT line off on Timex
			// machines. Treated exactly like IFF1 == 0: the interrupt is not
			// taken and NextFrameIntCycle is left in the past, so when the bit
			// clears the coalescing re-arm below fires one immediately - which
			// is what Fuse does explicitly on the disable->enable edge.
			// Bounded /INT window (hardware behaviour): the ULA asserts /INT for
			// IntWindow T-states at the frame top and then drops it. A pulse the
			// CPU could not take in time (IFF1=0 inside a long ISR or DI
			// section, or Timex INTDISABLE) is MISSED - counted, and the request
			// moves to the next frame top with phase kept. This is what makes an
			// overrunning ISR visible: on hardware it halves the frame rate; the
			// legacy sticky model (IntWindow==0) took it late, mid-frame.
			// Exception: with the Next's hardware interrupt controller enabled
			// (NR $C0 IM 2 vectoring) each source is LATCHED as a pending status
			// bit until the CPU services it, so the ULA frame interrupt keeps
			// the sticky semantics there - a CTC ISR spanning the frame top does
			// not lose the frame on that hardware.
			if (InOutCpu.m_IntWindow != 0 && InOutCpu.m_NextFrameIntCycle != UINT64_MAX &&
				InOutCpu.m_FrameIntPeriod != 0 && !Mem.IsIm2HwVectorEnabled())
			{
				while (InOutCpu.m_Cycles >= InOutCpu.m_NextFrameIntCycle + InOutCpu.m_IntWindow)
				{
					// $22 bit 2 masks the ULA (frame) interrupt: the pulse still
					// passes (slide the deadline so phase never drifts) but it is
					// suppressed, not "missed" - a masked frame int is intentional.
					if (!InOutCpu.m_bUlaIntMasked)
					{
						++InOutCpu.m_MissedInterrupts;
						if (InOutIo.m_IrqDiagnostics)
						{
							InOutIo.m_IrqDiagnostics->Expired(IrqDiagnostics::Frame, InOutCpu, InOutIo,
								InOutCpu.m_NextFrameIntCycle, InOutCpu.m_NextFrameIntCycle + InOutCpu.m_IntWindow);
						}
					}
					InOutCpu.m_NextFrameIntCycle += InOutCpu.m_FrameIntPeriod;
				}
			}
			if (!bEiAtStart && InOutCpu.IFF1 && !Mem.IsTimexIntDisabled() &&
				!InOutCpu.m_bUlaIntMasked // $22 bit 2 masks the ULA int
				&& InOutCpu.m_NextFrameIntCycle != UINT64_MAX && InOutCpu.m_Cycles >= InOutCpu.m_NextFrameIntCycle &&
				(!Mem.IsIm2HwVectorEnabled() || Mem.IsUlaInterruptEnabled()))
			{
				// Bus value the CPU sees during INT acknowledge. Defaults to
				// $FF (the floating-bus value on real hardware when no
				// device drives the bus). When the dot command has enabled
				// Spectrum Next's IM 2 hardware vector via NR_C0 bit 0, the
				// hardware routes a per-source value instead: the top 3
				// bits come from NR_C0[7:5] and the low 5 bits are the
				// source index * 2. For the ULA frame interrupt the source
				// index is $B (per the Next interrupt priority order), so
				// the low 5 bits are $16.
				const uint8_t BusValue =
					Mem.IsIm2HwVectorEnabled() ? uint8_t(Mem.Im2VectorBaseHi3() | uint8_t(0x0B << 1)) : uint8_t(0xFF);
#if INSPECCY_TRACE_SUPPORT
				TraceFrame* IntFrame = nullptr;
				if constexpr (bTrace)
				{
					IntFrame = Trace->BeginFrame();
					IntFrame->m_Flags = TraceFrame::kFlagInterrupt;
					IntFrame->m_StartCycles = InOutCpu.m_Cycles;
					IntFrame->m_Frame =
						(InOutCpu.m_FrameIntPeriod > 0) ? (InOutCpu.m_Cycles / InOutCpu.m_FrameIntPeriod) : 0;
					IntFrame->m_PrePC = InOutCpu.PC;
					IntFrame->m_PreAF = InOutCpu.AF;
					IntFrame->m_PreBC = InOutCpu.BC;
					IntFrame->m_PreDE = InOutCpu.DE;
					IntFrame->m_PreHL = InOutCpu.HL;
					IntFrame->m_PreIX = InOutCpu.IX;
					IntFrame->m_PreIY = InOutCpu.IY;
					IntFrame->m_PreSP = InOutCpu.SP;
					IntFrame->m_PreAFp = InOutCpu.AFp;
					IntFrame->m_PreBCp = InOutCpu.BCp;
					IntFrame->m_PreDEp = InOutCpu.DEp;
					IntFrame->m_PreHLp = InOutCpu.HLp;
					IntFrame->m_PreMemPtr = InOutCpu.m_MemPtr;
					IntFrame->m_PreI = InOutCpu.I;
					IntFrame->m_PreR = InOutCpu.R;
					IntFrame->m_PreIFF1 = InOutCpu.IFF1;
					IntFrame->m_PreIFF2 = InOutCpu.IFF2;
					IntFrame->m_PreHalted = InOutCpu.m_bHalted ? 1 : 0;
					const MemoryBus::Page8KInfo P = Mem.Page8KAt(InOutCpu.PC);
					IntFrame->m_PrePageTag = BinaryTrace::MakePageTag(P.m_bIsRom, P.m_Index);
					IntFrame->m_InterruptVectorByte = BusValue;
					IntFrame->m_OpcodeBytes[0] = BusValue;
					IntFrame->m_OpcodeBytes[1] = BinaryTrace::kIntAckMarker;
				}
#endif

				// Pixel-accurate: track the live interrupt phase so the copper
				// and the beam drawer anchor their frame origin to the ACTUAL
				// interrupt (which drifts during the dot command's setup, then
				// stabilises once it HALTs per frame). This is what puts the ISR
				// in the top border rather than on a visible scanline.
				if (Mem.IsPixelAccurate() && InOutCpu.m_FrameIntPeriod)
				{
					Mem.SetFrameIntPhase(InOutCpu.m_Cycles % InOutCpu.m_FrameIntPeriod);
				}
				if (InOutIo.m_IrqDiagnostics)
				{
					InOutIo.m_IrqDiagnostics->BeforeAccept(IrqDiagnostics::Frame, InOutCpu, InOutIo);
				}
				AcceptInterrupt<Pol>(InOutCpu, Mem, BusValue);

#if INSPECCY_TRACE_SUPPORT
				if constexpr (bTrace)
				{
					TraceSnapshotPost(InOutCpu, *IntFrame);
					Trace->CommitFrame();
				}
#endif

				// Re-arm next frame interrupt; coalesce if we missed several.
				InOutCpu.m_NextFrameIntCycle += InOutCpu.m_FrameIntPeriod;
				if (InOutCpu.m_NextFrameIntCycle < InOutCpu.m_Cycles)
				{
					InOutCpu.m_NextFrameIntCycle = InOutCpu.m_Cycles + InOutCpu.m_FrameIntPeriod;
				}
			}
			else if (InOutCpu.m_bUlaIntMasked && InOutCpu.m_NextFrameIntCycle != UINT64_MAX &&
					 InOutCpu.m_Cycles >= InOutCpu.m_NextFrameIntCycle)
			{
				// The ULA frame interrupt is explicitly MASKED ($22 bit 2): the
				// ULA still pulses INT every frame, the CPU just never takes it.
				// Its deadline must still advance - otherwise it sits stuck at a
				// past cycle, forcing the event gate every instruction and, if
				// the CPU halts waiting on ANOTHER source (e.g. the line int),
				// wedging the HALT fast-forward on a deadline that can never be
				// consumed. (A program that only DIs, without masking, keeps the
				// held-interrupt behaviour so an EI still catches the pending
				// int - that path is unchanged.)
				InOutCpu.m_NextFrameIntCycle += InOutCpu.m_FrameIntPeriod;
				if (InOutCpu.m_NextFrameIntCycle < InOutCpu.m_Cycles)
				{
					InOutCpu.m_NextFrameIntCycle = InOutCpu.m_Cycles + InOutCpu.m_FrameIntPeriod;
				}
			}

			// PSG capture tick - independent of IFF1, fires on cycle deadline.
			if (InOutCpu.m_NextPsgTickCycle != UINT64_MAX && InOutCpu.m_Cycles >= InOutCpu.m_NextPsgTickCycle)
			{
				if (InOutIo.m_Capture)
				{
					InOutIo.m_Capture->OnFrameTick();
				}
				if (InOutIo.m_Capture2)
				{
					InOutIo.m_Capture2->OnFrameTick();
				}
				InOutCpu.m_NextPsgTickCycle += InOutCpu.m_PsgTickPeriod;
				if (InOutCpu.m_NextPsgTickCycle < InOutCpu.m_Cycles)
				{
					InOutCpu.m_NextPsgTickCycle = InOutCpu.m_Cycles + InOutCpu.m_PsgTickPeriod;
				}
			}

			// Per-frame stats snapshot - independent of IFF1.
			if (InOutCpu.m_NextStatsTickCycle != UINT64_MAX && InOutCpu.m_Cycles >= InOutCpu.m_NextStatsTickCycle)
			{
				const uint64_t DeltaHalt = InOutCpu.m_HaltCycles - InOutCpu.m_LastStatsSnapshotHalt;
				InOutCpu.m_PerFrameHalt.push_back(uint32_t(DeltaHalt));
				InOutCpu.m_LastStatsSnapshotHalt = InOutCpu.m_HaltCycles;
				InOutCpu.m_NextStatsTickCycle += InOutCpu.m_StatsTickPeriod;
				if (InOutCpu.m_NextStatsTickCycle < InOutCpu.m_Cycles)
				{
					InOutCpu.m_NextStatsTickCycle = InOutCpu.m_Cycles + InOutCpu.m_StatsTickPeriod;
				}
				if (InOutIo.m_OnFrameTick != nullptr)
				{
					InOutIo.m_OnFrameTick(InOutIo.m_OnFrameTickUser); // frame-exact bmp checkpoints
				}
				if (InOutIo.m_Uart != nullptr) // deliver link bytes even if the program stopped polling
				{
					InOutIo.m_Uart->Advance(InOutIo.WallNanosNow(InOutCpu.m_Cycles), (InOutIo.m_Mem.GetNextReg(0xA0) & 0x10) != 0);
				}
			}

			// ZX Next Copper - a beam-synchronized coprocessor writing NextRegs
			// at raster deadlines. Executes only when armed; NextFireCycle is
			// UINT64_MAX when stopped/absent so this is a single predicted-not-
			// taken compare in the common case. StepCopper applies the MOVEs
			// due by now and re-arms NextFireCycle to its next WAIT.
			if (Mem.Copper().m_NextFireCycle != UINT64_MAX && InOutCpu.m_Cycles >= Mem.Copper().m_NextFireCycle)
			{
				StepCopper(Mem.Copper(), Mem, InOutCpu);
			}

			// Eager beam drawer: IFF1-INDEPENDENT per-frame boundary. The ULA
			// frame runs at 50 Hz whether or not the CPU accepts the interrupt
			// (a demo can run with DI throughout), so we must NOT
			// hang the drawer's per-frame finalisation off the IFF1-gated frame-
			// interrupt block above - that would leave the drawer stuck and every
			// frame blank under DI. Track the beam-frame boundary here on the raw
			// cycle deadline (same as the PSG tick). We're AFTER the copper this
			// iteration, so a HALT+catch-up copper has already composed the ending
			// frame; the flush then just finalises it (a no-op if writes already
			// rolled it over, or composes a still screen from live state).
			if (Mem.HasBeamBoundaryHook() && InOutCpu.m_FrameIntPeriod != 0 && NextBeamFrameCycle != UINT64_MAX)
			{
				int BeamGuard = 0;
				while (InOutCpu.m_Cycles >= NextBeamFrameCycle && BeamGuard++ < 4)
				{
					// Anchor the render frame origin to the ACTUAL beam phase
					// (the drawer/copper read this via FrameOriginFor). Under DI
					// the IFF1 block never sets it, so do it here.
					Mem.SetFrameIntPhase(NextBeamFrameCycle % InOutCpu.m_FrameIntPeriod);
					// The drawer is armed for every frame at setup (main.cpp): a
					// budget-relative "arm only the last frames" scheme leaves no
					// completed frame when the program ends early (RET from a dot
					// command, --until-pc, or a halt).
					if (Mem.IsBeamArmed(NextBeamFrameCycle))
					{
						Mem.FireBeamBoundary(NextBeamFrameCycle);
					}
					NextBeamFrameCycle += InOutCpu.m_FrameIntPeriod;
				}
			}

			ScheduleNext();
		}

		// F_READ time-slice: when the NextZxosVfs has a pending F_READ
		// and we're at the same Isr depth it was scheduled at, advance the
		// transfer by as many bytes as fit before the next event, charging
		// the per-byte cycle cost. Nested ISRs (depth > IsrDepthAtStart)
		// run their instructions normally below - F_READ stays paused
		// until they return - which matches real hw, where CTC0 nested
		// ISRs interleave with the file read (each writes a wav sample,
		// keeping audio alive during the read).
		if (UNLIKELY(InOutIo.m_DotCmd && InOutIo.m_DotCmd->FReadActive() &&
					 InOutCpu.m_IsrDepth == InOutIo.m_DotCmd->FReadDepth()))
		{
			const uint64_t PerByte = InOutIo.m_DotCmd->FReadPerByteCycles();
			uint32_t MaxBytes = 1;
			if (PerByte > 0 && InOutCpu.m_NextEventCycle > InOutCpu.m_Cycles)
			{
				const uint64_t Headroom = (InOutCpu.m_NextEventCycle - InOutCpu.m_Cycles) / PerByte;
				if (Headroom > 0)
				{
					MaxBytes = (Headroom > 0xFFFFFFFFull) ? 0xFFFFFFFFu : uint32_t(Headroom);
				}
			}
			uint64_t StepCharge = 0;
			InOutIo.m_DotCmd->StepFRead(Mem, MaxBytes, StepCharge);
			InOutCpu.m_Cycles += StepCharge;
			if (InOutIo.m_DotCmd->FReadComplete())
			{
				InOutIo.m_DotCmd->FinaliseFRead(InOutCpu);
			}
			continue;
		}

		// zxnDMA mid-flight chunking. While BytesRemaining > 0 the CPU is
		// bus-locked out of main code (regardless of HALT state) - but
		// ISRs can preempt; when one returns to depth 0 the drip-instr
		// quirk lets exactly one main instr execute before DMA resumes.
		// Placed BEFORE the HALT fast-forward so a halt+DMA combo still
		// moves bytes - real hw arbitrates DMA over the bus regardless
		// of the CPU's HALT state.
		if (UNLIKELY(InOutIo.m_Dma && InOutIo.m_Dma->m_BytesRemaining > 0 && InOutCpu.m_IsrDepth == 0))
		{
			if (InOutCpu.m_BDmaResumePendingOneInstr)
			{
				if (InOutIo.m_bTraceDrip)
				{
					uint8_t Op[4] = {0};
					for (int IdxI = 0; IdxI < 4; ++IdxI)
					{
						Op[IdxI] = Mem.Rd<Pol::Mem>(uint16_t(InOutCpu.PC + IdxI));
					}
					fprintf(stderr,
						"DRIP-FIRES:    cyc=%llu  PC=$%04X  running [%02X %02X %02X %02X]"
						"  bytes_left=%u\n",
						static_cast<unsigned long long>(InOutCpu.m_Cycles), InOutCpu.PC, Op[0], Op[1], Op[2], Op[3],
						InOutIo.m_Dma->m_BytesRemaining);
				}
				InOutCpu.m_BDmaResumePendingOneInstr = 0;
			}
			else
			{
				const uint64_t Headroom = (InOutCpu.m_NextEventCycle > InOutCpu.m_Cycles)
											  ? (InOutCpu.m_NextEventCycle - InOutCpu.m_Cycles) / 4
											  : 0;
				uint32_t MaxBytes = (Headroom == 0)				 ? 1u
									: (Headroom > 0xFFFFFFFFull) ? 0xFFFFFFFFu
																 : uint32_t(Headroom);
				if (MaxBytes > InOutIo.m_Dma->m_BytesRemaining)
				{
					MaxBytes = InOutIo.m_Dma->m_BytesRemaining;
				}
				uint32_t Done;
				if (InOutIo.m_Dma->m_bPortBIo)
				{
					// Port B = I/O: each byte goes through the normal port-write
					// dispatch so writes to $FE / $FFFD / etc. behave like the
					// CPU did them. StepZxnDma's tight-memcpy loop only handles
					// mem->mem; we replicate the cursor walk here.
					Done = MaxBytes;
					for (uint32_t IdxI = 0; IdxI < Done; ++IdxI)
					{
						const uint8_t B = Mem.Rd<Pol::Mem>(InOutIo.m_Dma->m_CurSrcAddr);
						WriteIoPort(InOutIo, InOutIo.m_Dma->m_CurDstAddr, B);
						if (!InOutIo.m_Dma->m_bPortAFixed)
						{
							InOutIo.m_Dma->m_CurSrcAddr = uint16_t(InOutIo.m_Dma->m_CurSrcAddr + (InOutIo.m_Dma->m_bPortADec ? -1 : 1));
						}
						if (InOutIo.m_Dma->m_bPortBInc)
						{
							InOutIo.m_Dma->m_CurDstAddr = uint16_t(InOutIo.m_Dma->m_CurDstAddr + 1);
						}
						else if (InOutIo.m_Dma->m_bPortBDec)
						{
							InOutIo.m_Dma->m_CurDstAddr = uint16_t(InOutIo.m_Dma->m_CurDstAddr - 1);
						}
					}
					InOutIo.m_Dma->m_BytesRemaining -= Done;
					InOutIo.m_Dma->m_BytesTransferred += Done;
				}
				else
				{
					Done = StepZxnDma(*InOutIo.m_Dma, Mem, MaxBytes);
				}
				InOutCpu.m_Cycles += uint64_t(Done) * 4;
				continue;
			}
		}

		// HALT fast-forward. With IFF1==1 we snap Cycles to the next event
		// deadline (interrupt OR PSG tick, whichever comes first) and advance
		// R by one IncR per skipped M1 (4 T-states each) to match real-hardware
		// R behaviour.
		if (UNLIKELY(InOutCpu.m_bHalted))
		{
			// Include CTC and line interrupts when choosing the next HALT deadline;
			// they may wake the CPU before the next ULA frame interrupt.
			const uint64_t CtcFire = Mem.CtcNextIrqDeadline();
			const bool bAnyWakeSource =
				(InOutCpu.m_NextFrameIntCycle != UINT64_MAX && InOutCpu.m_NextFrameIntCycle > InOutCpu.m_Cycles) ||
				(CtcFire != UINT64_MAX && CtcFire > InOutCpu.m_Cycles) ||
				(InOutCpu.m_NextLineIntCycle != UINT64_MAX && InOutCpu.m_NextLineIntCycle > InOutCpu.m_Cycles) ||
				(InOutCpu.m_NextUartPollCycle != UINT64_MAX && InOutCpu.m_NextUartPollCycle > InOutCpu.m_Cycles);
			if (bAnyWakeSource)
			{
				// Snap to the nearest deadline that is STRICTLY in the future.
				// A deadline at or before the current cycle is a due event the
				// event gate above owns - snapping to it would advance zero (or,
				// if it is stale/in the past, underflow) cycles and spin the CPU
				// here forever. bAnyWakeSource guarantees at least one real wake
				// source (frame/CTC/line) is strictly ahead, so Deadline is
				// always set to a value > Cpu.Cycles below.
				uint64_t Deadline = UINT64_MAX;
				auto Consider = [&](uint64_t D)
				{
					if (D > InOutCpu.m_Cycles && D < Deadline)
					{
						Deadline = D;
					}
				};
				Consider(InOutBudget); // A halted CPU must also stop at the requested run limit.
				// a recording/live sink must sample on time even when the next
				// interrupt is later. Otherwise HALT skips the capture boundary.
				if constexpr (!Pol::bPlain)
				{
					if (InOutIo.m_Video)
					{
						Consider(InOutIo.m_Video->m_NextEmitCycle);
					}
				}
				Consider(InOutCpu.m_NextFrameIntCycle);
				Consider(InOutCpu.m_NextPsgTickCycle);
				Consider(InOutCpu.m_NextStatsTickCycle);
				Consider(CtcFire);
				Consider(InOutCpu.m_NextLineIntCycle); // line int wakes a halted CPU too
				Consider(InOutCpu.m_NextUartPollCycle); // a UART Rx interrupt may wake it as well
				// The Copper runs in parallel to a halted CPU, but we do NOT stop
				// the fast-forward at each of its raster deadlines: StepCopper is
				// catch-up capable and reconstructs every intervening line in one
				// call (recording each MOVE at its own beam cycle for the
				// pixel-accurate timeline). So the HALT snaps straight to the next
				// real wake (the frame interrupt) and the copper is serviced once
				// per frame in the event gate, not ~192 times - the difference
				// between a demo emulating faster than real-time and slower. The
				// copper never actually WAKES the CPU, so it stays out of
				// bAnyWakeSource above.
				// Invariant: the filtered Consider() above only admits
				// deadlines > Cpu.Cycles, and bAnyWakeSource guarantees one
				// exists, so Deadline is a real future cycle. Guard anyway - a
				// fast-forward must never make zero/negative progress.
				if (Deadline == UINT64_MAX || Deadline <= InOutCpu.m_Cycles)
				{
					Result = RunResult::kRunResultHaltedInterruptsDisabled;
					goto done;
				}
				const uint64_t Skipped = Deadline - InOutCpu.m_Cycles;
				// R counts the halt's 4T NOPs from where it began, so a halt cut
				// into several skips (deadlines, a budget, a dump and resume)
				// advances it exactly as one skip would
				const uint64_t Start = InOutCpu.m_HaltStartCycle <= InOutCpu.m_Cycles ? InOutCpu.m_HaltStartCycle
																					   : InOutCpu.m_Cycles;
				const uint64_t Nops = (Deadline - Start) / 4 - (InOutCpu.m_Cycles - Start) / 4;
				InOutCpu.R = uint8_t((InOutCpu.R & 0x80) | (uint8_t((uint32_t(InOutCpu.R) + Nops) & 0x7F)));
				InOutCpu.m_HaltCycles += Skipped;
				InOutCpu.m_Cycles = Deadline;
			}
			else
			{
				// No interrupt source could ever wake us. IFF1=1 path: DI
				// cannot have been immediately prior (it sets IFF1=0), so
				// this is always the bug-shaped variant.
				Result = RunResult::kRunResultHaltedInterruptsDisabled;
				goto done;
			}
			continue;
		}

		// RestartTrap: PC at $0000 with ROM mapped at slot 0 is overwhelmingly
		// a crashed program (wild JP, RET to corrupt stack, RST 0). Dynamic
		// check - Next programs that put RAM at slot 0 via NextReg MMU
		// overlay won't trip. Suppressed on iteration 0 so SNAs that happen
		// to load with PC==0 can still execute.
		if (UNLIKELY(bResetTrapEnabled && InOutCpu.PC == 0 && InOutCpu.m_Cycles > 0 && Mem.Page8KAt(0).m_bIsRom))
		{
#if INSPECCY_TRACE_SUPPORT
			if constexpr (bTrace)
			{
				TraceFrame* TrapFrame = Trace->BeginFrame();
				TrapFrame->m_Flags = TraceFrame::kFlagResetTrap;
				TrapFrame->m_StartCycles = InOutCpu.m_Cycles;
				TrapFrame->m_Frame =
					(InOutCpu.m_FrameIntPeriod > 0) ? (InOutCpu.m_Cycles / InOutCpu.m_FrameIntPeriod) : 0;
				TrapFrame->m_PrePC = 0;
				const MemoryBus::Page8KInfo P = Mem.Page8KAt(0);
				TrapFrame->m_PrePageTag = BinaryTrace::MakePageTag(P.m_bIsRom, P.m_Index);
				Trace->CommitFrame();
			}
#endif
			Result = RunResult::kRunResultRestartTrap;
			goto done;
		}

		// Trap RST $08 at $0008. Pop the API-byte address, advance it by one,
		// and dispatch through NextZxosVfs. Require the preceding byte to be $CF
		// so a stray jump to $0008 does not become an arbitrary file operation.
		if (UNLIKELY(InOutIo.m_DotCmd && InOutCpu.PC == 0x0008))
		{
			const uint16_t ApiAddr =
				uint16_t(Mem.Rd<Pol::Mem>(InOutCpu.SP) | (Mem.Rd<Pol::Mem>(uint16_t(InOutCpu.SP + 1)) << 8));
			if (Mem.Rd<Pol::Mem>(uint16_t(ApiAddr - 1)) == 0xCF)
			{
				InOutCpu.SP = uint16_t(InOutCpu.SP + 2);
				const uint8_t Api = Mem.Rd<Pol::Mem>(ApiAddr);
				InOutCpu.PC = uint16_t(ApiAddr + 1);
				InOutIo.m_DotCmd->HandleRst08(Api, InOutCpu, Mem);
				continue;
			}
			// Fall through to normal execution: PC=$0008 will fetch+execute
			// whatever is in ROM/RAM at that address (typically NOPs in the
			// bundled 128K ROM area).
		}

		// Dot-command RST $20 trap. NextZXOS's "close DivMMC paging and jump
		// to HL" entry point - kickstarts of compressed dot commands use it
		// so that subsequent MMU0/MMU1 writes (which DivMMC normally
		// sandboxes away) actually take effect. We don't model DivMMC, so
		// the close-paging side is a no-op; we honour the jump-to-HL
		// contract by discarding the editor's RST-pushed return address
		// and setting PC = HL.
		if (UNLIKELY(InOutIo.m_DotCmd && InOutCpu.PC == 0x0020))
		{
			InOutCpu.SP = uint16_t(InOutCpu.SP + 2); // drop the rst-pushed return PC
			InOutCpu.PC = InOutCpu.HL;
			continue;
		}

		// .tap ROM-trap instant load. Fires at LD-BYTES ($0556); the current
		// tape block is copied to (IX) per A/DE and the routine's RET is
		// simulated with the carry flag reflecting success. Only active when a
		// tape is mounted and the ROM is actually mapped at $0556 (a program
		// that put RAM there is running its own loader, not the ROM one).
		// The ROM check is "the HOME ROM is mapped here", not merely "some ROM
		// is". On a 2068 the SCLD can page the EXROM over chunk 0, and EXROM
		// $0556 is ordinary code with nothing to do with LD-BYTES - trapping it
		// pops whatever happens to be on the stack into PC and resets the
		// machine. That only became reachable once $F4 banking existed.
		if (UNLIKELY(InOutIo.m_Tape && InOutCpu.PC == 0x0556))
		{
			const MemoryBus::Page8KInfo Chunk0 = Mem.Page8KAt(0);
			if (Chunk0.m_bIsRom && Chunk0.m_Index != kRomPageTimexEx)
			{
				InOutIo.m_Tape->HandleLdBytes(InOutCpu, Mem);
				continue;
			}
		}

		// Same trap for the TS/TC2068, whose tape routines live in the EXROM
		// rather than the Home ROM. LD-BYTES sits at EXROM $00FC and is
		// byte-identical to the 48K $0556 routine apart from its own return
		// address ($00E5 vs $053F) - same entry contract (A = expected flag,
		// IX = destination, DE = length, carry = load/verify), so the existing
		// handler serves both.
		//
		// The guard is stricter than the 48K one: it is not enough for chunk 0
		// to hold *a* ROM, it must hold the EXROM specifically. With the Home
		// ROM mapped there $00FC is unrelated code, and the pager swaps the two
		// constantly.
		if (UNLIKELY(InOutIo.m_Tape && InOutCpu.PC == 0x00FC && Mem.IsTimex2068()))
		{
			const MemoryBus::Page8KInfo Chunk0 = Mem.Page8KAt(0);
			if (Chunk0.m_bIsRom && Chunk0.m_Index == kRomPageTimexEx)
			{
				InOutIo.m_Tape->HandleLdBytes(InOutCpu, Mem);
				continue;
			}
		}

		// TR-DOS auto page-in / page-out. Single inline check (no function call)
		// so the trace pre-snapshot below sees the post-swap page.
		if (UNLIKELY(bTrdosSupportActive))
		{
			if (Mem.IsTrdosRomActive())
			{
				if (InOutCpu.PC >= 0x4000)
				{
					Mem.SetTrdosActive(false);
				}
			}
			else
			{
				if ((InOutCpu.PC & 0xFF00) == 0x3D00 && Mem.IsCurrentRomLowerHalf48K())
				{
					Mem.SetTrdosActive(true);
				}
			}
		}

		// Input event application. Events scheduled for the current (or earlier)
		// frame apply BEFORE the first instruction of that frame runs - so the
		// first instruction sees the new keyboard / Kempston state.
		if (UNLIKELY(bInputActive))
		{
			const uint64_t CurFrame =
				(InOutCpu.m_FrameIntPeriod > 0) ? (InOutCpu.m_Cycles / InOutCpu.m_FrameIntPeriod) : 0;
			while (const InputEvent* Ev = Input->PeekPending(CurFrame))
			{
#if INSPECCY_TRACE_SUPPORT
				if constexpr (bTrace)
				{
					if (bInputLog)
					{
						TraceFrame* IF = Trace->BeginFrame();
						IF->m_Flags = TraceFrame::kFlagInput;
						IF->m_StartCycles = InOutCpu.m_Cycles;
						IF->m_Frame = CurFrame;
						IF->m_bInputPress = Ev->m_bPress;
						memset(IF->m_InputName, 0, sizeof(IF->m_InputName));
						const size_t N = (Ev->m_Name.size() < sizeof(IF->m_InputName) - 1)
											 ? Ev->m_Name.size()
											 : sizeof(IF->m_InputName) - 1;
						memcpy(IF->m_InputName, Ev->m_Name.data(), N);
						Trace->CommitFrame();
					}
				}
#else
				(void)bInputLog;
#endif
				Input->ApplyAndAdvance();
			}
		}

#if INSPECCY_TRACE_SUPPORT
		if constexpr (bTrace)
		{
			PendingFrame = Trace->BeginFrame();
			TraceSnapshotPre(InOutCpu, Mem, InOutIo, *PendingFrame);
		}
#endif

		LastPC = InOutCpu.PC;

		// --until-pc breakpoint: stop BEFORE executing the target, with state
		// (registers, cycles, paging) as of that instant. Compile-time gated on
		// the policy: an ordinary (non-breakpoint) run has NO check here at all -
		// the whole block folds away, so the common path pays nothing.
		if constexpr (Pol::bBreak)
		{
			if (InOutCpu.PC == UntilPcTarget && (UntilPcPage < 0 || Mem.Phys8KPageAt(InOutCpu.PC) == UntilPcPage))
			{
				Result = RunResult::kRunResultReachedPc;
				goto done;
			}
		}

		// Fetch opcode, handle DD/FD prefix chains. Each prefix byte costs its
		// own 4T M1 cycle HERE, so the handlers below charge the UNPREFIXED
		// figure for the register forms (ld ix,nn = 4 + 10, add ix,bc = 4 + 11)
		// and only the genuinely longer (ix+d) forms differ from their (hl)
		// twins (ld r,(ix+d) = 4 + 15 = 19). Charging the documented prefixed
		// total in the handler as well double-counts the prefix.
		uint8_t Op = Fetch8<Pol>(InOutCpu, Mem);
		IncR(InOutCpu);
		int Idx = 0;
		while (UNLIKELY(Op == 0xDD || Op == 0xFD))
		{
			Idx = (Op == 0xDD) ? 1 : 2;
			InOutCpu.m_Cycles += 4;
			Op = Fetch8<Pol>(InOutCpu, Mem);
			IncR(InOutCpu);
		}
		LastOp = Op;

		// ------------------------------------------------------------------
		// CB prefix
		// ------------------------------------------------------------------
		if (UNLIKELY(Op == 0xCB))
		{
			int8_t Disp = 0;
			bool bIndexed = (Idx != 0);
			if (bIndexed)
			{
				Disp = int8_t(Fetch8<Pol>(InOutCpu, Mem));
			}
			uint8_t Op2 = Fetch8<Pol>(InOutCpu, Mem);
			if (!bIndexed)
			{
				IncR(InOutCpu);
			}

			const int CbOp = (Op2 >> 6) & 0x03; // 00=rot/shift, 01=BIT, 10=RES, 11=SET
			const int CbSub = (Op2 >> 3) & 0x07;
			const int CbReg = Op2 & 0x07;

			// Read source value
			uint8_t V;
			uint16_t IndexedAddr = 0;
			if (bIndexed)
			{
				const uint16_t Base = (Idx == 1) ? InOutCpu.IX : InOutCpu.IY;
				IndexedAddr = uint16_t(Base + Disp);
				V = Mem.Rd<Pol::Mem>(IndexedAddr);
				InOutCpu.m_Cycles += (CbOp == 1) ? 16 : 19; // BIT 20T, SET/RES/rot 23T, less the prefix's 4T
			}
			else
			{
				if (CbReg == 6)
				{
					V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
					InOutCpu.m_Cycles += (CbOp == 1) ? 12 : 15;
				}
				else
				{
					switch (CbReg)
					{
					case 0:
						V = uint8_t(InOutCpu.BC >> 8);
						break;
					case 1:
						V = uint8_t(InOutCpu.BC & 0xFF);
						break;
					case 2:
						V = uint8_t(InOutCpu.DE >> 8);
						break;
					case 3:
						V = uint8_t(InOutCpu.DE & 0xFF);
						break;
					case 4:
						V = uint8_t(InOutCpu.HL >> 8);
						break;
					case 5:
						V = uint8_t(InOutCpu.HL & 0xFF);
						break;
					case 7:
						V = InOutCpu.A();
						break;
					default:
						V = 0;
						break;
					}
					InOutCpu.m_Cycles += 8;
				}
			}

			uint8_t Rv = V;
			switch (CbOp)
			{
			case 0:
				switch (CbSub)
				{
				case 0:
					Rv = Rlc8(InOutCpu, V);
					break;
				case 1:
					Rv = Rrc8(InOutCpu, V);
					break;
				case 2:
					Rv = Rl8(InOutCpu, V);
					break;
				case 3:
					Rv = Rr8(InOutCpu, V);
					break;
				case 4:
					Rv = Sla8(InOutCpu, V);
					break;
				case 5:
					Rv = Sra8(InOutCpu, V);
					break;
				case 6:
					Rv = Sll8(InOutCpu, V);
					break;
				case 7:
					Rv = Srl8(InOutCpu, V);
					break;
				}
				break;
			case 1: // BIT n
				BitN(InOutCpu, CbSub, V);
				// For (HL)/indexed addressing, X/Y come from MEMPTR per real hardware;
				// we leave them sourced from V - acceptable for budget tracking.
				break;
			case 2: // RES n
				Rv = uint8_t(V & ~(1u << CbSub));
				break;
			case 3: // SET n
				Rv = uint8_t(V | (1u << CbSub));
				break;
			}

			if (CbOp != 1) // store result (and to register too, for indexed undocumented behavior)
			{
				if (bIndexed)
				{
					Mem.Wr<Pol::Mem>(IndexedAddr, Rv);
					// Undocumented: also write Rv into the register selected by CbReg if not 6.
					if (CbReg != 6)
					{
						switch (CbReg)
						{
						case 0:
							InOutCpu.BC = uint16_t((InOutCpu.BC & 0x00FF) | (uint16_t(Rv) << 8));
							break;
						case 1:
							InOutCpu.BC = uint16_t((InOutCpu.BC & 0xFF00) | Rv);
							break;
						case 2:
							InOutCpu.DE = uint16_t((InOutCpu.DE & 0x00FF) | (uint16_t(Rv) << 8));
							break;
						case 3:
							InOutCpu.DE = uint16_t((InOutCpu.DE & 0xFF00) | Rv);
							break;
						case 4:
							InOutCpu.HL = uint16_t((InOutCpu.HL & 0x00FF) | (uint16_t(Rv) << 8));
							break;
						case 5:
							InOutCpu.HL = uint16_t((InOutCpu.HL & 0xFF00) | Rv);
							break;
						case 7:
							InOutCpu.SetA(Rv);
							break;
						default:
							break;
						}
					}
				}
				else if (CbReg == 6)
				{
					Mem.Wr<Pol::Mem>(InOutCpu.HL, Rv);
				}
				else
				{
					switch (CbReg)
					{
					case 0:
						InOutCpu.BC = uint16_t((InOutCpu.BC & 0x00FF) | (uint16_t(Rv) << 8));
						break;
					case 1:
						InOutCpu.BC = uint16_t((InOutCpu.BC & 0xFF00) | Rv);
						break;
					case 2:
						InOutCpu.DE = uint16_t((InOutCpu.DE & 0x00FF) | (uint16_t(Rv) << 8));
						break;
					case 3:
						InOutCpu.DE = uint16_t((InOutCpu.DE & 0xFF00) | Rv);
						break;
					case 4:
						InOutCpu.HL = uint16_t((InOutCpu.HL & 0x00FF) | (uint16_t(Rv) << 8));
						break;
					case 5:
						InOutCpu.HL = uint16_t((InOutCpu.HL & 0xFF00) | Rv);
						break;
					case 7:
						InOutCpu.SetA(Rv);
						break;
					default:
						break;
					}
				}
			}
			continue;
		}

		// ------------------------------------------------------------------
		// ED prefix
		// ------------------------------------------------------------------
		if (UNLIKELY(Op == 0xED))
		{
			const uint8_t Op2 = Fetch8<Pol>(InOutCpu, Mem);
			IncR(InOutCpu);

			// Z80N exists only in the Next's CPU core. Every other model here
			// has a real (or faithfully cloned) Z80, where these ED slots are
			// undocumented no-ops - so a 48K/Timex/Pentagon/Evo/Uno program
			// that stumbles into ED 91 must not remap MMU slots or switch
			// the clock. Route them to the same undefined-ED handling as any
			// other unused slot (InvalidOpcode, or 8T under --lenient-undoc-ed).
			// Compile-time on the policy: a Next instantiation has no test here.
			if constexpr (!Pol::bNext)
			{
				if (IsZ80NOpcode(Op2))
				{
					if (InOutIo.m_bLenientUndocEd)
					{
						InOutCpu.m_Cycles += 8;
						continue;
					}
					Result = RunResult::kRunResultInvalidOpcode;
					goto done;
				}
			}

			// Z80N + standard ED.
			switch (Op2)
			{
			// ---- Z80N ----
			case 0x23: // SWAPNIB
			{
				uint8_t A = InOutCpu.A();
				InOutCpu.SetA(uint8_t((A << 4) | (A >> 4)));
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x24: // MIRROR A
			{
				uint8_t A = InOutCpu.A();
				A = uint8_t(((A & 0x01) << 7) | ((A & 0x02) << 5) | ((A & 0x04) << 3) | ((A & 0x08) << 1) |
							((A & 0x10) >> 1) | ((A & 0x20) >> 3) | ((A & 0x40) >> 5) | ((A & 0x80) >> 7));
				InOutCpu.SetA(A);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x27: // TEST n  (AND n but result not stored)
			{
				const uint8_t N = Fetch8<Pol>(InOutCpu, Mem);
				const uint8_t R = uint8_t(InOutCpu.A() & N);
				InOutCpu.SetF(uint8_t(SZ53P(R) | kFlagH));
				InOutCpu.m_Cycles += 11;
				break;
			}
			case 0x28: // BSLA DE,B
			{
				const uint8_t N = uint8_t(InOutCpu.BC >> 8) & 0x1F;
				InOutCpu.DE = uint16_t(InOutCpu.DE << N);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x29: // BSRA DE,B (arithmetic right)
			{
				const uint8_t N = uint8_t(InOutCpu.BC >> 8) & 0x1F;
				int16_t S = int16_t(InOutCpu.DE);
				InOutCpu.DE = uint16_t(S >> N);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x2A: // BSRL DE,B (logical right)
			{
				const uint8_t N = uint8_t(InOutCpu.BC >> 8) & 0x1F;
				InOutCpu.DE = uint16_t(InOutCpu.DE >> N);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x2B: // BSRF DE,B (right shift, fill with 1s)
			{
				const uint8_t N = uint8_t(InOutCpu.BC >> 8) & 0x1F;
				if (N == 0)
				{
					// No shift, no fill bits.
				}
				else if (N >= 16)
				{
					InOutCpu.DE = 0xFFFFu; // all-ones fill, value shifted out
				}
				else
				{
					const uint16_t Mask = uint16_t(0xFFFFu << (16 - N));
					InOutCpu.DE = uint16_t((InOutCpu.DE >> N) | Mask);
				}
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x2C: // BRLC DE,B (rotate left by B)
			{
				const uint8_t N = uint8_t(InOutCpu.BC >> 8) & 0x0F;
				if (N != 0)
				{
					InOutCpu.DE = uint16_t((InOutCpu.DE << N) | (InOutCpu.DE >> (16 - N)));
				}
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x30: // MUL D,E  ->  DE = D * E (unsigned)
			{
				const uint8_t D = uint8_t(InOutCpu.DE >> 8);
				const uint8_t E = uint8_t(InOutCpu.DE & 0xFF);
				InOutCpu.DE = uint16_t(uint16_t(D) * uint16_t(E));
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x31: // ADD HL,A
				InOutCpu.HL = uint16_t(InOutCpu.HL + InOutCpu.A());
				InOutCpu.m_Cycles += 8;
				break;
			case 0x32: // ADD DE,A
				InOutCpu.DE = uint16_t(InOutCpu.DE + InOutCpu.A());
				InOutCpu.m_Cycles += 8;
				break;
			case 0x33: // ADD BC,A
				InOutCpu.BC = uint16_t(InOutCpu.BC + InOutCpu.A());
				InOutCpu.m_Cycles += 8;
				break;
			case 0x34: // ADD HL,nn
			{
				const uint16_t N = Fetch16<Pol>(InOutCpu, Mem);
				InOutCpu.HL = uint16_t(InOutCpu.HL + N);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0x35: // ADD DE,nn
			{
				const uint16_t N = Fetch16<Pol>(InOutCpu, Mem);
				InOutCpu.DE = uint16_t(InOutCpu.DE + N);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0x36: // ADD BC,nn
			{
				const uint16_t N = Fetch16<Pol>(InOutCpu, Mem);
				InOutCpu.BC = uint16_t(InOutCpu.BC + N);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0x8A: // PUSH nn (big-endian)
			{
				const uint8_t Hi = Fetch8<Pol>(InOutCpu, Mem);
				const uint8_t Lo = Fetch8<Pol>(InOutCpu, Mem);
				Push16<Pol>(InOutCpu, Mem, uint16_t(uint16_t(Hi) << 8 | Lo));
				InOutCpu.m_Cycles += 23;
				break;
			}
			case 0x90: // OUTINB - OUT (C),(HL); HL++
			{
				WriteIoPort(InOutIo, InOutCpu.BC, Mem.Rd<Pol::Mem>(InOutCpu.HL));
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0x91: // NEXTREG reg,val
			{
				const uint8_t Reg = Mem.Rd<Pol::Mem>(InOutCpu.PC);
				const uint8_t Val = Mem.Rd<Pol::Mem>(uint16_t(InOutCpu.PC + 1));
				InOutIo.m_Mem.WriteNextReg(Reg, Val, InOutCpu.PC, InOutCpu.m_Cycles);
				InOutCpu.PC = uint16_t(InOutCpu.PC + 2);
				InOutCpu.m_Cycles += 20;
				break;
			}
			case 0x92: // NEXTREG reg,A
			{
				const uint8_t Reg = Mem.Rd<Pol::Mem>(InOutCpu.PC);
				InOutIo.m_Mem.WriteNextReg(Reg, InOutCpu.A(), InOutCpu.PC, InOutCpu.m_Cycles);
				InOutCpu.PC = uint16_t(InOutCpu.PC + 1);
				InOutCpu.m_Cycles += 17;
				break;
			}
			case 0x93: // PIXELDN - HL = HL + 1 line on 256x192 ULA screen
			{
				// ULA address encoding:
				//   H = 0 1 0 Y7 Y6 Y2 Y1 Y0
				//   L = Y5 Y4 Y3 X4 X3 X2 X1 X0
				// PIXELDN advances Y by 1, propagating carry through
				// the three Y sub-fields. See z80n_test.cpp PIXELDN
				// block for the human-readable spec.
				uint8_t H = uint8_t(InOutCpu.HL >> 8);
				uint8_t L = uint8_t(InOutCpu.HL & 0xFF);
				if ((H & 0x07) != 0x07)
				{
					H = uint8_t(H + 1); // bump Y[2..0]
				}
				else
				{
					H = uint8_t(H & 0xF8); // Y[2..0] := 0
					if ((L & 0xE0) != 0xE0)
					{
						L = uint8_t(L + 0x20); // bump Y[5..3]
					}
					else
					{
						L = uint8_t(L & 0x1F); // Y[5..3] := 0
						H = uint8_t(H + 0x08); // bump Y[7..6]
					}
				}
				InOutCpu.HL = uint16_t((uint16_t(H) << 8) | L);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x94: // PIXELAD - HL = ULA pixel address for D=y, E=x.
			{
				// Z80N PIXELAD: D=row, E=column; return the corresponding $4000-based
				// Spectrum screen address in HL, including scanline interleaving.
				const uint8_t Y = uint8_t(InOutCpu.DE >> 8);
				const uint8_t X = uint8_t(InOutCpu.DE & 0xFF);
				InOutCpu.HL = uint16_t(0x4000 | ((Y & 0xC0) << 5) | ((Y & 0x07) << 8) | ((Y & 0x38) << 2) | (X >> 3));
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x95: // SETAE - A = 1 << (7 - (E & 7))
			{
				InOutCpu.SetA(uint8_t(0x80u >> (InOutCpu.DE & 0x07)));
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x98: // JP (C)  - jump to (PC & 0xC000) | (C << 6)
			{
				InOutCpu.PC = uint16_t((InOutCpu.PC & 0xC000) | (uint16_t(InOutCpu.BC & 0xFF) << 6));
				InOutCpu.m_Cycles += 13;
				break;
			}
			case 0xA4: // LDIX - LDI but skip bytes equal to A
			{
				uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				if (V != InOutCpu.A())
				{
					Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				}
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xA5: // LDWS - *DE = *HL; INC L; INC D (both 8-bit, no carry)
			{
				uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				InOutCpu.HL = uint16_t((InOutCpu.HL & 0xFF00) | ((InOutCpu.HL + 1) & 0x00FF));
				InOutCpu.DE = uint16_t((InOutCpu.DE & 0x00FF) | ((InOutCpu.DE + 0x0100) & 0xFF00));
				InOutCpu.m_Cycles += 14;
				break;
			}
			case 0xAC: // LDDX - LDD with A-skip
			{
				uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				if (V != InOutCpu.A())
				{
					Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				}
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xB4: // LDIRX
			{
				uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				if (V != InOutCpu.A())
				{
					Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				}
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				if (InOutCpu.BC != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				break;
			}
			case 0xB7: // LDPIRX - pattern-fill variant
			{
				uint16_t Src = uint16_t((InOutCpu.HL & 0xFFF8) | (InOutCpu.DE & 0x07));
				uint8_t V = Mem.Rd<Pol::Mem>(Src);
				if (V != InOutCpu.A())
				{
					Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				}
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				if (InOutCpu.BC != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				break;
			}
			case 0xBC: // LDDRX
			{
				uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				if (V != InOutCpu.A())
				{
					Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				}
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				if (InOutCpu.BC != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				break;
			}

			// ---- Standard ED ----
			case 0x40:
			case 0x48:
			case 0x50:
			case 0x58:
			case 0x60:
			case 0x68:
			case 0x78: // IN r,(C)
			{
				const int Reg = (Op2 >> 3) & 0x07;
				const uint8_t V = ReadIoPort(InOutIo, InOutCpu.BC);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | SZ53P(V));
				InOutCpu.SetF(F);
				if (Reg != 6)
				{
					switch (Reg)
					{
					case 0:
						InOutCpu.BC = uint16_t((InOutCpu.BC & 0x00FF) | (uint16_t(V) << 8));
						break;
					case 1:
						InOutCpu.BC = uint16_t((InOutCpu.BC & 0xFF00) | V);
						break;
					case 2:
						InOutCpu.DE = uint16_t((InOutCpu.DE & 0x00FF) | (uint16_t(V) << 8));
						break;
					case 3:
						InOutCpu.DE = uint16_t((InOutCpu.DE & 0xFF00) | V);
						break;
					case 4:
						InOutCpu.HL = uint16_t((InOutCpu.HL & 0x00FF) | (uint16_t(V) << 8));
						break;
					case 5:
						InOutCpu.HL = uint16_t((InOutCpu.HL & 0xFF00) | V);
						break;
					case 7:
						InOutCpu.SetA(V);
						break;
					}
				}
				InOutCpu.m_Cycles += 12;
				break;
			}
			case 0x70: // IN (C) (flag-only)
			{
				const uint8_t V = ReadIoPort(InOutIo, InOutCpu.BC);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & kFlagC) | SZ53P(V)));
				InOutCpu.m_Cycles += 12;
				break;
			}
			case 0x41:
			case 0x49:
			case 0x51:
			case 0x59:
			case 0x61:
			case 0x69:
			case 0x71:
			case 0x79: // OUT (C),r
			{
				uint8_t V = 0;
				switch ((Op2 >> 3) & 0x07)
				{
				case 0:
					V = uint8_t(InOutCpu.BC >> 8);
					break;
				case 1:
					V = uint8_t(InOutCpu.BC & 0xFF);
					break;
				case 2:
					V = uint8_t(InOutCpu.DE >> 8);
					break;
				case 3:
					V = uint8_t(InOutCpu.DE & 0xFF);
					break;
				case 4:
					V = uint8_t(InOutCpu.HL >> 8);
					break;
				case 5:
					V = uint8_t(InOutCpu.HL & 0xFF);
					break;
				case 6:
					V = 0;
					break;
				case 7:
					V = InOutCpu.A();
					break;
				}
				WriteIoPort(InOutIo, InOutCpu.BC, V);
				InOutCpu.m_Cycles += 12;
				break;
			}
			case 0x42:
			case 0x52:
			case 0x62:
			case 0x72: // SBC HL,rr
			{
				const int P = (Op2 >> 4) & 0x03;
				uint16_t Src = (P == 0) ? InOutCpu.BC : (P == 1) ? InOutCpu.DE : (P == 2) ? InOutCpu.HL : InOutCpu.SP;
				InOutCpu.HL = AluSbc16(InOutCpu, InOutCpu.HL, Src);
				InOutCpu.m_Cycles += 15;
				break;
			}
			case 0x4A:
			case 0x5A:
			case 0x6A:
			case 0x7A: // ADC HL,rr
			{
				const int P = (Op2 >> 4) & 0x03;
				uint16_t Src = (P == 0) ? InOutCpu.BC : (P == 1) ? InOutCpu.DE : (P == 2) ? InOutCpu.HL : InOutCpu.SP;
				InOutCpu.HL = AluAdc16(InOutCpu, InOutCpu.HL, Src);
				InOutCpu.m_Cycles += 15;
				break;
			}
			case 0x43:
			case 0x53:
			case 0x63:
			case 0x73: // LD (nn),rr
			{
				const int P = (Op2 >> 4) & 0x03;
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				uint16_t V = (P == 0) ? InOutCpu.BC : (P == 1) ? InOutCpu.DE : (P == 2) ? InOutCpu.HL : InOutCpu.SP;
				Mem.Wr16<Pol::Mem>(Addr, V);
				InOutCpu.m_Cycles += 20;
				break;
			}
			case 0x4B:
			case 0x5B:
			case 0x6B:
			case 0x7B: // LD rr,(nn)
			{
				const int P = (Op2 >> 4) & 0x03;
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				uint16_t V = Mem.Rd16<Pol::Mem>(Addr);
				if (P == 0)
				{
					InOutCpu.BC = V;
				}
				else if (P == 1)
				{
					InOutCpu.DE = V;
				}
				else if (P == 2)
				{
					InOutCpu.HL = V;
				}
				else
				{
					InOutCpu.SP = V;
				}
				InOutCpu.m_Cycles += 20;
				break;
			}
			case 0x44:
			case 0x4C:
			case 0x54:
			case 0x5C:
			case 0x64:
			case 0x6C:
			case 0x74:
			case 0x7C: // NEG
			{
				const uint8_t A = InOutCpu.A();
				const uint8_t R = uint8_t(0u - A);
				uint8_t F = uint8_t(kSZ53Table[R] | kFlagN);
				if (A != 0)
				{
					F |= kFlagC;
				}
				if (A == 0x80)
				{
					F |= kFlagPv;
				}
				if (A & 0x0F)
				{
					F |= kFlagH;
				}
				InOutCpu.SetA(R);
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 8;
				break;
			}
			case 0x45:
			case 0x55:
			case 0x65:
			case 0x75: // RETN
			case 0x4D:
			case 0x5D:
			case 0x6D:
			case 0x7D: // RETI
			{
				InOutCpu.PC = Pop16<Pol>(InOutCpu, Mem);
				InOutCpu.IFF1 = InOutCpu.IFF2;
				InOutCpu.m_Cycles += 14;
				NotifyRetMaybeIsrReturn(InOutCpu, InOutCpu.PC, InOutCpu.SP,
					InOutIo.m_Dma && InOutIo.m_Dma->m_BytesRemaining > 0, InOutIo.m_bTraceDrip, &Mem);
				break;
			}
			case 0x46:
			case 0x4E:
			case 0x66:
			case 0x6E: // IM 0 / IM 0/1
				InOutCpu.IM = 0;
				InOutCpu.m_Cycles += 8;
				break;
			case 0x56:
			case 0x76: // IM 1
				InOutCpu.IM = 1;
				InOutCpu.m_Cycles += 8;
				break;
			case 0x5E:
			case 0x7E: // IM 2
				InOutCpu.IM = 2;
				InOutCpu.m_Cycles += 8;
				break;
			case 0x47: // LD I,A
				InOutCpu.I = InOutCpu.A();
				InOutCpu.m_Cycles += 9;
				break;
			case 0x4F: // LD R,A
				InOutCpu.R = InOutCpu.A();
				InOutCpu.m_Cycles += 9;
				break;
			case 0x57: // LD A,I
			{
				InOutCpu.SetA(InOutCpu.I);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kSZ53Table[InOutCpu.I]);
				if (InOutCpu.IFF2)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 9;
				break;
			}
			case 0x5F: // LD A,R
			{
				InOutCpu.SetA(InOutCpu.R);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kSZ53Table[InOutCpu.R]);
				if (InOutCpu.IFF2)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 9;
				break;
			}
			case 0x67: // RRD
			{
				const uint8_t MemV = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t NewA = uint8_t((A & 0xF0) | (MemV & 0x0F));
				const uint8_t NewMem = uint8_t((A << 4) | (MemV >> 4));
				Mem.Wr<Pol::Mem>(InOutCpu.HL, NewMem);
				InOutCpu.SetA(NewA);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & kFlagC) | SZ53P(NewA)));
				InOutCpu.m_Cycles += 18;
				break;
			}
			case 0x6F: // RLD
			{
				const uint8_t MemV = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t NewA = uint8_t((A & 0xF0) | (MemV >> 4));
				const uint8_t NewMem = uint8_t((MemV << 4) | (A & 0x0F));
				Mem.Wr<Pol::Mem>(InOutCpu.HL, NewMem);
				InOutCpu.SetA(NewA);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & kFlagC) | SZ53P(NewA)));
				InOutCpu.m_Cycles += 18;
				break;
			}

			case 0xA0: // LDI
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t(InOutCpu.F() & (kFlagS | kFlagZ | kFlagC));
				const uint8_t N = uint8_t(V + InOutCpu.A());
				F |= uint8_t(N & kFlagX);
				F |= uint8_t((N & 0x02) ? kFlagY : 0);
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xA1: // CPI
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t Diff = uint8_t(A - V);
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kFlagN);
				F |= uint8_t(Diff & kFlagS);
				if (Diff == 0)
				{
					F |= kFlagZ;
				}
				if (((A & 0x0F) - (V & 0x0F)) & 0x10)
				{
					F |= kFlagH;
				}
				const uint8_t N = (F & kFlagH) ? uint8_t(Diff - 1) : Diff;
				F |= uint8_t(N & kFlagX);
				F |= uint8_t((N & 0x02) ? kFlagY : 0);
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xA2: // INI
			case 0xAA: // IND
			case 0xB2: // INIR
			case 0xBA: // INDR
			{
				const int Direction = (Op2 & 0x08) ? -1 : 1;
				const uint8_t V = ReadIoPort(InOutIo, InOutCpu.BC); // Input uses B before decrement.
				const unsigned Sum = unsigned(V) + uint8_t(InOutCpu.BC + Direction);
				Mem.Wr<Pol::Mem>(InOutCpu.HL, V);
				InOutCpu.HL = uint16_t(InOutCpu.HL + Direction);
				InOutCpu.m_MemPtr = uint16_t(InOutCpu.BC + Direction);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 0x0100);
				FinishBlockIo(InOutCpu, V, Sum, (Op2 & 0x10) != 0);
				break;
			}
			case 0xA3: // OUTI
			case 0xAB: // OUTD
			case 0xB3: // OTIR
			case 0xBB: // OTDR
			{
				const int Direction = (Op2 & 0x08) ? -1 : 1;
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				InOutCpu.HL = uint16_t(InOutCpu.HL + Direction);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 0x0100);
				WriteIoPort(InOutIo, InOutCpu.BC, V); // Output uses B after decrement.
				const unsigned Sum = unsigned(V) + uint8_t(InOutCpu.HL);
				InOutCpu.m_MemPtr = uint16_t(InOutCpu.BC + Direction);
				FinishBlockIo(InOutCpu, V, Sum, (Op2 & 0x10) != 0);
				break;
			}
			case 0xA8: // LDD
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE - 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t(InOutCpu.F() & (kFlagS | kFlagZ | kFlagC));
				const uint8_t N = uint8_t(V + InOutCpu.A());
				F |= uint8_t(N & kFlagX);
				F |= uint8_t((N & 0x02) ? kFlagY : 0);
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xA9: // CPD
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t Diff = uint8_t(A - V);
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kFlagN);
				F |= uint8_t(Diff & kFlagS);
				if (Diff == 0)
				{
					F |= kFlagZ;
				}
				if (((A & 0x0F) - (V & 0x0F)) & 0x10)
				{
					F |= kFlagH;
				}
				const uint8_t N = (F & kFlagH) ? uint8_t(Diff - 1) : Diff;
				F |= uint8_t(N & kFlagX);
				F |= uint8_t((N & 0x02) ? kFlagY : 0);
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 16;
				break;
			}
			case 0xB0: // LDIR
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				if (InOutCpu.BC != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				InOutCpu.SetF(uint8_t(InOutCpu.F() & (kFlagS | kFlagZ | kFlagC)));
				break;
			}
			case 0xB1: // CPIR
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t Diff = uint8_t(A - V);
				InOutCpu.HL = uint16_t(InOutCpu.HL + 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kFlagN);
				F |= uint8_t(Diff & kFlagS);
				if (Diff == 0)
				{
					F |= kFlagZ;
				}
				if (((A & 0x0F) - (V & 0x0F)) & 0x10)
				{
					F |= kFlagH;
				}
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				if (InOutCpu.BC != 0 && Diff != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				break;
			}
			case 0xB8: // LDDR
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				Mem.Wr<Pol::Mem>(InOutCpu.DE, V);
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.DE = uint16_t(InOutCpu.DE - 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				if (InOutCpu.BC != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				InOutCpu.SetF(uint8_t(InOutCpu.F() & (kFlagS | kFlagZ | kFlagC)));
				break;
			}
			case 0xB9: // CPDR
			{
				const uint8_t V = Mem.Rd<Pol::Mem>(InOutCpu.HL);
				const uint8_t A = InOutCpu.A();
				const uint8_t Diff = uint8_t(A - V);
				InOutCpu.HL = uint16_t(InOutCpu.HL - 1);
				InOutCpu.BC = uint16_t(InOutCpu.BC - 1);
				uint8_t F = uint8_t((InOutCpu.F() & kFlagC) | kFlagN);
				F |= uint8_t(Diff & kFlagS);
				if (Diff == 0)
				{
					F |= kFlagZ;
				}
				if (((A & 0x0F) - (V & 0x0F)) & 0x10)
				{
					F |= kFlagH;
				}
				if (InOutCpu.BC != 0)
				{
					F |= kFlagPv;
				}
				InOutCpu.SetF(F);
				if (InOutCpu.BC != 0 && Diff != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC - 2);
					InOutCpu.m_Cycles += 21;
				}
				else
				{
					InOutCpu.m_Cycles += 16;
				}
				break;
			}
			default:
				// Undocumented ED instructions act as two-byte NOPs on Z80 hardware.
				// The default diagnostic policy traps them as InvalidOpcode;
				// --lenient-undoc-ed enables their hardware NOP behaviour.
				if (InOutIo.m_bLenientUndocEd)
				{
					InOutCpu.m_Cycles += 8;
					break;
				}
				Result = RunResult::kRunResultInvalidOpcode;
				goto done;
			}
			continue;
		}

		// ------------------------------------------------------------------
		// Base / DD-prefixed / FD-prefixed dispatch
		// ------------------------------------------------------------------
		const int Q = (Op >> 6) & 0x03;
		const int Y = (Op >> 3) & 0x07;
		const int Z = Op & 0x07;

		if (Q == 1)
		{
			// 0x40..0x7F: LD r,r'  (with HALT at 0x76)
			if (UNLIKELY(Op == 0x76))
			{
				// HALT. With IFF1=0 we cannot wake and exit. With IFF1=1 we
				// enter the halted state and the top-of-loop check will
				// fast-forward us to the next interrupt deadline.
				// DI-adjacency check (BEFORE bumping cycles): if the previous
				// instruction was DI, Cpu.Cycles equals LastDiCycles + 4 plus
				// whatever contention DI's fetch cost (banked at this loop top:
				// the Next's 28 MHz read wait, or a contended page).
				const bool bDiAdjacent = (InOutCpu.m_LastDiCycles != UINT64_MAX &&
										  InOutCpu.m_Cycles == InOutCpu.m_LastDiCycles + 4 + Drained);
				InOutCpu.m_bHalted = true;
				InOutCpu.m_Cycles += 4;
				InOutCpu.m_HaltStartCycle = InOutCpu.m_Cycles;
				if (UNLIKELY(InOutCpu.IFF1 == 0))
				{
					// Terminal HALT-with-IFF1=0. Classify based on DI-adjacency:
					// bare DI;HALT is a deliberate stop; any other path is the
					// bug-shaped "interrupts disabled" lockup.
					Result = bDiAdjacent ? RunResult::kRunResultHaltedDeliberateStop
										 : RunResult::kRunResultHaltedInterruptsDisabled;
					goto done;
				}
				continue;
			}
			// LD r,r': when one side is (HL), the other side's H/L stays plain H/L.
			const bool bDstIsMem = (Y == 6);
			const bool bSrcIsMem = (Z == 6);
			const bool bMemSide = bDstIsMem || bSrcIsMem;

			bool bDisp;
			int8_t D;
			const uint8_t V = Read8Reg<Pol>(InOutCpu, Mem, Z, Idx, bMemSide && !bSrcIsMem, bDisp, D);
			Write8Reg<Pol>(InOutCpu, Mem, Y, Idx, bMemSide && !bDstIsMem, D, bDisp, V);

			if (bMemSide)
			{
				InOutCpu.m_Cycles += (Idx == 0) ? 7 : 15;
			}
			else
			{
				InOutCpu.m_Cycles += 4;
			}
			continue;
		}

		if (Q == 2)
		{
			// 0x80..0xBF: ALU A,r
			bool bDisp;
			int8_t D;
			const uint8_t V = Read8Reg<Pol>(InOutCpu, Mem, Z, Idx, false, bDisp, D);
			switch (Y)
			{
			case 0:
				AluAdd(InOutCpu, V);
				break;
			case 1:
				AluAdc(InOutCpu, V);
				break;
			case 2:
				AluSub(InOutCpu, V);
				break;
			case 3:
				AluSbc(InOutCpu, V);
				break;
			case 4:
				AluAnd(InOutCpu, V);
				break;
			case 5:
				AluXor(InOutCpu, V);
				break;
			case 6:
				AluOr(InOutCpu, V);
				break;
			case 7:
				AluCp(InOutCpu, V);
				break;
			}
			if (Z == 6)
			{
				InOutCpu.m_Cycles += (Idx == 0) ? 7 : 15;
			}
			else
			{
				InOutCpu.m_Cycles += 4;
			}
			continue;
		}

		if (Q == 0)
		{
			switch (Op)
			{
			case 0x00: // NOP
				InOutCpu.m_Cycles += 4;
				continue;
			case 0x08: // EX AF,AF'
			{
				const uint16_t T = InOutCpu.AF;
				InOutCpu.AF = InOutCpu.AFp;
				InOutCpu.AFp = T;
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x10: // DJNZ e
			{
				const int8_t E = int8_t(Fetch8<Pol>(InOutCpu, Mem));
				const uint8_t B = uint8_t(uint8_t(InOutCpu.BC >> 8) - 1);
				InOutCpu.BC = uint16_t((InOutCpu.BC & 0x00FF) | (uint16_t(B) << 8));
				if (B != 0)
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC + E);
					InOutCpu.m_Cycles += 13;
				}
				else
				{
					InOutCpu.m_Cycles += 8;
				}
				continue;
			}
			case 0x18: // JR e
			{
				const int8_t E = int8_t(Fetch8<Pol>(InOutCpu, Mem));
				InOutCpu.PC = uint16_t(InOutCpu.PC + E);
				InOutCpu.m_Cycles += 12;
				continue;
			}
			case 0x20:
			case 0x28:
			case 0x30:
			case 0x38: // JR cc,e
			{
				const int8_t E = int8_t(Fetch8<Pol>(InOutCpu, Mem));
				const int kCC = (Op >> 3) & 0x03;
				if (TestCond(InOutCpu, kCC))
				{
					InOutCpu.PC = uint16_t(InOutCpu.PC + E);
					InOutCpu.m_Cycles += 12;
				}
				else
				{
					InOutCpu.m_Cycles += 7;
				}
				continue;
			}
			}

			// 0x01/11/21/31 LD rr,nn ; 0x09/19/29/39 ADD HL,rr ; 0x02/0A/12/1A LD (BC/DE),A / LD A,(BC/DE)
			// 0x22 LD (nn),HL ; 0x2A LD HL,(nn) ; 0x32 LD (nn),A ; 0x3A LD A,(nn)
			// 0x03/13/23/33 INC rr ; 0x0B/1B/2B/3B DEC rr ; 0x04/0C... INC r ; 0x05/0D... DEC r ; 0x06... LD r,n
			// 0x07 RLCA ; 0x0F RRCA ; 0x17 RLA ; 0x1F RRA ; 0x27 DAA ; 0x2F CPL ; 0x37 SCF ; 0x3F CCF

			switch (Op)
			{
			case 0x01:
			case 0x11:
			case 0x21:
			case 0x31:
			{
				const int P = (Op >> 4) & 0x03;
				const uint16_t N = Fetch16<Pol>(InOutCpu, Mem);
				Write16RegPair(InOutCpu, P, Idx, N);
				InOutCpu.m_Cycles += 10;
				continue;
			}
			case 0x09:
			case 0x19:
			case 0x29:
			case 0x39:
			{
				const int P = (Op >> 4) & 0x03;
				uint16_t* Dst = (Idx == 0) ? &InOutCpu.HL : (Idx == 1 ? &InOutCpu.IX : &InOutCpu.IY);
				uint16_t Src;
				if (P == 2)
				{
					Src = *Dst; // ADD IX,IX uses IX twice
				}
				else
				{
					Src = Read16RegPair(InOutCpu, P, Idx);
				}
				*Dst = AluAdd16(InOutCpu, *Dst, Src);
				InOutCpu.m_Cycles += 11;
				continue;
			}
			case 0x02:
				Mem.Wr<Pol::Mem>(InOutCpu.BC, InOutCpu.A());
				InOutCpu.m_Cycles += 7;
				continue;
			case 0x12:
				Mem.Wr<Pol::Mem>(InOutCpu.DE, InOutCpu.A());
				InOutCpu.m_Cycles += 7;
				continue;
			case 0x0A:
				InOutCpu.SetA(Mem.Rd<Pol::Mem>(InOutCpu.BC));
				InOutCpu.m_Cycles += 7;
				continue;
			case 0x1A:
				InOutCpu.SetA(Mem.Rd<Pol::Mem>(InOutCpu.DE));
				InOutCpu.m_Cycles += 7;
				continue;
			case 0x22: // LD (nn),HL/IX/IY
			{
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				Mem.Wr16<Pol::Mem>(Addr, (Idx == 0) ? InOutCpu.HL : (Idx == 1 ? InOutCpu.IX : InOutCpu.IY));
				InOutCpu.m_Cycles += 16;
				continue;
			}
			case 0x2A: // LD HL/IX/IY,(nn)
			{
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				const uint16_t V = Mem.Rd16<Pol::Mem>(Addr);
				if (Idx == 0)
				{
					InOutCpu.HL = V;
				}
				else if (Idx == 1)
				{
					InOutCpu.IX = V;
				}
				else
				{
					InOutCpu.IY = V;
				}
				InOutCpu.m_Cycles += 16;
				continue;
			}
			case 0x32: // LD (nn),A
			{
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				Mem.Wr<Pol::Mem>(Addr, InOutCpu.A());
				InOutCpu.m_Cycles += 13;
				continue;
			}
			case 0x3A: // LD A,(nn)
			{
				const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
				InOutCpu.SetA(Mem.Rd<Pol::Mem>(Addr));
				InOutCpu.m_Cycles += 13;
				continue;
			}
			case 0x03:
			case 0x13:
			case 0x23:
			case 0x33: // INC rr
			{
				const int P = (Op >> 4) & 0x03;
				Write16RegPair(InOutCpu, P, Idx, uint16_t(Read16RegPair(InOutCpu, P, Idx) + 1));
				InOutCpu.m_Cycles += 6;
				continue;
			}
			case 0x0B:
			case 0x1B:
			case 0x2B:
			case 0x3B: // DEC rr
			{
				const int P = (Op >> 4) & 0x03;
				Write16RegPair(InOutCpu, P, Idx, uint16_t(Read16RegPair(InOutCpu, P, Idx) - 1));
				InOutCpu.m_Cycles += 6;
				continue;
			}
			case 0x04:
			case 0x0C:
			case 0x14:
			case 0x1C:
			case 0x24:
			case 0x2C:
			case 0x34:
			case 0x3C: // INC r
			{
				bool bDisp;
				int8_t D;
				const uint8_t V = Read8Reg<Pol>(InOutCpu, Mem, Y, Idx, false, bDisp, D);
				const uint8_t R = AluInc(InOutCpu, V);
				Write8Reg<Pol>(InOutCpu, Mem, Y, Idx, false, D, bDisp, R);
				if (Y == 6)
				{
					InOutCpu.m_Cycles += (Idx == 0) ? 11 : 19;
				}
				else
				{
					InOutCpu.m_Cycles += 4;
				}
				continue;
			}
			case 0x05:
			case 0x0D:
			case 0x15:
			case 0x1D:
			case 0x25:
			case 0x2D:
			case 0x35:
			case 0x3D: // DEC r
			{
				bool bDisp;
				int8_t D;
				const uint8_t V = Read8Reg<Pol>(InOutCpu, Mem, Y, Idx, false, bDisp, D);
				const uint8_t R = AluDec(InOutCpu, V);
				Write8Reg<Pol>(InOutCpu, Mem, Y, Idx, false, D, bDisp, R);
				if (Y == 6)
				{
					InOutCpu.m_Cycles += (Idx == 0) ? 11 : 19;
				}
				else
				{
					InOutCpu.m_Cycles += 4;
				}
				continue;
			}
			case 0x06:
			case 0x0E:
			case 0x16:
			case 0x1E:
			case 0x26:
			case 0x2E:
			case 0x36:
			case 0x3E: // LD r,n
			{
				if (Y == 6 && Idx != 0)
				{
					const int8_t D = int8_t(Fetch8<Pol>(InOutCpu, Mem));
					const uint8_t N = Fetch8<Pol>(InOutCpu, Mem);
					const uint16_t Base = (Idx == 1) ? InOutCpu.IX : InOutCpu.IY;
					Mem.Wr<Pol::Mem>(uint16_t(Base + D), N);
					InOutCpu.m_Cycles += 15; // 19T less the prefix's 4
				}
				else
				{
					const uint8_t N = Fetch8<Pol>(InOutCpu, Mem);
					bool bDisp = false;
					int8_t D = 0;
					Write8Reg<Pol>(InOutCpu, Mem, Y, Idx, false, D, bDisp, N);
					if (Y == 6)
					{
						InOutCpu.m_Cycles += 10;
					}
					else
					{
						InOutCpu.m_Cycles += 7;
					}
				}
				continue;
			}
			case 0x07: // RLCA
			{
				const uint8_t A = InOutCpu.A();
				const uint8_t Cy = uint8_t(A >> 7);
				const uint8_t R = uint8_t((A << 1) | Cy);
				InOutCpu.SetA(R);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | (R & (kFlagY | kFlagX)) | Cy));
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x0F: // RRCA
			{
				const uint8_t A = InOutCpu.A();
				const uint8_t Cy = uint8_t(A & 1);
				const uint8_t R = uint8_t((A >> 1) | (Cy << 7));
				InOutCpu.SetA(R);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | (R & (kFlagY | kFlagX)) | Cy));
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x17: // RLA
			{
				const uint8_t A = InOutCpu.A();
				const uint8_t InC = (InOutCpu.F() & kFlagC) ? 1 : 0;
				const uint8_t Cy = uint8_t(A >> 7);
				const uint8_t R = uint8_t((A << 1) | InC);
				InOutCpu.SetA(R);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | (R & (kFlagY | kFlagX)) | Cy));
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x1F: // RRA
			{
				const uint8_t A = InOutCpu.A();
				const uint8_t InC = (InOutCpu.F() & kFlagC) ? 1 : 0;
				const uint8_t Cy = uint8_t(A & 1);
				const uint8_t R = uint8_t((A >> 1) | (InC << 7));
				InOutCpu.SetA(R);
				InOutCpu.SetF(uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | (R & (kFlagY | kFlagX)) | Cy));
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x27:
				Daa(InOutCpu);
				InOutCpu.m_Cycles += 4;
				continue;
			case 0x2F: // CPL
			{
				const uint8_t A = uint8_t(~InOutCpu.A());
				InOutCpu.SetA(A);
				InOutCpu.SetF(uint8_t(
					(InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv | kFlagC)) | kFlagH | kFlagN | (A & (kFlagY | kFlagX))));
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x37: // SCF
			{
				uint8_t F =
					uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | kFlagC | (InOutCpu.A() & (kFlagY | kFlagX)));
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 4;
				continue;
			}
			case 0x3F: // CCF
			{
				const uint8_t Cy = uint8_t(InOutCpu.F() & kFlagC);
				uint8_t F = uint8_t((InOutCpu.F() & (kFlagS | kFlagZ | kFlagPv)) | (Cy ? kFlagH : 0) |
									(Cy ? 0 : kFlagC) | (InOutCpu.A() & (kFlagY | kFlagX)));
				InOutCpu.SetF(F);
				InOutCpu.m_Cycles += 4;
				continue;
			}
			}
			// Fallthrough: shouldn't get here for Q=0.
			Result = RunResult::kRunResultInvalidOpcode;
			goto done;
		}

		// Q == 3: 0xC0..0xFF
		switch (Op)
		{
		case 0xC0:
		case 0xC8:
		case 0xD0:
		case 0xD8:
		case 0xE0:
		case 0xE8:
		case 0xF0:
		case 0xF8: // RET cc
		{
			const int kCC = (Op >> 3) & 0x07;
			if (TestCond(InOutCpu, kCC))
			{
				InOutCpu.PC = Pop16<Pol>(InOutCpu, Mem);
				InOutCpu.m_Cycles += 11;
				NotifyRetMaybeIsrReturn(InOutCpu, InOutCpu.PC, InOutCpu.SP,
					InOutIo.m_Dma && InOutIo.m_Dma->m_BytesRemaining > 0, InOutIo.m_bTraceDrip, &Mem);
			}
			else
			{
				InOutCpu.m_Cycles += 5;
			}
			continue;
		}
		case 0xC1:
		case 0xD1:
		case 0xE1:
		case 0xF1: // POP rr
		{
			const int P = (Op >> 4) & 0x03;
			const uint16_t V = Pop16<Pol>(InOutCpu, Mem);
			WritePushPopPair(InOutCpu, P, Idx, V);
			InOutCpu.m_Cycles += 10;
			continue;
		}
		case 0xC2:
		case 0xCA:
		case 0xD2:
		case 0xDA:
		case 0xE2:
		case 0xEA:
		case 0xF2:
		case 0xFA: // JP cc,nn
		{
			const int kCC = (Op >> 3) & 0x07;
			const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
			if (TestCond(InOutCpu, kCC))
			{
				InOutCpu.PC = Addr;
			}
			InOutCpu.m_Cycles += 10;
			continue;
		}
		case 0xC3: // JP nn
		{
			InOutCpu.PC = Fetch16<Pol>(InOutCpu, Mem);
			InOutCpu.m_Cycles += 10;
			continue;
		}
		case 0xC4:
		case 0xCC:
		case 0xD4:
		case 0xDC:
		case 0xE4:
		case 0xEC:
		case 0xF4:
		case 0xFC: // CALL cc,nn
		{
			const int kCC = (Op >> 3) & 0x07;
			const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
			if (TestCond(InOutCpu, kCC))
			{
				Push16<Pol>(InOutCpu, Mem, InOutCpu.PC);
				InOutCpu.PC = Addr;
				InOutCpu.m_Cycles += 17;
			}
			else
			{
				InOutCpu.m_Cycles += 10;
			}
			continue;
		}
		case 0xC5:
		case 0xD5:
		case 0xE5:
		case 0xF5: // PUSH rr
		{
			const int P = (Op >> 4) & 0x03;
			Push16<Pol>(InOutCpu, Mem, ReadPushPopPair(InOutCpu, P, Idx));
			InOutCpu.m_Cycles += 11;
			continue;
		}
		case 0xC6:
			AluAdd(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xCE:
			AluAdc(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xD6:
			AluSub(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xDE:
			AluSbc(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xE6:
			AluAnd(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xEE:
			AluXor(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xF6:
			AluOr(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xFE:
			AluCp(InOutCpu, Fetch8<Pol>(InOutCpu, Mem));
			InOutCpu.m_Cycles += 7;
			continue;
		case 0xC7:
		case 0xCF:
		case 0xD7:
		case 0xDF:
		case 0xE7:
		case 0xEF:
		case 0xF7:
		case 0xFF: // RST
		{
			Push16<Pol>(InOutCpu, Mem, InOutCpu.PC);
			InOutCpu.PC = uint16_t(Op & 0x38);
			InOutCpu.m_Cycles += 11;
			continue;
		}
		case 0xC9: // RET
		{
			InOutCpu.PC = Pop16<Pol>(InOutCpu, Mem);
			InOutCpu.m_Cycles += 10;
			NotifyRetMaybeIsrReturn(
				InOutCpu, InOutCpu.PC, InOutCpu.SP, InOutIo.m_Dma && InOutIo.m_Dma->m_BytesRemaining > 0);
			continue;
		}
		case 0xCD: // CALL nn
		{
			const uint16_t Addr = Fetch16<Pol>(InOutCpu, Mem);
			Push16<Pol>(InOutCpu, Mem, InOutCpu.PC);
			InOutCpu.PC = Addr;
			InOutCpu.m_Cycles += 17;
			continue;
		}
		case 0xD3: // OUT (n),A
		{
			const uint8_t N = Fetch8<Pol>(InOutCpu, Mem);
			const uint16_t Port = uint16_t((uint16_t(InOutCpu.A()) << 8) | N);
			WriteIoPort(InOutIo, Port, InOutCpu.A());
			InOutCpu.m_Cycles += 11;
			continue;
		}
		case 0xDB: // IN A,(n)
		{
			const uint8_t N = Fetch8<Pol>(InOutCpu, Mem);
			const uint16_t Port = uint16_t((uint16_t(InOutCpu.A()) << 8) | N);
			InOutCpu.SetA(ReadIoPort(InOutIo, Port));
			InOutCpu.m_Cycles += 11;
			continue;
		}
		case 0xE3: // EX (SP),HL/IX/IY
		{
			uint16_t Lo = Mem.Rd<Pol::Mem>(InOutCpu.SP);
			uint16_t Hi = Mem.Rd<Pol::Mem>(uint16_t(InOutCpu.SP + 1));
			uint16_t V = uint16_t(Lo | (Hi << 8));
			uint16_t* Tgt = (Idx == 0) ? &InOutCpu.HL : (Idx == 1 ? &InOutCpu.IX : &InOutCpu.IY);
			Mem.Wr<Pol::Mem>(InOutCpu.SP, uint8_t(*Tgt & 0xFF));
			Mem.Wr<Pol::Mem>(uint16_t(InOutCpu.SP + 1), uint8_t(*Tgt >> 8));
			*Tgt = V;
			InOutCpu.m_Cycles += 19;
			continue;
		}
		case 0xE9: // JP (HL/IX/IY)
		{
			InOutCpu.PC = (Idx == 0) ? InOutCpu.HL : (Idx == 1 ? InOutCpu.IX : InOutCpu.IY);
			InOutCpu.m_Cycles += 4;
			continue;
		}
		case 0xEB: // EX DE,HL  (NOT affected by DD/FD prefix)
		{
			const uint16_t T = InOutCpu.DE;
			InOutCpu.DE = InOutCpu.HL;
			InOutCpu.HL = T;
			InOutCpu.m_Cycles += 4;
			continue;
		}
		case 0xF3: // DI
			InOutCpu.m_LastDiCycles = InOutCpu.m_Cycles; // record BEFORE cycle bump for HALT-adjacency check
			InOutCpu.IFF1 = 0;
			InOutCpu.IFF2 = 0;
			InOutCpu.m_Cycles += 4;
			continue;
		case 0xFB: // EI
			InOutCpu.IFF1 = 1;
			InOutCpu.IFF2 = 1;
			InOutCpu.m_bEiPending = true; // defer interrupt accept by one instruction
			InOutCpu.m_Cycles += 4;
			continue;
		case 0xF9: // LD SP,HL/IX/IY
			InOutCpu.SP = (Idx == 0) ? InOutCpu.HL : (Idx == 1 ? InOutCpu.IX : InOutCpu.IY);
			InOutCpu.m_Cycles += 6;
			continue;
		case 0xD9: // EXX
		{
			uint16_t T;
			T = InOutCpu.BC;
			InOutCpu.BC = InOutCpu.BCp;
			InOutCpu.BCp = T;
			T = InOutCpu.DE;
			InOutCpu.DE = InOutCpu.DEp;
			InOutCpu.DEp = T;
			T = InOutCpu.HL;
			InOutCpu.HL = InOutCpu.HLp;
			InOutCpu.HLp = T;
			InOutCpu.m_Cycles += 4;
			continue;
		}
		default:
			Result = RunResult::kRunResultInvalidOpcode;
			goto done;
		}
	}

done:
#if INSPECCY_TRACE_SUPPORT
	if constexpr (bTrace)
	{
		if (PendingFrame != nullptr)
		{
			if (Result == RunResult::kRunResultInvalidOpcode)
			{
				PendingFrame->m_Flags |= TraceFrame::kFlagInvalid;
			}
			TraceSnapshotPost(InOutCpu, *PendingFrame);
			Trace->CommitFrame();
			PendingFrame = nullptr;
		}
		Mem.m_OnWrite = nullptr;
		Mem.m_OnWriteUser = nullptr;
	}
#endif

	RunSummary S;
	S.m_Result = Result;
	S.m_CyclesExecuted = InOutCpu.m_Cycles;
	S.m_Budget = InOutBudget;
	S.m_LastPC = LastPC;
	S.m_LastOpcode = LastOp;
	S.m_InterruptsAccepted = InOutCpu.m_InterruptsAccepted;
	S.m_MissedInterrupts = InOutCpu.m_MissedInterrupts;
	S.m_LineIntsAccepted = InOutCpu.m_LineIntsAccepted;
	S.m_LineIntsMissed = InOutCpu.m_LineIntsMissed;
	S.m_AyWritesObserved = InOutIo.m_Ay.m_WritesObserved;
	S.m_PsgFramesWritten = InOutIo.m_Capture ? InOutIo.m_Capture->FramesWritten() : 0;
	S.m_DiToHaltCycles =
		(InOutCpu.m_LastDiCycles != UINT64_MAX) ? (InOutCpu.m_Cycles - InOutCpu.m_LastDiCycles) : UINT64_MAX;
	S.m_HaltCycles = InOutCpu.m_HaltCycles;
	return S;
}

RunSummary RunUntil(Z80State& InOutCpu, IoContext& InOutIo, uint64_t& InOutBudget, TraceWriter* Trace)
{
	// The one place the per-run machine/option decisions are made. Each picks a
	// compile-time-specialised dispatcher, so the hot loop carries zero runtime
	// tests for any of them. Only the combinations that can occur are named, so
	// the binary does not grow with the unused corners of the policy space.
	const bool bNext = InOutIo.m_Mem.IsNext();
	const bool bBreak = InOutIo.m_bUntilPc; // --until-pc: the breakpoint check exists only in this arm
	// Plain run = the common case: nothing per-access is armed. Chosen ONCE here;
	// the plain dispatcher has no coverage/contention/watch/beam/video checks in
	// its hot path at all. --dispatch full forces the general one (equivalence
	// testing; must produce byte-identical results).
	using A = MemoryBus::Access;
	const bool bPlain = (Trace == nullptr) && !bBreak && !InOutIo.m_bForceFullDispatch && InOutIo.m_Video == nullptr &&
						InOutIo.m_Metrics == nullptr && InOutIo.m_Mem.IsDiagnosticFree();
	if (bPlain)
	{
		if (InOutIo.m_Mem.ContentionOn())
		{
			return bNext ? RunUntilImpl<DispatchPolicy<true, false, false, A::kAccessFastContended>>(
							   InOutCpu, InOutIo, InOutBudget, nullptr)
						 : RunUntilImpl<DispatchPolicy<false, false, false, A::kAccessFastContended>>(
							   InOutCpu, InOutIo, InOutBudget, nullptr);
		}
		return bNext ? RunUntilImpl<DispatchPolicy<true, false, false, A::kAccessFast>>(
						   InOutCpu, InOutIo, InOutBudget, nullptr)
					 : RunUntilImpl<DispatchPolicy<false, false, false, A::kAccessFast>>(
						   InOutCpu, InOutIo, InOutBudget, nullptr);
	}
#if INSPECCY_TRACE_SUPPORT
	if (Trace != nullptr)
	{
		if (bBreak)
		{
			return bNext ? RunUntilImpl<DispatchPolicy<true, true, true, MemoryBus::Access::kAccessFull>>(
							   InOutCpu, InOutIo, InOutBudget, Trace)
						 : RunUntilImpl<DispatchPolicy<false, true, true, MemoryBus::Access::kAccessFull>>(
							   InOutCpu, InOutIo, InOutBudget, Trace);
		}
		return bNext ? RunUntilImpl<DispatchPolicy<true, true, false, MemoryBus::Access::kAccessFull>>(
						   InOutCpu, InOutIo, InOutBudget, Trace)
					 : RunUntilImpl<DispatchPolicy<false, true, false, MemoryBus::Access::kAccessFull>>(
						   InOutCpu, InOutIo, InOutBudget, Trace);
	}
#else
	(void)Trace;
#endif
	if (bBreak)
	{
		return bNext ? RunUntilImpl<DispatchPolicy<true, false, true, MemoryBus::Access::kAccessFull>>(
						   InOutCpu, InOutIo, InOutBudget, nullptr)
					 : RunUntilImpl<DispatchPolicy<false, false, true, MemoryBus::Access::kAccessFull>>(
						   InOutCpu, InOutIo, InOutBudget, nullptr);
	}
	return bNext ? RunUntilImpl<DispatchPolicy<true, false, false, MemoryBus::Access::kAccessFull>>(
					   InOutCpu, InOutIo, InOutBudget, nullptr)
				 : RunUntilImpl<DispatchPolicy<false, false, false, MemoryBus::Access::kAccessFull>>(
					   InOutCpu, InOutIo, InOutBudget, nullptr);
}
