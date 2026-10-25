#include <inspeccy/version.h>
#include "dump.h"

#include "ay.h"
#include "base64.h"
#include "dma.h"
#include "hexdump.h"
#include "audio_render.h"
#include "sprites.h"
#include "input.h"
#include "json_parser.h"
#include "memory.h"
#include "nextzxos.h"
#include "psg.h"
#include "sha1.h"
#include "trdos.h"
#include "tape.h"
#include "z80.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <filesystem>
#include <string>
#include <tuple>

namespace
{

// ---------- tiny JSON writer ----------

struct JsonWriter
{
  public:
	explicit JsonWriter(std::string& Out) : m_Out(Out)
	{
	}

	void BeginObject(const char* Key = nullptr)
	{
		WriteKey(Key);
		m_Out += '{';
		m_bAtStart = true;
		++m_Depth;
	}
	void EndObject()
	{
		--m_Depth;
		m_Out += '\n';
		WriteIndent();
		m_Out += '}';
		m_bAtStart = false;
	}
	void BeginArrayInline(const char* Key)
	{
		WriteKey(Key);
		m_Out += '[';
		m_bArrayFirst = true;
		m_bAtStart = false;
	}
	void EndArrayInline()
	{
		m_Out += ']';
		m_bArrayFirst = false;
		m_bAtStart = false;
	}

	void Uint(const char* Key, uint64_t V)
	{
		WriteKey(Key);
		Append("%llu", (unsigned long long)V);
	}
	void Int(const char* Key, int64_t V)
	{
		WriteKey(Key);
		Append("%lld", (long long)V);
	}
	void Bool(const char* Key, bool bV)
	{
		WriteKey(Key);
		Append("%s", bV ? "true" : "false");
	}
	void Double(const char* Key, double V)
	{
		WriteKey(Key);
		Append("%.6g", V);
	}
	void Null(const char* Key)
	{
		WriteKey(Key);
		m_Out += "null";
		m_bAtStart = false;
	}
	void Hex16(const char* Key, uint16_t V)
	{
		WriteKey(Key);
		Append("\"$%04X\"", static_cast<unsigned int>(V));
	}
	void Hex8(const char* Key, uint8_t V)
	{
		WriteKey(Key);
		Append("\"$%02X\"", static_cast<unsigned int>(V));
	}
	void Hex64(const char* Key, uint64_t V)
	{
		WriteKey(Key);
		Append("\"$%016llX\"", (unsigned long long)V);
	}
	void Str(const char* Key, const std::string& V)
	{
		WriteKey(Key);
		m_Out += '"';
		EscapeInto(V);
		m_Out += '"';
		m_bAtStart = false;
	}

	// Inline-array helpers - emit "key": [v, v, v]
	void ArrayHex8(const char* Key, const uint8_t* Vals, size_t N)
	{
		BeginArrayInline(Key);
		for (size_t IdxI = 0; IdxI < N; ++IdxI)
		{
			if (IdxI)
			{
				m_Out += ", ";
			}
			char Buf[16];
			snprintf(Buf, sizeof(Buf), "\"$%02X\"", static_cast<unsigned int>(Vals[IdxI]));
			m_Out += Buf;
		}
		EndArrayInline();
	}

	// Emit a number array, one element per line, in groups. Used for
	// per_frame_halt (potentially long; line-wrapped for readability).
	void ArrayUint32Wrapped(const char* Key, const uint32_t* Vals, size_t N, int PerLine)
	{
		WriteKey(Key);
		m_Out += '[';
		++m_Depth;
		for (size_t IdxI = 0; IdxI < N; ++IdxI)
		{
			if (IdxI)
			{
				m_Out += ',';
			}
			if (IdxI % size_t(PerLine) == 0)
			{
				m_Out += '\n';
				WriteIndent();
			}
			else
			{
				m_Out += ' ';
			}
			char Buf[16];
			snprintf(Buf, sizeof(Buf), "%u", static_cast<unsigned int>(Vals[IdxI]));
			m_Out += Buf;
		}
		--m_Depth;
		if (N > 0)
		{
			m_Out += '\n';
			WriteIndent();
		}
		m_Out += ']';
		m_bAtStart = false;
	}

  private:
	void WriteKey(const char* Key)
	{
		if (!m_bAtStart)
		{
			m_Out += ',';
		}
		m_Out += '\n';
		WriteIndent();
		if (Key)
		{
			m_Out += '"';
			m_Out += Key;
			m_Out += "\": ";
		}
		m_bAtStart = false;
	}
	void Append(const char* Fmt, ...)
	{
		char Buf[64];
		va_list Args;
		va_start(Args, Fmt);
		std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
		va_end(Args);
		m_Out += Buf;
		m_bAtStart = false;
	}
	void WriteIndent()
	{
		m_Out.append(size_t(m_Depth * 2), ' ');
	}
	void EscapeInto(const std::string& V)
	{
		for (char C : V)
		{
			switch (C)
			{
			case '"':
				m_Out += "\\\"";
				break;
			case '\\':
				m_Out += "\\\\";
				break;
			case '\n':
				m_Out += "\\n";
				break;
			case '\r':
				m_Out += "\\r";
				break;
			case '\t':
				m_Out += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(C) < 0x20)
				{
					char Buf[8];
					snprintf(Buf, sizeof(Buf), "\\u%04X", static_cast<unsigned int>(static_cast<unsigned char>(C)));
					m_Out += Buf;
				}
				else
				{
					m_Out += C;
				}
				break;
			}
		}
	}

	std::string& m_Out;
	int m_Depth = 0;
	bool m_bAtStart = true;
	bool m_bArrayFirst = false;
};

// ---------- helpers ----------

// Convert AbsPath to a forward-slash path relative to BaseDir if possible.
// Falls back to an absolute forward-slash path (e.g. for paths on a different
// drive on Windows, or when std::filesystem::relative fails). Empty input
// passes through. The forward-slash form parses identically on Windows and
// Linux and avoids JSON backslash-escaping noise.
std::string PathRelativeToDir(const std::string& AbsPath, const std::filesystem::path& BaseDir)
{
	namespace fs = std::filesystem;
	if (AbsPath.empty())
	{
		return AbsPath;
	}
	std::error_code Ec;
	fs::path P(AbsPath);
	if (P.is_relative())
	{
		// The user passed a relative path; keep it relative to the CWD-when-relative
		// sense by normalising directly. Don't pretend it's "absolute".
		return P.lexically_normal().generic_string();
	}
	fs::path Abs = fs::absolute(P, Ec);
	if (Ec)
	{
		return P.generic_string();
	}
	fs::path Rel = fs::relative(Abs, BaseDir, Ec);
	if (Ec || Rel.empty())
	{
		return Abs.generic_string();
	}
	return Rel.generic_string();
}

std::string IsoUtcNow()
{
	const std::time_t T = time(nullptr);
	std::tm Gm{};
#if defined(_WIN32)
	gmtime_s(&Gm, &T);
#else
	gmtime_r(&T, &Gm);
#endif
	char Buf[32];
	std::strftime(Buf, sizeof(Buf), "%Y-%m-%dT%H:%M:%SZ", &Gm);
	return Buf;
}

void WriteCpu(JsonWriter& InOutW, const Z80State& C)
{
	InOutW.BeginObject("cpu");
	InOutW.Hex16("AF", C.AF);
	InOutW.Hex16("BC", C.BC);
	InOutW.Hex16("DE", C.DE);
	InOutW.Hex16("HL", C.HL);
	InOutW.Hex16("AFp", C.AFp);
	InOutW.Hex16("BCp", C.BCp);
	InOutW.Hex16("DEp", C.DEp);
	InOutW.Hex16("HLp", C.HLp);
	InOutW.Hex16("IX", C.IX);
	InOutW.Hex16("IY", C.IY);
	InOutW.Hex16("SP", C.SP);
	InOutW.Hex16("PC", C.PC);
	InOutW.Hex16("MemPtr", C.m_MemPtr);
	InOutW.Uint("I", C.I);
	InOutW.Uint("R", C.R);
	InOutW.Uint("IFF1", C.IFF1);
	InOutW.Uint("IFF2", C.IFF2);
	InOutW.Uint("IM", C.IM);
	InOutW.Bool("halted", C.m_bHalted);
	InOutW.Bool("ei_pending", C.m_bEiPending);
	InOutW.Uint("halt_start_cycle", C.m_HaltStartCycle);
	InOutW.Uint("interrupts_accepted", C.m_InterruptsAccepted);
	// The /INT pulse length as in effect (it scales with turbo), and the
	// frames it let slip.
	InOutW.Uint("int_window", C.m_IntWindow);
	InOutW.Uint("missed_interrupts", C.m_MissedInterrupts);
	// NR $22/$23 as the CPU side holds them: the register file alone does not
	// re-arm the line interrupt.
	InOutW.BeginObject("line_int");
	InOutW.Bool("enabled", C.m_bLineIntEnabled);
	InOutW.Uint("line", C.m_LineIntLine);
	InOutW.Bool("ula_masked", C.m_bUlaIntMasked);
	if (C.m_NextLineIntCycle == UINT64_MAX)
	{
		InOutW.Null("next_cycle");
	}
	else
	{
		InOutW.Uint("next_cycle", C.m_NextLineIntCycle);
	}
	InOutW.Uint("accepted", C.m_LineIntsAccepted);
	InOutW.Uint("missed", C.m_LineIntsMissed);
	InOutW.EndObject();
	// Interrupt nesting, for RETI matching, F_READ gating and DMA preemption.
	InOutW.BeginObject("isr");
	InOutW.Uint("depth", C.m_IsrDepth);
	InOutW.Uint("dma_resume_pending", C.m_BDmaResumePendingOneInstr);
	{
		uint8_t Bytes[Z80State::kIsrNestingMax * 4];
		for (int I = 0; I < Z80State::kIsrNestingMax; ++I)
		{
			Bytes[I * 4 + 0] = uint8_t(C.m_IsrReturnSP[I] & 0xFF);
			Bytes[I * 4 + 1] = uint8_t(C.m_IsrReturnSP[I] >> 8);
			Bytes[I * 4 + 2] = uint8_t(C.m_IsrReturnPC[I] & 0xFF);
			Bytes[I * 4 + 3] = uint8_t(C.m_IsrReturnPC[I] >> 8);
		}
		// per level: return SP, then return PC, each little-endian
		InOutW.ArrayHex8("return_sp_pc", Bytes, sizeof Bytes);
	}
	InOutW.EndObject();
	InOutW.EndObject();
}

void WriteMemory(JsonWriter& InOutW, const MemoryBus& M, const std::string& MachineName)
{
	InOutW.BeginObject("memory");
	// "kind" - coarse: 48k vs 128k. Use machine name.
	InOutW.Str("kind", MachineName);
	InOutW.Uint("ram_mb", uint64_t(M.GetEffectiveRamPages() / 128));
	InOutW.Hex8("port_7ffd", M.GetPort7FFD());
	if (M.IsPlus3())
	{
		InOutW.Hex8("port_1ffd", M.GetPort1FFD());
	}
	InOutW.Bool("paging_locked", M.IsPagingLocked());
	InOutW.Uint("border_colour", M.GetBorderColour());
	InOutW.Bool("trdos_rom_active", M.IsTrdosRomActive());
	// Timex SCLD state (ports $FF video mode / $F4 DOCK-bank). Emitted only for
	// Timex machines; the load path re-arms EnableTimex + these values so a
	// resumed Timex run renders in the same SCLD mode.
	if (M.IsTimex())
	{
		InOutW.Bool("timex", true);
		InOutW.Bool("timex_2068", M.IsTimex2068());
		InOutW.Hex8("scld_mode", M.GetScldMode());
		InOutW.Hex8("scld_bank", M.GetScldBank());
	}
	// ZX Evolution BaseConf: $EFF7 config, $xx77 video mode, RTC address, and the
	// four page-window overrides (-1 = window follows the standard mapping). The
	// load path re-arms EnableZxEvo + these so a resumed run keeps its mode/paging.
	if (M.IsZxEvo())
	{
		InOutW.Bool("zxevo", true);
		InOutW.Hex8("eff7", M.GetEff7());
		InOutW.Hex8("evo_ff77", M.GetEvoFf77());
		InOutW.Hex8("cmos_addr", M.GetCmosAddr());
		InOutW.Int("evo_win0", M.GetEvoWinPage(0));
		InOutW.Int("evo_win1", M.GetEvoWinPage(1));
		InOutW.Int("evo_win2", M.GetEvoWinPage(2));
		InOutW.Int("evo_win3", M.GetEvoWinPage(3));
	}
	// ZX-Uno: the $FC3B register-select latch, the reg $0B turbo/SCANDBL value,
	// and the SCLD mode (OUT 255,x). Re-armed via EnableZxUno on load.
	if (M.IsZxUno())
	{
		InOutW.Bool("zxuno", true);
		InOutW.Hex8("uno_reg_sel", M.GetUnoRegSel());
		InOutW.Hex8("uno_scandbl", M.GetUnoScanDbl());
		InOutW.Hex8("scld_mode", M.GetScldMode());
		// Radastan + ULAplus (so a resumed Radastan run keeps its mode + palette).
		InOutW.Hex8("radas_ctrl", M.GetUnoRadasCtrl());
		InOutW.Uint("radas_offset", M.GetUnoRadasOffset());
		InOutW.Hex8("radas_padding", M.GetUnoRadasPadding());
		InOutW.Hex8("radas_palbank", M.GetUnoRadasPalBank());
		InOutW.Bool("ulaplus_mode", M.IsUlaPlusMode());
		InOutW.ArrayHex8("ulaplus_cram", M.GetUlaPlusCram(), 64);
	}
	// The base slot table: what each 8K slot maps, before the Layer 2 window.
	// "ram 12 w" = RAM page 12, writable; "rom 2", "dock 3 w", "open".
	InOutW.BeginObject("base_slots");
	for (int S = 0; S < 8; ++S)
	{
		const MemoryBus::SlotDesc D = M.DescribeBaseSlot(S);
		char Key[8];
		char Val[24];
		snprintf(Key, sizeof Key, "slot_%d", S);
		const char* Store = D.m_Store == MemoryBus::SlotStore::kRam	? "ram"
							: D.m_Store == MemoryBus::SlotStore::kRom	? "rom"
							: D.m_Store == MemoryBus::SlotStore::kDock ? "dock"
																		: "open";
		if (D.m_Store == MemoryBus::SlotStore::kOpenBus)
		{
			snprintf(Val, sizeof Val, "open");
		}
		else
		{
			snprintf(Val, sizeof Val, "%s %u%s", Store, unsigned(D.m_Page), D.m_bWritable ? " w" : "");
		}
		InOutW.Str(Key, Val);
	}
	InOutW.EndObject();
	InOutW.BeginObject("vram_writes");
	InOutW.Uint("bank5_pixel", M.GetVramPixelWritesBank5());
	InOutW.Uint("bank5_attr", M.GetVramAttrWritesBank5());
	InOutW.Uint("bank7_pixel", M.GetVramPixelWritesBank7());
	InOutW.Uint("bank7_attr", M.GetVramAttrWritesBank7());
	InOutW.EndObject();
	InOutW.Uint("port_7ffd_writes", M.GetPort7FFDWrites());
	InOutW.EndObject();
}

void WriteRam(JsonWriter& InOutW, const MemoryBus& M)
{
	constexpr int kBankSize = 16384;
	InOutW.BeginObject("ram");
	InOutW.Str("encoding", "hexdump");
	InOutW.Uint("bytes_per_line", 32);

	// Bank ordering: 5, 2, current-$C000 bank, then rest in numeric order
	// (matches the SNA save order; keeps the most-likely-interesting banks
	// near the top of the file).
	const int CurrentBank = M.CurrentRamBank16K();
	int Order[8] = {5, 2};
	bool bSeen[8] = {};
	bSeen[5] = true;
	bSeen[2] = true;
	int Pos = 2;
	if (!bSeen[CurrentBank])
	{
		Order[Pos++] = CurrentBank;
		bSeen[CurrentBank] = true;
	}
	for (int IdxBank = 0; IdxBank < 8; ++IdxBank)
	{
		if (!bSeen[IdxBank])
		{
			Order[Pos++] = IdxBank;
			bSeen[IdxBank] = true;
		}
	}

	for (int IdxI = 0; IdxI < 8; ++IdxI)
	{
		const int B = Order[IdxI];
		if (B < 0)
		{
			break;
		}
		const uint8_t* Data = M.Ram16KBank(B);
		const std::string Encoded = EncodeBankAsHexdump(Data, kBankSize, B, /*bAllowZeroElision=*/true);
		char Key[16];
		snprintf(Key, sizeof(Key), "bank_%d", B);
		InOutW.Str(Key, Encoded);
	}
	// Emit extra Next RAM banks if allocated through IDE_BANK or nonzero.
	// Direct MMU writes can populate a bank without involving the allocator.
	for (int B = 8; B < kRamPageCount / 2; ++B)
	{
		const int kP0 = B * 2;
		const int kP1 = B * 2 + 1;
		const bool bAllocated = M.IsPage8KAllocated(kP0) || M.IsPage8KAllocated(kP1);
		const uint8_t* Data = M.Ram16KBank(B);
		bool bHasData = bAllocated;
		if (!bHasData)
		{
			for (int IdxI = 0; IdxI < kBankSize; ++IdxI)
			{
				if (Data[IdxI] != 0)
				{
					bHasData = true;
					break;
				}
			}
		}
		if (!bHasData)
		{
			continue;
		}
		const std::string Encoded = EncodeBankAsHexdump(Data, kBankSize, B, /*bAllowZeroElision=*/true);
		char Key[16];
		snprintf(Key, sizeof(Key), "bank_%d", B);
		InOutW.Str(Key, Encoded);
	}
	InOutW.EndObject();
}

