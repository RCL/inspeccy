#include <trace/disasm.h>

#include <stdio.h>
#include <string.h>

namespace
{

const char* const kReg8Names[8] = {"b", "c", "d", "e", "h", "l", "(hl)", "a"};
const char* const kReg16Sp[4] = {"bc", "de", "hl", "sp"};
const char* const kReg16Af[4] = {"bc", "de", "hl", "af"};
const char* const kCondNames[8] = {"nz", "z", "nc", "c", "po", "pe", "p", "m"};
const char* const kAluNames[8] = {"add a,", "adc a,", "sub", "sbc a,", "and", "xor", "or", "cp"};
const char* const kRotNames[8] = {"rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"};

// Map an 8-bit register index (b/c/d/e/h/l/(hl)/a) to its DEST_* mask. (HL) -> 0
// (memory writes covered by mem-write hook). Under DD/FD prefix, H/L map to
// DEST_IX or DEST_IY since the high/low halves are part of the index register.
inline uint16_t DestForReg8(int R, int PrefixIdx)
{
	switch (R)
	{
	case 0:
	case 1:
		return kDestBc;
	case 2:
	case 3:
		return kDestDe;
	case 4:
	case 5:
		if (PrefixIdx == 0)
		{
			return kDestHl;
		}
		return (PrefixIdx == 1) ? kDestIx : kDestIy;
	case 6:
		return 0;
	case 7:
		return kDestA;
	}
	return 0;
}

// Map a 16-bit register-pair index (Reg16Sp: bc/de/hl/sp) to its DEST_* mask.
inline uint16_t DestForRegPairSp(int P, int PrefixIdx)
{
	switch (P)
	{
	case 0:
		return kDestBc;
	case 1:
		return kDestDe;
	case 2:
		if (PrefixIdx == 0)
		{
			return kDestHl;
		}
		return (PrefixIdx == 1) ? kDestIx : kDestIy;
	case 3:
		return kDestSp;
	}
	return 0;
}

// Map a 16-bit register-pair index (Reg16Af: bc/de/hl/af) to its DEST_* mask.
inline uint16_t DestForRegPairAf(int P, int PrefixIdx)
{
	switch (P)
	{
	case 0:
		return kDestBc;
	case 1:
		return kDestDe;
	case 2:
		if (PrefixIdx == 0)
		{
			return kDestHl;
		}
		return (PrefixIdx == 1) ? kDestIx : kDestIy;
	case 3:
		return kDestA | kDestF;
	}
	return 0;
}

void IndexRegName(int PrefixIdx, char Half, char* OutBuf)
{
	const char* P = (PrefixIdx == 1) ? "ix" : "iy";
	if (Half == 'h' || Half == 'l')
	{
		snprintf(OutBuf, 6, "%s%c", P, Half);
	}
	else
	{
		snprintf(OutBuf, 6, "%s", P);
	}
}

void FormatIndexed(int PrefixIdx, int8_t Disp, char* OutBuf, size_t BufSize)
{
	const char* P = (PrefixIdx == 1) ? "ix" : "iy";
	if (Disp >= 0)
	{
		snprintf(OutBuf, BufSize, "(%s+$%02X)", P, uint8_t(Disp));
	}
	else
	{
		snprintf(OutBuf, BufSize, "(%s-$%02X)", P, uint8_t(-int(Disp)));
	}
}

void Reg8ForPrefix(int PrefixIdx, int R, int8_t Disp, char* OutBuf, size_t BufSize)
{
	if (PrefixIdx == 0)
	{
		snprintf(OutBuf, BufSize, "%s", kReg8Names[R]);
		return;
	}
	if (R == 6)
	{
		FormatIndexed(PrefixIdx, Disp, OutBuf, BufSize);
		return;
	}
	if (R == 4)
	{
		IndexRegName(PrefixIdx, 'h', OutBuf);
		return;
	}
	if (R == 5)
	{
		IndexRegName(PrefixIdx, 'l', OutBuf);
		return;
	}
	snprintf(OutBuf, BufSize, "%s", kReg8Names[R]);
}

// ---------------------------------------------------------------------------
// CB-prefixed disasm. Sets DM for the destination register (and F for non-BIT).
// ---------------------------------------------------------------------------

void DisasmCB(uint8_t Op2, int PrefixIdx, int8_t Disp, char* Out, size_t BufSize, uint16_t& InOutDM)
{
	const int CbOp = (Op2 >> 6) & 0x03;
	const int CbSub = (Op2 >> 3) & 0x07;
	const int CbReg = Op2 & 0x07;

	char Operand[16];
	if (PrefixIdx != 0)
	{
		FormatIndexed(PrefixIdx, Disp, Operand, sizeof(Operand));
	}
	else
	{
		snprintf(Operand, sizeof(Operand), "%s", kReg8Names[CbReg]);
	}

	// For DDCB/FDCB the operation also writes back to the named register if
	// CbReg != 6. The destination is therefore both (IX+d) memory AND the
	// 8-bit register slot. We mark only the register here; mem hook covers (IX+d).
	const uint16_t RegDM = (PrefixIdx != 0)
							   ? ((CbReg != 6) ? DestForReg8(CbReg, 0) : 0) // undocumented copy-back register
							   : DestForReg8(CbReg, 0);

	switch (CbOp)
	{
	case 0: // RLC/RRC/RL/RR/SLA/SRA/SLL/SRL - modifies r and F
		snprintf(Out, BufSize, "%s %s", kRotNames[CbSub], Operand);
		InOutDM |= RegDM | kDestF;
		break;
	case 1: // BIT n, r - modifies F only; show source register as context
		snprintf(Out, BufSize, "bit %d, %s", CbSub, Operand);
		InOutDM |= kDestF;
		if (CbReg == 6 && PrefixIdx != 0)
		{
			InOutDM |= (PrefixIdx == 1) ? kDestIx : kDestIy;
		}
		else if (CbReg == 6)
		{
			InOutDM |= kDestHl;
		}
		else
		{
			InOutDM |= DestForReg8(CbReg, 0);
		}
		break;
	case 2: // RES n, r - modifies r
		snprintf(Out, BufSize, "res %d, %s", CbSub, Operand);
		InOutDM |= RegDM;
		break;
	case 3: // SET n, r - modifies r
		snprintf(Out, BufSize, "set %d, %s", CbSub, Operand);
		InOutDM |= RegDM;
		break;
	}
}

// ---------------------------------------------------------------------------
// ED-prefixed disasm. Sets DM for each handled case.
// ---------------------------------------------------------------------------

int DisasmED(const uint8_t* Bytes, char* Out, size_t BufSize, uint16_t& InOutDM)
{
	const uint8_t Op = Bytes[1];

	// Z80N extensions used by the dispatcher (subset).
	switch (Op)
	{
	case 0x23:
		snprintf(Out, BufSize, "swapnib");
		InOutDM |= kDestA;
		return 1;
	case 0x24:
		snprintf(Out, BufSize, "mirror a");
		InOutDM |= kDestA;
		return 1;
	case 0x27:
		snprintf(Out, BufSize, "test $%02X", Bytes[2]);
		InOutDM |= kDestF;
		return 2;
	case 0x28:
		snprintf(Out, BufSize, "bsla de, b");
		InOutDM |= kDestDe;
		return 1;
	case 0x29:
		snprintf(Out, BufSize, "bsra de, b");
		InOutDM |= kDestDe;
		return 1;
	case 0x2A:
		snprintf(Out, BufSize, "bsrl de, b");
		InOutDM |= kDestDe;
		return 1;
	case 0x2B:
		snprintf(Out, BufSize, "bsrf de, b");
		InOutDM |= kDestDe;
		return 1;
	case 0x2C:
		snprintf(Out, BufSize, "brlc de, b");
		InOutDM |= kDestDe;
		return 1;
	case 0x30:
		snprintf(Out, BufSize, "mul d, e");
		InOutDM |= kDestDe;
		return 1;
	case 0x31:
		snprintf(Out, BufSize, "add hl, a");
		InOutDM |= kDestHl;
		return 1;
	case 0x32:
		snprintf(Out, BufSize, "add de, a");
		InOutDM |= kDestDe;
		return 1;
	case 0x33:
		snprintf(Out, BufSize, "add bc, a");
		InOutDM |= kDestBc;
		return 1;
	case 0x34:
		snprintf(Out, BufSize, "add hl, $%02X%02X", Bytes[3], Bytes[2]);
		InOutDM |= kDestHl;
		return 3;
	case 0x35:
		snprintf(Out, BufSize, "add de, $%02X%02X", Bytes[3], Bytes[2]);
		InOutDM |= kDestDe;
		return 3;
	case 0x36:
		snprintf(Out, BufSize, "add bc, $%02X%02X", Bytes[3], Bytes[2]);
		InOutDM |= kDestBc;
		return 3;
	case 0x8A:
		snprintf(Out, BufSize, "push $%02X%02X", Bytes[2], Bytes[3]);
		InOutDM |= kDestSp;
		return 3;
	case 0x90:
		snprintf(Out, BufSize, "outinb");
		InOutDM |= kDestHl;
		return 1;
	case 0x91:
		snprintf(Out, BufSize, "nextreg $%02X, $%02X", Bytes[2], Bytes[3]);
		return 3;
	case 0x92:
		snprintf(Out, BufSize, "nextreg $%02X, a", Bytes[2]);
		return 2;
	case 0x93:
		snprintf(Out, BufSize, "pixeldn");
		InOutDM |= kDestHl;
		return 1;
	case 0x94:
		snprintf(Out, BufSize, "pixelad");
		InOutDM |= kDestDe;
		return 1;
	case 0x95:
		snprintf(Out, BufSize, "setae");
		InOutDM |= kDestA;
		return 1;
	case 0x98:
		snprintf(Out, BufSize, "jp (c)");
		InOutDM |= kDestPc;
		return 1;
	case 0xA4:
		snprintf(Out, BufSize, "ldix");
		InOutDM |= kDestHl | kDestDe | kDestBc;
		return 1;
	case 0xA5:
		snprintf(Out, BufSize, "ldws");
		InOutDM |= kDestHl | kDestDe;
		return 1;
	case 0xAC:
		snprintf(Out, BufSize, "lddx");
		InOutDM |= kDestHl | kDestDe | kDestBc;
		return 1;
	case 0xB4:
		snprintf(Out, BufSize, "ldirx");
		InOutDM |= kDestHl | kDestDe | kDestBc;
		return 1;
	case 0xB7:
		snprintf(Out, BufSize, "ldpirx");
		InOutDM |= kDestHl | kDestDe | kDestBc;
		return 1;
	case 0xBC:
		snprintf(Out, BufSize, "lddrx");
		InOutDM |= kDestHl | kDestDe | kDestBc;
		return 1;
	default:
		break;
	}

	const int Y = (Op >> 3) & 0x07;
	const int Z = Op & 0x07;
	const int P = (Op >> 4) & 0x03;
	const int Q = (Op >> 3) & 0x01;
	const int X = (Op >> 6) & 0x03;

	if (X == 1)
	{
		switch (Z)
		{
		case 0:
			if (Y == 6)
			{
				snprintf(Out, BufSize, "in (c)");
				InOutDM |= kDestF;
				return 1;
			}
			snprintf(Out, BufSize, "in %s, (c)", kReg8Names[Y]);
			InOutDM |= DestForReg8(Y, 0) | kDestF;
			return 1;
		case 1:
			if (Y == 6)
			{
				snprintf(Out, BufSize, "out (c), 0");
				return 1;
			}
			snprintf(Out, BufSize, "out (c), %s", kReg8Names[Y]);
			return 1;
		case 2:
			if (Q == 0)
			{
				snprintf(Out, BufSize, "sbc hl, %s", kReg16Sp[P]);
				InOutDM |= kDestHl | kDestF;
				return 1;
			}
			snprintf(Out, BufSize, "adc hl, %s", kReg16Sp[P]);
			InOutDM |= kDestHl | kDestF;
			return 1;
		case 3:
		{
			const uint16_t Nn = uint16_t(Bytes[2]) | (uint16_t(Bytes[3]) << 8);
			if (Q == 0)
			{
				snprintf(Out, BufSize, "ld ($%04X), %s", Nn, kReg16Sp[P]);
				// memory write - mem hook covers it
			}
			else
			{
				snprintf(Out, BufSize, "ld %s, ($%04X)", kReg16Sp[P], Nn);
				InOutDM |= DestForRegPairSp(P, 0);
			}
			return 3;
		}
		case 4:
			snprintf(Out, BufSize, "neg");
			InOutDM |= kDestA | kDestF;
			return 1;
		case 5:
			snprintf(Out, BufSize, "%s", Y == 1 ? "reti" : "retn");
			InOutDM |= kDestPc | kDestSp;
			return 1;
		case 6:
			snprintf(Out, BufSize, "im %d", (Y == 0 || Y == 1 || Y == 4 || Y == 5) ? 0 : (Y == 2 || Y == 6 ? 1 : 2));
			return 1;
		case 7:
			switch (Y)
			{
			case 0:
				snprintf(Out, BufSize, "ld i, a");
				InOutDM |= kDestI;
				return 1;
			case 1:
				snprintf(Out, BufSize, "ld r, a");
				InOutDM |= kDestR;
				return 1;
			case 2:
				snprintf(Out, BufSize, "ld a, i");
				InOutDM |= kDestA | kDestF;
				return 1;
			case 3:
				snprintf(Out, BufSize, "ld a, r");
				InOutDM |= kDestA | kDestF;
				return 1;
			case 4:
				snprintf(Out, BufSize, "rrd");
				InOutDM |= kDestA | kDestF;
				return 1;
			case 5:
				snprintf(Out, BufSize, "rld");
				InOutDM |= kDestA | kDestF;
				return 1;
			default:
				snprintf(Out, BufSize, "nop");
				return 1;
			}
		}
	}

	if (X == 2 && Z <= 3 && Y >= 4)
	{
		// Block instructions: LDI/LDD/LDIR/LDDR (Z=0), CPI/CPD/CPIR/CPDR (Z=1),
		// INI/IND/INIR/INDR (Z=2), OUTI/OUTD/OTIR/OTDR (Z=3)
		static const char* const BlockNames[4][4] = {
			{"ldi", "cpi", "ini", "outi"},
			{"ldd", "cpd", "ind", "outd"},
			{"ldir", "cpir", "inir", "otir"},
			{"lddr", "cpdr", "indr", "otdr"},
		};
		snprintf(Out, BufSize, "%s", BlockNames[Y - 4][Z]);
		switch (Z)
		{
		case 0:
			InOutDM |= kDestHl | kDestDe | kDestBc | kDestF;
			break; // LD-class
		case 1:
			InOutDM |= kDestHl | kDestBc | kDestF;
			break; // CP-class
		case 2:
			InOutDM |= kDestHl | kDestBc | kDestF;
			break; // IN-class
		case 3:
			InOutDM |= kDestHl | kDestBc | kDestF;
			break; // OUT-class
		}
		return 1;
	}

	snprintf(Out, BufSize, "db $ED, $%02X", Op);
	return 1;
}

// ---------------------------------------------------------------------------
// Main disassembler.
// ---------------------------------------------------------------------------

int DisasmMain(const uint8_t* Bytes, uint16_t PC, int PrefixIdx, char* Out, size_t BufSize, uint16_t& InOutDM)
{
	uint8_t Op = Bytes[0];

	if (Op == 0xDD || Op == 0xFD)
	{
		const int NewIdx = (Op == 0xDD) ? 1 : 2;
		const int InnerLen = DisasmMain(Bytes + 1, uint16_t(PC + 1), NewIdx, Out, BufSize, InOutDM);
		return InnerLen + 1;
	}

	if (Op == 0xCB)
	{
		if (PrefixIdx != 0)
		{
			const int8_t Disp = int8_t(Bytes[1]);
			const uint8_t Op2 = Bytes[2];
			DisasmCB(Op2, PrefixIdx, Disp, Out, BufSize, InOutDM);
			return 3;
		}
		DisasmCB(Bytes[1], 0, 0, Out, BufSize, InOutDM);
		return 2;
	}

	if (Op == 0xED)
	{
		return DisasmED(Bytes, Out, BufSize, InOutDM) + 1;
	}

	const int X = (Op >> 6) & 0x03;
	const int Y = (Op >> 3) & 0x07;
	const int Z = Op & 0x07;
	const int P = (Op >> 4) & 0x03;
	const int Q = (Op >> 3) & 0x01;

	char R1[24], R2[24];

	if (X == 0)
	{
		switch (Z)
		{
		case 0:
			switch (Y)
			{
			case 0:
				snprintf(Out, BufSize, "nop");
				return 1;
			case 1:
				snprintf(Out, BufSize, "ex af, af'");
				InOutDM |= kDestA | kDestF | kDestAfp;
				return 1;
			case 2:
			{
				const int8_t D = int8_t(Bytes[1]);
				const uint16_t Target = uint16_t(PC + 2 + D);
				snprintf(Out, BufSize, "djnz $%04X", Target);
				InOutDM |= kDestBc | kDestPc;
				return 2;
			}
			case 3:
			{
				const int8_t D = int8_t(Bytes[1]);
				const uint16_t Target = uint16_t(PC + 2 + D);
				snprintf(Out, BufSize, "jr $%04X", Target);
				InOutDM |= kDestPc;
				return 2;
			}
			default:
			{
				const int8_t D = int8_t(Bytes[1]);
				const uint16_t Target = uint16_t(PC + 2 + D);
				snprintf(Out, BufSize, "jr %s, $%04X", kCondNames[Y - 4], Target);
				InOutDM |= kDestPc;
				return 2;
			}
			}
		case 1:
		{
			const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
			const char* DstHL = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
			if (Q == 0)
			{
				const char* Rp = (P == 2) ? DstHL : kReg16Sp[P];
				snprintf(Out, BufSize, "ld %s, $%04X", Rp, Nn);
				InOutDM |= DestForRegPairSp(P, PrefixIdx);
				return 3;
			}
			const char* SrcReg = (P == 2) ? DstHL : kReg16Sp[P];
			snprintf(Out, BufSize, "add %s, %s", DstHL, SrcReg);
			// Destination: always the prefix-aware HL.
			InOutDM |= ((PrefixIdx == 0) ? kDestHl : (PrefixIdx == 1 ? kDestIx : kDestIy)) | kDestF;
			return 1;
		}
		case 2:
		{
			switch ((Q << 2) | (P & 3))
			{
			case 0:
				snprintf(Out, BufSize, "ld (bc), a");
				return 1; // mem write - hook covers
			case 1:
				snprintf(Out, BufSize, "ld (de), a");
				return 1;
			case 2:
			{
				const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
				const char* Rp = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
				snprintf(Out, BufSize, "ld ($%04X), %s", Nn, Rp);
				return 3;
			}
			case 3:
			{
				const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
				snprintf(Out, BufSize, "ld ($%04X), a", Nn);
				return 3;
			}
			case 4:
				snprintf(Out, BufSize, "ld a, (bc)");
				InOutDM |= kDestA;
				return 1;
			case 5:
				snprintf(Out, BufSize, "ld a, (de)");
				InOutDM |= kDestA;
				return 1;
			case 6:
			{
				const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
				const char* Rp = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
				snprintf(Out, BufSize, "ld %s, ($%04X)", Rp, Nn);
				InOutDM |= (PrefixIdx == 0) ? kDestHl : (PrefixIdx == 1 ? kDestIx : kDestIy);
				return 3;
			}
			case 7:
			{
				const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
				snprintf(Out, BufSize, "ld a, ($%04X)", Nn);
				InOutDM |= kDestA;
				return 3;
			}
			}
			break;
		}
		case 3:
		{
			const char* Rp = kReg16Sp[P];
			if (PrefixIdx != 0 && P == 2)
			{
				Rp = (PrefixIdx == 1) ? "ix" : "iy";
			}
			snprintf(Out, BufSize, "%s %s", Q == 0 ? "inc" : "dec", Rp);
			InOutDM |= DestForRegPairSp(P, PrefixIdx);
			return 1;
		}
		case 4:
		case 5:
		{
			int8_t Disp = (PrefixIdx != 0 && Y == 6) ? int8_t(Bytes[1]) : 0;
			Reg8ForPrefix(PrefixIdx, Y, Disp, R1, sizeof(R1));
			const int Len = (PrefixIdx != 0 && Y == 6) ? 2 : 1;
			snprintf(Out, BufSize, "%s %s", Z == 4 ? "inc" : "dec", R1);
			InOutDM |= DestForReg8(Y, PrefixIdx) | kDestF;
			return Len;
		}
		case 6:
		{
			int8_t Disp = (PrefixIdx != 0 && Y == 6) ? int8_t(Bytes[1]) : 0;
			const int ImmPos = (PrefixIdx != 0 && Y == 6) ? 2 : 1;
			const uint8_t Imm = Bytes[ImmPos];
			Reg8ForPrefix(PrefixIdx, Y, Disp, R1, sizeof(R1));
			snprintf(Out, BufSize, "ld %s, $%02X", R1, Imm);
			InOutDM |= DestForReg8(Y, PrefixIdx);
			return ImmPos + 1;
		}
		case 7:
		{
			static const char* const Misc[8] = {"rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"};
			snprintf(Out, BufSize, "%s", Misc[Y]);
			// RLCA/RRCA/RLA/RRA/DAA/CPL touch A and F. SCF/CCF only F.
			InOutDM |= (Y < 6) ? (kDestA | kDestF) : kDestF;
			return 1;
		}
		}
	}

	if (X == 1)
	{
		if (Op == 0x76)
		{
			snprintf(Out, BufSize, "halt");
			return 1;
		}
		const bool bDstIsMem = (Y == 6);
		const bool bSrcIsMem = (Z == 6);
		const bool bIndexedMem = (PrefixIdx != 0) && (bDstIsMem || bSrcIsMem);
		int8_t Disp = bIndexedMem ? int8_t(Bytes[1]) : 0;
		const int Len = bIndexedMem ? 2 : 1;

		if (PrefixIdx == 0)
		{
			snprintf(Out, BufSize, "ld %s, %s", kReg8Names[Y], kReg8Names[Z]);
			InOutDM |= DestForReg8(Y, 0); // (HL) destination -> 0; mem hook covers
			return 1;
		}
		if (!bIndexedMem)
		{
			Reg8ForPrefix(PrefixIdx, Y, 0, R1, sizeof(R1));
			Reg8ForPrefix(PrefixIdx, Z, 0, R2, sizeof(R2));
			snprintf(Out, BufSize, "ld %s, %s", R1, R2);
			InOutDM |= DestForReg8(Y, PrefixIdx);
			return 1;
		}
		if (bDstIsMem)
		{
			FormatIndexed(PrefixIdx, Disp, R1, sizeof(R1));
			snprintf(Out, BufSize, "ld %s, %s", R1, kReg8Names[Z]);
			// mem dest covered by hook
			return Len;
		}
		FormatIndexed(PrefixIdx, Disp, R2, sizeof(R2));
		snprintf(Out, BufSize, "ld %s, %s", kReg8Names[Y], R2);
		InOutDM |= DestForReg8(Y, 0); // src is (IX+d), dest is plain reg
		return Len;
	}

	if (X == 2)
	{
		const int8_t Disp = (PrefixIdx != 0 && Z == 6) ? int8_t(Bytes[1]) : 0;
		const int Len = (PrefixIdx != 0 && Z == 6) ? 2 : 1;
		Reg8ForPrefix(PrefixIdx, Z, Disp, R1, sizeof(R1));
		snprintf(Out, BufSize, "%s %s", kAluNames[Y], R1);
		// ALU writes: ADD/ADC/SUB/SBC/AND/XOR/OR -> A | F. CP -> F only.
		// CP needs A visible too (it's compared against, even though unchanged).
		// AND/XOR/OR/CP also benefit from seeing the source register on the line.
		if (Y == 7)
		{
			InOutDM |= kDestA | kDestF; // CP: A is comparison context
		}
		else
		{
			InOutDM |= kDestA | kDestF; // ADD..OR: A is the dest
		}
		// Source-register context for the bitwise ops + CP. ADD/ADC/SUB/SBC
		// also "use" the source but the result lands in A; the source value
		// alone is rarely interesting there. Keep them as-is.
		if (Y >= 4)
		{
			// Source register: 6=(HL)/(IX+d)/(IY+d) -> DEST_HL or DEST_IX/IY
			// (pointer-as-context proxy); plain regs handled by DestForReg8.
			if (Z == 6 && PrefixIdx != 0)
			{
				InOutDM |= (PrefixIdx == 1) ? kDestIx : kDestIy;
			}
			else if (Z == 6)
			{
				InOutDM |= kDestHl;
			}
			else
			{
				InOutDM |= DestForReg8(Z, PrefixIdx);
			}
		}
		return Len;
	}

	// X == 3
	switch (Z)
	{
	case 0:
		snprintf(Out, BufSize, "ret %s", kCondNames[Y]);
		InOutDM |= kDestPc | kDestSp;
		return 1;
	case 1:
		if (Q == 0)
		{
			const char* Rp = kReg16Af[P];
			if (PrefixIdx != 0 && P == 2)
			{
				Rp = (PrefixIdx == 1) ? "ix" : "iy";
			}
			snprintf(Out, BufSize, "pop %s", Rp);
			InOutDM |= DestForRegPairAf(P, PrefixIdx) | kDestSp;
			return 1;
		}
		switch (P)
		{
		case 0:
			snprintf(Out, BufSize, "ret");
			InOutDM |= kDestPc | kDestSp;
			return 1;
		case 1:
			snprintf(Out, BufSize, "exx");
			InOutDM |= kDestBc | kDestDe | kDestHl | kDestAlt;
			return 1;
		case 2:
		{
			const char* Rp = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
			snprintf(Out, BufSize, "jp (%s)", Rp);
			InOutDM |= kDestPc;
			return 1;
		}
		case 3:
		{
			const char* Rp = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
			snprintf(Out, BufSize, "ld sp, %s", Rp);
			InOutDM |= kDestSp;
			return 1;
		}
		}
		break;
	case 2:
	{
		const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
		snprintf(Out, BufSize, "jp %s, $%04X", kCondNames[Y], Nn);
		InOutDM |= kDestPc;
		return 3;
	}
	case 3:
		switch (Y)
		{
		case 0:
		{
			const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
			snprintf(Out, BufSize, "jp $%04X", Nn);
			InOutDM |= kDestPc;
			return 3;
		}
		case 2:
			snprintf(Out, BufSize, "out ($%02X), a", Bytes[1]);
			return 2;
		case 3:
			snprintf(Out, BufSize, "in a, ($%02X)", Bytes[1]);
			InOutDM |= kDestA;
			return 2;
		case 4:
		{
			const char* Rp = (PrefixIdx == 0) ? "hl" : (PrefixIdx == 1 ? "ix" : "iy");
			snprintf(Out, BufSize, "ex (sp), %s", Rp);
			InOutDM |= (PrefixIdx == 0) ? kDestHl : (PrefixIdx == 1 ? kDestIx : kDestIy);
			return 1;
		}
		case 5:
			snprintf(Out, BufSize, "ex de, hl");
			InOutDM |= kDestDe | kDestHl;
			return 1;
		case 6:
			snprintf(Out, BufSize, "di");
			return 1;
		case 7:
			snprintf(Out, BufSize, "ei");
			return 1;
		}
		break;
	case 4:
	{
		const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
		snprintf(Out, BufSize, "call %s, $%04X", kCondNames[Y], Nn);
		InOutDM |= kDestPc | kDestSp;
		return 3;
	}
	case 5:
		if (Q == 0)
		{
			const char* Rp = kReg16Af[P];
			if (PrefixIdx != 0 && P == 2)
			{
				Rp = (PrefixIdx == 1) ? "ix" : "iy";
			}
			snprintf(Out, BufSize, "push %s", Rp);
			InOutDM |= kDestSp;
			return 1;
		}
		if (P == 0)
		{
			const uint16_t Nn = uint16_t(Bytes[1]) | (uint16_t(Bytes[2]) << 8);
			snprintf(Out, BufSize, "call $%04X", Nn);
			InOutDM |= kDestPc | kDestSp;
			return 3;
		}
		snprintf(Out, BufSize, "db $%02X", Op);
		return 1;
	case 6:
		// ALU n. CP n: show A as context (n is in the mnemonic).
		snprintf(Out, BufSize, "%s $%02X", kAluNames[Y], Bytes[1]);
		InOutDM |= kDestA | kDestF;
		return 2;
	case 7:
		snprintf(Out, BufSize, "rst $%02X", Y * 8);
		InOutDM |= kDestPc | kDestSp;
		return 1;
	}

	snprintf(Out, BufSize, "db $%02X", Op);
	return 1;
}

} // namespace

int Disassemble(const uint8_t* OpcodeBytes, uint16_t PC, char* OutBuf, size_t BufSize, uint16_t* OutDestMask)
{
	if (OpcodeBytes == nullptr || OutBuf == nullptr || BufSize < 4)
	{
		return 0;
	}
	uint16_t DM = 0;
	const int Len = DisasmMain(OpcodeBytes, PC, 0, OutBuf, BufSize, DM);
	if (OutDestMask)
	{
		*OutDestMask = DM;
	}
	return Len;
}
