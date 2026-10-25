#include <inspeccy/version.h>
#include "status.h"

#include "memory.h"

#include <algorithm>
#include <array>
#include <math.h>
#include <stdio.h>

void FillVramAnalysis(const MemoryBus& Mem, StatusReport& Out)
{
	const uint8_t Port7FFD = Mem.GetPort7FFD();
	Out.m_DisplayedBank = (Port7FFD & 0x08) ? 7 : 5;
	const uint8_t* Vram = Mem.Ram16KBank(Out.m_DisplayedBank);

	// Pixel area: $0000..$17FF (6144 bytes), Attr area: $1800..$1AFF (768 bytes).
	Out.m_FirstPixelByte = Vram[0];
	bool bAllZero = (Vram[0] == 0);
	bool bAllEqual = true;
	for (int IdxI = 1; IdxI < 6144; ++IdxI)
	{
		if (Vram[IdxI] != 0)
		{
			bAllZero = false;
		}
		if (Vram[IdxI] != Vram[0])
		{
			bAllEqual = false;
		}
		if (!bAllZero && !bAllEqual)
		{
			break;
		}
	}
	Out.m_bPixelsAllZero = bAllZero;
	Out.m_bPixelsAllEqual = bAllEqual;

	Out.m_FirstAttrByte = Vram[0x1800];
	bool bAttrsAllEqual = true;
	std::array<bool, 256> bSeen{};
	int Distinct = 0;
	for (int IdxI = 0; IdxI < 768; ++IdxI)
	{
		const uint8_t A = Vram[0x1800 + IdxI];
		if (A != Vram[0x1800])
		{
			bAttrsAllEqual = false;
		}
		if (!bSeen[A])
		{
			bSeen[A] = true;
			++Distinct;
		}
	}
	Out.m_bAttrsAllEqual = bAttrsAllEqual;
	Out.m_DistinctAttrs = Distinct;
	Out.m_bUniformScreen = Out.m_bPixelsAllEqual && Out.m_bAttrsAllEqual;
	Out.m_BorderColour = Mem.GetBorderColour();

	Out.m_PixelWritesBank5 = Mem.GetVramPixelWritesBank5();
	Out.m_AttrWritesBank5 = Mem.GetVramAttrWritesBank5();
	Out.m_PixelWritesBank7 = Mem.GetVramPixelWritesBank7();
	Out.m_AttrWritesBank7 = Mem.GetVramAttrWritesBank7();
	Out.m_CurrentPort7FFD = Port7FFD;
	Out.m_WritesTo7FFD = Mem.GetPort7FFDWrites();
}