void WriteAy(JsonWriter& InOutW, const AyState& A, const char* Key = "ay")
{
	InOutW.BeginObject(Key);
	InOutW.ArrayHex8("registers", A.m_Registers, 16);
	InOutW.Uint("latch", A.m_Latch);
	InOutW.Uint("writes_observed", A.m_WritesObserved);
	InOutW.Uint("envelope_writes", A.m_EnvelopeWrites);
	InOutW.Hex16("registers_touched", A.m_RegistersTouched);
	InOutW.EndObject();
}

void WritePsg(JsonWriter& InOutW, const PsgCapture* Capture, const std::filesystem::path& DumpDir,
	const char* Key = "psg")
{
	if (!Capture)
	{
		InOutW.Null(Key);
		return;
	}
	InOutW.BeginObject(Key);
	InOutW.Bool("active", true);
	InOutW.Str("out_path", PathRelativeToDir(Capture->OutPath(), DumpDir));
	InOutW.Uint("rate", uint64_t(Capture->RateHz()));
	InOutW.Hex16("dirty_mask", Capture->GetDirtyMask());
	InOutW.ArrayHex8("frame_values", Capture->GetFrameValues(), 16);
	InOutW.Uint("silent_frames", Capture->SilentFramesPending());
	InOutW.Uint("frames_written", Capture->FramesWritten());
	InOutW.Uint("writes_captured", Capture->WritesCaptured());
	const std::vector<uint8_t>& Body = Capture->BodySoFar();
	InOutW.Str("body_b64", EncodeBase64(Body.data(), Body.size()));
	InOutW.EndObject();
}

void WriteTape(JsonWriter& InOutW, const TapePlayer* Tape)
{
	// Emitted only for --tap runs. Records the block cursor so a resume knows
	// how far the tape had been read; the .tap path itself lives in the external
	// refs. Null when no tape (resume detects "no tape state").
	if (Tape == nullptr)
	{
		InOutW.Null("tape");
		return;
	}
	InOutW.BeginObject("tape");
	InOutW.Uint("block_cursor", Tape->Cursor());
	InOutW.Uint("block_count", Tape->BlockCount());
	InOutW.Uint("blocks_delivered", Tape->BlocksDelivered());
	InOutW.Uint("bytes_delivered", Tape->BytesDelivered());
	InOutW.EndObject();
}

void WriteTrdos(JsonWriter& InOutW, const Trdos* T)
{
	if (!T || !T->IsSupportActive())
	{
		InOutW.Null("trdos");
		return;
	}
	InOutW.BeginObject("trdos");
	InOutW.Bool("support_active", true);
	InOutW.BeginObject("fdc");
	InOutW.Hex8("command", T->GetCommandReg());
	InOutW.Hex8("status", T->GetStatusReg());
	InOutW.Uint("track", T->GetTrackReg());
	InOutW.Uint("sector", T->GetSectorReg());
	InOutW.Hex8("data", T->GetDataReg());
	InOutW.Hex8("system", T->GetSystemReg());
	InOutW.Uint("head_track", T->GetHeadTrack());
	InOutW.Uint("side", T->GetSide());
	InOutW.Int("buffer_pos", T->GetBufferPos());
	InOutW.Int("buffer_len", T->GetBufferLen());
	InOutW.Str("buffer_b64", EncodeBase64(T->GetBuffer(), 256));
	InOutW.Bool("busy", T->IsBusy());
	InOutW.Bool("irq", T->HasIrq());
	InOutW.Bool("drq", T->HasDrq());
	InOutW.Str("last_class", T->GetLastClassName());
	InOutW.Uint("index_pulse_counter", T->GetIndexPulseCounter());
	InOutW.Uint("read_addr_sector", T->GetReadAddrSector());
	InOutW.EndObject();
	InOutW.EndObject();
}

void WriteInput(JsonWriter& InOutW, const InputState* In, int DefaultTapFrames, bool bInputLog)
{
	if (!In || !In->HasScript())
	{
		InOutW.Null("input");
		return;
	}
	InOutW.BeginObject("input");
	InOutW.Uint("default_tap_frames", uint64_t(DefaultTapFrames));
	InOutW.Bool("log", bInputLog);
	InOutW.Uint("next_event", uint64_t(In->GetNextEventIndex()));
	InOutW.Uint("event_count", uint64_t(In->GetEventCount()));
	InOutW.ArrayHex8("keyboard_rows", In->GetKeyboardRows(), 8);
	InOutW.Hex8("kempston_state", In->GetKempstonState());
	// The events themselves, auto-typed keys included, so a resume does not
	// depend on rebuilding the same list: "<frame> <p|r> <k<row><bit>|j<bit>>,... <name>"
	InOutW.BeginObject("events");
	const auto& Events = In->Events();
	for (size_t E = 0; E < Events.size(); ++E)
	{
		const InputEvent& Ev = Events[E];
		std::string V = std::to_string(Ev.m_Frame) + (Ev.m_bPress ? " p " : " r ");
		for (size_t T = 0; T < Ev.m_Targets.size(); ++T)
		{
			const KeyTarget& K = Ev.m_Targets[T];
			char Buf[8];
			snprintf(Buf, sizeof Buf, K.m_bKempston ? "%sj%u" : "%sk%u%u", T ? "," : "", K.m_bKempston ? K.m_Bit : K.m_Row,
				K.m_Bit);
			V += Buf;
		}
		if (Ev.m_Targets.empty())
		{
			V += "-";
		}
		V += " " + Ev.m_Name;
		char Key[16];
		snprintf(Key, sizeof Key, "e%zu", E);
		InOutW.Str(Key, V);
	}
	InOutW.EndObject();
	InOutW.EndObject();
}

void WriteScheduler(JsonWriter& InOutW, const Z80State& C)
{
	InOutW.BeginObject("scheduler");
	auto WriteCycle = [&](const char* K, uint64_t V)
	{
		if (V == UINT64_MAX)
		{
			InOutW.Null(K);
		}
		else
		{
			InOutW.Uint(K, V);
		}
	};
	WriteCycle("next_frame_int_cycle", C.m_NextFrameIntCycle);
	WriteCycle("next_psg_tick_cycle", C.m_NextPsgTickCycle);
	WriteCycle("next_stats_tick_cycle", C.m_NextStatsTickCycle);
	WriteCycle("next_event_cycle", C.m_NextEventCycle);
	InOutW.Uint("frame_int_period", C.m_FrameIntPeriod);
	InOutW.Uint("psg_tick_period", C.m_PsgTickPeriod);
	InOutW.Uint("stats_tick_period", C.m_StatsTickPeriod);
	InOutW.EndObject();
}

void WriteStats(JsonWriter& InOutW, const Z80State& C, uint64_t Budget)
{
	InOutW.BeginObject("stats");
	InOutW.Uint("cycles_total", C.m_Cycles);
	InOutW.Uint("budget", Budget);
	InOutW.Uint("halt_cycles_total", C.m_HaltCycles);
	if (C.m_LastDiCycles == UINT64_MAX)
	{
		InOutW.Null("last_di_cycles");
	}
	else
	{
		InOutW.Uint("last_di_cycles", C.m_LastDiCycles);
	}
	InOutW.Uint("last_stats_snapshot_halt", C.m_LastStatsSnapshotHalt);
	InOutW.ArrayUint32Wrapped("per_frame_halt", C.m_PerFrameHalt.data(), C.m_PerFrameHalt.size(), 16);
	InOutW.EndObject();
}

void WriteNext(JsonWriter& InOutW, const MemoryBus& M)
{
	if (!M.IsNext())
	{
		InOutW.Null("next");
		return;
	}
	InOutW.BeginObject("next");
	InOutW.Bool("is_next", true);
	InOutW.Hex8("last_selected_nextreg", M.GetLastSelectedNextReg());
	InOutW.Uint("nextreg_writes", M.GetNextRegWrites());
	InOutW.Uint("palette_writes", M.GetPaletteWrites());
	InOutW.Uint("palette_write_parity", M.GetPaletteWriteParity());
	InOutW.Bool("any_l2_touched", M.GetAnyL2Touched());
	InOutW.Bool("l2_display_enabled", M.IsL2DisplayEnabled());
	{
		const char* Name = "slu";
		switch (M.GetLayerPriority())
		{
		case MemoryBus::LayerPriority::kLayerPrioritySlu:
			Name = "slu";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityLsu:
			Name = "lsu";
			break;
		case MemoryBus::LayerPriority::kLayerPrioritySul:
			Name = "sul";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityLus:
			Name = "lus";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityUsl:
			Name = "usl";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityUls:
			Name = "uls";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityUlaBlend:
			Name = "ula_blend";
			break;
		case MemoryBus::LayerPriority::kLayerPriorityUlaHalfBlend:
			Name = "ula_half_blend";
			break;
		}
		InOutW.Str("layer_priority", Name);
	}

	// 256-bit allocation mask: highest word first, 64 hex chars total.
	// Backwards compat: older 32-hex-char dumps (= 128 bits) are still
	// accepted by the load path; the high half is treated as zero there.
	{
		char Buf[72];
		snprintf(Buf, sizeof(Buf), "$%016llX%016llX%016llX%016llX", (unsigned long long)M.GetAllocatedPages8KWord(3),
			(unsigned long long)M.GetAllocatedPages8KWord(2), (unsigned long long)M.GetAllocatedPages8KWord(1),
			(unsigned long long)M.GetAllocatedPages8KWord(0));
		InOutW.Str("allocated_pages_8k_mask", Buf);
	}

	// NextRegs array, always: an elided one would restore as reset values.
	const uint8_t* kNR = M.GetNextRegsArray();
	InOutW.Str("nextregs_b64", EncodeBase64(kNR, 256));

	// Palettes: emit only the ones that have any non-zero data. Each palette
	// is 256 entries x 2 bytes = 512 bytes. Index meaning:
	//   0=ULA-1   1=L2-1    2=Spr-1  3=Tile-1
	//   4=ULA-2   5=L2-2    6=Spr-2  7=Tile-2 (NR43 bits 6:4)
	InOutW.BeginObject("palettes");
	for (int P = 0; P < 8; ++P)
	{
		const uint8_t(*Pal)[2] = M.GetPalette(P);
		const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Pal);
		// every palette, all-zero ones too: a missing one restores as reset
		char Key[8];
		snprintf(Key, sizeof(Key), "pal_%d", P);
		InOutW.Str(Key, EncodeBase64(Bytes, 512));
	}
	InOutW.EndObject();
	InOutW.EndObject();
}

void WriteNex(JsonWriter& InOutW, const DumpNexInfo& N, const std::filesystem::path& DumpDir)
{
	if (N.m_NexPath.empty())
	{
		InOutW.Null("nex");
		return;
	}
	InOutW.BeginObject("nex");
	InOutW.Str("path", PathRelativeToDir(N.m_NexPath, DumpDir));
	InOutW.Str("version", N.m_Version);
	InOutW.Uint("num_banks", N.m_NumBanks);
	InOutW.Hex8("screen_flags", N.m_ScreenFlags);
	InOutW.Uint("border", N.m_Border);
	InOutW.Uint("entry_bank", N.m_EntryBank);
	InOutW.Hex16("initial_sp", N.m_InitialSP);
	InOutW.Hex16("initial_pc", N.m_InitialPC);
	{
		char Buf[16];
		snprintf(Buf, sizeof(Buf), "%u.%u.%u", N.m_CoreVersion[0], N.m_CoreVersion[1], N.m_CoreVersion[2]);
		InOutW.Str("core_version", Buf);
	}
	InOutW.Bool("has_copper_code", N.m_bHasCopperCode);
	InOutW.Bool("has_loading_screen", N.m_bHasLoadingScreen);
	InOutW.Bool("loading_screen_applied", N.m_bLoadingScreenApplied);
	InOutW.Uint("banks_offset", N.m_BanksOffset);
	// banks_present as a space-joined string ("2 14 15") - keeps the dump
	// grep-friendly without needing a new JsonWriter inline-integer-array
	// helper. The expect_dump_field_eq runner accepts the substring.
	{
		std::string Joined;
		for (size_t IdxI = 0; IdxI < N.m_BanksPresent.size(); ++IdxI)
		{
			if (IdxI)
			{
				Joined += ", ";
			}
			char Buf[8];
			snprintf(Buf, sizeof(Buf), "%d", N.m_BanksPresent[IdxI]);
			Joined += Buf;
		}
		InOutW.Str("banks_present", Joined);
	}
	const std::string& Sha = N.m_NexSha1;
	InOutW.Str("identity_override", N.m_IdentityOverride);
	if (Sha.empty())
	{
		InOutW.Null("sha1");
	}
	else
	{
		InOutW.Str("sha1", Sha);
	}
	InOutW.EndObject();
}

void WriteCtc(JsonWriter& InOutW, const MemoryBus& M)
{
	bool bAny = false;
	for (int N = 0; N < 8; ++N)
	{
		const MemoryBus::CtcChannel& C = M.Ctc(N);
		if (C.m_bTcLoaded || C.m_InterruptsFired != 0 || C.m_bAwaitingTC || C.m_ControlWord != 0)
		{
			bAny = true;
		}
	}
	if (!bAny)
	{
		InOutW.Null("ctc");
		return;
	}
	InOutW.BeginObject("ctc");
	for (int N = 0; N < 8; ++N)
	{
		const MemoryBus::CtcChannel& C = M.Ctc(N);
		if (!C.m_bTcLoaded && C.m_InterruptsFired == 0 && !C.m_bAwaitingTC && C.m_ControlWord == 0)
		{
			continue; // untouched channel - keep the dump compact
		}
		char Key[16];
		snprintf(Key, sizeof Key, "channel_%d", N);
		InOutW.BeginObject(Key);
		InOutW.Bool("enabled", C.m_bEnabled);
		InOutW.Bool("running", C.m_bRunning);
		InOutW.Bool("counter_mode", C.m_bCounterMode);
		InOutW.Bool("awaiting_tc", C.m_bAwaitingTC);
		InOutW.Bool("tc_loaded", C.m_bTcLoaded);
		InOutW.Hex8("control_word", C.m_ControlWord);
		InOutW.Uint("time_constant", C.m_TimeConstant);
		InOutW.Uint("period_cycles", C.m_PeriodCycles);
		InOutW.Uint("grid_anchor", C.m_GridAnchor);
		if (C.m_NextFireCycle == UINT64_MAX)
		{
			InOutW.Null("next_fire_cycle");
		}
		else
		{
			InOutW.Uint("next_fire_cycle", C.m_NextFireCycle);
		}
		InOutW.Uint("interrupts_fired", C.m_InterruptsFired);
		InOutW.EndObject();
	}
	InOutW.EndObject();
}

void WriteAudio(JsonWriter& InOutW, const AudioRenderer* A, const std::filesystem::path& DumpDir)
{
	if (!A)
	{
		InOutW.Null("audio");
		return;
	}
	InOutW.BeginObject("audio");
	InOutW.Str("out_path", PathRelativeToDir(A->OutPath(), DumpDir));
	InOutW.Uint("rate_hz", A->RateHz());
	InOutW.Str("stereo", A->Stereo() == AudioRenderer::kStereoMono	? "mono"
						 : A->Stereo() == AudioRenderer::kStereoAcb ? "acb"
																	: "abc");
	InOutW.Uint("ay0_writes", A->AyWritesLogged(0));
	InOutW.Uint("ay1_writes", A->AyWritesLogged(1));
	InOutW.Uint("dac_writes", A->DacWritesLogged());
	InOutW.Uint("dac_derived_rate_hz", A->DacDerivedRateHz());
	InOutW.Uint("beeper_writes", A->BeeperEdgesLogged());
	InOutW.Uint("samples_written", A->SamplesWritten());
	if (A->FirstDacNanos() == UINT64_MAX)
	{
		InOutW.Null("first_dac_nanos");
	}
	else
	{
		InOutW.Uint("first_dac_nanos", A->FirstDacNanos());
	}
	InOutW.Uint("last_dac_nanos", A->LastDacNanos());
	// the log the renderer synthesises from: a resumed run continues it
	const std::vector<uint8_t> Packed = A->PackEvents();
	InOutW.Str("events_b64", EncodeBase64(Packed.data(), Packed.size()));
	InOutW.EndObject();
}

void WriteSprites(JsonWriter& InOutW, const SpritesState* S)
{
	if (!S || (S->m_AttrWrites == 0 && S->m_PatternWrites == 0 && S->m_SlotSelects == 0))
	{
		InOutW.Null("sprites");
		return;
	}
	InOutW.BeginObject("sprites");
	InOutW.Uint("slot_selects", S->m_SlotSelects);
	InOutW.Uint("attr_writes", S->m_AttrWrites);
	InOutW.Uint("pattern_writes", S->m_PatternWrites);
	InOutW.Uint("attr_slot", S->m_AttrSlot);
	InOutW.Uint("attr_byte_idx", S->m_AttrByteIdx);
	InOutW.Uint("pattern_slot", S->m_PatternSlot);
	InOutW.Uint("pattern_byte_idx", S->m_PatternByteIdx);

	// Count visible sprites (attr3 bit 7 = V).
	int VisibleCount = 0;
	for (int IdxI = 0; IdxI < 128; ++IdxI)
	{
		if (S->m_Slots[IdxI].m_Attr.m_Bytes[3] & 0x80)
		{
			++VisibleCount;
		}
	}
	InOutW.Int("visible_count", VisibleCount);

	// The attribute records (128 x 5) and the pattern memory (64 x 256) as
	// base64, so a resumed run draws what the dumped one drew. Without them
	// every visible sprite came back as an opaque black square: pattern
	// bytes of 0 are colour 0, not the transparent $E3.
	uint8_t Attrs[128 * 5];
	for (int IdxI = 0; IdxI < 128; ++IdxI)
	{
		memcpy(Attrs + IdxI * 5, S->m_Slots[IdxI].m_Attr.m_Bytes, 5);
	}
	InOutW.Str("attrs_b64", EncodeBase64(Attrs, sizeof(Attrs)));
	std::vector<uint8_t> Patterns(64 * 256);
	for (int IdxI = 0; IdxI < 64; ++IdxI)
	{
		memcpy(Patterns.data() + IdxI * 256, S->m_Slots[IdxI].m_Pattern, 256);
	}
	InOutW.Str("patterns_b64", EncodeBase64(Patterns.data(), Patterns.size()));
	InOutW.EndObject();
}

