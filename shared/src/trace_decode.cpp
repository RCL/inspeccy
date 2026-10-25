
#include <trace/sld.h>
#include <trace/text_format.h>
#include <trace/trace_binary.h>
#include <trace/trace_frame.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// Stream reader: pulls bytes from a FILE* with end-of-stream / short-read
// detection. Used by both the header parser and the per-record decode.
struct Reader
{
	FILE* m_F;
	bool m_bEof = false;
	bool m_bError = false;

	bool ReadExact(void* Dest, size_t N)
	{
		if (m_bError || m_bEof)
		{
			return false;
		}
		const size_t Got = fread(Dest, 1, N, m_F);
		if (Got != N)
		{
			if (std::feof(m_F))
			{
				m_bEof = true;
			}
			else
			{
				m_bError = true;
			}
			return false;
		}
		return true;
	}

	bool ReadU8(uint8_t& InOutV)
	{
		return ReadExact(&InOutV, 1);
	}
	bool ReadU16(uint16_t& InOutV)
	{
		uint8_t B[2];
		if (!ReadExact(B, 2))
		{
			return false;
		}
		InOutV = uint16_t(B[0]) | (uint16_t(B[1]) << 8);
		return true;
	}
	bool ReadU32(uint32_t& InOutV)
	{
		uint8_t B[4];
		if (!ReadExact(B, 4))
		{
			return false;
		}
		InOutV = uint32_t(B[0]) | (uint32_t(B[1]) << 8) | (uint32_t(B[2]) << 16) | (uint32_t(B[3]) << 24);
		return true;
	}
	bool ReadVarint(uint64_t& InOutV)
	{
		InOutV = 0;
		int Shift = 0;
		for (int IdxI = 0; IdxI < 10; ++IdxI)
		{
			uint8_t B;
			if (!ReadU8(B))
			{
				return false;
			}
			InOutV |= uint64_t(B & 0x7F) << Shift;
			if ((B & 0x80) == 0)
			{
				return true;
			}
			Shift += 7;
		}
		m_bError = true; // overlong varint
		return false;
	}
};

} // namespace

