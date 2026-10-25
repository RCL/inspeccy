#include <inspeccy/version.h>
#include "args.h"
#include "frame_path.h"
#include <errno.h>
#include <math.h>
#include <limits>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace
{
template <typename T> bool ParseUnsigned(const char* Text, T& OutValue, int Base = 0)
{
	if (!Text || !*Text || *Text == '-' || isspace(static_cast<unsigned char>(*Text)))
	{
		fprintf(stderr, "expected a non-negative integer\n");
		return false;
	}
	errno = 0;
	char* End = nullptr;
	const unsigned long long Value = strtoull(Text, &End, Base);
	if (errno == ERANGE || End == Text || *End ||
		Value > static_cast<unsigned long long>(std::numeric_limits<T>::max()))
	{
		fprintf(stderr, "invalid or out-of-range integer: %s\n", Text);
		return false;
	}
	OutValue = static_cast<T>(Value);
	return true;
}

bool ParseReal(const char* Text, double& OutValue)
{
	if (!Text || !*Text || isspace(static_cast<unsigned char>(*Text)))
	{
		return false;
	}
	errno = 0;
	char* End = nullptr;
	const double Value = strtod(Text, &End);
	if (errno == ERANGE || End == Text || *End || !isfinite(Value) || Value < 0.0)
	{
		fprintf(stderr, "invalid or out-of-range number: %s\n", Text);
		return false;
	}
	OutValue = Value;
	return true;
}
} // namespace

int Usage()
{
	fputs("inspeccy " INSPECCY_VERSION "\n\n"
		  "usage: inspeccy <source> (--seconds N | --cycles N | --until-frame N) [options]\n"
		  "\n"
		  "Sources (choose one):\n"
		  "  --sna FILE | --z80 FILE | --nex FILE | --dot FILE | --resume FILE\n"
		  "  --tap FILE                    Standard-speed ROM-loader tape\n"
		  "  --trd FILE --trd-run NAME      TR-DOS disk boot (.trd or .scl)\n"
		  "  --dck FILE                    Timex DOCK cartridge (may accompany --tap)\n"
		  "  --create-initial-dump         Load and dump without running\n"
		  "  --sld FILE                   Repeatable sjasmplus symbol files\n"
		  "  --build-manifest FILE        Validate program/symbol identity; load its SLD files\n"
		  "  --allow-build-mismatch WHY  Record intentional mismatched or legacy state\n"
		  "  --batch FILE                 Run a JSON experiment on one loaded state\n"
		  "  --batch-out FILE             Compact batch report (default FILE.result.json)\n"
		  "  --capabilities               Print supported interfaces as JSON and exit\n"
		  "  --version                    Print the release version and exit\n"
		  "\n"
		  "Static analysis of a build (runs nothing):\n"
		  "  --asm-layout --sld F --asm-config F [--source-root D] [--json]\n"
		  "  --asm-banks  --sld F --asm-config F [--source-root D] [--json]   (1 if errors)\n"
		  "  --asm-query SYMBOL --sld F [--source-root D] [--limit N] [--json]\n"
		  "  --write-manifest FILE --image FILE --sld FILE [--sld FILE ...]\n"
		  "  --inspect-out FILE           Compact stopped CPU/MMU/stack report\n"
		  "  --irq-diagnostics FILE       Bounded interrupt attribution configuration\n"
		  "  --irq-diagnostics-out FILE   IRQ report (default CONFIG.result.json)\n"
		  "\n"
		  "Machine and input:\n"
		  "  --machine 48k|128k|+2a|+3|pentagon|next50|next60|ts2068|tc2068|tc2048|zxevo|zxuno\n"
		  "  --rom48 FILE --rom128 FILE | --rom48128 FILE\n"
		  "  --rom-trdos FILE | --rom-plus3 FILE | --rom-zxevo FILE | --rom-zxuno FILE\n"
		  "  --mhz 3.5|7|14|28             Initial speed on Next/ZX-Uno\n"
		  "  --ram-mb 1|2                 Next physical RAM (default 2)\n"
		  "  --frame-int N | --no-interrupts\n"
		  "  --int-window auto|latch|N\n"
		  "  --contention on|off           Default on where contention is modeled\n"
		  "  --contention-x N              Legacy metadata; ULA pattern is fixed\n"
		  "  --cycle-exact on|off          Frame-locked sampling (default on)\n"
		  "  --until-pc ADDR               Optional physical page prefix: PP:ADDR\n"
		  "  --dispatch auto|full          General dispatcher override for comparisons\n"
		  "  --reset-trap on|off --lenient-undoc-ed --no-copper\n"
		  "  --input FILE --input-tap-frames N --input-log on|off\n"
		  "  --tap-type-frame N --trdos auto|on|off|sna\n"
		  "  --dot-arg TEXT --vfs DIR --vfs-drive C --vfs-readonly\n"
		  "  --uart-pi loop|COMMAND        Next UART 1 (Pi) <-> loopback or a child process's stdin/stdout\n"
		  "  --uart-esp loop|COMMAND       Next UART 0 (ESP) likewise\n"
		  "\n"
		  "Outputs:\n"
		  "  --out FILE                   SNA output on 48K/128K/Pentagon; otherwise a basename\n"
		  "  --status-out FILE | --no-status\n"
		  "  --dump-out FILE | --no-dump   JSON state; resume has peripheral limitations\n"
		  "  --bmp-out FILE | --no-bmp\n"
		  "  --realtime                   never run ahead of the wall clock (HALT included): for real-time peers on --uart-pi\n"
		  "  --bmp-composite | --bmp-per-layer --bmp-exact on|off --pixel-accurate\n"
		  "  --video-out PATTERN --video-fps N\n"
		  "  --motion-out FILE --histogram-out FILE --paper-out PATTERN --metrics-block N\n"
		  "  --audio-out FILE --audio-rate N --audio-stereo abc|acb|mono\n"
		  "  --specdrum-out FILE           Legacy alias for --audio-out\n"
		  "  --psg-out FILE --psg-rate 50|60 --no-ay --ts\n"
		  "  --coverage-out FILE --coverage-reset-at-pc ADDR\n"
		  "  --watch-phys BANK:OFFSET      Bank decimal; offset hex or d-prefixed decimal\n"
		  "  --invariants FILE             Repeatable read-only memory rules\n"
		  "  --violations-out FILE --exit-on-violation --max-violations N\n"
		  "\n"
		  "Frame patterns require one integer placeholder, e.g. frames/frame_%06d.bmp.\n"
		  "Input/output aliases are rejected. ROMs are not bundled; RAM-only NEX/dot\n"
		  "programs can run without them. See docs/operator-guide.md for resume limitations.\n"
#if INSPECCY_TRACE_SUPPORT
		  "\n"
		  "Trace and analysis (requires tracing support at build time):\n"
		  "  --trace | --trace-out FILE | --trace-out-bin FILE\n"
		  "  --trace-decode FILE           Decode binary trace to stdout\n"
		  "  --trace-index-out FILE | --no-trace-index\n"
		  "  --trace-format minimal|verbose --trace-r --trace-memptr --trace-diff-only --trace-drip\n"
		  "  --trace-queue-mb N            Writer queue, 1..1024 MB (default 16); not a file-size cap\n"
		  "  --trace-from-cycles N | --trace-from-seconds N\n"
		  "  --optimize-out FILE --optimize-profile speed|size|all --optimize-min-ratio N\n"
#endif
		,
		stderr);
	return 4;
}