namespace
{

// Only the helpers actually used at top level survive: AppendUint and
// AppendDouble. Removed AppendString / AppendInt / AppendBool /
// AppendHex16 / AppendNull (clang -Wunused-function in anon-namespace).

void AppendUint(std::string& Out, const char* Key, uint64_t Value, bool bComma)
{
	char Buf[64];
	snprintf(Buf, sizeof(Buf), "  \"%s\": %llu%s\n", Key, static_cast<unsigned long long>(Value), bComma ? "," : "");
	Out += Buf;
}

void AppendDouble(std::string& Out, const char* Key, double Value, bool bComma)
{
	char Buf[64];
	snprintf(Buf, sizeof(Buf), "  \"%s\": %.2f%s\n", Key, Value, bComma ? "," : "");
	Out += Buf;
}

// Object opener / closer. The caller manages the trailing-comma logic.
void OpenObject(std::string& Out, const char* Key)
{
	Out += "  \"";
	Out += Key;
	Out += "\": {\n";
}
void CloseObject(std::string& Out, bool bComma)
{
	if (Out.size() >= 2 && Out[Out.size() - 2] == ',')
	{
		Out.erase(Out.size() - 2, 1); // drop the trailing comma inside the object
	}
	Out += "  }";
	if (bComma)
	{
		Out += ',';
	}
	Out += '\n';
}

// Slightly more indented helpers for inside an object.
void IndentAppendUint(std::string& Out, const char* Key, uint64_t Value, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "    \"%s\": %llu%s\n", Key, (unsigned long long)Value, bComma ? "," : "");
	Out += Buf;
}
void IndentAppendInt(std::string& Out, const char* Key, int64_t Value, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "    \"%s\": %lld%s\n", Key, (long long)Value, bComma ? "," : "");
	Out += Buf;
}
void IndentAppendBool(std::string& Out, const char* Key, bool bValue, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "    \"%s\": %s%s\n", Key, bValue ? "true" : "false", bComma ? "," : "");
	Out += Buf;
}
void IndentAppendHex(std::string& Out, const char* Key, uint32_t Value, int Width, bool bComma)
{
	char Buf[96];
	if (Width == 4)
	{
		snprintf(Buf, sizeof(Buf), "    \"%s\": \"$%04X\"%s\n", Key, Value, bComma ? "," : "");
	}
	else
	{
		snprintf(Buf, sizeof(Buf), "    \"%s\": \"$%02X\"%s\n", Key, Value, bComma ? "," : "");
	}
	Out += Buf;
}
void IndentAppendString(std::string& Out, const char* Key, const std::string& Value, bool bComma)
{
	Out += "    \"";
	Out += Key;
	Out += "\": ";
	if (Value.empty())
	{
		Out += "null";
	}
	else
	{
		Out += '"';
		for (char C : Value)
		{
			if (C == '"' || C == '\\')
			{
				Out += '\\';
			}
			Out += (C == '\\') ? '\\' : C;
		}
		Out += '"';
	}
	if (bComma)
	{
		Out += ',';
	}
	Out += '\n';
}
void IndentAppendDouble(std::string& Out, const char* Key, double Value, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "    \"%s\": %.2f%s\n", Key, Value, bComma ? "," : "");
	Out += Buf;
}
void IndentOpenObject(std::string& Out, const char* Key)
{
	Out += "    \"";
	Out += Key;
	Out += "\": {\n";
}
void IndentCloseObject(std::string& Out, bool bComma)
{
	// Drop trailing comma on the LAST key inside the inner object (we wrote
	// it inside the inner object). The inner object's lines start with 6
	// spaces; outer with 4. We only drop on this 'inner' close.
	if (Out.size() >= 2 && Out[Out.size() - 2] == ',')
	{
		Out.erase(Out.size() - 2, 1);
	}
	Out += "    }";
	if (bComma)
	{
		Out += ',';
	}
	Out += '\n';
}
void InnerAppendUint(std::string& Out, const char* Key, uint64_t Value, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "      \"%s\": %llu%s\n", Key, (unsigned long long)Value, bComma ? "," : "");
	Out += Buf;
}
void InnerAppendDouble(std::string& Out, const char* Key, double Value, bool bComma)
{
	char Buf[96];
	snprintf(Buf, sizeof(Buf), "      \"%s\": %.2f%s\n", Key, Value, bComma ? "," : "");
	Out += Buf;
}

uint64_t Median(const std::vector<uint32_t>& V)
{
	if (V.empty())
	{
		return 0;
	}
	std::vector<uint32_t> Copy = V;
	const size_t Mid = Copy.size() / 2;
	std::nth_element(Copy.begin(), Copy.begin() + Mid, Copy.end());
	return Copy[Mid];
}

double MeanU32(const std::vector<uint32_t>& V)
{
	if (V.empty())
	{
		return 0.0;
	}
	uint64_t Sum = 0;
	for (uint32_t X : V)
	{
		Sum += X;
	}
	return double(Sum) / double(V.size());
}

} // namespace