void WriteDma(JsonWriter& InOutW, const ZxnDmaState* D)
{
	if (!D)
	{
		InOutW.Null("dma");
		return;
	}
	// All of it: the program a later LOAD/ENABLE re-runs, the half-written
	// register sequence, and a transfer still in flight.
	InOutW.BeginObject("dma");
	InOutW.Uint("mode", uint8_t(D->m_Mode));
	InOutW.Uint("wr0_pending", D->m_Wr0PendingBytes);
	InOutW.ArrayHex8("wr0_buffer", D->m_Wr0Buffer, 4);
	InOutW.Uint("wr4_pending", D->m_Wr4PendingBytes);
	InOutW.ArrayHex8("wr4_buffer", D->m_Wr4Buffer, 2);
	InOutW.Hex8("read_mask", D->m_ReadMask);
	InOutW.Hex16("port_a", D->m_PortASrcAddr);
	InOutW.Hex16("port_b", D->m_PortBDestAddr);
	InOutW.Uint("block_length", D->m_BlockLength);
	InOutW.Bool("port_a_fixed", D->m_bPortAFixed);
	InOutW.Bool("port_a_dec", D->m_bPortADec);
	InOutW.Bool("port_b_inc", D->m_bPortBInc);
	InOutW.Bool("port_b_dec", D->m_bPortBDec);
	InOutW.Bool("port_b_io", D->m_bPortBIo);
	InOutW.Bool("continuous", D->m_bContinuousMode);
	InOutW.Bool("stop_at_end", D->m_bStopAtEnd);
	InOutW.Bool("loaded", D->m_bLoaded);
	InOutW.Bool("force_ready", D->m_bForceReady);
	InOutW.Uint("bytes_remaining", D->m_BytesRemaining);
	InOutW.Hex16("cur_src", D->m_CurSrcAddr);
	InOutW.Hex16("cur_dst", D->m_CurDstAddr);
	InOutW.Uint("transfers", D->m_Transfers);
	InOutW.Uint("bytes_transferred", D->m_BytesTransferred);
	InOutW.Uint("unknown_bytes", D->m_UnknownBytes);
	InOutW.Uint("last_transfer_cycle", D->m_LastTransferCycle);
	InOutW.Uint("nested_arms", D->m_NestedArms);
	InOutW.EndObject();
}

// Bus state with no register to restore it through (MemoryBus::ResumeExtras).
void WriteBusExtras(JsonWriter& InOutW, const MemoryBus& M, const std::vector<uint8_t>* CoverageMap, int32_t ResetPc)
{
	MemoryBus::ResumeExtras X;
	M.ExportResumeExtras(X);
	InOutW.BeginObject("bus");
	InOutW.Hex8("port_123b", X.m_Port123B);
	InOutW.Uint("layer2_offset", X.m_Layer2Offset);
	InOutW.Uint("layer2_window_opens", X.m_Layer2WindowOpens);
	InOutW.Bool("l2_state_explicit", X.m_bL2StateExplicit);
	InOutW.ArrayHex8("clip_windows", &X.m_ClipWindow[0][0], 16);
	InOutW.ArrayHex8("clip_index", X.m_ClipIndex, 4);
	{
		uint8_t Set[4];
		for (int I = 0; I < 4; ++I)
		{
			Set[I] = X.m_bClipSet[I] ? 1 : 0;
		}
		InOutW.ArrayHex8("clip_set", Set, 4);
	}
	InOutW.Hex8("scld_mode", X.m_ScldMode);
	InOutW.Uint("next_cpu_speed", X.m_NextCpuSpeed);
	InOutW.Uint("frame_int_phase", X.m_FrameIntPhase);
	InOutW.ArrayHex8("evo_pal", X.m_EvoPal, 16);
	InOutW.Uint("evo_pal_idx", X.m_EvoPalIdx);
	InOutW.Str("cmos_b64", EncodeBase64(X.m_Cmos, sizeof X.m_Cmos));
	InOutW.Uint("uno_core_id_idx", X.m_UnoCoreIdIdx);
	InOutW.Uint("uno_radas_off_phase", X.m_UnoRadasOffPhase);
	InOutW.Hex8("ulaplus_reg", X.m_UlaPlusReg);
	if (X.m_DockRam.empty())
	{
		InOutW.Null("dock_ram_b64");
	}
	else
	{
		InOutW.Str("dock_ram_b64", EncodeBase64(X.m_DockRam.data(), X.m_DockRam.size()));
	}
	InOutW.ArrayHex8("dock_kind", X.m_DockKind.data(), X.m_DockKind.size());
	auto Hist = [&](const char* Key, const uint64_t* H)
	{
		InOutW.BeginObject(Key);
		for (int R = 0; R < 256; ++R)
		{
			if (H[R])
			{
				char K[4];
				snprintf(K, sizeof K, "%02X", R);
				InOutW.Uint(K, H[R]);
			}
		}
		InOutW.EndObject();
	};
	Hist("nextreg_write_hist", X.m_NextRegWriteHist);
	Hist("nextreg_read_hist", X.m_NextRegReadHist);
	auto Ports = [&](const char* Key, const std::map<uint16_t, uint64_t>& P)
	{
		InOutW.BeginObject(Key);
		for (const auto& [Port, N] : P)
		{
			char K[8];
			snprintf(K, sizeof K, "%04X", Port);
			InOutW.Uint(K, N);
		}
		InOutW.EndObject();
	};
	Ports("unhandled_port_reads", X.m_UnhandledPortReads);
	Ports("unhandled_port_writes", X.m_UnhandledPortWrites);
	InOutW.BeginObject("last_mmu_write");
	for (int S = 0; S < 8; ++S)
	{
		char K[8];
		char V[48];
		snprintf(K, sizeof K, "slot_%d", S);
		snprintf(V, sizeof V, "%llu %04X %02X", (unsigned long long)X.m_MmuWriteCycle[S], X.m_MmuWritePc[S],
			X.m_MmuWriteOld[S]);
		InOutW.Str(K, V);
	}
	InOutW.EndObject();
	// --coverage-out: the map so far and whether its one-shot reset has fired
	if (CoverageMap != nullptr)
	{
		InOutW.BeginObject("coverage");
		InOutW.Int("reset_pc", ResetPc);
		InOutW.Str("map_b64", EncodeBase64(CoverageMap->data(), CoverageMap->size()));
		InOutW.EndObject();
	}
	else
	{
		InOutW.Null("coverage");
	}
	InOutW.EndObject();
}

void WriteCopperDump(JsonWriter& InOutW, const MemoryBus* Mem)
{
	// Null only for a copper nobody has touched. Anything programmed or run is
	// written whole - instruction memory, programming cursor and run state - so
	// --resume carries on with the same list: with only the counters, a resumed
	// run had an empty, stopped copper and every copper effect vanished (a
	// copper-split tilemap scroll froze).
	bool bTouched = false;
	if (Mem && Mem->Copper().m_bPresent)
	{
		const CopperState& C = Mem->Copper();
		bTouched = C.m_Moves != 0 || C.m_Waits != 0 || C.m_bRunning || C.m_WriteAddr != 0 || C.m_bData16HaveHi;
		for (int I = 0; I < 2048 && !bTouched; ++I)
		{
			bTouched = C.m_Mem2K[I] != 0;
		}
	}
	if (!bTouched)
	{
		InOutW.Null("copper");
		return;
	}
	const CopperState& C = Mem->Copper();
	auto Deadline = [&](const char* Key, uint64_t V)
	{
		if (V == UINT64_MAX)
		{
			InOutW.Null(Key);
		}
		else
		{
			InOutW.Uint(Key, V);
		}
	};
	InOutW.BeginObject("copper");
	InOutW.Bool("running", C.m_bRunning);
	InOutW.Uint("mode", C.m_Mode);
	InOutW.Bool("restart_each_frame", C.m_bRestartEachFrame);
	InOutW.Uint("cpc", C.m_Cpc);
	InOutW.Uint("write_addr", C.m_WriteAddr);
	InOutW.Hex8("data16_hi", C.m_Data16Hi);
	InOutW.Bool("data16_have_hi", C.m_bData16HaveHi);
	Deadline("next_fire_cycle", C.m_NextFireCycle);
	Deadline("frame_origin", C.m_FrameOrigin);
	InOutW.Uint("moves", C.m_Moves);
	InOutW.Uint("waits", C.m_Waits);
	InOutW.Uint("restarts", C.m_Restarts);
	InOutW.Str("mem_b64", EncodeBase64(C.m_Mem2K, sizeof(C.m_Mem2K)));
	InOutW.EndObject();
}

void WriteDotCmd(JsonWriter& InOutW, const DumpDotCmdInfo& D, const std::filesystem::path& DumpDir)
{
	if (D.m_DotCmdPath.empty())
	{
		InOutW.Null("dot_cmd");
		return;
	}
	InOutW.BeginObject("dot_cmd");
	InOutW.Str("path", PathRelativeToDir(D.m_DotCmdPath, DumpDir));
	InOutW.Str("arg", D.m_ArgString);
	InOutW.Hex16("load_addr", D.m_LoadAddr);
	InOutW.Uint("binary_size", D.m_BinarySize);
	InOutW.Int("code_8k_page", D.m_Code8KPage);
	const std::string Sha = Sha1FileHex(D.m_DotCmdPath.c_str());
	if (Sha.empty())
	{
		InOutW.Null("sha1");
	}
	else
	{
		InOutW.Str("sha1", Sha);
	}
	InOutW.EndObject();
}

void WriteVfs(JsonWriter& InOutW, const NextZxosVfs* V, const std::filesystem::path& DumpDir)
{
	if (!V || !V->IsMounted())
	{
		InOutW.Null("vfs");
		return;
	}
	InOutW.BeginObject("vfs");
	InOutW.Str("root_path", PathRelativeToDir(V->Root(), DumpDir));
	InOutW.Str("drive", std::string(1, V->DriveLetter()));
	InOutW.BeginObject("stats");
	InOutW.Uint("files_opened", V->FilesOpened());
	InOutW.Uint("bytes_read", V->BytesRead());
	InOutW.Uint("bytes_written", V->BytesWritten());
	InOutW.Uint("ide_bank_allocs", V->IdeBankAllocs());
	InOutW.Uint("ide_bank_frees", V->IdeBankFrees());
	InOutW.Uint("dir_opens", V->DirOpens());
	InOutW.Uint("rst08_calls", V->Rst08Calls());
	InOutW.Uint("rst08_unknown", V->Rst08Unknown());
	InOutW.Uint("read_cycles_charged", V->ReadCyclesCharged());
	NextZxosVfs::ResumeState R;
	V->ExportResumeState(R);
	auto ByCode = [&](const char* Key, const std::map<uint8_t, uint64_t>& M)
	{
		InOutW.BeginObject(Key);
		for (const auto& [Code, N] : M)
		{
			char K[4];
			snprintf(K, sizeof K, "%02X", Code);
			InOutW.Uint(K, N);
		}
		InOutW.EndObject();
	};
	ByCode("rst08_by_code", R.m_Rst08ByCode);
	ByCode("rst08_unknown_by_code", R.m_Rst08UnknownByCode);
	InOutW.EndObject();
	// What a resume re-establishes (NextZxosVfs::ResumeState).
	InOutW.Str("cwd", R.m_Cwd);
	InOutW.Hex8("default_drive_id", R.m_DefaultDriveId);
	InOutW.Bool("read_only", R.m_bReadOnly);
	InOutW.Bool("use_ix_buffers", R.m_bUseIxForBuffers);
	InOutW.BeginObject("open_handles");
	char Key[8];
	for (const auto& H : R.m_Handles)
	{
		snprintf(Key, sizeof(Key), "h%d", H.m_Slot);
		InOutW.BeginObject(Key);
		InOutW.Str("opened_as", H.m_OpenedAs);
		InOutW.Str("resolved_path", PathRelativeToDir(H.m_ResolvedPath, DumpDir));
		InOutW.Bool("dir", H.m_bDir);
		InOutW.Int("position", int64_t(H.m_Position));
		InOutW.Bool("writable", H.m_bWritable);
		if (H.m_bDir)
		{
			InOutW.Uint("dir_pos", H.m_DirPos);
			InOutW.BeginObject("listing");
			for (size_t E = 0; E < H.m_Dir.size(); ++E)
			{
				char EKey[16];
				snprintf(EKey, sizeof EKey, "e%zu", E);
				InOutW.BeginObject(EKey);
				InOutW.Str("name", H.m_Dir[E].m_Name);
				InOutW.Bool("dir", H.m_Dir[E].m_bDir);
				InOutW.Bool("read_only", H.m_Dir[E].m_bReadOnly);
				InOutW.Uint("size", H.m_Dir[E].m_Size);
				InOutW.EndObject();
			}
			InOutW.EndObject();
		}
		InOutW.EndObject();
	}
	InOutW.EndObject();
	InOutW.BeginObject("f_read");
	InOutW.Bool("active", R.m_bFReadActive);
	InOutW.Uint("total", R.m_FReadTotal);
	InOutW.Uint("done", R.m_FReadDone);
	InOutW.Hex16("dest", R.m_FReadDest);
	InOutW.Bool("use_ix", R.m_bFReadUseIx);
	InOutW.Uint("isr_depth", R.m_FReadIsrDepth);
	InOutW.Uint("total_charge", R.m_FReadTotalCharge);
	InOutW.Uint("charged_so_far", R.m_FReadChargedSoFar);
	InOutW.Str("buf_b64", EncodeBase64(R.m_FReadBuf.data(), R.m_FReadBuf.size()));
	InOutW.EndObject();
	InOutW.EndObject();
}

void WriteExternalRefs(JsonWriter& InOutW, const DumpExternalRefs& E, const std::filesystem::path& DumpDir)
{
	auto OneRef = [&](const char* Key, const std::string& Path)
	{
		InOutW.BeginObject(Key);
		if (Path.empty())
		{
			InOutW.Null("path");
			InOutW.Null("sha1");
		}
		else
		{
			InOutW.Str("path", PathRelativeToDir(Path, DumpDir));
			// SHA1 of the file's full contents. Used on resume to detect
			// file changes ("did someone rebuild the ROM since the dump?").
			const std::string Sha = Sha1FileHex(Path.c_str());
			if (Sha.empty())
			{
				InOutW.Null("sha1");
			}
			else
			{
				InOutW.Str("sha1", Sha);
			}
		}
		InOutW.EndObject();
	};
	InOutW.BeginObject("external_refs");
	OneRef("rom48", E.m_Rom48Path);
	OneRef("rom128", E.m_Rom128Path);
	OneRef("rom_trdos", E.m_RomTrdPath);
	OneRef("tap", E.m_TapPath);
	OneRef("trd", E.m_TrdDiskPath);
	OneRef("input_script", E.m_InputScriptPath);
	OneRef("rom_plus3", E.m_RomPlus3Path);
	OneRef("dck", E.m_DckPath);
	InOutW.EndObject();
}

} // namespace