bool ParseArgs(int Argc, char** Argv, Args& Out)
{
	for (int IdxI = 1; IdxI < Argc; ++IdxI)
	{
		const std::string A = Argv[IdxI];
		auto Next = [&]() -> const char*
		{
			if (IdxI + 1 >= Argc)
			{
				return nullptr;
			}
			return Argv[++IdxI];
		};
		if (A == "--sna")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_SnaPath = P;
		}
		else if (A == "--z80")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_Z80Path = P;
		}
		else if (A == "--tap")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TapPath = P;
		}
		else if (A == "--tap-type-frame")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_TapTypeFrame, 0))
			{
				return false;
			}
			Out.m_bTapTypeFrameSet = true;
		}
		else if (A == "--dck")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_DckPath = P;
		}
		else if (A == "--rom48")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_Rom48Path = P;
		}
		else if (A == "--rom128")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_Rom128Path = P;
		}
		else if (A == "--rom-plus3")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_RomPlus3Path = P;
		}
		else if (A == "--rom48128")
		{
			// Shorthand for "use this combined 32 KB ROM for both --rom48 and
			// --rom128" - the loader picks the back half for 48K and the
			// front half for 128K, so the same file fills both slots.
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_Rom48Path = P;
			Out.m_Rom128Path = P;
		}
		else if (A == "--rom-trdos")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_RomTrdPath = P;
		}
		else if (A == "--rom-zxevo")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_RomZxEvoPath = P;
		}
		else if (A == "--rom-zxuno")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_RomZxUnoPath = P;
		}
		else if (A == "--out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_OutPath = P;
		}
		else if (A == "--seconds")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseReal(P, Out.m_Seconds))
			{
				return false;
			}
			Out.m_bHaveSeconds = true;
		}
		else if (A == "--cycles")
		{
			// Direct cycle-count budget. Bypasses the secondsxMhz conversion
			// and disables the turbo-callback rescale: cycles are absolute.
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_Cycles, 0))
			{
				return false;
			}
			Out.m_bHaveCycles = true;
		}
		else if (A == "--trace-from-cycles")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_TraceStartCycle, 0))
			{
				return false;
			}
			Out.m_bHaveTraceStart = true;
		}
		else if (A == "--trace-from-seconds")
		{
			// Recorded here; main converts to an initial cycle deadline and
			// updates it across turbo transitions before recording starts.
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseReal(P, Out.m_TraceStartSeconds))
			{
				return false;
			}
			Out.m_bTraceStartInSeconds = true;
		}
		else if (A == "--until-frame")
		{
			// Shorthand: set --seconds = N / fps (50 for next50, 60 for next60).
			// Lets debugging target a specific captured frame without hand-
			// computing the seconds. fps comes from the machine; if --machine
			// hasn't been parsed yet by this point we default to 50 and the
			// user can override with --seconds.
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			const double Fps = (Out.m_Machine == MachineModel::kMachineModelNext60) ? 60.0 : 50.0;
			if (!ParseReal(P, Out.m_UntilFrame))
			{
				return false;
			}
			Out.m_bHaveUntilFrame = true;
			Out.m_Seconds = (Out.m_UntilFrame + 0.5) / Fps; // provisional; main recomputes at the machine's real rate
			Out.m_bHaveSeconds = true;
		}
		else if (A == "--mhz")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseReal(P, Out.m_Mhz))
			{
				return false;
			}
			Out.m_bMhzExplicit = true;
		}
		else if (A == "--contention")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_bContention = (strcmp(P, "on") == 0);
			Out.m_bContentionExplicit = true;
		}
		else if (A == "--cycle-exact")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "on") != 0 && strcmp(P, "off") != 0)
			{
				return false;
			}
			Out.m_bCycleExact = (strcmp(P, "off") != 0);
			Out.m_bCycleExactExplicit = true;
		}
		else if (A == "--int-window")
		{
			// auto = the machine's ULA pulse length (hardware); latch = legacy
			// sticky request; <N> = explicit T-states.
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_bIntWindowExplicit = true;
			if (strcmp(P, "auto") == 0)
			{
				Out.m_IntWindowMode = -1;
			}
			else if (strcmp(P, "latch") == 0)
			{
				Out.m_IntWindowMode = 0;
			}
			else
			{
				if (!ParseUnsigned(P, Out.m_IntWindowMode, 0))
				{
					return false;
				}
			}
		}
		else if (A == "--dispatch")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "auto") != 0 && strcmp(P, "full") != 0)
			{
				return false;
			}
			Out.m_bDispatchFull = (strcmp(P, "full") == 0); // "auto" (default) picks plain when possible
		}
		else if (A == "--until-pc")
		{
			// <addr> = a 16-bit Z80 PC (0xNNNN), or a banked breakpoint
			// "PP:NNNN" / "0xPPNNNN" where PP is the 8K physical page that must
			// be mapped at PC's slot too (24-bit address on the Next).
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			const char* Colon = strchr(P, ':');
			long Page = -1;
			long Pc;
			if (Colon)
			{
				if (!ParseUnsigned(std::string(P, Colon).c_str(), Page))
				{
					return false;
				}
				if (!ParseUnsigned(Colon + 1, Pc))
				{
					return false;
				}
			}
			else
			{
				unsigned long V = 0;
				if (!ParseUnsigned(P, V) || V > 0xffffffUL)
				{
					return false;
				}
				if (V > 0xFFFF) // 0xPPNNNN packed form
				{
					Page = long((V >> 16) & 0xFF);
					Pc = long(V & 0xFFFF);
				}
				else
				{
					Pc = long(V);
				}
			}
			if (Pc < 0 || Pc > 0xFFFF || Page > 0xFF)
			{
				fprintf(stderr, "--until-pc: address out of range: %s\n", P);
				return false;
			}
			Out.m_bUntilPc = true;
			Out.m_UntilPc = uint16_t(Pc);
			Out.m_UntilPcPage = (Page >= 0) ? int32_t(Page) : -1;
		}
		else if (A == "--contention-x")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_ContentionX, 0))
			{
				return false;
			}
		}
		else if (A == "--audio-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_AudioOutPath = P;
		}
		else if (A == "--audio-rate")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_AudioRateHz, 0))
			{
				return false;
			}
			if (Out.m_AudioRateHz < 8000 || Out.m_AudioRateHz > 192000)
			{
				fprintf(stderr, "--audio-rate must be 8000..192000\n");
				return false;
			}
		}
		else if (A == "--audio-stereo")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "abc") == 0)
			{
				Out.m_AudioStereo = 0;
			}
			else if (strcmp(P, "acb") == 0)
			{
				Out.m_AudioStereo = 1;
			}
			else if (strcmp(P, "mono") == 0)
			{
				Out.m_AudioStereo = 2;
			}
			else
			{
				fprintf(stderr, "--audio-stereo must be abc, acb or mono\n");
				return false;
			}
		}
		else if (A == "--machine")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "48k") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelSpectrum48K;
			}
			else if (strcmp(P, "128k") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelSpectrum128K;
			}
			else if (strcmp(P, "pentagon") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelPentagon128;
			}
			else if (strcmp(P, "next50") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelNext50;
			}
			else if (strcmp(P, "next60") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelNext60;
			}
			else if (strcmp(P, "ts2068") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelTs2068;
			}
			else if (strcmp(P, "tc2068") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelTc2068;
			}
			else if (strcmp(P, "tc2048") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelTc2048;
			}
			else if (strcmp(P, "zxevo") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelZxEvo;
			}
			else if (strcmp(P, "zxuno") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelZxUno;
			}
			else if (strcmp(P, "+2a") == 0 || strcmp(P, "plus2a") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelSpectrumPlus2A;
			}
			else if (strcmp(P, "+3") == 0 || strcmp(P, "plus3") == 0)
			{
				Out.m_Machine = MachineModel::kMachineModelSpectrumPlus3;
			}
			else
			{
				fprintf(stderr, "--machine must be 48k, 128k, pentagon, +2a, +3, next50, next60, ts2068, tc2068, "
								"tc2048, zxevo, or zxuno\n");
				return false;
			}
			Out.m_bMachineExplicit = true;
		}
		else if (A == "--frame-int")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_FrameIntOverride, 0))
			{
				return false;
			}
			Out.m_bFrameIntExplicit = true;
		}
		else if (A == "--no-interrupts")
		{
			Out.m_bNoInterrupts = true;
		}
		else if (A == "--trd")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TrdPath = P;
		}
		else if (A == "--trd-run")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TrdRunName = P;
		}
		else if (A == "--trdos")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_bTrdosModeExplicit = true;
			if (strcmp(P, "auto") == 0)
			{
				Out.m_TrdosModeFlag = TrdosMode::kTrdosModeAuto;
			}
			else if (strcmp(P, "on") == 0)
			{
				Out.m_TrdosModeFlag = TrdosMode::kTrdosModeOn;
			}
			else if (strcmp(P, "off") == 0)
			{
				Out.m_TrdosModeFlag = TrdosMode::kTrdosModeOff;
			}
			else if (strcmp(P, "sna") == 0)
			{
				Out.m_TrdosModeFlag = TrdosMode::kTrdosModeSna;
			}
			else
			{
				fprintf(stderr, "--trdos must be auto, on, off, or sna\n");
				return false;
			}
		}
		else if (A == "--lenient-undoc-ed")
		{
			Out.m_bLenientUndocEd = true;
		}
		else if (A == "--no-copper")
		{
			Out.m_bNoCopper = true;
		}
		else if (A == "--pixel-accurate")
		{
			Out.m_bPixelAccurate = true;
		}
		else if (A == "--reset-trap")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_bResetTrapExplicit = true;
			if (strcmp(P, "on") == 0)
			{
				Out.m_bResetTrap = true;
			}
			else if (strcmp(P, "off") == 0)
			{
				Out.m_bResetTrap = false;
			}
			else
			{
				fprintf(stderr, "--reset-trap must be on or off\n");
				return false;
			}
		}
		else if (A == "--input")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_InputPath = P;
			Out.m_bInputExplicit = true;
		}
		else if (A == "--input-tap-frames")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_InputTapFrames, 10))
			{
				return false;
			}
			if (Out.m_InputTapFrames < 1)
			{
				fprintf(stderr, "--input-tap-frames must be >= 1\n");
				return false;
			}
		}
		else if (A == "--input-log")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "on") == 0)
			{
				Out.m_bInputLog = true;
			}
			else if (strcmp(P, "off") == 0)
			{
				Out.m_bInputLog = false;
			}
			else
			{
				fprintf(stderr, "--input-log must be on or off\n");
				return false;
			}
		}
		else if (A == "--psg-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_PsgOutPath = P;
		}
		else if (A == "--ts")
		{
			Out.m_bTurboSound = true;
		}
		else if (A == "--specdrum-out")
		{
			fprintf(stderr, "--specdrum-out was removed; use --audio-out (unified render: AY + DAC + beeper)\n");
			return false;
		}
		else if (A == "--coverage-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_CoverageOutPath = P;
		}
		else if (A == "--optimize-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_OptimizeOutPath = P;
		}
		else if (A == "--optimize-profile")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			const std::string V(P);
			if (V != "speed" && V != "size" && V != "all")
			{
				fprintf(stderr, "--optimize-profile must be speed, size or all\n");
				return false;
			}
			Out.m_OptimizeProfile = V;
		}
		else if (A == "--optimize-min-ratio")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseReal(P, Out.m_OptimizeMinRatio))
			{
				return false;
			}
			if (!(Out.m_OptimizeMinRatio >= 0.0 && Out.m_OptimizeMinRatio <= 1.0))
			{
				fprintf(stderr, "--optimize-min-ratio must be between 0 and 1\n");
				return false;
			}
		}
		else if (A == "--coverage-reset-at-pc")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			std::string V = P;
			int Base = 16; // hex default (Z80 convention), $/0x explicit, d=decimal
			if (!V.empty() && V[0] == '$')
			{
				V.erase(0, 1);
				Base = 16;
			}
			else if (V.size() >= 2 && V.substr(0, 2) == "0x")
			{
				V.erase(0, 2);
				Base = 16;
			}
			else if (!V.empty() && V[0] == 'd')
			{
				V.erase(0, 1);
				Base = 10;
			}
			uint32_t Addr = 0;
			if (!ParseUnsigned(V.c_str(), Addr, Base) || Addr > 0xFFFF)
			{
				fprintf(stderr, "--coverage-reset-at-pc must be a 16-bit address, got %s\n", P);
				return false;
			}
			Out.m_CoverageResetPC = int32_t(Addr);
		}
		else if (A == "--video-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_VideoOutPattern = P;
		}
		else if (A == "--motion-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_MotionOutPath = P;
		}
		else if (A == "--histogram-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_HistogramOutPath = P;
		}
		else if (A == "--paper-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_PaperOutPattern = P;
		}
		else if (A == "--metrics-block")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_MetricsBlock, 10))
			{
				return false;
			}
		}
		else if (A == "--ram-mb")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_RamMb, 10))
			{
				return false;
			}
			Out.m_bRamMbExplicit = true;
			if (Out.m_RamMb != 1 && Out.m_RamMb != 2)
			{
				fprintf(stderr, "--ram-mb must be 1 or 2\n");
				return false;
			}
		}
		else if (A == "--watch-phys")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			// Expect "<bank>:<offset>", offset may be 0xNNNN, $NNNN, or decimal.
			std::string Spec = P;
			const size_t Colon = Spec.find(':');
			if (Colon == std::string::npos)
			{
				fprintf(stderr, "--watch-phys requires <bank>:<offset>, got %s\n", P);
				return false;
			}
			const std::string BankStr = Spec.substr(0, Colon);
			std::string OffStr = Spec.substr(Colon + 1);
			// Offsets default to HEX (typical Z80 debug convention) - match
			// the format dump files use for their hex-dump output. Prefix
			// with 'd' to mean decimal if you really want it: e.g. `40:d4352`.
			int Base = 16;
			if (!OffStr.empty() && OffStr[0] == '$')
			{
				OffStr.erase(0, 1);
				Base = 16;
			}
			else if (OffStr.size() >= 2 && OffStr.substr(0, 2) == "0x")
			{
				OffStr.erase(0, 2);
				Base = 16;
			}
			else if (!OffStr.empty() && OffStr[0] == 'd')
			{
				OffStr.erase(0, 1);
				Base = 10;
			}
			if (!ParseUnsigned(BankStr.c_str(), Out.m_WatchBank16, 10) || Out.m_WatchBank16 > 127 ||
				!ParseUnsigned(OffStr.c_str(), Out.m_WatchBankOffset, Base) || Out.m_WatchBankOffset >= 0x4000)
			{
				fprintf(stderr, "--watch-phys needs bank 0..127 and offset 0..$3FFF\n");
				return false;
			}
		}
		else if (A == "--invariants")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_InvariantsPaths.emplace_back(P);
		}
		else if (A == "--violations-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_ViolationsOutPath = P;
		}
		else if (A == "--exit-on-violation")
		{
			Out.m_bExitOnViolation = true;
		}
		else if (A == "--max-violations")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_MaxViolations, 10))
			{
				return false;
			}
		}
		else if (A == "--video-fps")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_VideoFps, 10))
			{
				return false;
			}
			if (Out.m_VideoFps <= 0 || Out.m_VideoFps > 1000)
			{
				fprintf(stderr, "--video-fps must be 1..1000\n");
				return false;
			}
		}
		else if (A == "--psg-rate")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_PsgRate, 10))
			{
				return false;
			}
			if (Out.m_PsgRate != 50 && Out.m_PsgRate != 60)
			{
				fprintf(stderr, "--psg-rate must be 50 or 60\n");
				return false;
			}
		}
		else if (A == "--no-ay")
		{
			Out.m_bAyEnabled = false;
		}
		else if (A == "--status-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_StatusOutPath = P;
		}
		else if (A == "--no-status")
		{
			Out.m_bNoStatus = true;
		}
		else if (A == "--bmp-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_BmpOutPath = P;
		}
		else if (A == "--realtime")
		{
			Out.m_bRealtime = true;
		}
		else if (A == "--no-bmp")
		{
			Out.m_bNoBmp = true;
		}
		else if (A == "--bmp-per-layer")
		{
			Out.m_bBmpPerLayer = true;
		}
		else if (A == "--bmp-exact")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "on") != 0 && strcmp(P, "off") != 0)
			{
				return false;
			}
			Out.m_bBmpExact = (strcmp(P, "off") != 0);
		}
		else if (A == "--bmp-composite")
		{ /* legacy no-op: composite is now the default */
		}
		else if (A == "--dump-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_DumpOutPath = P;
		}
		else if (A == "--no-dump")
		{
			Out.m_bNoDump = true;
		}
		else if (A == "--sld")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_SldPaths.push_back(P);
		}
		else if (A == "--build-manifest" || A == "--allow-build-mismatch")
		{
			const char* P = Next();
			if (!P || !*P)
			{
				return false;
			}
			(A == "--build-manifest" ? Out.m_BuildManifestPath : Out.m_AllowBuildMismatch) = P;
		}
		else if (A == "--batch" || A == "--batch-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			(A == "--batch" ? Out.m_BatchPath : Out.m_BatchOutPath) = P;
		}
		else if (A == "--inspect-out" || A == "--irq-diagnostics" || A == "--irq-diagnostics-out")
		{
			const char* P = Next();
			if (!P || !*P) return false;
			(A == "--inspect-out" ? Out.m_InspectOutPath : A == "--irq-diagnostics" ? Out.m_IrqDiagnosticsPath : Out.m_IrqDiagnosticsOutPath) = P;
		}
		else if (A == "--resume")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_ResumePath = P;
		}
		else if (A == "--dot")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_DotPath = P;
		}
		else if (A == "--nex")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_NexPath = P;
		}
		else if (A == "--dot-arg")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_DotArg = P;
		}
		else if (A == "--vfs")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_VfsPath = P;
		}
		else if (A == "--uart-pi" || A == "--uart-esp")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			(A == "--uart-pi" ? Out.m_UartPi : Out.m_UartEsp) = P;
		}
		else if (A == "--vfs-drive")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (P[0] == 0 || P[1] != 0)
			{
				fprintf(stderr, "--vfs-drive must be a single letter\n");
				return false;
			}
			Out.m_VfsDrive = char(toupper(uint8_t(P[0])));
		}
		else if (A == "--vfs-readonly")
		{
			Out.m_bVfsReadOnly = true;
		}
		else if (A == "--create-initial-dump")
		{
			Out.m_bCreateInitialDump = true;
		}