bool WriteStatusJson(const StatusReport& R, const char* Path, std::string& OutError)
{
	std::string J;
	J.reserve(2048);
	J += "{\n";

	AppendUint(J, "version", 1, true);
	J += "  \"tool_version\": \"" INSPECCY_VERSION "\",\n";

	// exit
	OpenObject(J, "exit");
	IndentAppendString(J, "result", R.m_ResultName, true);
	IndentAppendInt(J, "exit_code", R.m_ExitCode, true);
	IndentAppendHex(J, "last_pc", R.m_LastPC, 4, true);
	IndentAppendHex(J, "last_op", R.m_LastOpcode, 2, R.m_DiToHaltCycles != UINT64_MAX);
	if (R.m_DiToHaltCycles != UINT64_MAX)
	{
		IndentAppendUint(J, "di_to_halt_cycles", R.m_DiToHaltCycles, false);
	}
	CloseObject(J, true);

	// cycles
	OpenObject(J, "cycles");
	const uint64_t Executing = (R.m_CyclesTotal > R.m_HaltCycles) ? (R.m_CyclesTotal - R.m_HaltCycles) : 0;
	const double BusyPct = R.m_CyclesTotal ? (100.0 * double(Executing) / double(R.m_CyclesTotal)) : 0.0;
	IndentAppendUint(J, "total", R.m_CyclesTotal, true);
	IndentAppendUint(J, "budget", R.m_Budget, true);
	IndentAppendInt(J, "overshoot", R.m_Overshoot, true);
	IndentAppendUint(J, "executing", Executing, true);
	IndentAppendUint(J, "halt", R.m_HaltCycles, true);
	IndentAppendDouble(J, "cpu_busy_pct", BusyPct, true);
	if (R.m_PerFrameHalt && !R.m_PerFrameHalt->empty() && R.m_StatsTickPeriod > 0)
	{
		IndentOpenObject(J, "per_frame");
		const double AvgHalt = MeanU32(*R.m_PerFrameHalt);
		const uint64_t MedHalt = Median(*R.m_PerFrameHalt);
		const double Period = double(R.m_StatsTickPeriod);
		const double AvgBusyPct = 100.0 * (Period - AvgHalt) / Period;
		const double MedBusyPct = 100.0 * (Period - double(MedHalt)) / Period;
		// per-frame busy cycles = frame period - halt this frame (DMA-free targets).
		// Mean/stddev/min/max give the per-update cost distribution directly.
		double Sum = 0.0, Sumsq = 0.0;
		uint32_t Bmin = UINT32_MAX, Bmax = 0;
		const size_t Nf = R.m_PerFrameHalt->size();
		for (uint32_t H : *R.m_PerFrameHalt)
		{
			double B = Period - double(H);
			if (B < 0.0)
			{
				B = 0.0;
			}
			Sum += B;
			Sumsq += B * B;
			const uint32_t Bu = uint32_t(B);
			if (Bu < Bmin)
			{
				Bmin = Bu;
			}
			if (Bu > Bmax)
			{
				Bmax = Bu;
			}
		}
		const double AvgBusy = Nf ? Sum / double(Nf) : 0.0;
		const double VarBusy = Nf ? (Sumsq / double(Nf) - AvgBusy * AvgBusy) : 0.0;
		const double StdBusy = VarBusy > 0.0 ? sqrt(VarBusy) : 0.0;
		InnerAppendDouble(J, "avg_busy_pct", AvgBusyPct, true);
		InnerAppendDouble(J, "median_busy_pct", MedBusyPct, true);
		InnerAppendDouble(J, "avg_halt_cycles", AvgHalt, true);
		InnerAppendUint(J, "median_halt_cycles", MedHalt, true);
		InnerAppendUint(J, "frames", uint64_t(Nf), true);
		InnerAppendDouble(J, "avg_busy_cycles", AvgBusy, true);
		InnerAppendDouble(J, "stddev_busy_cycles", StdBusy, true);
		InnerAppendUint(J, "min_busy_cycles", Bmin == UINT32_MAX ? 0 : Bmin, true);
		InnerAppendUint(J, "max_busy_cycles", Bmax, false);
		IndentCloseObject(J, false);
	}
	else
	{
		// Emit per_frame: null
		J += "    \"per_frame\": null\n";
	}
	CloseObject(J, true);

	// interrupts
	// ---- unsupported-feature histograms (compatibility sweeps)
	{
		auto EmitMap = [&](const char* Key, const std::map<uint16_t, uint64_t>& M, int Width, bool bComma)
		{
			IndentOpenObject(J, Key);
			size_t N = 0;
			for (const auto& Kv : M)
			{
				char K[16];
				snprintf(K, sizeof(K), "0x%0*X", Width, unsigned(Kv.first));
				IndentAppendUint(J, K, Kv.second, ++N < M.size());
			}
			IndentCloseObject(J, bComma);
		};
		std::map<uint16_t, uint64_t> ByCode(R.m_Rst08ByCode.begin(), R.m_Rst08ByCode.end());
		std::map<uint16_t, uint64_t> UnkCode(R.m_Rst08UnknownByCode.begin(), R.m_Rst08UnknownByCode.end());
		OpenObject(J, "esxdos");
		IndentAppendUint(J, "calls", R.m_Rst08Calls, true);
		IndentAppendUint(J, "unknown", R.m_Rst08Unknown, true);
		EmitMap("calls_by_code", ByCode, 2, true);
		EmitMap("unknown_by_code", UnkCode, 2, false);
		CloseObject(J, true);
		OpenObject(J, "nextreg_writes");
		{
			size_t N = 0;
			for (const auto& Kv : R.m_NextRegWrites)
			{
				char K[16];
				snprintf(K, sizeof(K), "0x%02X", unsigned(Kv.first));
				IndentAppendUint(J, K, Kv.second, ++N < R.m_NextRegWrites.size());
			}
		}
		CloseObject(J, true);

		OpenObject(J, "nextreg_reads");
		{
			size_t N = 0;
			for (const auto& Kv : R.m_NextRegReads)
			{
				char K[16];
				snprintf(K, sizeof(K), "0x%02X", unsigned(Kv.first));
				IndentAppendUint(J, K, Kv.second, ++N < R.m_NextRegReads.size());
			}
		}
		CloseObject(J, true);
		IndentAppendHex(J, "layer2_port_123b", R.m_Layer2Port, 2, true);
		IndentAppendUint(J, "layer2_window_opens", R.m_Layer2WindowOpens, true);
		OpenObject(J, "ports_unhandled");
		EmitMap("reads", R.m_PortsUnhandledReads, 4, true);
		EmitMap("writes", R.m_PortsUnhandledWrites, 4, false);
		CloseObject(J, true);
	}

	OpenObject(J, "interrupts");
	if (!R.m_IrqDiagnosticsJson.empty()) J += "    \"diagnostics\": " + R.m_IrqDiagnosticsJson + ",\n";
	IndentAppendUint(J, "accepted", R.m_InterruptsAccepted, true);
	IndentAppendUint(J, "missed", R.m_MissedInterrupts, true);
	IndentAppendUint(J, "line_accepted", R.m_LineIntsAccepted, true);
	IndentAppendUint(J, "line_missed", R.m_LineIntsMissed, true);
	IndentAppendUint(J, "line", R.m_LineIntLine, true);
	IndentAppendBool(J, "line_enabled", R.m_bLineIntEnabled, true);
	IndentAppendUint(J, "frame_int_period", R.m_FrameIntPeriod, true);
	const uint64_t FramesObserved = R.m_FrameIntPeriod ? (R.m_CyclesTotal / R.m_FrameIntPeriod) : 0;
	IndentAppendUint(J, "frames_observed", FramesObserved, false);
	CloseObject(J, true);

	// ay
	OpenObject(J, "ay");
	IndentAppendUint(J, "writes", R.m_AyWrites, true);
	IndentAppendHex(J, "registers_touched", R.m_RegistersTouched, 4, true);
	IndentAppendUint(J, "final_latch", R.m_AyFinalLatch, false);
	CloseObject(J, true);

	// psg
	OpenObject(J, "psg");
	IndentAppendUint(J, "frames_written", R.m_PsgFramesWritten, true);
	IndentAppendString(J, "out_path", R.m_PsgOutPath, false);
	CloseObject(J, true);

	// vram
	OpenObject(J, "vram");
	IndentAppendInt(J, "displayed_bank", R.m_DisplayedBank, true);
	IndentAppendUint(J, "pixel_writes_bank5", R.m_PixelWritesBank5, true);
	IndentAppendUint(J, "attr_writes_bank5", R.m_AttrWritesBank5, true);
	IndentAppendUint(J, "pixel_writes_bank7", R.m_PixelWritesBank7, true);
	IndentAppendUint(J, "attr_writes_bank7", R.m_AttrWritesBank7, true);
	IndentAppendBool(J, "pixels_all_zero", R.m_bPixelsAllZero, true);
	IndentAppendBool(J, "pixels_all_equal", R.m_bPixelsAllEqual, true);
	IndentAppendHex(J, "first_pixel_byte", R.m_FirstPixelByte, 2, true);
	IndentAppendBool(J, "attrs_all_equal", R.m_bAttrsAllEqual, true);
	IndentAppendHex(J, "first_attr_byte", R.m_FirstAttrByte, 2, true);
	IndentAppendInt(J, "distinct_attrs", R.m_DistinctAttrs, true);
	IndentAppendBool(J, "uniform_screen", R.m_bUniformScreen, true);
	IndentAppendInt(J, "border_colour", R.m_BorderColour, false);
	CloseObject(J, true);

	// paging
	OpenObject(J, "paging");
	IndentAppendHex(J, "current_7ffd", R.m_CurrentPort7FFD, 2, true);
	IndentAppendUint(J, "writes_to_7ffd", R.m_WritesTo7FFD, false);
	CloseObject(J, true);

	// outputs
	OpenObject(J, "outputs");
	IndentAppendString(J, "sna", R.m_OutputSna, true);
	IndentAppendString(J, "psg", R.m_PsgOutPath, true);
	IndentAppendString(J, "bmp", R.m_OutputBmp, true);
	IndentAppendString(J, "trace", R.m_OutputTrace, true);
	IndentAppendString(J, "status", R.m_OutputStatus, false);
	CloseObject(J, true);

	AppendDouble(J, "mhz", R.m_Mhz, true);
	AppendDouble(J, "nominal_mhz", R.m_NominalMhz, true);
	AppendDouble(J, "contention_x", double(R.m_ContentionX), true);
	AppendDouble(J, "wall_ms", R.m_WallMs, false);

	J += "}\n";

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open status output: ") + Path;
		return false;
	}
	const size_t Wrote = fwrite(J.data(), 1, J.size(), F);
	const bool bClosed = fclose(F) == 0;
	if (Wrote != J.size() || !bClosed)
	{
		OutError = std::string("short write to status output: ") + Path;
		return false;
	}
	return true;
}