bool WriteDumpJson(const DumpReport& R, const char* Path, std::string& OutError)
{
	if (!R.m_Cpu || !R.m_Mem || !R.m_Ay)
	{
		OutError = "WriteDumpJson: required component pointer missing (Cpu/Mem/Ay)";
		return false;
	}

	std::string Out;
	Out.reserve(512 * 1024);
	JsonWriter W(Out);

	W.BeginObject();
	W.Uint("dump_version", 1);
	W.Str("tool_version", INSPECCY_VERSION);
	W.Str("created_at_utc", IsoUtcNow());
	W.Str("machine", R.m_MachineName);
	W.Double("mhz", R.m_Mhz);
	W.Double("nominal_mhz", R.m_NominalMhz);
	W.Uint("contention_x", R.m_ContentionX);
	W.Uint("frame_int_period", R.m_FrameIntPeriod);
	// The emulated wall clock (audio event time) as its own checkpoint: wall
	// nanos at a cycle, advancing at the clock's rate from there. A resume
	// restores the checkpoint itself - re-anchoring at "now" would round the
	// sub-nanosecond remainder away.
	W.BeginObject("wall_clock");
	W.Uint("checkpoint_nanos", R.m_WallCheckpointNanos);
	W.Uint("checkpoint_cycle", R.m_WallCheckpointCycle);
	W.Uint("now_nanos", R.m_WallNanos);
	W.EndObject();
	{
		const DumpRunOptions& O = R.m_Options;
		W.BeginObject("run_options");
		W.Bool("reset_trap", O.m_bResetTrap);
		W.Bool("lenient_undoc_ed", O.m_bLenientUndocEd);
		W.Bool("no_copper", O.m_bNoCopper);
		W.Bool("pixel_accurate", O.m_bPixelAccurate);
		W.Bool("ay_enabled", O.m_bAyEnabled);
		W.Bool("turbosound", O.m_bTurboSound);
		W.Bool("vfs_readonly", O.m_bVfsReadOnly);
		W.Int("int_window_mode", O.m_IntWindowMode);
		W.Uint("tap_type_frame", O.m_TapTypeFrame);
		W.Bool("tap_type_frame_set", O.m_bTapTypeFrameSet);
		W.EndObject();
	}

	// Dump-relative path anchor - used so external_refs and psg.out_path are
	// emitted as relative paths when reachable from the dump's directory.
	// Both cross-platform (forward slashes) and avoid leaking absolute paths.
	namespace fs = std::filesystem;
	std::error_code Ec;
	fs::path DumpDir = fs::absolute(fs::path(Path), Ec).parent_path();
	if (Ec)
	{
		DumpDir = fs::current_path();
	}
	WriteCpu(W, *R.m_Cpu);
	WriteMemory(W, *R.m_Mem, R.m_MachineName);
	WriteRam(W, *R.m_Mem);
	WriteAy(W, *R.m_Ay);
	WritePsg(W, R.m_Capture, DumpDir);
	// TurboSound: the second chip, its capture, and which chip the next
	// register write goes to
	if (R.m_Ay2)
	{
		WriteAy(W, *R.m_Ay2, "ay2");
		WritePsg(W, R.m_Capture2, DumpDir, "psg2");
	}
	W.Uint("ay_active", R.m_AyActive);
	WriteTape(W, R.m_Tape);
	WriteTrdos(W, R.m_Trd);
	// Input section needs a couple of run-config bits (DefaultTapFrames, bInputLog);
	// those don't live on InputState, so we use defaults here. Main.cpp will pass
	// them through when DUMP2 needs to round-trip.
	WriteInput(W, R.m_Input, R.m_Options.m_InputTapFrames, R.m_Options.m_bInputLog);
	WriteScheduler(W, *R.m_Cpu);
	{
		// Budget isn't on Z80State; reconstruct from the run by approximating as
		// (max(Cycles, NextEventCycle if not max)). For DUMP1 we accept that the
		// budget isn't part of the dump (resume picks a new budget); emit 0.
		WriteStats(W, *R.m_Cpu, /*Budget=*/0);
	}
	WriteNext(W, *R.m_Mem);
	WriteCtc(W, *R.m_Mem);
	WriteDma(W, R.m_Dma);
	WriteCopperDump(W, R.m_Mem);
	WriteBusExtras(W, *R.m_Mem, R.m_CoverageMap, R.m_CoverageResetPc);
	{
		const DumpCaptureState& C = R.m_Recording;
		W.BeginObject("capture");
		if (C.m_bVideo)
		{
			W.BeginObject("video");
			W.Uint("frames", C.m_VideoFrames);
			W.Uint("next_emit_cycle", C.m_VideoNextEmit);
			W.Uint("period_cycles", C.m_VideoPeriod);
			W.EndObject();
		}
		else
		{
			W.Null("video");
		}
		if (C.m_bMetrics)
		{
			W.BeginObject("metrics");
			W.Uint("frames", C.m_MetricsFrames);
			W.Uint("next_emit_cycle", C.m_MetricsNextEmit);
			W.Uint("period_cycles", C.m_MetricsPeriod);
			W.BeginObject("history");
			for (size_t I = 0; I < C.m_MetricsHistory.size(); ++I)
			{
				char Key[16];
				snprintf(Key, sizeof Key, "f%zu", I);
				W.Str(Key, EncodeBase64(C.m_MetricsHistory[I].data(), C.m_MetricsHistory[I].size()));
			}
			W.EndObject();
			W.EndObject();
		}
		else
		{
			W.Null("metrics");
		}
		W.EndObject();
	}
	WriteAudio(W, R.m_Audio, DumpDir);
	WriteSprites(W, R.m_Sprites);
	WriteDotCmd(W, R.m_DotCmd, DumpDir);
	WriteNex(W, R.m_Nex, DumpDir);
	WriteVfs(W, R.m_DotVfs, DumpDir);
	WriteExternalRefs(W, R.m_ExternalRefs, DumpDir);

	W.EndObject();
	Out += '\n';

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open dump output: ") + Path;
		return false;
	}
	const size_t Wrote = fwrite(Out.data(), 1, Out.size(), F);
	const bool bClosed = fclose(F) == 0;
	if (Wrote != Out.size() || !bClosed)
	{
		OutError = std::string("short write to dump output: ") + Path;
		return false;
	}
	return true;
}

// =====================================================================
// DUMP2 (resume side): JSON -> component state
// =====================================================================

namespace
{

// ---- field extraction helpers ----

bool ExpectObj(const JsonValue& Root, const char* Group, const JsonValue*& Out, std::string& OutError)
{
	const JsonValue* V = Root.Find(Group);
	if (!V || !V->IsObject())
	{
		OutError = std::string("missing or non-object: ") + Group;
		return false;
	}
	Out = V;
	return true;
}

// Parse up to 16 hex digits, with an optional dollar prefix.
bool ParseHex64Quoted(const std::string& S, uint64_t& Out)
{
	if (S.empty())
	{
		Out = 0;
		return true;
	}
	size_t IdxI = 0;
	if (S[0] == '$')
	{
		IdxI = 1;
	}
	Out = 0;
	if (IdxI >= S.size() || S.size() - IdxI > 16)
	{
		return false;
	}
	for (; IdxI < S.size(); ++IdxI)
	{
		const char C = S[IdxI];
		uint64_t Nib;
		if (C >= '0' && C <= '9')
		{
			Nib = uint64_t(C - '0');
		}
		else if (C >= 'a' && C <= 'f')
		{
			Nib = uint64_t(C - 'a' + 10);
		}
		else if (C >= 'A' && C <= 'F')
		{
			Nib = uint64_t(C - 'A' + 10);
		}
		else
		{
			return false;
		}
		Out = (Out << 4) | Nib;
	}
	return true;
}

// Accept either a JSON number OR a "$XXXX"-style hex string. This way a human
// can hand-edit either form.
bool ReadNumLike(const JsonValue& Obj, const char* Key, uint64_t& Out, std::string& OutError, uint64_t Max = UINT64_MAX)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V)
	{
		OutError = std::string("missing field: ") + Key;
		return false;
	}
	if (V->IsString())
	{
		uint64_t Tmp;
		if (V->m_S.empty() || V->m_S[0] != '$' || !ParseHex64Quoted(V->m_S, Tmp) || Tmp > Max)
		{
			OutError = std::string("bad hex value for: ") + Key;
			return false;
		}
		Out = uint64_t(Tmp);
		return true;
	}
	uint64_t Value;
	if (V->TryAsUInt(Value) && Value <= Max)
	{
		Out = Value;
		return true;
	}
	OutError = std::string("invalid unsigned integer or hex-string for: ") + Key;
	return false;
}

bool ReadBool(const JsonValue& Obj, const char* Key, bool& OutbValue, std::string& OutError)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V || !V->IsBool())
	{
		OutError = std::string("expected bool for: ") + Key;
		return false;
	}
	OutbValue = V->m_bB;
	return true;
}

bool ReadOptionalNum(
	const JsonValue& Obj, const char* Key, uint64_t& Out, std::string& OutError, uint64_t Max = UINT64_MAX)
{
	return !Obj.Find(Key) || ReadNumLike(Obj, Key, Out, OutError, Max);
}

bool ReadOptionalBool(const JsonValue& Obj, const char* Key, bool& OutbValue, std::string& OutError)
{
	return !Obj.Find(Key) || ReadBool(Obj, Key, OutbValue, OutError);
}

bool ReadString(const JsonValue& Obj, const char* Key, std::string& Out, std::string& OutError)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V)
	{
		OutError = std::string("missing field: ") + Key;
		return false;
	}
	if (V->IsNull())
	{
		Out.clear();
		return true;
	}
	if (!V->IsString())
	{
		OutError = std::string("expected string for: ") + Key;
		return false;
	}
	Out = V->m_S;
	return true;
}

bool ReadOptString(const JsonValue& Obj, const char* Key, std::string& Out)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V || V->IsNull())
	{
		Out.clear();
		return true;
	}
	if (!V->IsString())
	{
		return false;
	}
	Out = V->m_S;
	return true;
}

// Read an array of 16 hex-byte strings (or numbers) into a uint8_t[16].
// An array of N hex-byte strings/numbers into Out[N].
bool ReadHex8ArrayN(const JsonValue& Obj, const char* Key, uint8_t* Out, size_t N, std::string& OutError)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V || !V->IsArray() || V->m_A.size() != N)
	{
		OutError = std::string("expected ") + std::to_string(N) + "-element array for: " + Key;
		return false;
	}
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		if (V->m_A[IdxI].IsString())
		{
			uint32_t Tmp;
			if (!ParseHexQuoted(V->m_A[IdxI].m_S, Tmp) || Tmp > UINT8_MAX)
			{
				OutError = std::string("bad hex in array: ") + Key;
				return false;
			}
			Out[IdxI] = uint8_t(Tmp);
		}
		else if (V->m_A[IdxI].IsNumber())
		{
			uint64_t Value;
			if (!V->m_A[IdxI].TryAsUInt(Value) || Value > UINT8_MAX)
			{
				OutError = std::string("byte out of range in: ") + Key;
				return false;
			}
			Out[IdxI] = uint8_t(Value);
		}
		else
		{
			OutError = std::string("bad array element type in: ") + Key;
			return false;
		}
	}
	return true;
}

bool ReadHex8Array16(const JsonValue& Obj, const char* Key, uint8_t Out[16], std::string& OutError)
{
	return ReadHex8ArrayN(Obj, Key, Out, 16, OutError);
}

// Read an array of 8 hex-byte strings/numbers into uint8_t[8].
bool ReadHex8Array8(const JsonValue& Obj, const char* Key, uint8_t Out[8], std::string& OutError)
{
	const JsonValue* V = Obj.Find(Key);
	if (!V || !V->IsArray() || V->m_A.size() != 8)
	{
		OutError = std::string("expected 8-element array for: ") + Key;
		return false;
	}
	for (size_t IdxI = 0; IdxI < 8; ++IdxI)
	{
		if (V->m_A[IdxI].IsString())
		{
			uint32_t Tmp;
			if (!ParseHexQuoted(V->m_A[IdxI].m_S, Tmp) || Tmp > UINT8_MAX)
			{
				OutError = std::string("bad hex in array: ") + Key;
				return false;
			}
			Out[IdxI] = uint8_t(Tmp);
		}
		else if (V->m_A[IdxI].IsNumber())
		{
			uint64_t Value;
			if (!V->m_A[IdxI].TryAsUInt(Value) || Value > UINT8_MAX)
			{
				OutError = std::string("byte out of range in: ") + Key;
				return false;
			}
			Out[IdxI] = uint8_t(Value);
		}
		else
		{
			OutError = std::string("bad array element type in: ") + Key;
			return false;
		}
	}
	return true;
}

// ---- file I/O ----

bool ReadEntireFile(const char* Path, std::string& Out)
{
	FILE* F = fopen(Path, "rb");
	if (!F)
	{
		return false;
	}
	const bool bSeeked = fseek(F, 0, SEEK_END) == 0;
	const long Sz = bSeeked ? ftell(F) : -1;
	// A dump of the largest supported RAM is well below this bound.
	if (Sz < 0 || Sz > 128 * 1024 * 1024 || fseek(F, 0, SEEK_SET) != 0)
	{
		fclose(F);
		return false;
	}
	Out.resize(size_t(Sz));
	const size_t Got = fread(Out.data(), 1, size_t(Sz), F);
	const bool bRead = Got == size_t(Sz) && !ferror(F);
	return fclose(F) == 0 && bRead;
}

} // namespace

std::string ResolveRefPathFromDump(const std::string& StoredPath, const std::string& DumpPath)
{
	namespace fs = std::filesystem;
	if (StoredPath.empty())
	{
		return StoredPath;
	}
	fs::path P(StoredPath);
	if (P.is_absolute())
	{
		return P.generic_string();
	}
	fs::path DumpDir = fs::path(DumpPath).parent_path();
	fs::path Resolved = (DumpDir.empty() ? fs::current_path() : DumpDir) / P;
	return Resolved.lexically_normal().generic_string();
}

bool ReadAndParseDump(const char* Path, JsonValue& OutRoot, std::string& OutError)
{
	std::string Raw;
	if (!ReadEntireFile(Path, Raw))
	{
		OutError = std::string("cannot read dump file: ") + Path;
		return false;
	}
	if (!ParseJson(Raw, OutRoot, OutError))
	{
		OutError = std::string("dump JSON parse error: ") + OutError;
		return false;
	}
	if (!OutRoot.IsObject())
	{
		OutError = "dump root is not an object";
		return false;
	}

	uint64_t Version = 0;
	if (!ReadNumLike(OutRoot, "dump_version", Version, OutError))
	{
		return false;
	}
	if (Version != 1)
	{
		OutError = "dump_version != 1 (refusing to resume)";
		return false;
	}
	return true;
}

bool LoadExternalRefsFromDump(const JsonValue& Root, ParsedExternalRefs& Out, std::string& OutError)
{
	const JsonValue* E = nullptr;
	if (!ExpectObj(Root, "external_refs", E, OutError))
	{
		return false;
	}
	auto OneRef = [&](const char* Key, std::string& OutPath, std::string& OutSha) -> bool
	{
		const JsonValue* R = E->Find(Key);
		if (!R || !R->IsObject())
		{
			OutError = std::string("missing ref object: ") + Key;
			return false;
		}
		ReadOptString(*R, "path", OutPath);
		ReadOptString(*R, "sha1", OutSha);
		return true;
	};
	if (!OneRef("rom48", Out.m_Rom48Path, Out.m_Rom48Sha1))
	{
		return false;
	}
	if (!OneRef("rom128", Out.m_Rom128Path, Out.m_Rom128Sha1))
	{
		return false;
	}
	if (!OneRef("rom_trdos", Out.m_RomTrdPath, Out.m_RomTrdSha1))
	{
		return false;
	}
	if (!OneRef("tap", Out.m_TapPath, Out.m_TapSha1))
	{
		return false;
	}
	if (!OneRef("trd", Out.m_TrdDiskPath, Out.m_TrdDiskSha1))
	{
		return false;
	}
	if (!OneRef("input_script", Out.m_InputScriptPath, Out.m_InputScriptSha1))
	{
		return false;
	}
	// Newer references; an older dump has none (the CLI must name them).
	for (const auto& [Key, Path, Sha] :
		{std::tuple<const char*, std::string*, std::string*>{"rom_plus3", &Out.m_RomPlus3Path, &Out.m_RomPlus3Sha1},
			std::tuple<const char*, std::string*, std::string*>{"dck", &Out.m_DckPath, &Out.m_DckSha1}})
	{
		if (E->Find(Key) && !OneRef(Key, *Path, *Sha))
		{
			return false;
		}
	}
	return true;
}