// Reads InPath (.btr), writes the text trace to Out (stdout). Returns 0 on
// success, non-zero on format error. Output is byte-identical to what
// `--trace-out <Out>` would have produced for the same run, modulo the
// invocation banner. The round-trip invariant is locked in by
// tests/python/test_btr_roundtrip.py.
int DecodeBinaryTraceToText(const char* InPath, FILE* Out, const SldSymbolMap* Symbols)
{
	FILE* F = fopen(InPath, "rb");
	if (F == nullptr)
	{
		fprintf(stderr, "trace-decode: cannot open %s: %s\n", InPath, std::strerror(errno));
		return 3;
	}

	using namespace BinaryTrace;

	// --- Read the fixed-prefix header + length-prefixed blobs + CRC ----------
	Header H{};
	Reader R{F};
	if (!R.ReadExact(&H, sizeof(H)))
	{
		fprintf(stderr, "trace-decode: short read in header\n");
		fclose(F);
		return 3;
	}
	if (H.m_Magic != kMagic)
	{
		fprintf(stderr, "trace-decode: not a .btr file (magic=0x%08X)\n", H.m_Magic);
		fclose(F);
		return 3;
	}
	if (H.m_Version != kVersion)
	{
		fprintf(stderr, "trace-decode: unsupported version %u (this build expects %u)\n", H.m_Version, kVersion);
		fclose(F);
		return 3;
	}

	uint16_t CliLen = 0;
	if (!R.ReadU16(CliLen))
	{
		fclose(F);
		return 3;
	}
	std::vector<uint8_t> CliBytes(CliLen);
	if (CliLen && !R.ReadExact(CliBytes.data(), CliLen))
	{
		fclose(F);
		return 3;
	}
	uint16_t SldLen = 0;
	if (!R.ReadU16(SldLen))
	{
		fclose(F);
		return 3;
	}
	std::vector<uint8_t> SldBytes(SldLen);
	if (SldLen && !R.ReadExact(SldBytes.data(), SldLen))
	{
		fclose(F);
		return 3;
	}
	uint32_t StoredCrc = 0;
	if (!R.ReadU32(StoredCrc))
	{
		fclose(F);
		return 3;
	}
	{
		std::vector<uint8_t> CrcBuf;
		CrcBuf.insert(CrcBuf.end(), reinterpret_cast<uint8_t*>(&H), reinterpret_cast<uint8_t*>(&H) + sizeof(H));
		const uint8_t CliLenLE[2] = {uint8_t(CliLen), uint8_t(CliLen >> 8)};
		CrcBuf.insert(CrcBuf.end(), CliLenLE, CliLenLE + 2);
		CrcBuf.insert(CrcBuf.end(), CliBytes.begin(), CliBytes.end());
		const uint8_t SldLenLE[2] = {uint8_t(SldLen), uint8_t(SldLen >> 8)};
		CrcBuf.insert(CrcBuf.end(), SldLenLE, SldLenLE + 2);
		CrcBuf.insert(CrcBuf.end(), SldBytes.begin(), SldBytes.end());
		const uint32_t Computed = Crc32(CrcBuf.data(), CrcBuf.size());
		if (Computed != StoredCrc)
		{
			fprintf(stderr,
				"trace-decode: header CRC32 mismatch (stored=0x%08X, "
				"computed=0x%08X) - file likely truncated mid-write\n",
				StoredCrc, Computed);
			fclose(F);
			return 3;
		}
	}

	// --- Re-emit the text header lines -------------------------------------
	std::fputs("# ", Out);
	if (CliLen)
	{
		fwrite(CliBytes.data(), 1, CliLen, Out);
	}
	std::fputc('\n', Out);
	if (Symbols != nullptr)
	{
		std::fputs("# columns: cycles | frame | address | symbol | mnemonic | effects\n", Out);
	}
	else
	{
		std::fputs("# columns: cycles | frame | address | mnemonic | effects\n", Out);
	}

	// --- Reconstruct format options from header flags ----------------------
	TraceFormatOptions Opt;
	Opt.m_bVerbose = (H.m_Flags & kFlag_Verbose) != 0;
	Opt.m_bIncludeR = (H.m_Flags & kFlag_IncludeR) != 0;
	Opt.m_bIncludeMemPtr = (H.m_Flags & kFlag_IncludeMemPtr) != 0;
	Opt.m_bDiffOnly = (H.m_Flags & kFlag_DiffOnly) != 0;
	Opt.m_bB16KPageView = (H.m_Flags & kFlag_16KPageView) != 0;

	// --- Running CPU state for delta application ---------------------------
	// Init from header's snapshot of "state just before the first record".
	uint16_t RunPC = H.m_InitPC, RunAF = H.m_InitAF, RunBC = H.m_InitBC, RunDE = H.m_InitDE, RunHL = H.m_InitHL,
			 RunIX = H.m_InitIX, RunIY = H.m_InitIY, RunSP = H.m_InitSP;
	uint16_t RunAFp = H.m_InitAFp, RunBCp = H.m_InitBCp, RunDEp = H.m_InitDEp, RunHLp = H.m_InitHLp;
	uint16_t RunMemPtr = H.m_InitMemPtr;
	uint8_t RunI = H.m_InitI, RunR = H.m_InitR;
	uint8_t RunIFF1 = H.m_InitIFF1, RunIFF2 = H.m_InitIFF2;
	uint8_t RunHalted = 0;
	uint64_t RunCycle = H.m_StartCycle;
	uint64_t RunFrame = H.m_StartFrame;
	uint64_t LastEmittedFrame = UINT64_MAX;

	// --- Walk record stream until EOF --------------------------------------
	for (;;)
	{
		uint8_t Tag = 0;
		if (!R.ReadU8(Tag))
		{
			if (R.m_bEof)
			{
				break;
			}
			fprintf(stderr, "trace-decode: short read at record tag\n");
			fclose(F);
			return 3;
		}

		// FrameBoundary: advance the running frame counter and cycle, then
		// continue without emitting a text line. The text writer doesn't have
		// an explicit "frame boundary" line; it emits inter-frame dividers
		// inline from FormatTextLine when it sees Frame.Frame change.
		if (Tag == kRecord_FrameBoundary)
		{
			uint64_t CycleDelta = 0, FrameNumber = 0;
			if (!R.ReadVarint(CycleDelta) || !R.ReadVarint(FrameNumber))
			{
				fprintf(stderr, "trace-decode: bad varint in FrameBoundary\n");
				fclose(F);
				return 3;
			}
			RunCycle += CycleDelta;
			RunFrame = FrameNumber;
			continue;
		}

		// v4 video-state events: zero-width records with their own short
		// shape. Decoded and printed as comment lines so a text trace shows
		// where the display changed, then skipped - they carry no register
		// state for the instruction formatter below.
		if (Tag == kRecord_NextRegWrite || Tag == kRecord_PortWrite)
		{
			uint64_t Delta = 0;
			if (!R.ReadVarint(Delta))
			{
				fclose(F);
				return 3;
			}
			RunCycle += Delta;
			if (Tag == kRecord_NextRegWrite)
			{
				uint8_t Reg = 0, Val = 0, Src = 0;
				if (!R.ReadU8(Reg) || !R.ReadU8(Val) || !R.ReadU8(Src))
				{
					fclose(F);
					return 3;
				}
				static const char* const SrcName[] = {"nextreg", "port $253B", "copper"};
				fprintf(Out, "# %llu video: NR $%02X = $%02X (%s)\n", (unsigned long long)RunCycle, Reg, Val,
					Src < 3 ? SrcName[Src] : "?");
			}
			else
			{
				uint16_t Port = 0;
				uint8_t Val = 0;
				if (!R.ReadU16(Port) || !R.ReadU8(Val))
				{
					fclose(F);
					return 3;
				}
				fprintf(Out, "# %llu video: port $%04X = $%02X\n", (unsigned long long)RunCycle, Port, Val);
			}
			continue;
		}

		// Tags we don't emit-via-text. The TrappedCall slot is still reserved;
		// a forward-compatible reader skips cleanly by knowing each record's
		// shape. v1 decoder hard-errors on unknown tags.
		if (Tag != kRecord_Instr && Tag != kRecord_LdirIter && Tag != kRecord_Interrupt && Tag != kRecord_DmaDrip &&
			Tag != kRecord_InvalidOpcode && Tag != kRecord_RestartTrap)
		{
			fprintf(stderr, "trace-decode: unknown record tag 0x%02X at offset ~%lld\n", Tag, (long long)ftell(F));
			fclose(F);
			return 3;
		}

		uint64_t CycleDelta = 0;
		if (!R.ReadVarint(CycleDelta))
		{
			fclose(F);
			return 3;
		}
		RunCycle += CycleDelta;

		uint16_t RecPC = 0;
		uint8_t RecPageTag = 0;
		uint8_t Opcodes[4] = {};
		if (!R.ReadU16(RecPC) || !R.ReadU8(RecPageTag) || !R.ReadExact(Opcodes, 4))
		{
			fclose(F);
			return 3;
		}

		// reg_diff_mask + per-set-bit values.
		uint16_t Mask = 0;
		if (!R.ReadU16(Mask))
		{
			fclose(F);
			return 3;
		}
		uint16_t PostPC = RunPC, PostAF = RunAF, PostBC = RunBC, PostDE = RunDE, PostHL = RunHL, PostIX = RunIX,
				 PostIY = RunIY, PostSP = RunSP;
		uint16_t PostAFp = RunAFp, PostBCp = RunBCp, PostDEp = RunDEp, PostHLp = RunHLp;
		uint16_t PostMemPtr = RunMemPtr;
		uint8_t PostI = RunI, PostR = RunR;
		uint8_t PostIFFsPacked = uint8_t((RunIFF1 & 1) | ((RunIFF2 & 1) << 1) | ((RunHalted & 1) << 7));
		// Per-bit decoders: if the diff mask says this field changed, read
		// the new value off the stream; otherwise leave Dst as the running
		// state. Returns false on short read so the caller can bail.
		auto Rd16IfSet = [&](uint16_t Bit, uint16_t& OutDst) -> bool
		{
			if (!(Mask & Bit))
			{
				return true;
			}
			return R.ReadU16(OutDst);
		};
		auto Rd8IfSet = [&](uint16_t Bit, uint8_t& OutDst) -> bool
		{
			if (!(Mask & Bit))
			{
				return true;
			}
			return R.ReadU8(OutDst);
		};
		// 15 register fields x the same bail-on-short-read pattern. Wrap in
		// an OR-chain so we have one error site instead of fifteen - and the
		// short-circuit nature of `||` aborts on the first failure as
		// expected.
		const bool bRegsOk = Rd16IfSet(kRegDiffBitDiffPc, PostPC) && Rd16IfSet(kRegDiffBitDiffAf, PostAF) &&
							 Rd16IfSet(kRegDiffBitDiffBc, PostBC) && Rd16IfSet(kRegDiffBitDiffDe, PostDE) &&
							 Rd16IfSet(kRegDiffBitDiffHl, PostHL) && Rd16IfSet(kRegDiffBitDiffIx, PostIX) &&
							 Rd16IfSet(kRegDiffBitDiffIy, PostIY) && Rd16IfSet(kRegDiffBitDiffSp, PostSP) &&
							 Rd16IfSet(kRegDiffBitDiffAFp, PostAFp) && Rd16IfSet(kRegDiffBitDiffBCp, PostBCp) &&
							 Rd16IfSet(kRegDiffBitDiffDEp, PostDEp) && Rd16IfSet(kRegDiffBitDiffHLp, PostHLp) &&
							 Rd16IfSet(kRegDiffBitDiffMemPtr, PostMemPtr) && Rd8IfSet(kRegDiffBitDiffI, PostI) &&
							 Rd8IfSet(kRegDiffBitDiffR, PostR) && Rd8IfSet(kRegDiffBitDiffIFFs, PostIFFsPacked);
		if (!bRegsOk)
		{
			fclose(F);
			return 3;
		}

		// Memory ops.
		uint8_t NumMem = 0;
		if (!R.ReadU8(NumMem))
		{
			fclose(F);
			return 3;
		}
		if (NumMem > 4)
		{
			fprintf(stderr, "trace-decode: NumMemWrites=%u exceeds 4\n", NumMem);
			fclose(F);
			return 3;
		}
		TraceFrame Frame{};
		Frame.m_NumMemWrites = NumMem;
		for (int IdxI = 0; IdxI < NumMem; ++IdxI)
		{
			uint16_t Addr = 0;
			uint8_t Val = 0;
			if (!R.ReadU16(Addr) || !R.ReadU8(Val))
			{
				fclose(F);
				return 3;
			}
			Frame.m_MemWrites[IdxI].m_Addr = Addr;
			Frame.m_MemWrites[IdxI].m_Value = Val;
		}

		// Map tag back to TraceFrame Flag bits.
		switch (Tag)
		{
		case kRecord_InvalidOpcode:
			Frame.m_Flags |= TraceFrame::kFlagInvalid;
			break;
		case kRecord_RestartTrap:
			Frame.m_Flags |= TraceFrame::kFlagResetTrap;
			break;
		case kRecord_Interrupt:
			Frame.m_Flags |= TraceFrame::kFlagInterrupt;
			// Older records omitted the bus value; use the unvectored ULA default.
			Frame.m_InterruptVectorByte = Opcodes[1] == kIntAckMarker ? Opcodes[0] : 0xFF;
			break;
		case kRecord_DmaDrip:
			Frame.m_Flags |= TraceFrame::kFlagDma;
			break;
		default:
			break;
		}

		// Fill TraceFrame for the shared formatter. Pre = running state
		// (= the state just before this instruction executed); Post = state
		// after applying this record's deltas to the running state.
		Frame.m_StartCycles = RunCycle;
		Frame.m_Frame = RunFrame;
		Frame.m_PrePC = RunPC;
		Frame.m_PreAF = RunAF;
		Frame.m_PreBC = RunBC;
		Frame.m_PreDE = RunDE;
		Frame.m_PreHL = RunHL;
		Frame.m_PreIX = RunIX;
		Frame.m_PreIY = RunIY;
		Frame.m_PreSP = RunSP;
		Frame.m_PreAFp = RunAFp;
		Frame.m_PreBCp = RunBCp;
		Frame.m_PreDEp = RunDEp;
		Frame.m_PreHLp = RunHLp;
		Frame.m_PreMemPtr = RunMemPtr;
		Frame.m_PreI = RunI;
		Frame.m_PreR = RunR;
		Frame.m_PreIFF1 = RunIFF1;
		Frame.m_PreIFF2 = RunIFF2;
		Frame.m_PreHalted = RunHalted;
		Frame.m_PrePageTag = RecPageTag;
		memcpy(Frame.m_OpcodeBytes, Opcodes, 4);

		Frame.m_PostPC = PostPC;
		Frame.m_PostAF = PostAF;
		Frame.m_PostBC = PostBC;
		Frame.m_PostDE = PostDE;
		Frame.m_PostHL = PostHL;
		Frame.m_PostIX = PostIX;
		Frame.m_PostIY = PostIY;
		Frame.m_PostSP = PostSP;
		Frame.m_PostAFp = PostAFp;
		Frame.m_PostBCp = PostBCp;
		Frame.m_PostDEp = PostDEp;
		Frame.m_PostHLp = PostHLp;
		Frame.m_PostMemPtr = PostMemPtr;
		Frame.m_PostI = PostI;
		Frame.m_PostR = PostR;
		Frame.m_PostIFF1 = uint8_t(PostIFFsPacked & 1);
		Frame.m_PostIFF2 = uint8_t((PostIFFsPacked >> 1) & 1);
		Frame.m_PostHalted = uint8_t((PostIFFsPacked >> 7) & 1);

		FormatTextLine(Frame, Opt, Symbols, Out, LastEmittedFrame);

		// Advance running state.
		RunPC = PostPC;
		RunAF = PostAF;
		RunBC = PostBC;
		RunDE = PostDE;
		RunHL = PostHL;
		RunIX = PostIX;
		RunIY = PostIY;
		RunSP = PostSP;
		RunAFp = PostAFp;
		RunBCp = PostBCp;
		RunDEp = PostDEp;
		RunHLp = PostHLp;
		RunMemPtr = PostMemPtr;
		RunI = PostI;
		RunR = PostR;
		RunIFF1 = Frame.m_PostIFF1;
		RunIFF2 = Frame.m_PostIFF2;
		RunHalted = Frame.m_PostHalted;
	}

	fclose(F);
	return 0;
}