#if INSPECCY_TRACE_SUPPORT
		else if (A == "--trace")
		{
			Out.m_bTrace = true;
		}
		else if (A == "--trace-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TraceOutPath = P;
		}
		else if (A == "--trace-out-bin")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TraceOutBinPath = P;
		}
		else if (A == "--trace-decode")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TraceDecodePath = P;
		}
		else if (A == "--trace-index-out")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			Out.m_TraceIndexOutPath = P;
		}
		else if (A == "--no-trace-index")
		{
			Out.m_bNoTraceIndex = true;
		}
		else if (A == "--trace-format")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (strcmp(P, "verbose") == 0)
			{
				Out.m_bTraceVerbose = true;
			}
			else if (strcmp(P, "minimal") == 0)
			{
				Out.m_bTraceVerbose = false;
			}
			else
			{
				fprintf(stderr, "--trace-format must be minimal or verbose\n");
				return false;
			}
		}
		else if (A == "--trace-r")
		{
			Out.m_bTraceR = true;
		}
		else if (A == "--trace-memptr")
		{
			Out.m_bTraceMemPtr = true;
		}
		else if (A == "--trace-diff-only")
		{
			Out.m_bTraceDiffOnly = true;
		}
		else if (A == "--trace-drip")
		{
			Out.m_bTraceDrip = true;
		}
		else if (A == "--trace-queue-mb")
		{
			const char* P = Next();
			if (!P)
			{
				return false;
			}
			if (!ParseUnsigned(P, Out.m_TraceQueueMb, 10))
			{
				return false;
			}
			if (Out.m_TraceQueueMb < 1 || Out.m_TraceQueueMb > 1024)
			{
				fprintf(stderr, "--trace-queue-mb must be 1..1024\n");
				return false;
			}
		}