bool LoadRunConfigFromDump(const JsonValue& Root, DumpRunConfig& Out, std::string& OutError)
{
	if (!ReadString(Root, "machine", Out.m_Machine, OutError))
	{
		return false;
	}
	bool bKnownMachine = false;
	for (const char* Name : {"spectrum48k", "spectrum128k", "pentagon128", "next50", "next60", "ts2068", "tc2068",
			 "tc2048", "zxevo", "zxuno", "spectrumplus2a", "spectrumplus3"})
	{
		bKnownMachine = bKnownMachine || Out.m_Machine == Name;
	}
	if (!bKnownMachine)
	{
		OutError = "unknown machine in dump: " + Out.m_Machine;
		return false;
	}
	const JsonValue* Memory = Root.Find("memory");
	if (Memory && Memory->Find("ram_mb"))
	{
		uint64_t RamMb = 2;
		if (!ReadNumLike(*Memory, "ram_mb", RamMb, OutError, 2) || RamMb < 1)
		{
			OutError = "memory.ram_mb must be 1 or 2";
			return false;
		}
		Out.m_RamMb = int(RamMb);
	}
	const JsonValue* MhzV = Root.Find("mhz");
	if (MhzV)
	{
		if (!MhzV->IsNumber() || !(MhzV->AsDouble() > 0.0 && MhzV->AsDouble() <= 28.0))
		{
			OutError = "mhz must be a positive clock rate at most 28";
			return false;
		}
		Out.m_Mhz = MhzV->AsDouble();
	}
	uint64_t CX = 0;
	if (Root.Find("contention_x"))
	{
		if (!ReadNumLike(Root, "contention_x", CX, OutError, UINT32_MAX))
		{
			return false;
		}
		Out.m_ContentionX = uint32_t(CX);
	}
	uint64_t FIP = 0;
	if (Root.Find("frame_int_period"))
	{
		if (!ReadNumLike(Root, "frame_int_period", FIP, OutError))
		{
			return false;
		}
		Out.m_FrameIntPeriod = FIP;
	}
	// PSG (optional object)
	const JsonValue* Psg = Root.Find("psg");
	if (Psg && Psg->IsObject())
	{
		bool bActive = false;
		if (!ReadOptionalBool(*Psg, "active", bActive, OutError))
		{
			return false;
		}
		if (bActive)
		{
			Out.m_bHasPsg = true;
			ReadOptString(*Psg, "out_path", Out.m_PsgOutPath);
			uint64_t R = 50;
			if (Psg->Find("rate"))
			{
				if (!ReadNumLike(*Psg, "rate", R, OutError, 60))
				{
					return false;
				}
				if (R != 50 && R != 60)
				{
					OutError = "PSG rate must be 50 or 60";
					return false;
				}
				Out.m_PsgRate = int(R);
			}
		}
	}

	// Input (optional)
	const JsonValue* In = Root.Find("input");
	if (In && In->IsObject())
	{
		Out.m_bHasInput = true;
		uint64_t Tap = 2;
		if (In->Find("default_tap_frames"))
		{
			if (!ReadNumLike(*In, "default_tap_frames", Tap, OutError, INT_MAX))
			{
				return false;
			}
			Out.m_InputTapFrames = int(Tap);
		}
		bool bLog = true;
		if (!ReadOptionalBool(*In, "log", bLog, OutError))
		{
			return false;
		}
		Out.m_bInputLog = bLog;
	}

	// TRDOS (optional)
	const JsonValue* Trd = Root.Find("trdos");
	if (Trd && Trd->IsObject())
	{
		if (!ReadOptionalBool(*Trd, "support_active", Out.m_bTrdosSupportActive, OutError))
		{
			return false;
		}
	}

	// The options that shape behaviour. Older dumps have no block: their
	// defaults are the CLI's, and main.cpp keeps its old per-source rules.
	Out.m_bAyEnabled = true;
	const JsonValue* Opt = Root.Find("run_options");
	if (Opt && Opt->IsObject())
	{
		DumpRunOptions& O = Out.m_Options;
		O.m_bPresent = true;
		if (!ReadBool(*Opt, "reset_trap", O.m_bResetTrap, OutError) ||
			!ReadBool(*Opt, "lenient_undoc_ed", O.m_bLenientUndocEd, OutError) ||
			!ReadBool(*Opt, "no_copper", O.m_bNoCopper, OutError) ||
			!ReadBool(*Opt, "pixel_accurate", O.m_bPixelAccurate, OutError) ||
			!ReadBool(*Opt, "ay_enabled", O.m_bAyEnabled, OutError) ||
			!ReadBool(*Opt, "turbosound", O.m_bTurboSound, OutError) ||
			!ReadBool(*Opt, "vfs_readonly", O.m_bVfsReadOnly, OutError) ||
			!ReadBool(*Opt, "tap_type_frame_set", O.m_bTapTypeFrameSet, OutError) ||
			!ReadNumLike(*Opt, "tap_type_frame", O.m_TapTypeFrame, OutError))
		{
			return false;
		}
		const JsonValue* Iw = Opt->Find("int_window_mode");
		int64_t IwV = -1;
		if (!Iw || !Iw->TryAsInt(IwV) || IwV < -1)
		{
			OutError = "run_options.int_window_mode must be -1 or a T-state count";
			return false;
		}
		O.m_IntWindowMode = IwV;
		O.m_InputTapFrames = Out.m_InputTapFrames;
		O.m_bInputLog = Out.m_bInputLog;
		Out.m_bAyEnabled = O.m_bAyEnabled;
	}

	// NEX path + open file handle position (for streaming-aware resume), and
	// the header facts the next dump of a resumed run reports again.
	const JsonValue* Nex = Root.Find("nex");
	if (Nex && Nex->IsObject())
	{
		ReadOptString(*Nex, "path", Out.m_NexPath);
		ReadOptString(*Nex, "sha1", Out.m_NexSha1);
		ReadOptString(*Nex, "identity_override", Out.m_IdentityOverride);
		DumpNexInfo& N = Out.m_Nex;
		ReadOptString(*Nex, "version", N.m_Version);
		uint64_t V = 0;
		if (ReadNumLike(*Nex, "num_banks", V, OutError, UINT8_MAX))
		{
			N.m_NumBanks = uint8_t(V);
		}
		if (ReadNumLike(*Nex, "screen_flags", V, OutError, UINT8_MAX))
		{
			N.m_ScreenFlags = uint8_t(V);
		}
		if (ReadNumLike(*Nex, "border", V, OutError, UINT8_MAX))
		{
			N.m_Border = uint8_t(V);
		}
		if (ReadNumLike(*Nex, "entry_bank", V, OutError, UINT8_MAX))
		{
			N.m_EntryBank = uint8_t(V);
		}
		if (ReadNumLike(*Nex, "initial_sp", V, OutError, UINT16_MAX))
		{
			N.m_InitialSP = uint16_t(V);
		}
		if (ReadNumLike(*Nex, "initial_pc", V, OutError, UINT16_MAX))
		{
			N.m_InitialPC = uint16_t(V);
		}
		if (ReadNumLike(*Nex, "banks_offset", V, OutError, UINT32_MAX))
		{
			N.m_BanksOffset = uint32_t(V);
		}
		std::string Core;
		unsigned C0 = 0, C1 = 0, C2 = 0;
		if (ReadOptString(*Nex, "core_version", Core) && sscanf(Core.c_str(), "%u.%u.%u", &C0, &C1, &C2) == 3)
		{
			N.m_CoreVersion[0] = uint8_t(C0);
			N.m_CoreVersion[1] = uint8_t(C1);
			N.m_CoreVersion[2] = uint8_t(C2);
		}
		ReadOptionalBool(*Nex, "has_copper_code", N.m_bHasCopperCode, OutError);
		ReadOptionalBool(*Nex, "has_loading_screen", N.m_bHasLoadingScreen, OutError);
		ReadOptionalBool(*Nex, "loading_screen_applied", N.m_bLoadingScreenApplied, OutError);
		std::string Banks;
		if (ReadOptString(*Nex, "banks_present", Banks))
		{
			for (const char* P = Banks.c_str(); *P;)
			{
				char* End = nullptr;
				const long B = strtol(P, &End, 10);
				if (End == P)
				{
					++P;
					continue;
				}
				N.m_BanksPresent.push_back(int(B));
				P = End;
			}
		}
		OutError.clear();
	}
	const JsonValue* Vfs = Root.Find("vfs");
	if (Vfs && Vfs->IsObject())
	{
		const JsonValue* Handles = Vfs->Find("open_handles");
		if (Handles && Handles->IsObject())
		{
			const JsonValue* kH1 = Handles->Find("h1");
			if (kH1 && kH1->IsObject())
			{
				uint64_t Pos = 0;
				if (kH1->Find("position"))
				{
					if (!ReadNumLike(*kH1, "position", Pos, OutError, LONG_MAX))
					{
						return false;
					}
					Out.m_bHasNexHandle = true;
					Out.m_NexHandlePosition = long(Pos);
				}
			}
		}
	}

	return true;
}

bool LoadCpuFromDump(const JsonValue& Root, Z80State& Out, std::string& OutError)
{
	const JsonValue* C = nullptr;
	if (!ExpectObj(Root, "cpu", C, OutError))
	{
		return false;
	}
	uint64_t Tmp;
	auto Hex16 = [&](const char* K, uint16_t& OutDst) -> bool
	{
		if (!ReadNumLike(*C, K, Tmp, OutError, UINT16_MAX))
		{
			return false;
		}
		OutDst = uint16_t(Tmp);
		return true;
	};
	auto Num = [&](const char* K, uint64_t& OutDst) -> bool
	{
		if (!ReadNumLike(*C, K, Tmp, OutError))
		{
			return false;
		}
		OutDst = Tmp;
		return true;
	};

	if (!Hex16("AF", Out.AF))
	{
		return false;
	}
	if (!Hex16("BC", Out.BC))
	{
		return false;
	}
	if (!Hex16("DE", Out.DE))
	{
		return false;
	}
	if (!Hex16("HL", Out.HL))
	{
		return false;
	}
	if (!Hex16("AFp", Out.AFp))
	{
		return false;
	}
	if (!Hex16("BCp", Out.BCp))
	{
		return false;
	}
	if (!Hex16("DEp", Out.DEp))
	{
		return false;
	}
	if (!Hex16("HLp", Out.HLp))
	{
		return false;
	}
	if (!Hex16("IX", Out.IX))
	{
		return false;
	}
	if (!Hex16("IY", Out.IY))
	{
		return false;
	}
	if (!Hex16("SP", Out.SP))
	{
		return false;
	}
	if (!Hex16("PC", Out.PC))
	{
		return false;
	}
	if (!Hex16("MemPtr", Out.m_MemPtr))
	{
		return false;
	}

	uint64_t T;
	if (!ReadNumLike(*C, "I", T, OutError, UINT8_MAX))
	{
		return false;
	}
	Out.I = uint8_t(T);
	if (!ReadNumLike(*C, "R", T, OutError, UINT8_MAX))
	{
		return false;
	}
	Out.R = uint8_t(T);
	if (!ReadNumLike(*C, "IFF1", T, OutError, 1))
	{
		return false;
	}
	Out.IFF1 = uint8_t(T);
	if (!ReadNumLike(*C, "IFF2", T, OutError, 1))
	{
		return false;
	}
	Out.IFF2 = uint8_t(T);
	if (!ReadNumLike(*C, "IM", T, OutError, 2))
	{
		return false;
	}
	Out.IM = uint8_t(T);

	if (!ReadBool(*C, "halted", Out.m_bHalted, OutError))
	{
		return false;
	}
	if (!ReadBool(*C, "ei_pending", Out.m_bEiPending, OutError))
	{
		return false;
	}
	if (C->Find("halt_start_cycle") && !Num("halt_start_cycle", Out.m_HaltStartCycle))
	{
		return false;
	}

	if (!Num("interrupts_accepted", Out.m_InterruptsAccepted))
	{
		return false;
	}
	// Newer blocks; an older dump leaves them at reset (and main.cpp keeps
	// its own int window).
	if (C->Find("int_window") && (!Num("int_window", Out.m_IntWindow) ||
									 !Num("missed_interrupts", Out.m_MissedInterrupts)))
	{
		return false;
	}
	const JsonValue* L = C->Find("line_int");
	if (L && L->IsObject())
	{
		uint64_t Line = 0;
		if (!ReadBool(*L, "enabled", Out.m_bLineIntEnabled, OutError) ||
			!ReadNumLike(*L, "line", Line, OutError, 511) ||
			!ReadBool(*L, "ula_masked", Out.m_bUlaIntMasked, OutError) ||
			!ReadNumLike(*L, "accepted", Out.m_LineIntsAccepted, OutError) ||
			!ReadNumLike(*L, "missed", Out.m_LineIntsMissed, OutError))
		{
			return false;
		}
		Out.m_LineIntLine = uint32_t(Line);
		const JsonValue* Nc = L->Find("next_cycle");
		Out.m_NextLineIntCycle = UINT64_MAX;
		if (Nc && !Nc->IsNull() && !Nc->TryAsUInt(Out.m_NextLineIntCycle))
		{
			OutError = "invalid cpu.line_int.next_cycle";
			return false;
		}
	}
	const JsonValue* Isr = C->Find("isr");
	if (Isr && Isr->IsObject())
	{
		uint64_t V = 0;
		if (!ReadNumLike(*Isr, "depth", V, OutError, Z80State::kIsrNestingMax))
		{
			return false;
		}
		Out.m_IsrDepth = uint8_t(V);
		if (!ReadNumLike(*Isr, "dma_resume_pending", V, OutError, 1))
		{
			return false;
		}
		Out.m_BDmaResumePendingOneInstr = uint8_t(V);
		uint8_t Bytes[Z80State::kIsrNestingMax * 4] = {};
		if (!ReadHex8ArrayN(*Isr, "return_sp_pc", Bytes, sizeof Bytes, OutError))
		{
			return false;
		}
		for (int I = 0; I < Z80State::kIsrNestingMax; ++I)
		{
			Out.m_IsrReturnSP[I] = uint16_t(Bytes[I * 4] | (Bytes[I * 4 + 1] << 8));
			Out.m_IsrReturnPC[I] = uint16_t(Bytes[I * 4 + 2] | (Bytes[I * 4 + 3] << 8));
		}
	}
	return true;
}

bool LoadMemoryFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* M = nullptr;
	if (!ExpectObj(Root, "memory", M, OutError))
	{
		return false;
	}

	std::string Kind;
	if (!ReadString(*M, "kind", Kind, OutError))
	{
		return false;
	}

	uint64_t Port7FFD = 0;
	if (!ReadNumLike(*M, "port_7ffd", Port7FFD, OutError, UINT8_MAX))
	{
		return false;
	}

	bool bPagingLocked = false;
	if (!ReadOptionalBool(*M, "paging_locked", bPagingLocked, OutError))
	{
		return false;
	}
	bool bTrdosRom = false;
	if (!ReadOptionalBool(*M, "trdos_rom_active", bTrdosRom, OutError))
	{
		return false;
	}

	uint64_t Border = 7;
	if (!ReadOptionalNum(*M, "border_colour", Border, OutError, 7))
	{
		return false;
	}

	bool bTimex = false;
	if (!ReadOptionalBool(*M, "timex", bTimex, OutError))
	{
		return false;
	}
	uint64_t Port1FFD = 0;
	if (Kind == "spectrumplus2a" || Kind == "spectrumplus3")
	{
		if (!ReadNumLike(*M, "port_1ffd", Port1FFD, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.SetIsPlus3(true);
		Out.MapPlus3(uint8_t(Port7FFD), uint8_t(Port1FFD));
	}
	else if (Kind == "spectrum48k" || bTimex)
	{
		// Timex machines use the 48K memory layout; SCLD state is re-armed below.
		Out.Map48K();
	}
	else
	{
		Out.Map128K(uint8_t(Port7FFD), bTrdosRom);
	}
	Out.SetTrdosActive(bTrdosRom);
	// the latches as written, not as Map* left them (the lock included)
	Out.RestorePagingLatches(uint8_t(Port7FFD), uint8_t(Port1FFD), M->Find("paging_locked") ? bPagingLocked : Out.IsPagingLocked());

	if (bTimex)
	{
		bool bB2068 = false;
		if (!ReadOptionalBool(*M, "timex_2068", bB2068, OutError))
		{
			return false;
		}
		Out.EnableTimex(bB2068); // resets SCLD, so restore mode/bank afterwards
		uint64_t ScldMode = 0, ScldBank = 0;
		if (!ReadOptionalNum(*M, "scld_mode", ScldMode, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "scld_bank", ScldBank, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.SetScldMode(uint8_t(ScldMode));
		Out.SetScldBank(uint8_t(ScldBank));
	}

	// ZX Evolution BaseConf: re-arm + restore $EFF7 / $xx77 / RTC-addr and the
	// four page-window overrides, then re-apply the windows on top of the 128K map.
	bool bZxEvo = false;
	if (!ReadOptionalBool(*M, "zxevo", bZxEvo, OutError))
	{
		return false;
	}
	if (bZxEvo)
	{
		Out.EnableZxEvo(); // arms the flag + resets extension state
		uint64_t Eff7 = 0, Ff77 = 0x03, CmosA = 0;
		if (!ReadOptionalNum(*M, "eff7", Eff7, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "evo_ff77", Ff77, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "cmos_addr", CmosA, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.SetEff7(uint8_t(Eff7));
		Out.SetEvoFf77(uint8_t(Ff77));
		Out.SetCmosAddr(uint8_t(CmosA));
		const char* WinKeys[4] = {"evo_win0", "evo_win1", "evo_win2", "evo_win3"};
		for (int Wn = 0; Wn < 4; ++Wn)
		{
			int64_t Page = -1;
			const JsonValue* Value = M->Find(WinKeys[Wn]);
			if (Value && (!Value->TryAsInt(Page) || Page < -1 || Page > 255))
			{
				OutError = "invalid Evo window page";
				return false;
			}
			Out.SetEvoWinPage(Wn, int16_t(Page));
		}
		Out.ApplyEvoWindows();
	}

	// ZX-Uno: re-arm + restore the register-select latch, reg $0B turbo, SCLD mode.
	bool bZxUno = false;
	if (!ReadOptionalBool(*M, "zxuno", bZxUno, OutError))
	{
		return false;
	}
	if (bZxUno)
	{
		Out.EnableZxUno();
		uint64_t RegSel = 0, ScanDbl = 0, ScldMode = 0;
		if (!ReadOptionalNum(*M, "uno_reg_sel", RegSel, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "uno_scandbl", ScanDbl, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "scld_mode", ScldMode, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.SetUnoRegSel(uint8_t(RegSel));
		Out.SetUnoScanDbl(uint8_t(ScanDbl));
		Out.SetScldMode(uint8_t(ScldMode));

		// Radastan + ULAplus.
		uint64_t RadasCtrl = 0, RadasOffset = 0, RadasPadding = 0, RadasPalBank = 0;
		if (!ReadOptionalNum(*M, "radas_ctrl", RadasCtrl, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "radas_offset", RadasOffset, OutError, UINT16_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "radas_padding", RadasPadding, OutError, UINT8_MAX))
		{
			return false;
		}
		if (!ReadOptionalNum(*M, "radas_palbank", RadasPalBank, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.SetUnoRadasCtrl(uint8_t(RadasCtrl));
		Out.SetUnoRadasOffset(uint16_t(RadasOffset));
		Out.SetUnoRadasPadding(uint8_t(RadasPadding));
		Out.SetUnoRadasPalBank(uint8_t(RadasPalBank));
		bool bUlaMode = false;
		if (!ReadOptionalBool(*M, "ulaplus_mode", bUlaMode, OutError))
		{
			return false;
		}
		Out.SetUlaPlusMode(bUlaMode);
		const JsonValue* Cram = M->Find("ulaplus_cram");
		if (Cram)
		{
			if (!Cram->IsArray() || Cram->m_A.size() != 64)
			{
				OutError = "ulaplus_cram must contain 64 bytes";
				return false;
			}
			for (int Idx = 0; Idx < 64; ++Idx)
			{
				const JsonValue& Entry = Cram->m_A[Idx];
				uint64_t Value = 0;
				if (Entry.IsString())
				{
					if (!ParseHex64Quoted(Entry.m_S, Value) || Value > UINT8_MAX)
					{
						OutError = "invalid ULAplus byte";
						return false;
					}
				}
				else if (!Entry.TryAsUInt(Value) || Value > UINT8_MAX)
				{
					OutError = "invalid ULAplus byte";
					return false;
				}
				Out.SetUlaPlusCramEntry(Idx, uint8_t(Value));
			}
		}
	}

	// Counters + border are restored AFTER mapping (Map*K may reset them).
	uint64_t B5P = 0, B5A = 0, B7P = 0, B7A = 0, Port7W = 0;
	const JsonValue* kVV = M->Find("vram_writes");
	if (kVV && kVV->IsObject())
	{
		if (!ReadOptionalNum(*kVV, "bank5_pixel", B5P, OutError))
		{
			return false;
		}
		if (!ReadOptionalNum(*kVV, "bank5_attr", B5A, OutError))
		{
			return false;
		}
		if (!ReadOptionalNum(*kVV, "bank7_pixel", B7P, OutError))
		{
			return false;
		}
		if (!ReadOptionalNum(*kVV, "bank7_attr", B7A, OutError))
		{
			return false;
		}
	}
	if (!ReadOptionalNum(*M, "port_7ffd_writes", Port7W, OutError))
	{
		return false;
	}
	Out.RestoreCountersForDump(B5P, B5A, B7P, B7A, Port7W, uint8_t(Border));
	return true;
}

bool LoadRamFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* R = nullptr;
	if (!ExpectObj(Root, "ram", R, OutError))
	{
		return false;
	}
	// Restore every bank the dump carries. WriteRam emits banks 0-7 always
	// plus any NextRAM banks 8..(RAM_PAGE_COUNT/2-1) that hold non-zero
	// data. Match that range here, otherwise Layer 2 framebuffers stored
	// in NextRAM come back as zeros and the screen is black after resume.
	for (int B = 0; B < kRamPageCount / 2; ++B)
	{
		char Key[16];
		snprintf(Key, sizeof(Key), "bank_%d", B);
		const JsonValue* V = R->Find(Key);
		if (!V)
		{
			continue; // missing bank is fine - leave at default (zeros)
		}
		if (!V->IsString())
		{
			OutError = std::string("expected string for: ") + Key;
			return false;
		}
		if (!DecodeBankFromHexdump(V->m_S, Out.Ram16KBank(B), 16384, OutError))
		{
			return false;
		}
	}
	return true;
}

bool LoadAyFromDump(const JsonValue& Root, AyState& Out, std::string& OutError, const char* Key)
{
	const JsonValue* A = nullptr;
	if (!ExpectObj(Root, Key, A, OutError))
	{
		return false;
	}
	if (A->Find("envelope_writes") && !ReadNumLike(*A, "envelope_writes", Out.m_EnvelopeWrites, OutError))
	{
		return false;
	}
	if (!ReadHex8Array16(*A, "registers", Out.m_Registers, OutError))
	{
		return false;
	}
	uint64_t T;
	if (!ReadNumLike(*A, "latch", T, OutError, 15))
	{
		return false;
	}
	Out.m_Latch = uint8_t(T & 0x0F);
	if (!ReadNumLike(*A, "writes_observed", T, OutError))
	{
		return false;
	}
	Out.m_WritesObserved = T;
	if (!ReadNumLike(*A, "registers_touched", T, OutError, UINT16_MAX))
	{
		return false;
	}
	Out.m_RegistersTouched = uint16_t(T);
	return true;
}

bool LoadPsgFromDump(const JsonValue& Root, PsgCapture& InOutCapture, std::string& OutError, const char* Key)
{
	const JsonValue* P = Root.Find(Key);
	if (!P || !P->IsObject())
	{
		return true; // nothing to do
	}

	bool bActive = false;
	if (!ReadOptionalBool(*P, "active", bActive, OutError))
	{
		return false;
	}
	if (!bActive)
	{
		return true;
	}
	uint64_t Dirty = 0, Silent = 0, Frames = 0, Writes = 0;
	if (!ReadNumLike(*P, "dirty_mask", Dirty, OutError, UINT16_MAX))
	{
		return false;
	}
	if (!ReadNumLike(*P, "silent_frames", Silent, OutError, UINT32_MAX))
	{
		return false;
	}
	if (!ReadNumLike(*P, "frames_written", Frames, OutError))
	{
		return false;
	}
	if (!ReadNumLike(*P, "writes_captured", Writes, OutError))
	{
		return false;
	}
	uint8_t Vals[16];
	if (!ReadHex8Array16(*P, "frame_values", Vals, OutError))
	{
		return false;
	}
	std::string BodyB64;
	if (!ReadOptString(*P, "body_b64", BodyB64))
	{
		OutError = "psg.body_b64 must be string";
		return false;
	}
	std::vector<uint8_t> Body;
	if (!BodyB64.empty() && !DecodeBase64(BodyB64, Body, OutError))
	{
		return false;
	}
	InOutCapture.LoadStream(Body, uint16_t(Dirty), Vals, uint32_t(Silent), Frames, Writes);
	return true;
}

bool LoadTrdosFromDump(const JsonValue& Root, Trdos& InOutTrd, std::string& OutError)
{
	const JsonValue* T = Root.Find("trdos");
	if (!T || !T->IsObject())
	{
		return true;
	}
	bool bSupport = false;
	if (!ReadOptionalBool(*T, "support_active", bSupport, OutError))
	{
		return false;
	}
	if (!bSupport)
	{
		return true;
	}
	const JsonValue* F = T->Find("fdc");
	if (!F || !F->IsObject())
	{
		OutError = "trdos.fdc missing";
		return false;
	}

	uint64_t U;
	auto N = [&](const char* K, uint8_t& OutDst) -> bool
	{
		if (!ReadNumLike(*F, K, U, OutError, UINT8_MAX))
		{
			return false;
		}
		OutDst = uint8_t(U);
		return true;
	};

	uint8_t Cmd, Stat, Trk, Sec, Dat, Sys, HeadTrk, Sd;
	if (!N("command", Cmd) || !N("status", Stat) || !N("track", Trk) || !N("sector", Sec) || !N("data", Dat) ||
		!N("system", Sys) || !N("head_track", HeadTrk) || !N("side", Sd))
	{
		return false;
	}

	int BufPos = 0;
	int BufLen = 0;
	if (!ReadNumLike(*F, "buffer_pos", U, OutError, 256))
	{
		return false;
	}
	BufPos = int(U);
	if (!ReadNumLike(*F, "buffer_len", U, OutError, 256))
	{
		return false;
	}
	BufLen = int(U);
	if (BufPos > BufLen)
	{
		OutError = "FDC buffer position exceeds its length";
		return false;
	}
	std::string B64;
	ReadOptString(*F, "buffer_b64", B64);
	std::vector<uint8_t> Buf;
	if (!B64.empty())
	{
		if (!DecodeBase64(B64, Buf, OutError))
		{
			return false;
		}
	}
	if (Buf.size() > 256 || Buf.size() < size_t(BufLen))
	{
		OutError = "invalid FDC buffer size";
		return false;
	}
	Buf.resize(256, 0);

	bool bBusy = false, bIrq = false, bDrq = false;
	if (!ReadOptionalBool(*F, "busy", bBusy, OutError))
	{
		return false;
	}
	if (!ReadOptionalBool(*F, "irq", bIrq, OutError))
	{
		return false;
	}
	if (!ReadOptionalBool(*F, "drq", bDrq, OutError))
	{
		return false;
	}

	std::string LastClass = "TypeIV";
	ReadOptString(*F, "last_class", LastClass);

	uint64_t IndexPulse = 0, ReadAddr = 1;
	if (F->Find("index_pulse_counter") && (!ReadNumLike(*F, "index_pulse_counter", IndexPulse, OutError, UINT32_MAX) ||
											 !ReadNumLike(*F, "read_addr_sector", ReadAddr, OutError, UINT8_MAX)))
	{
		return false;
	}
	InOutTrd.RestorePhase(uint32_t(IndexPulse), uint8_t(ReadAddr));
	InOutTrd.RestoreFromDump(
		Cmd, Stat, Trk, Sec, Dat, Sys, HeadTrk, Sd, Buf.data(), BufPos, BufLen, bBusy, bIrq, bDrq, LastClass);
	return true;
}

bool LoadInputFromDump(const JsonValue& Root, InputState& InOutInput, std::string& OutError)
{
	const JsonValue* I = Root.Find("input");
	if (!I || !I->IsObject())
	{
		return true;
	}
	uint8_t Rows[8];
	if (!ReadHex8Array8(*I, "keyboard_rows", Rows, OutError))
	{
		return false;
	}
	uint64_t Kemp = 0;
	if (!ReadNumLike(*I, "kempston_state", Kemp, OutError, UINT8_MAX))
	{
		return false;
	}
	uint64_t Next = 0;
	if (!ReadNumLike(*I, "next_event", Next, OutError))
	{
		return false;
	}
	const JsonValue* Ev = I->Find("events");
	if (Ev && Ev->IsObject())
	{
		std::vector<InputEvent> Events;
		for (const auto& [K, V] : Ev->m_O)
		{
			if (!V.IsString())
			{
				OutError = "input.events." + K + " must be a string";
				return false;
			}
			InputEvent E;
			char PR = 0;
			unsigned long long Frame = 0;
			char Targets[256] = {};
			int Used = 0;
			if (sscanf(V.m_S.c_str(), "%llu %c %255s %n", &Frame, &PR, Targets, &Used) < 3 || (PR != 'p' && PR != 'r'))
			{
				OutError = "invalid input.events." + K + ": " + V.m_S;
				return false;
			}
			E.m_Frame = Frame;
			E.m_bPress = PR == 'p';
			E.m_Name = (Used > 0 && size_t(Used) <= V.m_S.size()) ? V.m_S.substr(size_t(Used)) : std::string();
			for (const char* P = Targets; *P && *P != '-';)
			{
				KeyTarget T{};
				if (P[0] == 'j' && P[1])
				{
					T.m_bKempston = true;
					T.m_Bit = uint8_t(P[1] - '0');
					P += 2;
				}
				else if (P[0] == 'k' && P[1] && P[2])
				{
					T.m_bKempston = false;
					T.m_Row = uint8_t(P[1] - '0');
					T.m_Bit = uint8_t(P[2] - '0');
					P += 3;
				}
				else
				{
					OutError = "invalid input.events." + K + " target: " + V.m_S;
					return false;
				}
				E.m_Targets.push_back(T);
				if (*P == ',')
				{
					++P;
				}
			}
			Events.push_back(std::move(E));
		}
		InOutInput.SetEvents(std::move(Events));
	}
	InOutInput.RestoreFromDump(Rows, uint8_t(Kemp), size_t(Next));
	return true;
}

bool TakeInputScriptOnResume(const JsonValue& Root, InputState& InOutInput,
	const std::vector<InputEvent>& Script, std::string& OutError)
{
	// The same script: the dump's events and cursor are exactly right.
	const std::vector<InputEvent>& Had = InOutInput.Events();
	bool bSame = Had.size() == Script.size();
	for (size_t IdxI = 0; bSame && IdxI < Had.size(); ++IdxI)
	{
		bSame = Had[IdxI].m_Frame == Script[IdxI].m_Frame && Had[IdxI].m_bPress == Script[IdxI].m_bPress &&
				Had[IdxI].m_Name == Script[IdxI].m_Name;
	}
	if (bSame)
	{
		return false;
	}
	// The frame the dump stopped in: events before it would all fire at once.
	uint64_t Cycles = 0;
	uint64_t Period = 0;
	const JsonValue* Stats = Root.Find("stats");
	const JsonValue* Sched = Root.Find("scheduler");
	if (!Stats || !Sched || !ReadNumLike(*Stats, "cycles_total", Cycles, OutError) ||
		!ReadNumLike(*Sched, "frame_int_period", Period, OutError) || Period == 0)
	{
		if (OutError.empty())
		{
			OutError = "resume input: the dump has no frame position";
		}
		return false;
	}
	const uint64_t Frame = Cycles / Period;
	std::vector<InputEvent> Future;
	for (const InputEvent& E : Script)
	{
		if (E.m_Frame >= Frame)
		{
			Future.push_back(E);
		}
	}
	uint8_t Rows[8];
	for (int IdxI = 0; IdxI < 8; ++IdxI)
	{
		Rows[IdxI] = InOutInput.GetKeyboardRows()[IdxI];
	}
	const uint8_t Kemp = InOutInput.GetKempstonState();
	InOutInput.SetEvents(std::move(Future));
	InOutInput.RestoreFromDump(Rows, Kemp, 0);
	return true;
}

bool LoadCtcFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* CtcGroup = Root.Find("ctc");
	if (!CtcGroup || CtcGroup->IsNull())
	{
		return true; // no CTC state recorded
	}
	if (!CtcGroup->IsObject())
	{
		OutError = "ctc must be object or null";
		return false;
	}
	for (int N = 0; N < 8; ++N)
	{
		char Key[16];
		snprintf(Key, sizeof Key, "channel_%d", N);
		const JsonValue* C = CtcGroup->Find(Key);
		if (!C || !C->IsObject())
		{
			continue;
		}
		MemoryBus::CtcChannel& Dst = Out.Ctc(N);
		bool bEnabled = false;
		if (!ReadBool(*C, "enabled", bEnabled, OutError))
		{
			return false;
		}
		uint64_t T = 0;
		if (C->Find("control_word"))
		{
			if (!ReadNumLike(*C, "control_word", T, OutError, UINT8_MAX))
			{
				return false;
			}
			Dst.m_ControlWord = uint8_t(T);
		}
		if (C->Find("time_constant"))
		{
			if (!ReadNumLike(*C, "time_constant", T, OutError, UINT8_MAX))
			{
				return false;
			}
			Dst.m_TimeConstant = uint8_t(T);
		}
		if (C->Find("period_cycles"))
		{
			if (!ReadNumLike(*C, "period_cycles", T, OutError))
			{
				return false;
			}
			Dst.m_PeriodCycles = T;
		}
		const JsonValue* Nfc = C->Find("next_fire_cycle");
		if (Nfc && Nfc->IsNull())
		{
			Dst.m_NextFireCycle = UINT64_MAX;
		}
		else if (Nfc && Nfc->IsNumber())
		{
			if (!Nfc->TryAsUInt(Dst.m_NextFireCycle))
			{
				OutError = "invalid next_fire_cycle";
				return false;
			}
		}
		if (C->Find("interrupts_fired"))
		{
			if (!ReadNumLike(*C, "interrupts_fired", T, OutError))
			{
				return false;
			}
			Dst.m_InterruptsFired = T;
		}
		Dst.m_bEnabled = bEnabled;
		Dst.m_bAwaitingTC = false; // an older dump has no awaiting_tc: post-TC
		// Newer fields; pre-cascade dumps lack them - derive sane defaults
		// (the old model only ran a channel while its IRQ was enabled).
		Dst.m_bRunning = bEnabled;
		Dst.m_bCounterMode = false;
		if (!ReadOptionalBool(*C, "running", Dst.m_bRunning, OutError) ||
			!ReadOptionalBool(*C, "counter_mode", Dst.m_bCounterMode, OutError))
		{
			return false;
		}
		Dst.m_bTcLoaded = Dst.m_PeriodCycles > 0 || Dst.m_TimeConstant != 0;
		// written explicitly since the dump learned them: a channel between its
		// control word and its time constant, and one loaded with TC 0 (= 256)
		if (!ReadOptionalBool(*C, "awaiting_tc", Dst.m_bAwaitingTC, OutError) ||
			!ReadOptionalBool(*C, "tc_loaded", Dst.m_bTcLoaded, OutError))
		{
			return false;
		}
		if (C->Find("grid_anchor"))
		{
			if (!ReadNumLike(*C, "grid_anchor", T, OutError))
			{
				return false;
			}
			Dst.m_GridAnchor = T;
		}
		else if (Dst.m_NextFireCycle != UINT64_MAX && Dst.m_PeriodCycles > 0 &&
				 Dst.m_NextFireCycle >= Dst.m_PeriodCycles)
		{
			Dst.m_GridAnchor = Dst.m_NextFireCycle - Dst.m_PeriodCycles;
		}
		OutError.clear();
	}
	return true;
}

// The copper as WriteCopperDump left it. A dump from before the full state
// (counters only, no mem_b64) restores nothing, as resuming always did.
// Presence and the machine's line constants are set later, at machine setup.
bool LoadCopperFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* C = Root.Find("copper");
	if (!C || C->IsNull())
	{
		return true;
	}
	if (!C->IsObject())
	{
		OutError = "copper must be object or null";
		return false;
	}
	std::string B64;
	if (!ReadOptString(*C, "mem_b64", B64) || B64.empty())
	{
		return true;
	}
	std::vector<uint8_t> Buf;
	if (!DecodeBase64(B64, Buf, OutError))
	{
		return false;
	}
	CopperState& Dst = Out.Copper();
	if (Buf.size() != sizeof(Dst.m_Mem2K))
	{
		OutError = "copper.mem_b64 must decode to 2048 bytes";
		return false;
	}
	memcpy(Dst.m_Mem2K, Buf.data(), sizeof(Dst.m_Mem2K));
	uint64_t T = 0;
	if (!ReadNumLike(*C, "mode", T, OutError, 3))
	{
		return false;
	}
	Dst.m_Mode = uint8_t(T);
	if (!ReadNumLike(*C, "cpc", T, OutError, 0x03FF))
	{
		return false;
	}
	Dst.m_Cpc = uint16_t(T);
	if (!ReadNumLike(*C, "write_addr", T, OutError, 0x07FF))
	{
		return false;
	}
	Dst.m_WriteAddr = uint16_t(T);
	if (!ReadNumLike(*C, "data16_hi", T, OutError, UINT8_MAX))
	{
		return false;
	}
	Dst.m_Data16Hi = uint8_t(T);
	if (!ReadBool(*C, "running", Dst.m_bRunning, OutError) ||
		!ReadBool(*C, "restart_each_frame", Dst.m_bRestartEachFrame, OutError) ||
		!ReadBool(*C, "data16_have_hi", Dst.m_bData16HaveHi, OutError))
	{
		return false;
	}
	auto Deadline = [&](const char* Key, uint64_t& OutV) -> bool
	{
		const JsonValue* V = C->Find(Key);
		OutV = UINT64_MAX;
		if (!V || V->IsNull())
		{
			return true;
		}
		if (!V->TryAsUInt(OutV))
		{
			OutError = std::string("invalid copper.") + Key;
			return false;
		}
		return true;
	};
	if (!Deadline("next_fire_cycle", Dst.m_NextFireCycle) || !Deadline("frame_origin", Dst.m_FrameOrigin))
	{
		return false;
	}
	if (!ReadNumLike(*C, "moves", Dst.m_Moves, OutError) || !ReadNumLike(*C, "waits", Dst.m_Waits, OutError) ||
		!ReadNumLike(*C, "restarts", Dst.m_Restarts, OutError))
	{
		return false;
	}
	return true;
}

// The audio renderer's log and counters, so the resumed run's WAV continues
// the dumped run's (absent, or without events, in older dumps: nothing).
bool LoadAudioFromDump(const JsonValue& Root, AudioRenderer& Out, std::string& OutError)
{
	const JsonValue* A = Root.Find("audio");
	if (!A || !A->IsObject() || !A->Find("events_b64"))
	{
		return true;
	}
	std::string B64;
	std::vector<uint8_t> Packed;
	uint64_t Ay0 = 0, Ay1 = 0, Dac = 0, Beep = 0, Samples = 0, LastDac = 0, FirstDac = UINT64_MAX;
	const JsonValue* Fd = A->Find("first_dac_nanos");
	if (!ReadString(*A, "events_b64", B64, OutError) || (!B64.empty() && !DecodeBase64(B64, Packed, OutError)) ||
		!ReadNumLike(*A, "ay0_writes", Ay0, OutError) || !ReadNumLike(*A, "ay1_writes", Ay1, OutError) ||
		!ReadNumLike(*A, "dac_writes", Dac, OutError) || !ReadNumLike(*A, "beeper_writes", Beep, OutError) ||
		!ReadNumLike(*A, "samples_written", Samples, OutError) ||
		!ReadNumLike(*A, "last_dac_nanos", LastDac, OutError) || (Fd && !Fd->IsNull() && !Fd->TryAsUInt(FirstDac)))
	{
		if (OutError.empty())
		{
			OutError = "invalid audio.first_dac_nanos";
		}
		return false;
	}
	if (!Out.RestoreFromDump(Packed, Ay0, Ay1, Dac, Beep, FirstDac, LastDac, Samples))
	{
		OutError = "audio.events_b64 is not a whole number of events";
		return false;
	}
	return true;
}

bool LoadCaptureFromDump(const JsonValue& Root, DumpCaptureState& Out, std::string& OutError)
{
	const JsonValue* C = Root.Find("capture");
	if (!C || !C->IsObject())
	{
		return true;
	}
	const JsonValue* V = C->Find("video");
	if (V && V->IsObject())
	{
		Out.m_bVideo = true;
		if (!ReadNumLike(*V, "frames", Out.m_VideoFrames, OutError) ||
			!ReadNumLike(*V, "next_emit_cycle", Out.m_VideoNextEmit, OutError) ||
			!ReadNumLike(*V, "period_cycles", Out.m_VideoPeriod, OutError))
		{
			return false;
		}
	}
	const JsonValue* M = C->Find("metrics");
	if (M && M->IsObject())
	{
		Out.m_bMetrics = true;
		if (!ReadNumLike(*M, "frames", Out.m_MetricsFrames, OutError) ||
			!ReadNumLike(*M, "next_emit_cycle", Out.m_MetricsNextEmit, OutError) ||
			!ReadNumLike(*M, "period_cycles", Out.m_MetricsPeriod, OutError))
		{
			return false;
		}
		const JsonValue* H = M->Find("history");
		if (H && H->IsObject())
		{
			for (const auto& [K, Val] : H->m_O)
			{
				std::vector<uint8_t> Frame;
				if (!Val.IsString() || !DecodeBase64(Val.m_S, Frame, OutError))
				{
					if (OutError.empty())
					{
						OutError = "invalid capture.metrics.history." + K;
					}
					return false;
				}
				Out.m_MetricsHistory.push_back(std::move(Frame));
			}
		}
	}
	return true;
}

// The zxnDMA as WriteDma left it. An older dump (counters only) restores the
// counters and leaves the DMA at reset.
bool LoadDmaFromDump(const JsonValue& Root, ZxnDmaState& Out, std::string& OutError)
{
	const JsonValue* D = Root.Find("dma");
	if (!D || !D->IsObject())
	{
		return true;
	}
	if (!ReadNumLike(*D, "transfers", Out.m_Transfers, OutError) ||
		!ReadNumLike(*D, "bytes_transferred", Out.m_BytesTransferred, OutError) ||
		!ReadNumLike(*D, "unknown_bytes", Out.m_UnknownBytes, OutError) ||
		!ReadNumLike(*D, "last_transfer_cycle", Out.m_LastTransferCycle, OutError) ||
		!ReadNumLike(*D, "nested_arms", Out.m_NestedArms, OutError))
	{
		return false;
	}
	if (!D->Find("mode"))
	{
		return true;
	}
	uint64_t V = 0;
	auto Num = [&](const char* Key, uint64_t Max) -> bool
	{
		return ReadNumLike(*D, Key, V, OutError, Max);
	};
	if (!Num("mode", 3))
	{
		return false;
	}
	Out.m_Mode = ZxnDmaState::WaitMode(V);
	if (!Num("wr0_pending", 4))
	{
		return false;
	}
	Out.m_Wr0PendingBytes = uint8_t(V);
	if (!Num("wr4_pending", 2))
	{
		return false;
	}
	Out.m_Wr4PendingBytes = uint8_t(V);
	if (!Num("read_mask", UINT8_MAX))
	{
		return false;
	}
	Out.m_ReadMask = uint8_t(V);
	if (!Num("port_a", UINT16_MAX))
	{
		return false;
	}
	Out.m_PortASrcAddr = uint16_t(V);
	if (!Num("port_b", UINT16_MAX))
	{
		return false;
	}
	Out.m_PortBDestAddr = uint16_t(V);
	if (!Num("block_length", UINT16_MAX))
	{
		return false;
	}
	Out.m_BlockLength = uint16_t(V);
	if (!Num("bytes_remaining", UINT32_MAX))
	{
		return false;
	}
	Out.m_BytesRemaining = uint32_t(V);
	if (!Num("cur_src", UINT16_MAX))
	{
		return false;
	}
	Out.m_CurSrcAddr = uint16_t(V);
	if (!Num("cur_dst", UINT16_MAX))
	{
		return false;
	}
	Out.m_CurDstAddr = uint16_t(V);
	return ReadHex8ArrayN(*D, "wr0_buffer", Out.m_Wr0Buffer, 4, OutError) &&
		   ReadHex8ArrayN(*D, "wr4_buffer", Out.m_Wr4Buffer, 2, OutError) &&
		   ReadBool(*D, "port_a_fixed", Out.m_bPortAFixed, OutError) &&
		   ReadBool(*D, "port_a_dec", Out.m_bPortADec, OutError) &&
		   ReadBool(*D, "port_b_inc", Out.m_bPortBInc, OutError) &&
		   ReadBool(*D, "port_b_dec", Out.m_bPortBDec, OutError) &&
		   ReadBool(*D, "port_b_io", Out.m_bPortBIo, OutError) &&
		   ReadBool(*D, "continuous", Out.m_bContinuousMode, OutError) &&
		   ReadBool(*D, "stop_at_end", Out.m_bStopAtEnd, OutError) &&
		   ReadBool(*D, "loaded", Out.m_bLoaded, OutError) &&
		   ReadBool(*D, "force_ready", Out.m_bForceReady, OutError);
}

// MemoryBus::ResumeExtras from the dump's bus block (absent in older dumps).
bool LoadBusExtrasFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* B = Root.Find("bus");
	if (!B || !B->IsObject())
	{
		return true;
	}
	MemoryBus::ResumeExtras X;
	uint64_t V = 0;
	auto Byte = [&](const char* Key, uint8_t& Dst) -> bool
	{
		if (!ReadNumLike(*B, Key, V, OutError, UINT8_MAX))
		{
			return false;
		}
		Dst = uint8_t(V);
		return true;
	};
	uint8_t Set[4] = {};
	std::string Cmos;
	if (!Byte("port_123b", X.m_Port123B) ||
		!Byte("layer2_offset", X.m_Layer2Offset) ||
		!ReadNumLike(*B, "layer2_window_opens", X.m_Layer2WindowOpens, OutError) ||
		!ReadBool(*B, "l2_state_explicit", X.m_bL2StateExplicit, OutError) ||
		!ReadHex8ArrayN(*B, "clip_windows", &X.m_ClipWindow[0][0], 16, OutError) ||
		!ReadHex8ArrayN(*B, "clip_index", X.m_ClipIndex, 4, OutError) ||
		!ReadHex8ArrayN(*B, "clip_set", Set, 4, OutError) || !Byte("scld_mode", X.m_ScldMode) ||
		!Byte("next_cpu_speed", X.m_NextCpuSpeed) || !ReadHex8ArrayN(*B, "evo_pal", X.m_EvoPal, 16, OutError) ||
		!Byte("evo_pal_idx", X.m_EvoPalIdx) || !ReadString(*B, "cmos_b64", Cmos, OutError) ||
		!Byte("uno_core_id_idx", X.m_UnoCoreIdIdx) || !Byte("uno_radas_off_phase", X.m_UnoRadasOffPhase) ||
		!Byte("ulaplus_reg", X.m_UlaPlusReg) ||
		!ReadHex8ArrayN(*B, "dock_kind", X.m_DockKind.data(), X.m_DockKind.size(), OutError))
	{
		return false;
	}
	for (int I = 0; I < 4; ++I)
	{
		X.m_bClipSet[I] = Set[I] != 0;
	}
	if (B->Find("frame_int_phase") && !ReadNumLike(*B, "frame_int_phase", X.m_FrameIntPhase, OutError))
	{
		return false;
	}
	std::vector<uint8_t> Buf;
	if (!DecodeBase64(Cmos, Buf, OutError))
	{
		return false;
	}
	if (Buf.size() != sizeof X.m_Cmos)
	{
		OutError = "bus.cmos_b64 must decode to 64 bytes";
		return false;
	}
	memcpy(X.m_Cmos, Buf.data(), Buf.size());
	std::string Dock;
	if (ReadOptString(*B, "dock_ram_b64", Dock) && !Dock.empty() && !DecodeBase64(Dock, X.m_DockRam, OutError))
	{
		return false;
	}
	auto Hist = [&](const char* Key, uint64_t* H) -> bool
	{
		const JsonValue* O = B->Find(Key);
		if (!O || !O->IsObject())
		{
			return true;
		}
		for (const auto& [K, Val] : O->m_O)
		{
			const unsigned long R = strtoul(K.c_str(), nullptr, 16);
			if (R > 255 || !Val.TryAsUInt(H[R]))
			{
				OutError = std::string("invalid bus.") + Key + "." + K;
				return false;
			}
		}
		return true;
	};
	auto Ports = [&](const char* Key, std::map<uint16_t, uint64_t>& P) -> bool
	{
		const JsonValue* O = B->Find(Key);
		if (!O || !O->IsObject())
		{
			return true;
		}
		for (const auto& [K, Val] : O->m_O)
		{
			const unsigned long Port = strtoul(K.c_str(), nullptr, 16);
			uint64_t N = 0;
			if (Port > 0xFFFF || !Val.TryAsUInt(N))
			{
				OutError = std::string("invalid bus.") + Key + "." + K;
				return false;
			}
			P[uint16_t(Port)] = N;
		}
		return true;
	};
	const JsonValue* Mmu = B->Find("last_mmu_write");
	if (Mmu && Mmu->IsObject())
	{
		for (int S = 0; S < 8; ++S)
		{
			char K[8];
			snprintf(K, sizeof K, "slot_%d", S);
			std::string V;
			unsigned long long Cycle = 0;
			unsigned Pc = 0, Old = 0;
			if (!ReadString(*Mmu, K, V, OutError) || sscanf(V.c_str(), "%llu %x %x", &Cycle, &Pc, &Old) != 3)
			{
				if (OutError.empty())
				{
					OutError = std::string("invalid bus.last_mmu_write.") + K;
				}
				return false;
			}
			X.m_MmuWriteCycle[S] = Cycle;
			X.m_MmuWritePc[S] = uint16_t(Pc);
			X.m_MmuWriteOld[S] = uint8_t(Old);
		}
	}
	if (!Hist("nextreg_write_hist", X.m_NextRegWriteHist) || !Hist("nextreg_read_hist", X.m_NextRegReadHist) ||
		!Ports("unhandled_port_reads", X.m_UnhandledPortReads) ||
		!Ports("unhandled_port_writes", X.m_UnhandledPortWrites))
	{
		return false;
	}
	Out.ImportResumeExtras(X);
	return true;
}

// The base slot table as the dump recorded it. Runs after every other loader:
// the port and NextReg restores re-derive a mapping on their way, and this is
// the mapping that was actually in effect. Older dumps without it keep the
// re-derived one.
bool LoadSlotsFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* M = Root.Find("memory");
	const JsonValue* Slots = (M && M->IsObject()) ? M->Find("base_slots") : nullptr;
	if (!Slots || !Slots->IsObject())
	{
		return true;
	}
	for (int S = 0; S < 8; ++S)
	{
		char Key[8];
		snprintf(Key, sizeof Key, "slot_%d", S);
		std::string V;
		if (!ReadString(*Slots, Key, V, OutError))
		{
			return false;
		}
		MemoryBus::SlotDesc D;
		char Store[8] = {};
		unsigned Page = 0;
		char W[4] = {};
		const int N = sscanf(V.c_str(), "%7s %u %3s", Store, &Page, W);
		const std::string Kind = Store;
		if (Kind == "open" && N == 1)
		{
			D.m_Store = MemoryBus::SlotStore::kOpenBus;
		}
		else if (N >= 2 && (Kind == "ram" || Kind == "rom" || Kind == "dock") && Page <= 0xFFFF)
		{
			D.m_Store = Kind == "ram"	? MemoryBus::SlotStore::kRam
						: Kind == "rom" ? MemoryBus::SlotStore::kRom
										: MemoryBus::SlotStore::kDock;
			D.m_Page = uint16_t(Page);
			D.m_bWritable = (N == 3 && std::string(W) == "w");
		}
		else
		{
			OutError = std::string("invalid memory.base_slots.") + Key + ": " + V;
			return false;
		}
		if (!Out.RestoreBaseSlot(S, D))
		{
			OutError = std::string("memory.base_slots.") + Key + " is outside its store: " + V;
			return false;
		}
	}
	Out.ApplyRestoredSlots();
	return true;
}

bool LoadSchedulerFromDump(const JsonValue& Root, Z80State& Out, std::string& OutError)
{
	const JsonValue* S = nullptr;
	if (!ExpectObj(Root, "scheduler", S, OutError))
	{
		return false;
	}
	auto CycleField = [&](const char* K, uint64_t& OutDst) -> bool
	{
		const JsonValue* V = S->Find(K);
		if (!V || V->IsNull())
		{
			OutDst = UINT64_MAX;
			return true;
		}
		if (V->TryAsUInt(OutDst))
		{
			return true;
		}
		OutError = std::string("expected number or null for: ") + K;
		return false;
	};
	if (!CycleField("next_frame_int_cycle", Out.m_NextFrameIntCycle))
	{
		return false;
	}
	if (!CycleField("next_psg_tick_cycle", Out.m_NextPsgTickCycle))
	{
		return false;
	}
	if (!CycleField("next_stats_tick_cycle", Out.m_NextStatsTickCycle))
	{
		return false;
	}
	if (!CycleField("next_event_cycle", Out.m_NextEventCycle))
	{
		return false;
	}
	uint64_t T;
	if (S->Find("frame_int_period"))
	{
		if (!ReadNumLike(*S, "frame_int_period", T, OutError))
		{
			return false;
		}
		Out.m_FrameIntPeriod = T;
	}
	if (S->Find("psg_tick_period"))
	{
		if (!ReadNumLike(*S, "psg_tick_period", T, OutError))
		{
			return false;
		}
		Out.m_PsgTickPeriod = T;
	}
	if (S->Find("stats_tick_period"))
	{
		if (!ReadNumLike(*S, "stats_tick_period", T, OutError))
		{
			return false;
		}
		Out.m_StatsTickPeriod = T;
	}
	return true;
}

bool LoadStatsFromDump(const JsonValue& Root, Z80State& Out, std::string& OutError)
{
	const JsonValue* S = nullptr;
	if (!ExpectObj(Root, "stats", S, OutError))
	{
		return false;
	}
	uint64_t T;
	if (!ReadNumLike(*S, "cycles_total", T, OutError))
	{
		return false;
	}
	Out.m_Cycles = T;
	if (S->Find("halt_cycles_total"))
	{
		if (!ReadNumLike(*S, "halt_cycles_total", T, OutError))
		{
			return false;
		}
		Out.m_HaltCycles = T;
	}
	// last_di_cycles may be null
	const JsonValue* kLD = S->Find("last_di_cycles");
	if (kLD && kLD->IsNumber())
	{
		if (!kLD->TryAsUInt(Out.m_LastDiCycles))
		{
			OutError = "invalid last_di_cycles";
			return false;
		}
	}
	else
	{
		Out.m_LastDiCycles = UINT64_MAX;
	}
	if (S->Find("last_stats_snapshot_halt"))
	{
		if (!ReadNumLike(*S, "last_stats_snapshot_halt", T, OutError))
		{
			return false;
		}
		Out.m_LastStatsSnapshotHalt = T;
	}
	const JsonValue* kPFH = S->Find("per_frame_halt");
	if (kPFH && kPFH->IsArray())
	{
		Out.m_PerFrameHalt.clear();
		Out.m_PerFrameHalt.reserve(kPFH->m_A.size());
		for (const JsonValue& V : kPFH->m_A)
		{
			if (V.IsNumber())
			{
				uint64_t Value;
				if (!V.TryAsUInt(Value) || Value > UINT32_MAX)
				{
					OutError = "invalid per_frame_halt entry";
					return false;
				}
				Out.m_PerFrameHalt.push_back(uint32_t(Value));
			}
		}
	}
	return true;
}

// --- Next / dot-cmd / VFS load side. All three tolerate a missing or null
// block so existing pre-Phase-E dumps still resume cleanly.

bool LoadNextFromDump(const JsonValue& Root, MemoryBus& Out, std::string& OutError)
{
	const JsonValue* N = Root.Find("next");
	if (!N || N->IsNull())
	{
		return true;
	}
	if (!N->IsObject())
	{
		OutError = "next must be object or null";
		return false;
	}

	bool bIsNext = false;
	if (!ReadOptionalBool(*N, "is_next", bIsNext, OutError))
	{
		return false;
	}
	Out.SetIsNext(bIsNext);
	if (!bIsNext)
	{
		return true;
	}
	uint64_t LastSel = 0, NRW = 0, PalW = 0, Parity = 0;
	if (!ReadOptionalNum(*N, "last_selected_nextreg", LastSel, OutError, UINT8_MAX))
	{
		return false;
	}
	if (!ReadOptionalNum(*N, "nextreg_writes", NRW, OutError))
	{
		return false;
	}
	if (!ReadOptionalNum(*N, "palette_writes", PalW, OutError))
	{
		return false;
	}
	if (!ReadOptionalNum(*N, "palette_write_parity", Parity, OutError, 1))
	{
		return false;
	}
	bool bL2Touched = false;
	if (!ReadOptionalBool(*N, "any_l2_touched", bL2Touched, OutError))
	{
		return false;
	}

	uint64_t Words[4] = {0, 0, 0, 0};
	const JsonValue* kAM = N->Find("allocated_pages_8k_mask");
	if (kAM && kAM->IsString())
	{
		// 64-char hex string (256 bits): W3 W2 W1 W0 (highest word first).
		// Older dumps: 32 chars (128 bits) -> W3=W2=0, parse high-low as W1 W0.
		// Older older: 16 chars (64 bits) -> only W0 set.
		std::string S = kAM->m_S;
		if (!S.empty() && S[0] == '$')
		{
			S.erase(0, 1);
		}
		auto ParseChunk = [&](const std::string& C, uint64_t& OutDst) -> bool
		{
			return ParseHex64Quoted("$" + C, OutDst);
		};
		if (S.size() == 64)
		{
			if (!ParseChunk(S.substr(0, 16), Words[3]) || !ParseChunk(S.substr(16, 16), Words[2]) ||
				!ParseChunk(S.substr(32, 16), Words[1]) || !ParseChunk(S.substr(48, 16), Words[0]))
			{
				OutError = "bad 256-bit alloc mask";
				return false;
			}
		}
		else if (S.size() == 32)
		{
			if (!ParseChunk(S.substr(0, 16), Words[1]) || !ParseChunk(S.substr(16, 16), Words[0]))
			{
				OutError = "bad 128-bit alloc mask";
				return false;
			}
		}
		else
		{
			if (!ParseChunk(S, Words[0]))
			{
				OutError = "bad alloc mask";
				return false;
			}
		}
	}

	uint8_t Regs[256] = {};
	std::string NrB64;
	if (ReadOptString(*N, "nextregs_b64", NrB64) && !NrB64.empty())
	{
		std::vector<uint8_t> Buf;
		if (!DecodeBase64(NrB64, Buf, OutError))
		{
			return false;
		}
		if (Buf.size() != 256)
		{
			OutError = "nextregs_b64 must decode to 256 bytes";
			return false;
		}
		for (int IdxI = 0; IdxI < 256; ++IdxI)
		{
			Regs[IdxI] = Buf[IdxI];
		}
	}

	Out.RestoreNextRegsFromDump(
		Regs, uint8_t(LastSel), uint8_t(Parity), bL2Touched, Words[0], Words[1], Words[2], Words[3], NRW, PalW);

	// Palettes (optional sub-object; per-key entry).
	const JsonValue* kPG = N->Find("palettes");
	if (kPG && kPG->IsObject())
	{
		char Key[8];
		for (int P = 0; P < 8; ++P)
		{
			snprintf(Key, sizeof(Key), "pal_%d", P);
			std::string B64;
			if (!ReadOptString(*kPG, Key, B64) || B64.empty())
			{
				continue;
			}
			std::vector<uint8_t> Buf;
			if (!DecodeBase64(B64, Buf, OutError))
			{
				return false;
			}
			if (Buf.size() != 512)
			{
				OutError = std::string("palettes.") + Key + " must decode to 512 bytes";
				return false;
			}
			memcpy(Out.MutablePalette(P), Buf.data(), 512);
		}
	}
	return true;
}

bool LoadSpritesFromDump(const JsonValue& Root, SpritesState& Out, std::string& OutError)
{
	const JsonValue* S = Root.Find("sprites");
	if (!S || S->IsNull())
	{
		return true; // a classic machine, or a dump from before the block existed
	}
	if (!S->IsObject())
	{
		OutError = "sprites must be object or null";
		return false;
	}
	uint64_t V = 0;
	if (!ReadOptionalNum(*S, "slot_selects", V, OutError))
	{
		return false;
	}
	Out.m_SlotSelects = V;
	V = 0;
	if (!ReadOptionalNum(*S, "attr_writes", V, OutError))
	{
		return false;
	}
	Out.m_AttrWrites = V;
	V = 0;
	if (!ReadOptionalNum(*S, "pattern_writes", V, OutError))
	{
		return false;
	}
	Out.m_PatternWrites = V;
	V = 0;
	if (!ReadOptionalNum(*S, "attr_slot", V, OutError, 127))
	{
		return false;
	}
	Out.m_AttrSlot = uint8_t(V);
	V = 0;
	if (!ReadOptionalNum(*S, "attr_byte_idx", V, OutError, 4))
	{
		return false;
	}
	Out.m_AttrByteIdx = uint8_t(V);
	V = 0;
	if (!ReadOptionalNum(*S, "pattern_slot", V, OutError, 63))
	{
		return false;
	}
	Out.m_PatternSlot = uint8_t(V);
	V = 0;
	if (!ReadOptionalNum(*S, "pattern_byte_idx", V, OutError, 255))
	{
		return false;
	}
	Out.m_PatternByteIdx = uint8_t(V);

	std::string B64;
	if (ReadOptString(*S, "attrs_b64", B64) && !B64.empty())
	{
		std::vector<uint8_t> Buf;
		if (!DecodeBase64(B64, Buf, OutError))
		{
			return false;
		}
		if (Buf.size() != 128 * 5)
		{
			OutError = "sprites.attrs_b64 must decode to 640 bytes";
			return false;
		}
		for (int IdxI = 0; IdxI < 128; ++IdxI)
		{
			memcpy(Out.m_Slots[IdxI].m_Attr.m_Bytes, Buf.data() + IdxI * 5, 5);
		}
	}
	B64.clear();
	if (ReadOptString(*S, "patterns_b64", B64) && !B64.empty())
	{
		std::vector<uint8_t> Buf;
		if (!DecodeBase64(B64, Buf, OutError))
		{
			return false;
		}
		if (Buf.size() != 64 * 256)
		{
			OutError = "sprites.patterns_b64 must decode to 16384 bytes";
			return false;
		}
		for (int IdxI = 0; IdxI < 64; ++IdxI)
		{
			memcpy(Out.m_Slots[IdxI].m_Pattern, Buf.data() + IdxI * 256, 256);
		}
	}
	return true;
}

bool LoadDotCmdFromDump(const JsonValue& Root, ParsedDotCmdInfo& Out, std::string& OutError)
{
	const JsonValue* D = Root.Find("dot_cmd");
	if (!D || D->IsNull())
	{
		Out.m_bPresent = false;
		return true;
	}
	if (!D->IsObject())
	{
		OutError = "dot_cmd must be object or null";
		return false;
	}

	Out.m_bPresent = true;
	if (!ReadString(*D, "path", Out.m_Path, OutError))
	{
		return false;
	}
	ReadOptString(*D, "arg", Out.m_ArgString);
	uint64_t T = 0;
	if (D->Find("load_addr"))
	{
		if (!ReadNumLike(*D, "load_addr", T, OutError, UINT16_MAX))
		{
			return false;
		}
		Out.m_LoadAddr = uint16_t(T);
	}
	if (D->Find("binary_size"))
	{
		if (!ReadNumLike(*D, "binary_size", T, OutError))
		{
			return false;
		}
		Out.m_BinarySize = T;
	}
	const JsonValue* kCP = D->Find("code_8k_page");
	if (kCP && kCP->IsNumber())
	{
		int64_t Page;
		if (!kCP->TryAsInt(Page) || Page < -1 || Page > 255)
		{
			OutError = "invalid code8k_page";
			return false;
		}
		Out.m_Code8KPage = int(Page);
	}
	return true;
}

bool LoadVfsFromDump(const JsonValue& Root, ParsedVfsInfo& Out, std::string& OutError)
{
	const JsonValue* V = Root.Find("vfs");
	if (!V || V->IsNull())
	{
		Out.m_bPresent = false;
		return true;
	}
	if (!V->IsObject())
	{
		OutError = "vfs must be object or null";
		return false;
	}

	Out.m_bPresent = true;
	if (!ReadString(*V, "root_path", Out.m_RootPath, OutError))
	{
		return false;
	}
	std::string D;
	if (ReadOptString(*V, "drive", D) && !D.empty())
	{
		Out.m_Drive = D[0];
	}
	const JsonValue* S = V->Find("stats");
	if (S && S->IsObject())
	{
		uint64_t T = 0;
		if (S->Find("files_opened"))
		{
			if (!ReadNumLike(*S, "files_opened", T, OutError))
			{
				return false;
			}
			Out.m_FilesOpened = T;
		}
		if (S->Find("bytes_read"))
		{
			if (!ReadNumLike(*S, "bytes_read", T, OutError))
			{
				return false;
			}
			Out.m_BytesRead = T;
		}
		if (S->Find("bytes_written"))
		{
			if (!ReadNumLike(*S, "bytes_written", T, OutError))
			{
				return false;
			}
			Out.m_BytesWritten = T;
		}
		if (S->Find("ide_bank_allocs"))
		{
			if (!ReadNumLike(*S, "ide_bank_allocs", T, OutError))
			{
				return false;
			}
			Out.m_IdeBankAllocs = T;
		}
		if (S->Find("ide_bank_frees"))
		{
			if (!ReadNumLike(*S, "ide_bank_frees", T, OutError))
			{
				return false;
			}
			Out.m_IdeBankFrees = T;
		}
		if (S->Find("rst08_calls"))
		{
			if (!ReadNumLike(*S, "rst08_calls", T, OutError))
			{
				return false;
			}
			Out.m_Rst08Calls = T;
		}
		if (S->Find("rst08_unknown"))
		{
			if (!ReadNumLike(*S, "rst08_unknown", T, OutError))
			{
				return false;
			}
			Out.m_Rst08Unknown = T;
		}
	}
	return true;
}

// The VFS as WriteVfs recorded it, for NextZxosVfs::ImportResumeState. False
// with OutbPresent false (and no error) when the dump predates the resume
// fields; host paths come back resolved against the dump's directory.
bool LoadVfsResumeFromDump(const JsonValue& Root, const std::string& DumpPath, NextZxosVfs::ResumeState& Out,
	bool& OutbPresent, std::string& OutRootPath, char& OutDrive, std::string& OutError)
{
	OutbPresent = false;
	const JsonValue* V = Root.Find("vfs");
	if (!V || !V->IsObject() || !V->Find("cwd"))
	{
		return true;
	}
	std::string Root_, Drive;
	if (!ReadString(*V, "root_path", Root_, OutError) || !ReadString(*V, "cwd", Out.m_Cwd, OutError))
	{
		return false;
	}
	OutRootPath = ResolveRefPathFromDump(Root_, DumpPath);
	OutDrive = (ReadOptString(*V, "drive", Drive) && !Drive.empty()) ? Drive[0] : 'C';
	uint64_t T = 0;
	if (!ReadNumLike(*V, "default_drive_id", T, OutError, UINT8_MAX))
	{
		return false;
	}
	Out.m_DefaultDriveId = uint8_t(T);
	if (!ReadBool(*V, "read_only", Out.m_bReadOnly, OutError) ||
		!ReadBool(*V, "use_ix_buffers", Out.m_bUseIxForBuffers, OutError))
	{
		return false;
	}
	const JsonValue* S = V->Find("stats");
	if (S && S->IsObject())
	{
		if (!ReadNumLike(*S, "files_opened", Out.m_FilesOpened, OutError) ||
			!ReadNumLike(*S, "bytes_read", Out.m_BytesRead, OutError) ||
			!ReadNumLike(*S, "bytes_written", Out.m_BytesWritten, OutError) ||
			!ReadNumLike(*S, "ide_bank_allocs", Out.m_IdeBankAllocs, OutError) ||
			!ReadNumLike(*S, "ide_bank_frees", Out.m_IdeBankFrees, OutError) ||
			!ReadNumLike(*S, "dir_opens", Out.m_DirOpens, OutError) ||
			!ReadNumLike(*S, "rst08_calls", Out.m_Rst08Calls, OutError) ||
			!ReadNumLike(*S, "rst08_unknown", Out.m_Rst08Unknown, OutError) ||
			!ReadNumLike(*S, "read_cycles_charged", Out.m_ReadCyclesCharged, OutError))
		{
			return false;
		}
		for (const auto& [Key, Map] :
			{std::pair<const char*, std::map<uint8_t, uint64_t>*>{"rst08_by_code", &Out.m_Rst08ByCode},
				std::pair<const char*, std::map<uint8_t, uint64_t>*>{"rst08_unknown_by_code", &Out.m_Rst08UnknownByCode}})
		{
			const JsonValue* O = S->Find(Key);
			if (!O || !O->IsObject())
			{
				continue;
			}
			for (const auto& [K, Val] : O->m_O)
			{
				const unsigned long Code = strtoul(K.c_str(), nullptr, 16);
				uint64_t N = 0;
				if (Code > 255 || !Val.TryAsUInt(N))
				{
					OutError = std::string("invalid vfs.stats.") + Key + "." + K;
					return false;
				}
				(*Map)[uint8_t(Code)] = N;
			}
		}
	}
	const JsonValue* H = V->Find("open_handles");
	if (H && H->IsObject())
	{
		for (const auto& [K, HV] : H->m_O)
		{
			NextZxosVfs::ResumeHandle R;
			R.m_Slot = atoi(K.c_str() + 1);
			std::string Resolved;
			int64_t Pos = 0;
			const JsonValue* P = HV.Find("position");
			if (!ReadString(HV, "opened_as", R.m_OpenedAs, OutError) ||
				!ReadString(HV, "resolved_path", Resolved, OutError) || !P || !P->TryAsInt(Pos) ||
				!ReadBool(HV, "writable", R.m_bWritable, OutError) ||
				!ReadOptionalBool(HV, "dir", R.m_bDir, OutError))
			{
				if (OutError.empty())
				{
					OutError = "invalid vfs.open_handles." + K;
				}
				return false;
			}
			R.m_ResolvedPath = ResolveRefPathFromDump(Resolved, DumpPath);
			R.m_Position = long(Pos);
			if (R.m_bDir)
			{
				uint64_t DirPos = 0;
				if (!ReadNumLike(HV, "dir_pos", DirPos, OutError))
				{
					return false;
				}
				R.m_DirPos = size_t(DirPos);
				const JsonValue* L = HV.Find("listing");
				if (L && L->IsObject())
				{
					for (const auto& [EK, EV] : L->m_O)
					{
						VfsDirEntry E;
						uint64_t Size = 0;
						if (!ReadString(EV, "name", E.m_Name, OutError) || !ReadBool(EV, "dir", E.m_bDir, OutError) ||
							!ReadBool(EV, "read_only", E.m_bReadOnly, OutError) ||
							!ReadNumLike(EV, "size", Size, OutError, UINT32_MAX))
						{
							return false;
						}
						E.m_Size = uint32_t(Size);
						R.m_Dir.push_back(std::move(E));
					}
				}
			}
			Out.m_Handles.push_back(std::move(R));
		}
	}
	const JsonValue* F = V->Find("f_read");
	if (F && F->IsObject())
	{
		std::string Buf;
		if (!ReadBool(*F, "active", Out.m_bFReadActive, OutError) || !ReadNumLike(*F, "total", T, OutError, UINT16_MAX))
		{
			return false;
		}
		Out.m_FReadTotal = uint16_t(T);
		if (!ReadNumLike(*F, "done", T, OutError, UINT16_MAX))
		{
			return false;
		}
		Out.m_FReadDone = uint16_t(T);
		if (!ReadNumLike(*F, "dest", T, OutError, UINT16_MAX))
		{
			return false;
		}
		Out.m_FReadDest = uint16_t(T);
		if (!ReadNumLike(*F, "isr_depth", T, OutError, UINT8_MAX))
		{
			return false;
		}
		Out.m_FReadIsrDepth = uint8_t(T);
		if (!ReadBool(*F, "use_ix", Out.m_bFReadUseIx, OutError) ||
			!ReadNumLike(*F, "total_charge", Out.m_FReadTotalCharge, OutError) ||
			!ReadNumLike(*F, "charged_so_far", Out.m_FReadChargedSoFar, OutError) ||
			!ReadString(*F, "buf_b64", Buf, OutError) || (!Buf.empty() && !DecodeBase64(Buf, Out.m_FReadBuf, OutError)))
		{
			return false;
		}
	}
	OutbPresent = true;
	return true;
}