#else
		else if (A == "--trace" || A == "--trace-out" || A == "--trace-out-bin" || A == "--trace-decode" ||
				 A == "--trace-format" || A == "--trace-index-out" || A == "--no-trace-index" || A == "--trace-r" ||
				 A == "--trace-memptr" || A == "--trace-queue-mb" || A == "--trace-diff-only")
		{
			fprintf(stderr, "trace support not compiled in (build with -DINSPECCY_TRACE_SUPPORT=ON)\n");
			return false;
		}
#endif
		else if (A == "-h" || A == "--help")
		{
			return false;
		}
		else
		{
			fprintf(stderr, "unknown argument: %s\n", A.c_str());
			return false;
		}
	}
	if (!Out.m_IrqDiagnosticsPath.empty() && Out.m_IrqDiagnosticsOutPath.empty())
		Out.m_IrqDiagnosticsOutPath = Out.m_IrqDiagnosticsPath + ".result.json";
	if (Out.m_IrqDiagnosticsPath.empty() && !Out.m_IrqDiagnosticsOutPath.empty())
	{
		fprintf(stderr, "--irq-diagnostics-out requires --irq-diagnostics\n");
		return false;
	}
	std::string CheckedPath;
	for (const std::string* Pattern : {&Out.m_VideoOutPattern, &Out.m_PaperOutPattern})
	{
		if (!Pattern->empty() && !FormatFramePath(*Pattern, 0, CheckedPath))
		{
			fprintf(stderr, "frame filename needs exactly one integer placeholder (for example %%06d)\n");
			return false;
		}
	}
	// --trace-decode is a standalone read-only operation - no emulation.
	// Skip all the input-and-budget validation in that case.
	if (!Out.m_TraceDecodePath.empty())
	{
		return true;
	}
	// --create-initial-dump skips RunUntil so a budget isn't required there.
	if (!Out.m_BatchPath.empty())
	{
		if (Out.m_bHaveSeconds || Out.m_bHaveCycles || Out.m_bCreateInitialDump || Out.m_bUntilPc ||
			Out.m_bHaveTraceStart)
		{
			fprintf(stderr, "--batch uses per-step budgets; do not combine with duration, initial-dump, until-pc or "
							"deferred trace flags\n");
			return false;
		}
		Out.m_bHaveCycles = true;
		Out.m_Cycles = 1; // setup only; each batch run replaces this budget
		if (Out.m_BatchOutPath.empty())
		{
			Out.m_BatchOutPath = Out.m_BatchPath + ".result.json";
		}
	}
	else if (!Out.m_BatchOutPath.empty())
	{
		fprintf(stderr, "--batch-out requires --batch\n");
		return false;
	}
	// Otherwise need either --seconds or --cycles (mutually exclusive).
	if (Out.m_bHaveSeconds && Out.m_bHaveCycles)
	{
		fprintf(stderr, "--seconds and --cycles are mutually exclusive\n");
		return false;
	}
	if (!Out.m_bHaveSeconds && !Out.m_bHaveCycles && !Out.m_bCreateInitialDump)
	{
		return false;
	}
	{
		const int Count = (!Out.m_SnaPath.empty()) + (!Out.m_Z80Path.empty()) + (!Out.m_TapPath.empty()) +
						  (!Out.m_TrdRunName.empty()) + (!Out.m_ResumePath.empty()) + (!Out.m_DotPath.empty()) +
						  (!Out.m_NexPath.empty());
		// A cartridge is a boot source in its own right: the 2068 cold-boots
		// its Home ROM, which finds the LROS in the dock and runs it. It is
		// also legitimately combined with a tape, so it is excused from the
		// "exactly one source" count rather than added to it.
		if (Count == 0 && Out.m_DckPath.empty())
		{
			fprintf(
				stderr, "one of --sna / --z80 / --tap / --trd-run / --resume / --dot / --nex / --dck is required\n");
			return false;
		}
		if (!Out.m_TrdRunName.empty() && Out.m_TrdPath.empty())
		{
			fprintf(stderr, "--trd-run requires --trd <image>\n");
			return false;
		}
		// --resume + --nex is allowed: the --nex provides the (possibly
		// relocated) path for re-opening the streaming file handle, while
		// --resume restores all other state. Other combinations stay
		// mutually exclusive.
		const bool bResumeNex = !Out.m_ResumePath.empty() && !Out.m_NexPath.empty();
		if (Count > 1 && !(bResumeNex && Count == 2))
		{
			fprintf(stderr, "--sna / --z80 / --tap / --trd-run / --resume / --dot / --nex are mutually exclusive "
							"(except --resume + --nex)\n");
			return false;
		}
	}
	if (Out.m_Mhz != 3.5 && Out.m_Mhz != 7.0 && Out.m_Mhz != 14.0 && Out.m_Mhz != 28.0)
	{
		fprintf(stderr, "--mhz must be 3.5, 7, 14, or 28\n");
		return false;
	}
	if (Out.m_OutPath.empty())
	{
		// Auto-derive from --sna / --resume / --dot / --nex, stripping any
		// obvious suffix. For --dot and --nex runs we don't tack on `.sna`
		// - SaveSna is suppressed (NextRAM + NextRegs can't be captured in
		// an SNA) and OutPath is only used as the basename for derived
		// artifacts (BMP, dump, status, PSG).
		std::string Base = !Out.m_SnaPath.empty()	   ? Out.m_SnaPath
						   : !Out.m_Z80Path.empty()	   ? Out.m_Z80Path
						   : !Out.m_TapPath.empty()	   ? Out.m_TapPath
						   : !Out.m_TrdRunName.empty() ? Out.m_TrdPath
						   : !Out.m_ResumePath.empty() ? Out.m_ResumePath
						   : !Out.m_DotPath.empty()	   ? Out.m_DotPath
													   : Out.m_NexPath;
		auto EndsWith = [](const std::string& S, const char* Suf) -> bool
		{
			const size_t L = strlen(Suf);
			return S.size() >= L && S.compare(S.size() - L, L, Suf) == 0;
		};
		if (EndsWith(Base, ".inspec.dump.json"))
		{
			Base.erase(Base.size() - 17);
		}
		else if (EndsWith(Base, ".sna"))
		{
			Base.erase(Base.size() - 4);
		}
		else if (EndsWith(Base, ".dot"))
		{
			Base.erase(Base.size() - 4);
		}
		else if (EndsWith(Base, ".nex"))
		{
			Base.erase(Base.size() - 4);
		}
		else if (EndsWith(Base, ".z80"))
		{
			Base.erase(Base.size() - 4);
		}
		else if (EndsWith(Base, ".tap"))
		{
			Base.erase(Base.size() - 4);
		}
		else if (EndsWith(Base, ".trd") || EndsWith(Base, ".scl"))
		{
			Base.erase(Base.size() - 4);
		}
		const bool bNeedsRom = Out.m_DotPath.empty() && Out.m_NexPath.empty();
		Out.m_OutPath = Base + (bNeedsRom ? ".out.sna" : ".out");
	}

	// CLI conflict checks
	if (Out.m_bNoStatus && !Out.m_StatusOutPath.empty())
	{
		fprintf(stderr, "--no-status conflicts with --status-out\n");
		return false;
	}
	if (Out.m_bNoBmp && !Out.m_BmpOutPath.empty())
	{
		fprintf(stderr, "--no-bmp conflicts with --bmp-out\n");
		return false;
	}
	if (Out.m_bNoDump && !Out.m_DumpOutPath.empty())
	{
		fprintf(stderr, "--no-dump conflicts with --dump-out\n");
		return false;
	}

	// Auto-derive default status / BMP paths from --out (strip trailing .sna).
	auto DeriveFromOut = [&](const std::string& Suffix) -> std::string
	{
		std::string Base = Out.m_OutPath;
		if (Base.size() >= 4 && Base.compare(Base.size() - 4, 4, ".sna") == 0)
		{
			Base.erase(Base.size() - 4);
		}
		return Base + Suffix;
	};
	if (!Out.m_bNoStatus && Out.m_StatusOutPath.empty())
	{
		Out.m_StatusOutPath = DeriveFromOut(".status.json");
	}
	if (!Out.m_bNoBmp && Out.m_BmpOutPath.empty())
	{
		Out.m_BmpOutPath = DeriveFromOut(".bmp");
	}
	if (!Out.m_bNoDump && Out.m_DumpOutPath.empty())
	{
		Out.m_DumpOutPath = DeriveFromOut(".inspec.dump.json");
	}

	return true;
}
