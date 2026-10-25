#include <inspeccy/version.h>
#include "args.h"
#include "asm_cli.h"
#include "batch.h"
#include "build_identity.h"
#include <trace/sld.h>
#include "output_paths.h"
#include "ay.h"
#include "bmp.h"
#include "dck.h"
#include "dma.h"
#include "dotcmd.h"
#include "nex.h"
#include "dump.h"
#include "json_parser.h"
#include "sha1.h"
#include "input.h"
#include "invariants.h"
#include "io.h"
#include "uart.h"
#include "irq_report.h"
#include "diagnostic_json.h"
#include "memory.h"
#include "nextzxos.h"
#include "peephole.h"
#include "psg.h"
#include "rom.h"
#include "audio_render.h"
#include "sprites.h"
#include "base64.h"
#include "video.h"
#include "metrics.h"
#include "sna.h"
#include "z80snap.h"
#include "tape.h"
#include "autotype.h"
#include "status.h"
#include "replay.h"
#include "trdos.h"
#include "z80.h"
#if INSPECCY_TRACE_SUPPORT
#include "trace.h"
#endif

#include <chrono>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <filesystem>
#include <memory>
#include <string>

namespace
{

// Eager beam drawer plumbing: the MemoryBus boundary hook is a C fn-ptr + user;
// this trampoline forwards a display-write's cycle to the drawer so it flushes
// the beam up to that point from live state before the write lands.
struct BeamHookCtx
{
	BeamDrawer* m_Drawer = nullptr;
	MemoryBus* m_Mem = nullptr;
	SpritesState* m_Sprites = nullptr;
};
void BeamHookTrampoline(void* User, uint64_t Cycle)
{
	BeamHookCtx* C = static_cast<BeamHookCtx*>(User);
	C->m_Drawer->FlushTo(*C->m_Mem, C->m_Sprites, Cycle);
}

// Machines that page like a 128K (RAM banks + $7FFD), so SNA/tape loaders treat
// them as K128 and the memory maps as 128K. ZX-Evo and ZX-Uno are clones of the
// 128K/Pentagon family; their extra paging (page-window regs, register files)
// layers on top of the 128K base.
bool Is128ClassMachine(MachineModel M)
{
	return M == MachineModel::kMachineModelSpectrum128K || M == MachineModel::kMachineModelPentagon128 ||
		   M == MachineModel::kMachineModelSpectrumPlus2A || M == MachineModel::kMachineModelSpectrumPlus3 ||
		   M == MachineModel::kMachineModelZxEvo || M == MachineModel::kMachineModelZxUno;
}

const char* ResultName(RunResult R)
{
	switch (R)
	{
	case RunResult::kRunResultBudgetReached:
		return "BudgetReached";
	case RunResult::kRunResultInvalidOpcode:
		return "InvalidOpcode";
	case RunResult::kRunResultHaltedInterruptsDisabled:
		return "HaltedInterruptsDisabled";
	case RunResult::kRunResultHaltedDeliberateStop:
		return "HaltedDeliberateStop";
	case RunResult::kRunResultRestartTrap:
		return "RestartTrap";
	case RunResult::kRunResultReachedPc:
		return "ReachedPc";
	}
	return "?";
}

} // namespace

#if INSPECCY_TRACE_SUPPORT
int DecodeBinaryTraceToText(const char* InPath, FILE* Out, const SldSymbolMap* Symbols);
#endif

int main(int Argc, char** Argv)
{
	if (Argc == 2 && strcmp(Argv[1], "--version") == 0)
	{
		puts("inspeccy " INSPECCY_VERSION);
		return 0;
	}
	if (Argc == 2 && strcmp(Argv[1], "--capabilities") == 0)
	{
		puts(InspectionCapabilitiesJson().c_str());
		return 0;
	}
	if (Argc == 2 && (strcmp(Argv[1], "--help") == 0 || strcmp(Argv[1], "-h") == 0))
	{
		Usage();
		return 0;
	}
	// Static analysis of a build: reads .sld and sources, runs nothing.
	int AsmExit = 0;
	if (RunAsmCommand(Argc, Argv, AsmExit))
	{
		return AsmExit;
	}
	Args A;
	if (!ParseArgs(Argc, Argv, A))
	{
		return Usage();
	}

#if INSPECCY_TRACE_SUPPORT
	// --trace-decode is a read-only operation - skip all the emulator setup
	// and just walk the binary file. Symbols come from any --sld files the
	// user passed (consistent with how the text writer uses them).
	if (!A.m_TraceDecodePath.empty())
	{
		SldSymbolMap SldMap;
		for (const std::string& SldPath : A.m_SldPaths)
		{
			std::string SldErr;
			if (!SldMap.LoadFile(SldPath.c_str(), SldErr))
			{
				fprintf(stderr, "%s\n", SldErr.c_str());
				return 3;
			}
		}
		const SldSymbolMap* SymsPtr = (SldMap.SymbolCount() > 0) ? &SldMap : nullptr;
		return DecodeBinaryTraceToText(A.m_TraceDecodePath.c_str(), stdout, SymsPtr);
	}
#endif

	// --- Source resolution: SNA load or dump resume ---
	const bool bResuming = !A.m_ResumePath.empty();
	JsonValue DumpRoot;
	DumpRunConfig DumpCfg;
	ParsedExternalRefs DumpRefs;
	ParsedDotCmdInfo DumpDot; // a resumed dot command: its path, argument and load facts
	SnaKind SrcKind = SnaKind::kSnaKindK128;
	bool bSrcWantsTrdos = false;

	if (bResuming)
	{
		std::string Err;
		if (!ReadAndParseDump(A.m_ResumePath.c_str(), DumpRoot, Err))
		{
			fprintf(stderr, "%s\n", Err.c_str());
			return 3;
		}
		if (!LoadRunConfigFromDump(DumpRoot, DumpCfg, Err) || !LoadExternalRefsFromDump(DumpRoot, DumpRefs, Err))
		{
			fprintf(stderr, "%s\n", Err.c_str());
			return 3;
		}
		if (!A.m_bRamMbExplicit)
		{
			A.m_RamMb = DumpCfg.m_RamMb;
		}
		// A dot command's run: its RST $08 trap and the next dump's dot_cmd
		// block depend on the run knowing it is one (the binary is in RAM).
		if (!LoadDotCmdFromDump(DumpRoot, DumpDot, Err))
		{
			fprintf(stderr, "%s", Err.c_str());
			return 3;
		}
		if (DumpDot.m_bPresent && A.m_DotPath.empty())
		{
			A.m_DotPath = ResolveRefPathFromDump(DumpDot.m_Path, A.m_ResumePath);
			A.m_DotArg = DumpDot.m_ArgString;
		}
		// The options that shape behaviour come back with the dump unless the
		// command line names them; the on-only switches stay on if either asks.
		// A dump older than the run_options block gets the per-source default
		// a fresh run would (a NEX run has no reset trap).
		const DumpRunOptions& DO = DumpCfg.m_Options;
		if (!A.m_bResetTrapExplicit)
		{
			A.m_bResetTrap = DO.m_bPresent ? DO.m_bResetTrap : DumpCfg.m_NexPath.empty();
		}
		if (DO.m_bPresent)
		{
			A.m_bLenientUndocEd = A.m_bLenientUndocEd || DO.m_bLenientUndocEd;
			A.m_bNoCopper = A.m_bNoCopper || DO.m_bNoCopper;
			A.m_bPixelAccurate = A.m_bPixelAccurate || DO.m_bPixelAccurate;
			A.m_bAyEnabled = A.m_bAyEnabled && DO.m_bAyEnabled;
			A.m_bTurboSound = A.m_bTurboSound || DO.m_bTurboSound;
			A.m_bVfsReadOnly = A.m_bVfsReadOnly || DO.m_bVfsReadOnly;
			if (!A.m_bIntWindowExplicit)
			{
				A.m_IntWindowMode = DO.m_IntWindowMode;
			}
			if (!A.m_bTapTypeFrameSet)
			{
				A.m_TapTypeFrame = DO.m_TapTypeFrame;
				A.m_bTapTypeFrameSet = DO.m_bTapTypeFrameSet;
			}
		}
		// Apply CLI overrides (explicit CLI wins; otherwise use dump's path).
		// Dump paths are stored relative to the dump file; resolve them against
		// the dump's directory so they work regardless of the host CWD.
		if (A.m_Rom48Path.empty())
		{
			A.m_Rom48Path = ResolveRefPathFromDump(DumpRefs.m_Rom48Path, A.m_ResumePath);
		}
		if (A.m_Rom128Path.empty())
		{
			A.m_Rom128Path = ResolveRefPathFromDump(DumpRefs.m_Rom128Path, A.m_ResumePath);
		}
		if (A.m_RomTrdPath.empty())
		{
			A.m_RomTrdPath = ResolveRefPathFromDump(DumpRefs.m_RomTrdPath, A.m_ResumePath);
		}
		if (A.m_TrdPath.empty())
		{
			A.m_TrdPath = ResolveRefPathFromDump(DumpRefs.m_TrdDiskPath, A.m_ResumePath);
		}
		if (A.m_TapPath.empty())
		{
			A.m_TapPath = ResolveRefPathFromDump(DumpRefs.m_TapPath, A.m_ResumePath);
		}
		if (A.m_RomPlus3Path.empty())
		{
			A.m_RomPlus3Path = ResolveRefPathFromDump(DumpRefs.m_RomPlus3Path, A.m_ResumePath);
		}
		if (A.m_DckPath.empty())
		{
			A.m_DckPath = ResolveRefPathFromDump(DumpRefs.m_DckPath, A.m_ResumePath);
		}
		if (A.m_InputPath.empty())
		{
			A.m_InputPath = ResolveRefPathFromDump(DumpRefs.m_InputScriptPath, A.m_ResumePath);
			A.m_InputTapFrames = DumpCfg.m_InputTapFrames;
			A.m_bInputLog = DumpCfg.m_bInputLog;
		}
		if (!A.m_bMachineExplicit)
		{
			if (DumpCfg.m_Machine == "spectrum48k")
			{
				A.m_Machine = MachineModel::kMachineModelSpectrum48K;
			}
			else if (DumpCfg.m_Machine == "spectrum128k")
			{
				A.m_Machine = MachineModel::kMachineModelSpectrum128K;
			}
			else if (DumpCfg.m_Machine == "pentagon128")
			{
				A.m_Machine = MachineModel::kMachineModelPentagon128;
			}
			else if (DumpCfg.m_Machine == "spectrumplus2a")
			{
				A.m_Machine = MachineModel::kMachineModelSpectrumPlus2A;
			}
			else if (DumpCfg.m_Machine == "spectrumplus3")
			{
				A.m_Machine = MachineModel::kMachineModelSpectrumPlus3;
			}
			else if (DumpCfg.m_Machine == "next50")
			{
				A.m_Machine = MachineModel::kMachineModelNext50;
			}
			else if (DumpCfg.m_Machine == "next60")
			{
				A.m_Machine = MachineModel::kMachineModelNext60;
			}
			else if (DumpCfg.m_Machine == "ts2068")
			{
				A.m_Machine = MachineModel::kMachineModelTs2068;
			}
			else if (DumpCfg.m_Machine == "tc2068")
			{
				A.m_Machine = MachineModel::kMachineModelTc2068;
			}
			else if (DumpCfg.m_Machine == "tc2048")
			{
				A.m_Machine = MachineModel::kMachineModelTc2048;
			}
			else if (DumpCfg.m_Machine == "zxevo")
			{
				A.m_Machine = MachineModel::kMachineModelZxEvo;
			}
			else if (DumpCfg.m_Machine == "zxuno")
			{
				A.m_Machine = MachineModel::kMachineModelZxUno;
			}
			A.m_bMachineExplicit = true;
		}
		if (DumpCfg.m_bHasPsg && A.m_PsgOutPath.empty())
		{
			A.m_PsgOutPath = ResolveRefPathFromDump(DumpCfg.m_PsgOutPath, A.m_ResumePath);
			A.m_PsgRate = DumpCfg.m_PsgRate;
		}
		// If the dump came from a --nex run, plumb the .nex path through so
		// we can re-open the streaming file handle below.
		if (!DumpCfg.m_NexPath.empty() && A.m_NexPath.empty())
		{
			A.m_NexPath = ResolveRefPathFromDump(DumpCfg.m_NexPath, A.m_ResumePath);
		}
		// Restore the effective CPU clock from the dump. Without this,
		// a NEX run that flipped into 28 MHz turbo on cold-start (most
		// demos do) gets resumed with the 3.5 MHz default - so Budget
		// = Cpu.Cycles + Mhz*Seconds*1e6 falls 8x short and the demo
		// barely advances per iteration. The dump's clock is authoritative
		// unless the user explicitly overrides; marking it explicit lets
		// the nominal-clock resolution below keep it. On machines without
		// a speed switch the nominal clock wins regardless (old dumps
		// recorded the pre-nominal 3.5 for e.g. ts2068).
		if (DumpCfg.m_Mhz > 0.0 && !A.m_bMhzExplicit && MachineHonoursMhzFlag(A.m_Machine))
		{
			A.m_Mhz = DumpCfg.m_Mhz;
			A.m_bMhzExplicit = true;
		}
		// Contention on/off round-trips through the dump unless the user
		// explicitly overrides it on the resume command line. Since contention
		// is now default-on, a dump that was made with it OFF must resume OFF -
		// so treat the dump's recorded state as explicit (contention_x > 0 = on).
		if (!A.m_bContentionExplicit)
		{
			A.m_bContention = (DumpCfg.m_ContentionX > 0);
			A.m_bContentionExplicit = true;
			if (DumpCfg.m_ContentionX > 0)
			{
				A.m_ContentionX = DumpCfg.m_ContentionX;
			}
		}
		// Timex machines use the 48K memory layout / ROM (Home ROM via --rom48).
		const bool bResume48Class =
			(A.m_Machine == MachineModel::kMachineModelSpectrum48K ||
				A.m_Machine == MachineModel::kMachineModelTs2068 || A.m_Machine == MachineModel::kMachineModelTc2068 ||
				A.m_Machine == MachineModel::kMachineModelTc2048);
		SrcKind = bResume48Class ? SnaKind::kSnaKindK48 : SnaKind::kSnaKindK128;
		bSrcWantsTrdos = DumpCfg.m_bTrdosSupportActive;

		// SHA1 verification - warn (don't fail) if a referenced file's contents
		// differ from what the dump recorded. CLI overrides are checked too:
		// the user gets to see whether their override matches the dump's
		// expectation, which is often the most useful diagnostic.
		auto VerifyRefSha = [&](const std::string& Path, const std::string& Expected, const char* Label)
		{
			if (Path.empty() || Expected.empty())
			{
				return;
			}
			const std::string Actual = Sha1FileHex(Path.c_str());
			if (Actual.empty())
			{
				return; // file missing -> later load step errors properly
			}
			if (Actual != Expected)
			{
				fprintf(stderr,
					"warning: %s at '%s' has sha1 %s but dump expected %s; resume may not be deterministic\n", Label,
					Path.c_str(), Actual.c_str(), Expected.c_str());
			}
		};
		VerifyRefSha(A.m_Rom48Path, DumpRefs.m_Rom48Sha1, "rom48");
		VerifyRefSha(A.m_Rom128Path, DumpRefs.m_Rom128Sha1, "rom128");
		VerifyRefSha(A.m_RomTrdPath, DumpRefs.m_RomTrdSha1, "rom_trdos");
		VerifyRefSha(A.m_TrdPath, DumpRefs.m_TrdDiskSha1, "trd");
		VerifyRefSha(A.m_InputPath, DumpRefs.m_InputScriptSha1, "input_script");
	}
	else if (!A.m_DotPath.empty())
	{
		// Dot-command source - Next/128K-style memory, no SNA probe needed.
		SrcKind = SnaKind::kSnaKindK128;
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = MachineModel::kMachineModelNext50;
			A.m_bMachineExplicit = true;
		}
	}
	else if (!A.m_NexPath.empty())
	{
		// NEX files use the Next/128K layout and default to Next50.
		// Disable the reset trap because NEX programs may map RAM at $0000.
		SrcKind = SnaKind::kSnaKindK128;
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = MachineModel::kMachineModelNext50;
			A.m_bMachineExplicit = true;
		}
		A.m_bResetTrap = false;
	}
	else if (!A.m_TapPath.empty())
	{
		// .tap cold-boots the ROM (no snapshot to probe). Default to 48K unless
		// the user asked for 128K - 128K tapes load via the boot menu's Tape
		// Loader. The tape is served during the run via the LD-BYTES trap.
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = MachineModel::kMachineModelSpectrum48K;
			A.m_bMachineExplicit = true;
		}
		SrcKind = Is128ClassMachine(A.m_Machine) ? SnaKind::kSnaKindK128 : SnaKind::kSnaKindK48;
	}
	else if (!A.m_DckPath.empty())
	{
		// A cartridge on its own: cold-boot the Home ROM and let it find the
		// LROS. Only the 2068 has a dock, so default the machine to it rather
		// than making every cartridge run spell out --machine.
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = MachineModel::kMachineModelTs2068;
			A.m_bMachineExplicit = true;
		}
		SrcKind = SnaKind::kSnaKindK48;
	}
	else if (!A.m_TrdRunName.empty())
	{
		// TR-DOS run-by-name cold-boots BASIC; TR-DOS pages in when the auto-typed
		// RANDOMIZE USR 15616 jumps PC into $3Dxx. 48K by default; --machine 128k
		// boots the 128 menu, selects 128 BASIC (which leaves $7FFD paging
		// unlocked, unlike 48 BASIC) so 128K demos can page. Needs the ROMs.
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = MachineModel::kMachineModelSpectrum48K;
			A.m_bMachineExplicit = true;
		}
		SrcKind = Is128ClassMachine(A.m_Machine) ? SnaKind::kSnaKindK128 : SnaKind::kSnaKindK48;
	}
	else if (!A.m_Z80Path.empty())
	{
		// Probe .z80 to auto-detect the machine (from the hardware-mode byte)
		// and thus which ROMs are needed. Timex/48K need --rom48; 128K/Pentagon
		// need both. The detected machine wins unless --machine was explicit.
		Z80SnapProbeResult Probe = ProbeZ80(A.m_Z80Path.c_str());
		if (!Probe.m_bOk)
		{
			fprintf(stderr, ".z80 error: %s\n", Probe.m_ErrorMessage.c_str());
			return 3;
		}
		if (!A.m_bMachineExplicit)
		{
			A.m_Machine = Probe.m_Machine;
			A.m_bMachineExplicit = true;
		}
		SrcKind = Is128ClassMachine(Probe.m_Machine) ? SnaKind::kSnaKindK128 : SnaKind::kSnaKindK48;
	}
	else
	{
		// Probe SNA to know which ROMs are needed before loading.
		SnaProbeResult Probe = ProbeSna(A.m_SnaPath.c_str());
		if (!Probe.m_bOk)
		{
			fprintf(stderr, "SNA error: %s\n", Probe.m_ErrorMessage.c_str());
			return 3;
		}
		SrcKind = Probe.m_Kind;
	}

	// Heap-allocated: MemoryBus carries up to 2 MB of NextRAM (256 8K pages
	// of physical storage), which overflows the default thread stack on
	// Windows. The effective RAM size defaults to 2 MB (args.h RamMb=2);
	// drop to 1 MB (= real Spectrum Next stock) via --ram-mb 1.
	auto MemHolder = std::make_unique<MemoryBus>();
	MemoryBus& Mem = *MemHolder;
	Mem.SetRamSizeMb(A.m_RamMb);

	// Load ROMs. Combined 32 KB ROMs (front=128K editor, back=48K BASIC) are
	// accepted for both --rom48 and --rom128; the loader picks the right half.
	if (!A.m_Rom48Path.empty())
	{
		RomLoadResult R = LoadRom16K(A.m_Rom48Path.c_str(), /*bUseBackHalfIfCombined=*/true);
		if (!R.m_bOk)
		{
			fprintf(stderr, "%s\n", R.m_ErrorMessage.c_str());
			return 3;
		}
		Mem.SetRom48(R.m_Data.data());
	}
	if (!A.m_Rom128Path.empty())
	{
		RomLoadResult R = LoadRom16K(A.m_Rom128Path.c_str(), /*bUseBackHalfIfCombined=*/false);
		if (!R.m_bOk)
		{
			fprintf(stderr, "%s\n", R.m_ErrorMessage.c_str());
			return 3;
		}
		Mem.SetRom128(R.m_Data.data());
	}
	// +2A/+3: four 16K ROMs from a single 64 KB image (--rom-plus3).
	if ((A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
			A.m_Machine == MachineModel::kMachineModelSpectrumPlus3) &&
		!A.m_RomPlus3Path.empty())
	{
		if (FILE* F = fopen(A.m_RomPlus3Path.c_str(), "rb"))
		{
			std::vector<uint8_t> Buf(size_t(4) * 16384);
			const size_t Got = fread(Buf.data(), 1, Buf.size(), F);
			fclose(F);
			if (Got == Buf.size())
			{
				for (int Ri = 0; Ri < 4; ++Ri)
				{
					Mem.SetRomPlus3(Ri, Buf.data() + size_t(Ri) * 16384);
				}
			}
			else
			{
				fprintf(stderr, "--rom-plus3: expected 64 KB (four 16K ROMs), got %zu bytes\n", Got);
			}
		}
		else
		{
			fprintf(stderr, "--rom-plus3: cannot open %s\n", A.m_RomPlus3Path.c_str());
		}
	}
	// TS/TC2068 EXROM (8K). Not optional in any meaningful sense: tape I/O and
	// the extended-video entry points live in the EXROM, not the Home ROM, so a
	// 2068 without it cannot load a tape. Fuse names the pair tc2068-0.rom /
	// tc2068-1.rom, so derive the second from whatever Home ROM was passed to
	// --rom48 rather than demanding another flag. Absence is a warning, not a
	// failure - the Home ROM alone is still enough to run a snapshot.
	if (A.m_Machine == MachineModel::kMachineModelTs2068 || A.m_Machine == MachineModel::kMachineModelTc2068)
	{
		std::string ExPath;
		const std::string Tail = "-0.rom";
		if (A.m_Rom48Path.size() > Tail.size() &&
			A.m_Rom48Path.compare(A.m_Rom48Path.size() - Tail.size(), Tail.size(), Tail) == 0)
		{
			ExPath = A.m_Rom48Path.substr(0, A.m_Rom48Path.size() - Tail.size()) + "-1.rom";
		}
		bool bExLoaded = false;
		if (!ExPath.empty())
		{
			if (FILE* F = fopen(ExPath.c_str(), "rb"))
			{
				std::vector<uint8_t> Ex(8192, 0xFF);
				const size_t Got = fread(Ex.data(), 1, Ex.size(), F);
				fclose(F);
				if (Got == Ex.size())
				{
					Mem.SetRomTimexEx(Ex.data());
					bExLoaded = true;
				}
			}
		}
		if (!bExLoaded)
		{
			fprintf(stderr,
				"warning: no 2068 EXROM found (looked for '%s').\n"
				"         Tape loading and DOCK/EXROM paging need it; expected the\n"
				"         tc2068-1.rom that ships beside the Home ROM.\n",
				ExPath.empty() ? "<derive from --rom48 name>" : ExPath.c_str());
		}
	}

	// Timex cartridge. The DOCK bank is 2068-only hardware; the SCLD paging that
	// reaches it is already there, this just gives it something to page in. The
	// Home ROM does the rest: it looks for an LROS header at DOCK $0000 during
	// its own startup and jumps into the cartridge, so no boot special-casing
	// belongs here.
	if (!A.m_DckPath.empty())
	{
		if (A.m_Machine != MachineModel::kMachineModelTs2068 && A.m_Machine != MachineModel::kMachineModelTc2068)
		{
			fprintf(stderr, "--dck: only the TS/TC2068 has a dock; "
							"use --machine ts2068\n");
			return 4;
		}
		Dck::Cartridge Cart;
		std::string DckErr;
		if (!Dck::LoadFile(A.m_DckPath.c_str(), Cart, DckErr))
		{
			fprintf(stderr, "--dck: %s (%s)\n", DckErr.c_str(), A.m_DckPath.c_str());
			return 4;
		}
		for (const Dck::Block& B : Cart.m_Blocks)
		{
			for (int IdxI = 0; IdxI < Dck::kChunksPerBank; ++IdxI)
			{
				const Dck::Chunk& C = B.m_Chunks[IdxI];
				if (C.m_Kind == Dck::ChunkKind::kEKindAbsent)
				{
					continue;
				}
				const bool bRam = (C.m_Kind != Dck::ChunkKind::kEKindRom);
				if (B.m_Bank == Dck::Bank::kEBankDock)
				{
					Mem.SetDockChunk(IdxI, C.m_Data.data(), bRam);
				}
				else if (B.m_Bank == Dck::Bank::kEBankExrom && IdxI == 0)
				{
					// A cartridge may carry its own EXROM. It is 8K and answers
					// every chunk it is selected for, so only chunk 0 is
					// meaningful; it replaces whatever tc2068-1.rom supplied.
					Mem.SetRomTimexEx(C.m_Data.data());
				}
				// HOME chunks in a .dck would overwrite the machine's own RAM
				// image before the ROM has booted, which the cold-boot path
				// would immediately undo. Ignored rather than half-applied.
			}
		}
	}
	if (!A.m_RomTrdPath.empty())
	{
		RomLoadResult R = LoadRom16K(A.m_RomTrdPath.c_str());
		if (!R.m_bOk)
		{
			fprintf(stderr, "%s\n", R.m_ErrorMessage.c_str());
			return 3;
		}
		Mem.SetRomTrd(R.m_Data.data());
	}

	// Validate ROM availability against source kind. NEX and dot-cmd sources
	// can run without any user-supplied ROM - they immediately remap RAM into
	// slot 0/1 via NR_50/NR_51 and route any RST $08 / IM 1 activity through
	// our C++ traps + the $5B00 stub. Classic SNA loads (48K / 128K /
	// Pentagon) genuinely need the Sinclair ROMs for the BASIC environment.
	const bool bRomOptional = !A.m_DotPath.empty() || !A.m_NexPath.empty();
	if (!bRomOptional)
	{
		if (SrcKind == SnaKind::kSnaKindK48)
		{
			if (!Mem.HasRom48())
			{
				fprintf(stderr, "48K source requires --rom48 (or --rom48128 for a combined 32 KB ROM)\n");
				return 4;
			}
		}
		else if (A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
				 A.m_Machine == MachineModel::kMachineModelSpectrumPlus3)
		{
			if (!Mem.HasRomPlus3())
			{
				fprintf(stderr, "+2A/+3 source requires --rom-plus3 <64 KB four-ROM image>\n");
				return 4;
			}
		}
		else
		{
			if (!Mem.HasRom48() || !Mem.HasRom128())
			{
				fprintf(stderr, "128K source requires both --rom48 and --rom128 (or pass --rom48128 with a combined 32 "
								"KB ROM file)\n");
				return 4;
			}
		}
	}
	else if (!Mem.HasRom48() || !Mem.HasRom128())
	{
		// ROM-free --dot / --nex runs still need an IM 1 vector: a program
		// that EIs in IM 1 takes an interrupt through $0038, which with no
		// ROM loaded is open bus ($FF) and execution grinds. Install a
		// minimal synthetic ROM: all $FF plus a self-contained $0038 handler
		// (bump FRAMES, preserve HL, ei/reti - no $5B00 dependency).
		std::vector<uint8_t> Fallback(0x4000, 0xFF);
		static const uint8_t Im1Handler[] = {
			0xE5, // push hl
			0x2A, 0x78, 0x5C, // ld hl, (FRAMES = $5C78)
			0x23, // inc hl
			0x22, 0x78, 0x5C, // ld (FRAMES), hl
			0xE1, // pop hl
			0xFB, // ei
			0xED, 0x4D // reti
		};
		memcpy(Fallback.data() + 0x38, Im1Handler, sizeof Im1Handler);
		if (!Mem.HasRom48())
		{
			Mem.SetRom48(Fallback.data());
		}
		if (!Mem.HasRom128())
		{
			Mem.SetRom128(Fallback.data());
		}
	}

	// Z80 + memory state: either fresh SNA load, or restore from dump.
	Z80State Cpu;
	SnaLoadResult Load;
	Load.m_Kind = SrcKind;
	Load.m_bWantsTrdosRom = bSrcWantsTrdos;
	Load.m_bOk = true;
	DotCmdLoadResult DotCmdLoad;
	// Establish identity once, before execution. On resume the snapshot's
	// original identity survives even if the executable was rebuilt or moved.
	const std::string CurrentNexSha = A.m_NexPath.empty() ? "" : Sha1FileHex(A.m_NexPath.c_str());
	const std::string LoadedNexSha = bResuming ? DumpCfg.m_NexSha1 : CurrentNexSha;
	if (bResuming && !LoadedNexSha.empty() && !CurrentNexSha.empty() && CurrentNexSha != LoadedNexSha &&
		A.m_AllowBuildMismatch.empty())
	{
		fprintf(stderr,
			"resume NEX differs from the loaded build; use --allow-build-mismatch REASON for an intentional patch\n");
		return 4;
	}
	if (!A.m_BuildManifestPath.empty())
	{
		std::string IdentityError;
		if (!ValidateBuildManifest(
				A.m_BuildManifestPath, LoadedNexSha, A.m_SldPaths, A.m_AllowBuildMismatch, IdentityError))
		{
			fprintf(stderr, "%s\n", IdentityError.c_str());
			return 4;
		}
	}

	NexLoadResult NexLoad;
	Z80SnapLoadResult Z80Load;
	TapePlayer Tape;
	if (bResuming && !A.m_TapPath.empty())
	{
		// The tape, wound on to where the dumped run had read it.
		std::string TapErr;
		if (!Tape.Load(A.m_TapPath.c_str(), TapErr))
		{
			fprintf(stderr, ".tap error: %s\n", TapErr.c_str());
			return 3;
		}
		const JsonValue* T = DumpRoot.Find("tape");
		if (T && T->IsObject())
		{
			uint64_t Cursor = 0, Bytes = 0, Blocks = 0;
			const JsonValue* C = T->Find("block_cursor");
			const JsonValue* By = T->Find("bytes_delivered");
			const JsonValue* Bl = T->Find("blocks_delivered");
			if (!C || !C->TryAsUInt(Cursor) || Cursor > Tape.BlockCount() || !By || !By->TryAsUInt(Bytes) || !Bl ||
				!Bl->TryAsUInt(Blocks))
			{
				fprintf(stderr, "resume load error: invalid tape block\n");
				return 3;
			}
			Tape.SetCursor(size_t(Cursor));
			Tape.RestoreCounters(Bytes, Blocks);
		}
	}
	if (bResuming)
	{
		std::string Err;
		if (!LoadMemoryFromDump(DumpRoot, Mem, Err) || !LoadRamFromDump(DumpRoot, Mem, Err) ||
			!LoadCpuFromDump(DumpRoot, Cpu, Err) || !LoadSchedulerFromDump(DumpRoot, Cpu, Err) ||
			!LoadStatsFromDump(DumpRoot, Cpu, Err) || !LoadNextFromDump(DumpRoot, Mem, Err) ||
			!LoadCtcFromDump(DumpRoot, Mem, Err) || !LoadCopperFromDump(DumpRoot, Mem, Err) ||
			!LoadBusExtrasFromDump(DumpRoot, Mem, Err) || !LoadSlotsFromDump(DumpRoot, Mem, Err))
		{
			fprintf(stderr, "resume load error: %s\n", Err.c_str());
			return 3;
		}
	}
	else if (!A.m_DotPath.empty())
	{
		// Dot-command load: map 128K-style memory (bank 0 at $C000), then
		// copy the binary at $2000 and set up registers per the convention.
		Mem.Map128K(0, false);
		DotCmdLoadResult R = LoadDotCmd(A.m_DotPath.c_str(), A.m_DotArg, Cpu, Mem);
		if (!R.m_bOk)
		{
			fprintf(stderr, "dot-command error: %s\n", R.m_ErrorMessage.c_str());
			return 3;
		}
		DotCmdLoad = R;
	}
	else if (!A.m_NexPath.empty())
	{
		std::string NexErr;
		NexLoad = LoadNex(A.m_NexPath.c_str(), Cpu, Mem, NexErr);
		if (!NexLoad.m_bOk)
		{
			fprintf(stderr, "NEX error: %s\n", NexErr.c_str());
			return 3;
		}
	}
	else if (!A.m_TapPath.empty())
	{
		// Cold boot: map the ROM and start the CPU at reset ($0000). The ROM
		// brings up BASIC; the auto-typed LOAD "" (added to the input stream
		// below) drives it, and the LD-BYTES trap serves the tape blocks.
		std::string TapErr;
		if (!Tape.Load(A.m_TapPath.c_str(), TapErr))
		{
			fprintf(stderr, ".tap error: %s\n", TapErr.c_str());
			return 3;
		}
		if (A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
			A.m_Machine == MachineModel::kMachineModelSpectrumPlus3)
		{
			Mem.SetIsPlus3(true);
			Mem.MapPlus3(0x00, 0x00); // ROM 0 (128 editor/menu), normal paging
		}
		else if (SrcKind == SnaKind::kSnaKindK128)
		{
			Mem.Map128K(0, /*bUseTrdosRom=*/false);
		}
		else
		{
			Mem.Map48K();
		}
		Cpu = Z80State{}; // reset registers
		Cpu.PC = 0x0000;
		Cpu.SP = 0xFFFF;
		Cpu.AF = 0xFFFF;
		Cpu.IX = 0xFFFF;
		Cpu.IY = 0xFFFF;
		Cpu.IM = 0;
		Cpu.IFF1 = 0;
		Cpu.IFF2 = 0;
		Load.m_Kind = SrcKind;
	}
	else if (!A.m_TrdRunName.empty())
	{
		// Cold boot; the disk is mounted below and the auto-typed key sequence
		// (added to the input stream) drives BASIC -> TR-DOS -> RUN "name".
		if (A.m_Machine == MachineModel::kMachineModelSpectrum128K ||
			A.m_Machine == MachineModel::kMachineModelPentagon128)
		{
			// Boot 48K BASIC on a 128K machine with paging left UNLOCKED
			// (Port7FFD bit4=1 selects ROM48, bit5=0 keeps $7FFD writable). This
			// is the Pentagon-like state: RANDOMIZE USR 15616 pages TR-DOS in
			// (the 48K lower-half ROM is active) AND the loaded demo can still
			// page banks via OUT $7FFD - unlike the 128 menu's "48 BASIC" which
			// locks paging. Avoids the boot-menu navigation entirely.
			Mem.Map128K(0x10, /*bUseTrdosRom=*/false);
		}
		else
		{
			Mem.Map48K();
		}
		Cpu = Z80State{};
		Cpu.PC = 0x0000;
		Cpu.SP = 0xFFFF;
		Cpu.AF = 0xFFFF;
		Cpu.IX = 0xFFFF;
		Cpu.IY = 0xFFFF;
		Cpu.IM = 0;
		Cpu.IFF1 = 0;
		Cpu.IFF2 = 0;
		Load.m_Kind = SrcKind;
	}
	else if (!A.m_DckPath.empty())
	{
		// Cartridge cold boot. Nothing to load into RAM: the dock chunks were
		// populated when the file was read, and the ROM's startup does the
		// rest - it reads the LROS header out of dock $0000 and jumps to it.
		Mem.Map48K();
		Cpu = Z80State{};
		Cpu.PC = 0x0000;
		Cpu.SP = 0xFFFF;
		Cpu.AF = 0xFFFF;
		Cpu.IX = 0xFFFF;
		Cpu.IY = 0xFFFF;
		Cpu.IM = 0;
		Cpu.IFF1 = 0;
		Cpu.IFF2 = 0;
		Load.m_Kind = SrcKind;
	}
	else if (!A.m_Z80Path.empty())
	{
		Z80Load = LoadZ80(A.m_Z80Path.c_str(), Cpu, Mem);
		if (!Z80Load.m_bOk)
		{
			fprintf(stderr, ".z80 error: %s\n", Z80Load.m_ErrorMessage.c_str());
			return 3;
		}
		Mem.SetBorderColour(Z80Load.m_Border);
		// Keep Load.Kind consistent so the machine-mismatch warnings below (which
		// read Load.Kind) don't fire spuriously on a 128K/Timex .z80.
		Load.m_Kind = SrcKind;
	}
	else
	{
		Load = LoadSna(A.m_SnaPath.c_str(), Cpu, Mem);
		if (!Load.m_bOk)
		{
			fprintf(stderr, "SNA error: %s\n", Load.m_ErrorMessage.c_str());
			return 3;
		}
		if (Load.m_Kind == SnaKind::kSnaKindK128 && Load.m_bWantsTrdosRom && !Mem.HasRomTrd())
		{
			fprintf(stderr, "128K SNA has TR-DOS flag set; --rom-trdos required\n");
			return 4;
		}
	}

	// Classic snapshots do not store the +3's second paging latch. Select
	// the corresponding BASIC/editor ROM and enable subsequent $1FFD writes.
	if (!bResuming && (!A.m_SnaPath.empty() || !A.m_Z80Path.empty()) &&
		(A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
			A.m_Machine == MachineModel::kMachineModelSpectrumPlus3))
	{
		const uint8_t Port7ffd = Mem.GetPort7FFD();
		Mem.SetIsPlus3(true);
		Mem.MapPlus3(Port7ffd, (Port7ffd & 0x10) ? 0x04 : 0x00);
	}

	// Machine model selection. Default from SNA kind; warn on mismatch in either direction.
	if (!A.m_bMachineExplicit)
	{
		A.m_Machine = (Load.m_Kind == SnaKind::kSnaKindK128) ? MachineModel::kMachineModelSpectrum128K
															 : MachineModel::kMachineModelSpectrum48K;
	}
	{
		const bool bSnaIs48K = (Load.m_Kind == SnaKind::kSnaKindK48);
		// Timex machines are 48K-class (48K memory layout + Spectrum timing), so
		// a Timex --machine on a 48K SNA is a match, not a mismatch.
		const bool bMachIs48K =
			(A.m_Machine == MachineModel::kMachineModelSpectrum48K ||
				A.m_Machine == MachineModel::kMachineModelTs2068 || A.m_Machine == MachineModel::kMachineModelTc2068 ||
				A.m_Machine == MachineModel::kMachineModelTc2048);
		if (bSnaIs48K && !bMachIs48K)
		{
			fprintf(stderr, "warning: --machine selects 128K-style timing on a 48K SNA\n");
		}
		else if (!bSnaIs48K && bMachIs48K)
		{
			fprintf(stderr, "warning: --machine 48k on a 128K SNA - paging and timing will not match\n");
		}
	}

	// ---- Nominal machine clock. The machine is final by here (--machine,
	// NEX header, SNA kind default, or resume dump), so resolve the effective
	// CPU clock: every machine runs at its real nominal speed; --mhz picks the
	// initial speed only where real hardware has such a switch. Emulated
	// seconds are wall seconds on the real machine from this point on.
	{
		const double NominalMhz = NominalMhzFor(A.m_Machine);
		if (A.m_bMhzExplicit && !MachineHonoursMhzFlag(A.m_Machine) && A.m_Mhz != NominalMhz)
		{
			fprintf(stderr,
				"--mhz is not available on this machine: it runs at its nominal %.4f MHz.\n"
				"(speed switches exist on next50/next60/zxuno; zxevo switches via #EFF7)\n",
				NominalMhz);
			return 4;
		}
		if (!(A.m_bMhzExplicit && MachineHonoursMhzFlag(A.m_Machine)))
		{
			// ZxEvo hardware boots with turbo ON (#EFF7 bit4 = 0 => 7 MHz);
			// runtime #EFF7/#FF77 writes switch it thereafter.
			// A resumed Evo runs at the clock its latches were left at (FireEvoTurbo
			// below then has nothing to rescale); a cold one boots at 7.
			A.m_Mhz = (A.m_Machine == MachineModel::kMachineModelZxEvo)
						  ? ((bResuming && DumpCfg.m_Mhz > 0.0) ? DumpCfg.m_Mhz : 7.0)
						  : NominalMhz;
		}
		if (A.m_bHaveUntilFrame)
		{
			// Land just past frame N at the machine's real frame rate
			// (recomputes the provisional parse-time seconds).
			const uint64_t P = FrameIntPeriodFor(A.m_Machine, A.m_Mhz);
			A.m_Seconds = (A.m_UntilFrame + 0.5) * double(P) / (A.m_Mhz * 1'000'000.0);
		}
		if (A.m_bTraceStartInSeconds)
		{
			const double TraceCycles = A.m_Mhz * 1'000'000.0 * A.m_TraceStartSeconds + 0.5;
			if (!(TraceCycles >= 0.0 && TraceCycles < 18446744073709551616.0))
			{
				fprintf(stderr, "trace start exceeds the cycle counter range\n");
				return 4;
			}
			A.m_TraceStartCycle = uint64_t(TraceCycles);
			A.m_bHaveTraceStart = true;
		}
	}

	// Budget is RELATIVE on resume - adds Seconds*Mhz cycles to Cpu.Cycles
	// restored from the dump. RunUntil exits when Cpu.Cycles >= Budget.
	// Non-const so the NR_07 (TURBO_CONTROL) callback below can rescale it
	// when the dot command flips into 7/14/28 MHz turbo: the user's
	// --seconds is wall-clock time, so a faster CPU means more cycles
	// before the budget runs out.
	// --cycles bypasses the secondsxMhz path and is treated as an absolute
	// (or relative-on-resume) cycle delta. Turbo rescale is skipped below.
	const double SecondsCycles = A.m_Mhz * 1'000'000.0 * A.m_Seconds + 0.5;
	if (!A.m_bHaveCycles && !(SecondsCycles >= 0.0 && SecondsCycles < 18446744073709551616.0))
	{
		fprintf(stderr, "duration exceeds the cycle counter range\n");
		return 4;
	}
	const uint64_t BudgetDelta = A.m_bHaveCycles ? A.m_Cycles : uint64_t(SecondsCycles);
	if (bResuming && BudgetDelta > UINT64_MAX - Cpu.m_Cycles)
	{
		fprintf(stderr, "resumed duration exceeds the cycle counter range\n");
		return 4;
	}
	uint64_t Budget = bResuming ? (Cpu.m_Cycles + BudgetDelta) : BudgetDelta;

	// Frame interrupt period: --no-interrupts > explicit --frame-int 0 disable > --frame-int N override > machine
	// default.
	const bool bDisable = A.m_bNoInterrupts || (A.m_bFrameIntExplicit && A.m_FrameIntOverride == 0);
	const uint64_t FrameIntPeriod = bDisable					? 0
									: A.m_FrameIntOverride != 0 ? A.m_FrameIntOverride
																: FrameIntPeriodFor(A.m_Machine, A.m_Mhz);
	// When resuming, scheduler state (NextFrameIntCycle, NextStatsTickCycle,
	// NextPsgTickCycle, FrameIntPeriod, StatsTickPeriod, PsgTickPeriod) is
	// already restored from the dump. Don't re-arm - EXCEPT when the user
	// explicitly asked for a different period: silently keeping the dump's
	// period once cost a measurement session.
	if (!bResuming)
	{
		if (FrameIntPeriod != 0)
		{
			EnableFrameInterrupts(Cpu, FrameIntPeriod);
		}
		// The stats tick doubles as the frame cadence for the frame-exact
		// --bmp-out checkpoints, so arm it when either consumer wants it.
		if (!A.m_bNoStatus || (A.m_bBmpExact && !A.m_bNoBmp))
		{
			const uint64_t StatsPeriod = FrameIntPeriod ? FrameIntPeriod : FrameIntPeriodFor(A.m_Machine, A.m_Mhz);
			EnableStatsTicks(Cpu, StatsPeriod);
		}
	}
	else if (Cpu.m_StatsTickPeriod == 0 && (!A.m_bNoStatus || (A.m_bBmpExact && !A.m_bNoBmp)))
	{
		// the dumped run had no stats tick; this one's status or exact
		// --bmp-out needs it
		EnableStatsTicks(Cpu, FrameIntPeriod ? FrameIntPeriod : FrameIntPeriodFor(A.m_Machine, A.m_Mhz));
	}
	if (bResuming && (A.m_bFrameIntExplicit || A.m_bNoInterrupts))
	{
		if (FrameIntPeriod != 0)
		{
			EnableFrameInterrupts(Cpu, FrameIntPeriod); // re-arm at the override
		}
		else
		{
			DisableFrameInterrupts(Cpu);
		}
	}

	// TR-DOS: optionally mount a disk, then decide whether auto page-in/out is active.
	Trdos Trd;
	if (!A.m_TrdPath.empty())
	{
		if (!Mem.HasRomTrd())
		{
			fprintf(stderr, "--trd requires --rom-trdos\n");
			return 4;
		}
		std::string MountErr;
		if (!Trd.MountDisk(A.m_TrdPath.c_str(), MountErr))
		{
			fprintf(stderr, "%s\n", MountErr.c_str());
			return 3;
		}
	}
	{
		const bool bSnaTrdos = (Load.m_Kind == SnaKind::kSnaKindK128 && Load.m_bWantsTrdosRom);
		bool bSupportActive = false;
		switch (A.m_TrdosModeFlag)
		{
		case TrdosMode::kTrdosModeOff:
			bSupportActive = false;
			break;
		case TrdosMode::kTrdosModeOn:
			bSupportActive = Mem.HasRomTrd();
			break;
		case TrdosMode::kTrdosModeSna:
			bSupportActive = bSnaTrdos && Mem.HasRomTrd();
			break;
		case TrdosMode::kTrdosModeAuto:
			bSupportActive = Mem.HasRomTrd() && (Trd.IsDiskMounted() || bSnaTrdos);
			break;
		}
		if (bResuming && !A.m_bTrdosModeExplicit)
		{
			// what the dumped run had decided, whatever mode decided it
			bSupportActive = DumpCfg.m_bTrdosSupportActive && Mem.HasRomTrd();
		}
		Trd.SetSupportActive(bSupportActive);
	}
	if (bResuming)
	{
		std::string Err;
		if (!LoadTrdosFromDump(DumpRoot, Trd, Err))
		{
			fprintf(stderr, "resume trdos error: %s\n", Err.c_str());
			return 3;
		}
	}

	OutputPaths OutputFiles;
	for (const std::string* Path :
		{&A.m_SnaPath, &A.m_Z80Path, &A.m_TapPath, &A.m_DckPath, &A.m_TrdPath, &A.m_Rom48Path, &A.m_Rom128Path,
			&A.m_RomTrdPath, &A.m_RomPlus3Path, &A.m_RomZxEvoPath, &A.m_RomZxUnoPath, &A.m_InputPath, &A.m_ResumePath,
			&A.m_DotPath, &A.m_NexPath, &A.m_BatchPath, &A.m_BuildManifestPath, &A.m_IrqDiagnosticsPath})
	{
		OutputFiles.AddInput(*Path);
	}
	for (const std::string& Path : A.m_InvariantsPaths)
	{
		OutputFiles.AddInput(Path);
	}
	for (const std::string& Path : A.m_SldPaths)
	{
		OutputFiles.AddInput(Path);
	}
	for (const std::string* Path : {&A.m_AudioOutPath, &A.m_PsgOutPath, &A.m_MotionOutPath, &A.m_HistogramOutPath,
			 &A.m_CoverageOutPath, &A.m_OptimizeOutPath, &A.m_StatusOutPath, &A.m_BmpOutPath, &A.m_DumpOutPath,
			 &A.m_TraceOutBinPath, &A.m_BatchOutPath, &A.m_InspectOutPath, &A.m_IrqDiagnosticsOutPath})
	{
		OutputFiles.AddOutput(*Path);
	}
	if (A.m_DotPath.empty() && A.m_NexPath.empty() &&
		(A.m_Machine == MachineModel::kMachineModelSpectrum48K ||
			A.m_Machine == MachineModel::kMachineModelSpectrum128K ||
			A.m_Machine == MachineModel::kMachineModelPentagon128))
	{
		OutputFiles.AddOutput(A.m_OutPath);
	}
	if (A.m_TraceOutPath != "-")
	{
		OutputFiles.AddOutput(A.m_TraceOutPath);
	}
	if (A.m_ViolationsOutPath != "-")
	{
		OutputFiles.AddOutput(A.m_ViolationsOutPath);
	}
	if (!A.m_TraceOutBinPath.empty() && !A.m_bNoTraceIndex)
	{
		OutputFiles.AddOutput(A.m_TraceIndexOutPath.empty() ? A.m_TraceOutBinPath + ".fidx" : A.m_TraceIndexOutPath);
	}
	if (!A.m_PsgOutPath.empty() && A.m_bTurboSound)
	{
		std::string Second = A.m_PsgOutPath;
		const size_t Dot = Second.find_last_of('.');
		if (Dot == std::string::npos || Second.find_first_of("/\\", Dot) != std::string::npos)
		{
			Second += ".2.psg";
		}
		else
		{
			Second.insert(Dot, ".2");
		}
		OutputFiles.AddOutput(Second);
	}
	std::string PathError;
	if (!OutputFiles.AddPattern(A.m_VideoOutPattern) || !OutputFiles.AddPattern(A.m_PaperOutPattern) ||
		!OutputFiles.Validate(PathError))
	{
		fprintf(stderr, "%s\n", PathError.empty() ? "invalid frame output pattern" : PathError.c_str());
		return 4;
	}

	// SLD symbol map (loaded regardless of whether trace is enabled - cheap and
	// diagnostic messages alone may be useful).
	SldSymbolMap SldMap;
	for (const std::string& SldPath : A.m_SldPaths)
	{
		std::string SldErr;
		if (!SldMap.LoadFile(SldPath.c_str(), SldErr))
		{
			fprintf(stderr, "%s\n", SldErr.c_str());
			return 3;
		}
	}

	const InspectionIdentity InspectIdentity{
		LoadedNexSha, A.m_AllowBuildMismatch.empty() ? DumpCfg.m_IdentityOverride : A.m_AllowBuildMismatch};
	const bool BoundSymbols = !A.m_BuildManifestPath.empty();
	IrqDiagnostics Irq;
	if (!A.m_IrqDiagnosticsPath.empty())
	{
		try
		{
			LoadIrqDiagnostics(A.m_IrqDiagnosticsPath, Mem, SldMap, BoundSymbols, Irq);
		}
		catch (const std::exception& E)
		{
			fprintf(stderr, "IRQ diagnostics: %s\n", E.what());
			return 4;
		}
	}

#if INSPECCY_TRACE_SUPPORT
	TraceWriter Writer;
	// Peephole reporting is opt-in and costs nothing when the flag is absent:
	// no analyzer is constructed, the trace machinery never starts, and the
	// dispatcher stays on its non-tracing path exactly as before.
	PeepholeAnalyzer Peephole;
	const bool bWantPeephole = !A.m_OptimizeOutPath.empty();
	const bool bWantTraceText = A.m_bTrace || !A.m_TraceOutPath.empty();
	const bool bWantTraceBin = !A.m_TraceOutBinPath.empty();
	const bool bWantTrace = bWantTraceText || bWantTraceBin || bWantPeephole;
	if (bWantTraceText && bWantTraceBin)
	{
		fprintf(stderr, "use either --trace-out or --trace-out-bin, "
						"not both\n");
		return 4;
	}
	if (bWantPeephole)
	{
		uint8_t Mask = PeepholeAnalyzer::kProfileProfBoth;
		if (A.m_OptimizeProfile == "speed")
		{
			Mask = PeepholeAnalyzer::kProfileProfSpeed;
		}
		else if (A.m_OptimizeProfile == "size")
		{
			Mask = PeepholeAnalyzer::kProfileProfSize;
		}
		Peephole.SetProfileMask(Mask);
		Peephole.SetMinRatio(A.m_OptimizeMinRatio);
		if (SldMap.SymbolCount() > 0)
		{
			Peephole.SetSymbols(&SldMap);
		}
		Writer.SetPeepholeAnalyzer(&Peephole);
	}
	if (bWantTrace)
	{
		if (A.m_bTrace && !A.m_TraceOutPath.empty())
		{
			fprintf(stderr, "use either --trace or --trace-out, not both\n");
			return 4;
		}
		TraceFormatOptions Opt;
		Opt.m_bVerbose = A.m_bTraceVerbose;
		Opt.m_bIncludeR = A.m_bTraceR;
		Opt.m_bIncludeMemPtr = A.m_bTraceMemPtr;
		Opt.m_bDiffOnly = A.m_bTraceDiffOnly;
		const size_t QueueBytes = size_t(A.m_TraceQueueMb) * 1024 * 1024;
		const SldSymbolMap* SymsPtr = SldMap.SymbolCount() > 0 ? &SldMap : nullptr;
		bool bOpened = false;
		if (bWantTraceBin)
		{
			BinaryOpenContext Ctx;
			Ctx.m_InitCpu = &Cpu;
			Ctx.m_InitMem = &Mem;
			Ctx.m_BaseMhz = A.m_Mhz;
			Ctx.m_FrameIntPeriod = FrameIntPeriod; // nominal T-states/frame (0 if --no-interrupts)
			Ctx.m_Machine = A.m_Machine;
			Ctx.m_bTrdosActive = false; // not yet plumbed; set by TR-DOS later
			Ctx.m_SldPaths = A.m_SldPaths;
			// Index sidecar destination:
			//   --no-trace-index    -> nullptr; writer skips .fidx entirely.
			//   --trace-index-out X -> use X verbatim.
			//   default             -> "<trace-out-bin path>.fidx".
			std::string IndexPath;
			const char* IndexPathPtr = nullptr;
			if (!A.m_bNoTraceIndex)
			{
				IndexPath = !A.m_TraceIndexOutPath.empty() ? A.m_TraceIndexOutPath : (A.m_TraceOutBinPath + ".fidx");
				IndexPathPtr = IndexPath.c_str();
			}
			bOpened =
				Writer.OpenBinary(A.m_TraceOutBinPath.c_str(), Opt, QueueBytes, SymsPtr, Argc, Argv, Ctx, IndexPathPtr);
			if (!bOpened)
			{
				fprintf(stderr, "failed to open binary trace output: %s\n", A.m_TraceOutBinPath.c_str());
				return 3;
			}
		}
		else if (bWantPeephole && !bWantTraceText)
		{
			// The analyzer is the only consumer: run the ring without any file
			// output, so --optimize-out does not also cost a trace on disk.
			bOpened = Writer.OpenAnalysisOnly(Opt, QueueBytes);
		}
		else
		{
			const char* Path = A.m_TraceOutPath.empty() ? "-" : A.m_TraceOutPath.c_str();
			bOpened = Writer.Open(Path, Opt, QueueBytes, SymsPtr, Argc, Argv);
			if (!bOpened)
			{
				fprintf(stderr, "failed to open trace output: %s\n", Path);
				return 3;
			}
		}
	}
#endif

	// Input script (optional).
	InputState Input;
	if (!A.m_InputPath.empty())
	{
		std::string InErr;
		if (!Input.LoadScript(A.m_InputPath.c_str(), A.m_InputTapFrames, InErr))
		{
			fprintf(stderr, "%s\n", InErr.c_str());
			return 3;
		}
	}
	// --tap auto-boot: inject the keystrokes that trigger the ROM tape load.
	// 48K types LOAD "" at the K cursor; 128K just presses ENTER on the boot
	// menu's default "Tape Loader" item.
	if (!A.m_TapPath.empty() && !bResuming)
	{
		AutoTypeParams TP;
		TP.m_StartFrame = A.m_TapTypeFrame;
		if (!A.m_bTapTypeFrameSet)
		{
			// The default start frame is calibrated on the 48K's 69888-cycle
			// frames at 3.5 MHz, but the ROM takes a fixed WALL time to reach
			// the K cursor. On short-frame machines (ts2068: 58688 T, 60 Hz)
			// frame 96 arrives too early and the keystrokes garble - convert
			// the reference to wall seconds, then to this machine's frames.
			const uint64_t Period = FrameIntPeriodFor(A.m_Machine, A.m_Mhz);
			const double RefSeconds = double(A.m_TapTypeFrame) * (69888.0 / 3'500'000.0);
			TP.m_StartFrame = uint64_t(RefSeconds * (A.m_Mhz * 1'000'000.0) / double(Period) + 0.999);
		}
		// Menu-boot machines (128K / Pentagon / +2A / +3) reach their tape
		// loader by pressing ENTER on the startup menu's default item. The +3's
		// menu default is "Loader"; driving it with a single ENTER is far more
		// robust than typing LOAD"" (a multi-key sequence garbles when frame
		// timing shifts, e.g. under the +2A/+3's heavier contention). The 48K
		// and Timex boot to a K cursor and need the LOAD"" path.
		const bool bMenuBoot = A.m_Machine == MachineModel::kMachineModelSpectrum128K ||
							   A.m_Machine == MachineModel::kMachineModelPentagon128 ||
							   A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
							   A.m_Machine == MachineModel::kMachineModelSpectrumPlus3;
		Input.AddEvents(bMenuBoot ? BuildEnterEvents(TP) : BuildLoadBlankEvents(TP));
	}
	// --trd-run: two-stage RANDOMIZE USR 15619 / RUN "name" auto-type. Stage 1
	// starts after the 48K boot reaches the K prompt (~130 frames) and runs ~9
	// keystrokes; stage 2 (RUN "name") waits for TR-DOS to bring up its A> prompt.
	if (!A.m_TrdRunName.empty() && !bResuming)
	{
		// 48K BASIC boots the same on 48K and on the 128K/Pentagon "48-BASIC-
		// unlocked" state, so one timing works for all: RANDOMIZE USR 15616 once
		// the K prompt is up, then RUN "name" once TR-DOS shows its A> prompt.
		AutoTypeParams TP;
		TP.m_StartFrame = 160;
		Input.AddEvents(BuildTrdosRunEvents(A.m_TrdRunName, TP,
			/*SecondStage=*/160, /*ThirdStage=*/400,
			/*bVia128Menu=*/false));
	}
	// A resumed run gets its input back from the dump even when no input
	// option names it (an auto-typed --tap / --trd-run boot is not a script).
	const JsonValue* DumpInput = bResuming ? DumpRoot.Find("input") : nullptr;
	const bool bDumpHasInput = DumpInput && DumpInput->IsObject();
	InputState* InputPtr =
		(A.m_InputPath.empty() && A.m_TapPath.empty() && A.m_TrdRunName.empty() && !bDumpHasInput) ? nullptr : &Input;
	if (bResuming && InputPtr)
	{
		std::string Err;
		const std::vector<InputEvent> Script = Input.Events();
		if (!LoadInputFromDump(DumpRoot, Input, Err))
		{
			fprintf(stderr, "resume input error: %s\n", Err.c_str());
			return 3;
		}
		// A new script on the command line drives the resumed run from here
		// on; without this the dump's own events silently won and the new
		// keys were never pressed.
		if (A.m_bInputExplicit)
		{
			if (TakeInputScriptOnResume(DumpRoot, Input, Script, Err))
			{
				fprintf(stderr, "resume: %zu input events from %s replace the dump's\n", Input.GetEventCount(),
					A.m_InputPath.c_str());
			}
			else if (!Err.empty())
			{
				fprintf(stderr, "resume input error: %s\n", Err.c_str());
				return 3;
			}
		}
	}

	// AY + optional PSG capture. AY is on by default; --no-ay disables.
	if (!A.m_bAyEnabled && !A.m_PsgOutPath.empty())
	{
		fprintf(stderr, "--no-ay conflicts with --psg-out\n");
		return 4;
	}
	AyState Ay;
	AyState Ay2; // TurboSound second chip (--ts); inert otherwise
	std::unique_ptr<PsgCapture> Capture;
	std::unique_ptr<PsgCapture> Capture2;
	if (!A.m_PsgOutPath.empty())
	{
		Capture = std::make_unique<PsgCapture>(A.m_PsgOutPath, A.m_PsgRate);
		if (A.m_bTurboSound)
		{
			// Second chip's dump: <base>.2.psg next to the primary.
			std::string Path2 = A.m_PsgOutPath;
			const size_t Dot = Path2.find_last_of('.');
			if (Dot == std::string::npos || Path2.find_first_of("/\\", Dot) != std::string::npos)
			{
				Path2 += ".2.psg";
			}
			else
			{
				Path2.insert(Dot, ".2");
			}
			Capture2 = std::make_unique<PsgCapture>(Path2, A.m_PsgRate);
		}
		if (!bResuming)
		{
			// Tick the PSG capture at the machine's FRAME INTERRUPT period, not
			// at wall-clock 1/rate: music engines are timed to the interrupt, and
			// e.g. the 48K frame is 69888 T (50.08 Hz) vs 70000 T for exact 50 Hz.
			// With the old wall-clock period the two rates beat: every ~625 frames
			// two interrupt-driven register bursts landed inside one capture tick
			// and merged - a music frame silently vanished from the .psg.
			const uint64_t PsgPeriod = FrameIntPeriodFor(A.m_Machine, A.m_Mhz);
			EnablePsgTicks(Cpu, PsgPeriod);
		}
		// On resume, PsgTickPeriod + NextPsgTickCycle are already restored -
		// unless the dumped run captured nothing, when the resumed one starts
		// the tick itself.
		else if (Cpu.m_PsgTickPeriod == 0)
		{
			EnablePsgTicks(Cpu, FrameIntPeriodFor(A.m_Machine, A.m_Mhz));
		}
	}
	if (bResuming)
	{
		std::string Err;
		if (!LoadAyFromDump(DumpRoot, Ay, Err))
		{
			fprintf(stderr, "resume ay error: %s\n", Err.c_str());
			return 3;
		}
		if (Capture && !LoadPsgFromDump(DumpRoot, *Capture, Err))
		{
			fprintf(stderr, "resume psg error: %s\n", Err.c_str());
			return 3;
		}
		const JsonValue* DumpAy2 = DumpRoot.Find("ay2");
		if (A.m_bTurboSound && DumpAy2 && DumpAy2->IsObject() &&
			(!LoadAyFromDump(DumpRoot, Ay2, Err, "ay2") ||
				(Capture2 && !LoadPsgFromDump(DumpRoot, *Capture2, Err, "psg2"))))
		{
			fprintf(stderr, "resume ay2 error: %s\n", Err.c_str());
			return 3;
		}
	}

	// NextZXOS VFS (optional). Mounts a host directory as the dot command's
	// / NEX program's filesystem. For --nex with no --vfs given we
	// auto-mount the directory containing the .nex file so the program can
	// F_OPEN sibling files (and so the .nex's own filename resolves under
	// the VFS).
	NextZxosVfs DotVfs;
	std::string EffectiveVfsPath = A.m_VfsPath;
	// A resume mounts what the dumped run had mounted (unless --vfs names
	// another root) and takes its handles, directory and pending read back.
	NextZxosVfs::ResumeState VfsResume;
	bool bVfsResume = false;
	if (bResuming)
	{
		std::string VErr, DumpVfsRoot;
		char DumpVfsDrive = 'C';
		if (!LoadVfsResumeFromDump(DumpRoot, A.m_ResumePath, VfsResume, bVfsResume, DumpVfsRoot, DumpVfsDrive, VErr))
		{
			fprintf(stderr, "resume load error: %s\n", VErr.c_str());
			return 3;
		}
		if (bVfsResume && EffectiveVfsPath.empty())
		{
			EffectiveVfsPath = DumpVfsRoot;
			A.m_VfsDrive = DumpVfsDrive;
		}
	}
	if (EffectiveVfsPath.empty() && !A.m_NexPath.empty())
	{
		const size_t Slash = A.m_NexPath.find_last_of("/\\");
		EffectiveVfsPath = (Slash == std::string::npos) ? std::string(".") : A.m_NexPath.substr(0, Slash);
		if (EffectiveVfsPath.empty())
		{
			EffectiveVfsPath = ".";
		}
	}
	if (!EffectiveVfsPath.empty())
	{
		std::string VErr;
		if (!DotVfs.Mount(EffectiveVfsPath.c_str(), A.m_VfsDrive, VErr))
		{
			fprintf(stderr, "%s\n", VErr.c_str());
			return 3;
		}
		DotVfs.SetReadOnly(A.m_bVfsReadOnly); // --vfs-readonly: refuse guest writes
		if (bVfsResume)
		{
			VfsResume.m_bReadOnly = VfsResume.m_bReadOnly || A.m_bVfsReadOnly;
			if (!DotVfs.ImportResumeState(VfsResume, VErr))
			{
				fprintf(stderr, "resume: %s\n", VErr.c_str());
				return 3;
			}
		}
	}
	// Pre-register the open NEX file as VFS handle 1 + write that byte to
	// [fileHandleCfg] so the demo can F_READ the appended payload (typically
	// a WAV stream for the DAC music). On resume of a NEX run, re-open the file
	// and seek it to the position the dump recorded - without this every
	// resume returns the demo's F_READ to PayloadOffset and the streaming
	// breaks.
	constexpr int kNexFileHandle = 1;
	if (bResuming && !bVfsResume && !A.m_NexPath.empty() && DumpCfg.m_bHasNexHandle)
	{
		FILE* F = fopen(A.m_NexPath.c_str(), "rb");
		if (!F)
		{
			fprintf(stderr, "resume: cannot re-open NEX file: %s\n", A.m_NexPath.c_str());
			return 3;
		}
		if (fseek(F, DumpCfg.m_NexHandlePosition, SEEK_SET) != 0)
		{
			fprintf(stderr, "resume: cannot seek NEX to position %ld\n", DumpCfg.m_NexHandlePosition);
			fclose(F);
			return 3;
		}
		NexLoad.m_OpenFile = F;
	}
	if (NexLoad.m_OpenFile != nullptr)
	{
		if (!DotVfs.RegisterPreOpenedHandle(kNexFileHandle, NexLoad.m_OpenFile, A.m_NexPath, A.m_NexPath))
		{
			fprintf(stderr, "could not register NEX file handle in slot %d\n", kNexFileHandle);
			fclose(NexLoad.m_OpenFile);
			NexLoad.m_OpenFile = nullptr;
			return 3;
		}
		if (!bResuming && NexLoad.m_FileHandleCfg >= 0x4000)
		{
			Mem.Wr(NexLoad.m_FileHandleCfg, uint8_t(kNexFileHandle));
		}
		else if (!bResuming && NexLoad.m_FileHandleCfg != 0)
		{
			Cpu.BC = uint16_t(kNexFileHandle);
		}
		if (!bResuming)
		{
			// RAM, so a resume has it already - writing it again would clobber
			// whatever the program has kept at $5B00 since.
			InstallNextZxosIm1Stub(Mem);
		}
		// NEX programs pass buffer pointers in IX for esxDOS file APIs
		// (vs HL for dot commands).
		DotVfs.SetUseIxForBuffers(true);
	}
	// RST $08 trap is active for any program that NextZXOS would have given
	// esxDOS access to: dot commands always, NEX programs when fileHandleCfg
	// requested streaming. Programs without streaming get the classic
	// "ROM at $0008" behaviour.
	// Every NEX program gets the esxDOS/NextZXOS API (the RST $08 trap), not
	// only the ones streaming from their own file: on hardware the API is
	// always there, and a program that opens its data files itself
	// otherwise runs the ROM's ERROR-1 at $0008, which resets SP from ERR_SP
	// and returns into garbage. NEX programs pass buffers/filenames in IX.
	NextZxosVfs* DotCmdPtr = (!A.m_DotPath.empty() || !A.m_NexPath.empty()) ? &DotVfs : nullptr;
	if (!A.m_NexPath.empty())
	{
		DotVfs.SetUseIxForBuffers(true);
	}

	// Mark MemoryBus as a Next machine when running a dot command / NEX or
	// when the user explicitly selected a Next-family --machine. Drives
	// DetectActiveLayer + the NR_05 seeding below.
	Mem.SetIsNext(!A.m_DotPath.empty() || !A.m_NexPath.empty() || A.m_Machine == MachineModel::kMachineModelNext50 ||
				  A.m_Machine == MachineModel::kMachineModelNext60);

	// Timex SCLD: arm the port $FF/$F4 decode + the SCLD renderer. The .sna/.z80
	// load path has already populated the 48K memory map and (for classic sources)
	// mapped a Spectrum-compatible ROM, so this only flips the mode - it does NOT
	// remap memory here; MapTimex sets up the ROM-boot mapping. 2068
	// variants (ts2068/tc2068) get the EXROM/DOCK/AY feature set.
	// On resume, LoadMemoryFromDump has already re-armed EnableTimex + the SCLD
	// mode/bank from the dump, so skip this (it would zero the restored state).
	if (!bResuming &&
		(A.m_Machine == MachineModel::kMachineModelTs2068 || A.m_Machine == MachineModel::kMachineModelTc2068 ||
			A.m_Machine == MachineModel::kMachineModelTc2048))
	{
		const bool bB2068 = (A.m_Machine != MachineModel::kMachineModelTc2048);
		Mem.EnableTimex(bB2068);
		// EnableTimex zeroes the SCLD state; a .z80 snapshot carries a live SCLD
		// mode/bank (ports $FF/$F4) that must survive, so re-apply it afterwards.
		if (!A.m_Z80Path.empty() && Z80Load.m_bIsTimex)
		{
			Mem.SetScldMode(Z80Load.m_ScldMode);
			Mem.SetScldBank(Z80Load.m_ScldBank);
		}
	}

	// ZX Evolution BaseConf: arm the $EFF7/$FFBF/window/RTC decodes + ATM
	// renderer. Like the Timex arm, this only flips the flag on the snapshot
	// path (the 128K map is already populated); the program itself drives $EFF7
	// to select its video mode. On resume the dump has already re-armed it.
	if (!bResuming && A.m_Machine == MachineModel::kMachineModelZxEvo)
	{
		Mem.EnableZxEvo();
	}

	// ZX-Uno: arm the $FC3B/$FD3B register file + the Timex-style SCLD ($FF)
	// hicolour path + CoreID detection. Snapshot path only flips the flag.
	if (!bResuming && A.m_Machine == MachineModel::kMachineModelZxUno)
	{
		Mem.EnableZxUno();
	}

	// Initialize firmware palette/register defaults only on cold start.
	// A resumed run must keep the palette and transparency already restored.
	if (Mem.IsNext() && !bResuming)
	{
		const uint8_t Pal = (A.m_Machine == MachineModel::kMachineModelNext60) ? 0x04 : 0x00;
		const_cast<uint8_t*>(Mem.GetNextRegsArray())[0x05] = Pal;

		// Default the ULA-1 palette to the 16 standard Spectrum colours
		// (indices 0-7 = non-bright, 8-15 = bright). Real Next firmware sets
		// these up at boot; without them every ULA pixel decodes to (0,0)
		// = the default $00 transparency colour and gets skipped by the
		// compositor's transparency check. Format: each entry is two bytes
		// [Hi, Lo] where Hi = RRR_GGG_BB and Lo bit 0 = 9th (lsb) blue bit.
		// Non-bright "high" channel = 6 (binary 110), bright "high" = 7 (111).
		static constexpr uint8_t DefaultUlaPalette[16][2] = {
			{0x00, 0}, {0x03, 0}, {0xC0, 0}, {0xC3, 0}, //  0..3  black, blue, red, magenta
			{0x18, 0}, {0x1B, 0}, {0xD8, 0}, {0xDB, 0}, //  4..7  green, cyan, yellow, white
			{0x00, 0}, {0x03, 1}, {0xE0, 0}, {0xE3, 1}, //  8..11 bright black, blue, red, magenta
			{0x1C, 0}, {0x1F, 1}, {0xFC, 0}, {0xFF, 1}, // 12..15 bright green, cyan, yellow, white
		};
		uint8_t* UlaPal = Mem.MutablePalette(0);
		for (int Idx = 0; Idx < 16; ++Idx)
		{
			UlaPal[Idx * 2 + 0] = DefaultUlaPalette[Idx][0];
			UlaPal[Idx * 2 + 1] = DefaultUlaPalette[Idx][1];
		}

		// NR_14 (Global Transparency Colour) - default $E3 per the Next
		// spec. With it left at 0 (the default for an all-zeroed regs
		// block), the compositor mistakes every black ULA pixel for
		// transparent and the screen renders as a hole.
		const_cast<uint8_t*>(Mem.GetNextRegsArray())[0x14] = 0xE3;
	}

	// zxnDMA: only on Next machines (port $6B has no meaning elsewhere).
	ZxnDmaState Dma;
	ZxnDmaState* DmaPtr = Mem.IsNext() ? &Dma : nullptr;
	if (bResuming && DmaPtr)
	{
		std::string Err;
		if (!LoadDmaFromDump(DumpRoot, Dma, Err))
		{
			fprintf(stderr, "resume load error: %s\n", Err.c_str());
			return 3;
		}
	}

	// Unified audio render (optional): all AY chips + DAC ports + beeper,
	// one WAV, mixed offline at exit from wall-nanos-stamped events.
	std::unique_ptr<AudioRenderer> Audio;
	if (!A.m_AudioOutPath.empty())
	{
		Audio = std::make_unique<AudioRenderer>(
			A.m_AudioOutPath, A.m_AudioRateHz, AudioRenderer::StereoMode(A.m_AudioStereo));
		std::string AuErr;
		if (bResuming && !LoadAudioFromDump(DumpRoot, *Audio, AuErr))
		{
			fprintf(stderr, "resume load error: %s\n", AuErr.c_str());
			return 3;
		}
	}

	// Sprite engine (Next-only).
	std::unique_ptr<SpritesState> Sprites;
	if (Mem.IsNext())
	{
		Sprites = std::make_unique<SpritesState>();
		Mem.SetSprites(Sprites.get()); // lets WriteNextReg route NR $34-$39/$75-$79
		if (!A.m_ResumePath.empty())
		{
			std::string Err;
			if (!LoadSpritesFromDump(DumpRoot, *Sprites, Err))
			{
				fprintf(stderr, "resume load error: %s\n", Err.c_str());
				return 3;
			}
		}
	}

	// ZX Next Copper (NextRegs $60-$63): present on Next machines unless the
	// user forces it off with --no-copper (used to isolate its cost). The
	// copper VM lives on MemoryBus; here we just mark it present and give it
	// the machine's raster line count for WAIT->cycle mapping.
	if (Mem.IsNext() && !A.m_bNoCopper)
	{
		Mem.Copper().m_bPresent = true;
		Mem.Copper().m_LinesPerFrame = LinesPerFrameFor(A.m_Machine);
		Mem.Copper().m_TopBorderLines = TopBorderLinesFor(A.m_Machine);
	}

	// --pixel-accurate: the copper and the drawer anchor their frame origin to
	// the ACTUAL ULA interrupt phase (z80.cpp keeps it live per accepted
	// interrupt); rendering goes through the eager beam drawer below.
	// --video-out implies it: a recording is only worth what a display showed,
	// and leaving it to a flag only let recordings come out wrong.
	if (A.m_bPixelAccurate || !A.m_VideoOutPattern.empty())
	{
		Mem.SetPixelAccurate(true);
	}

	// Eager beam drawer (pixel-exact rendering: renders the composite in beam
	// order via boundary hooks, incl. memory writes). It is the engine for
	// --video-out (every frame emitted -> beam-accurate video, one frame at a time)
	// and for --pixel-accurate --bmp-out (the last completed beam frame instead
	// of the end-of-frame snapshot). Next only. The run loop drives per-frame
	// finalisation independently of interrupt acceptance (a DI demo still gets a
	// frame per 50 Hz beam frame - see NextBeamFrameCycle). There is no other
	// pixel-accurate path.
	BeamDrawer Beam;
	BeamHookCtx BeamCtx;
	const bool bVideoOut = !A.m_VideoOutPattern.empty();
	const bool bBeam = Mem.IsNext() && (bVideoOut || A.m_bPixelAccurate);
	if (bBeam)
	{
		// Frame-origin phase alignment (raster line 0 == the ULA interrupt) - the
		// drawer and copper need it. z80.cpp keeps it live per interrupt under
		// --pixel-accurate and per beam frame while the hook is set.
		Mem.SetFrameIntPeriodHint(FrameIntPeriod);
		if (FrameIntPeriod != 0 && Cpu.m_NextFrameIntCycle != UINT64_MAX)
		{
			// a resume keeps the phase the dumped run had last sampled
			if (!(bResuming && DumpRoot.Find("bus")))
			{
				Mem.SetFrameIntPhase(Cpu.m_NextFrameIntCycle % FrameIntPeriod);
			}
		}
		Beam.Configure(FrameIntPeriod, LinesPerFrameFor(A.m_Machine), TopBorderLinesFor(A.m_Machine));
		BeamCtx.m_Drawer = &Beam;
		BeamCtx.m_Mem = &Mem;
		BeamCtx.m_Sprites = Sprites.get();
		Mem.SetBeamBoundaryHook(BeamHookTrampoline, &BeamCtx);
		Mem.SetCyclePtr(&Cpu.m_Cycles); // memory-write boundaries (chunky streaming)
		// Arm every beam frame so an early stop still has a completed frame.
		// This includes dot-command returns, PC breakpoints and HALTs.
		Mem.SetBeamArmCycle(0);
	}

	// Video capture (--video-out). Cadence = base-MHz cycles divided by fps.
	// fps default: 60 for next60, 50 otherwise. The user can override via
	// --video-fps. Output is one composite BMP per period; ffmpeg consumes
	// the printf-style pattern at the matching framerate.
	std::unique_ptr<VideoCapture> Video;
	if (!A.m_VideoOutPattern.empty())
	{
		Video = std::make_unique<VideoCapture>();
		Video->m_OutputPaths = &OutputFiles;
		Video->m_Pattern = A.m_VideoOutPattern;
		// Cycle-exact (default): frame-LOCK the sampler to the real frame
		// period, so exactly one BMP is emitted per raster frame, at the
		// interrupt phase - the same instant Fuse latches a movie frame
		// (spectrum_frame() runs at tstates==tstates_per_frame, just before
		// z80_interrupt()). This makes the capture the same length and phase
		// as real hardware / Fuse instead of walking through the raster at a
		// wall-clock fps that is not the machine's true refresh (ts2068: the
		// old 58800-T cadence vs the real 58688-T frame drifted ~1 frame/500).
		// The true refresh, reported for ffmpeg muxing, is clock/period Hz.
		const bool bFrameLock = A.m_bCycleExact && FrameIntPeriod != 0 && Cpu.m_NextFrameIntCycle != UINT64_MAX;
		if (bFrameLock)
		{
			Video->m_PeriodCycles = FrameIntPeriod;
			Video->m_NextEmitCycle = Cpu.m_NextFrameIntCycle; // first real frame boundary
			const double TrueFps = (A.m_Mhz * 1e6) / static_cast<double>(FrameIntPeriod);
			fprintf(stderr, "video: %s @ %.4f fps (frame-locked, cycle-exact)\n", A.m_VideoOutPattern.c_str(), TrueFps);
		}
		else
		{
			const int Fps =
				(A.m_VideoFps > 0) ? A.m_VideoFps : ((A.m_Machine == MachineModel::kMachineModelNext60) ? 60 : 50);
			Video->m_PeriodCycles = static_cast<uint64_t>((A.m_Mhz * 1e6) / static_cast<double>(Fps) + 0.5);
			// First frame fires at cycle = PeriodCycles (= end of the first
			// 1/fps slice). The state at that moment reflects one frame of
			// emulated time, which is what a real display would have latched.
			Video->m_NextEmitCycle = Cpu.m_Cycles + Video->m_PeriodCycles;
			// Print fps + pattern only - NOT the cycle count. PeriodCycles is
			// initialised at the base MHz here but the turbo callback (NR_07)
			// rescales it on the demo's first turbo write, so any cycle figure
			// at startup is stale for any NEX/dot demo that turbos at boot.
			fprintf(stderr, "video: %s @ %d fps%s\n", A.m_VideoOutPattern.c_str(), Fps,
				A.m_bCycleExact ? " (unlocked: no frame interrupt)" : " (cycle-exact off)");
		}
	}

	// --motion-out / --histogram-out. Same cadence as the video capture (and
	// the same frame-lock when it applies), so a metrics row and a video frame
	// describe the same instant.
	std::unique_ptr<MetricsCapture> Metrics;
	if (!A.m_MotionOutPath.empty() || !A.m_HistogramOutPath.empty() || !A.m_PaperOutPattern.empty())
	{
		Metrics = std::make_unique<MetricsCapture>();
		Metrics->m_OutputPaths = &OutputFiles;
		Metrics->m_MotionPath = A.m_MotionOutPath;
		Metrics->m_HistPath = A.m_HistogramOutPath;
		Metrics->m_PaperPattern = A.m_PaperOutPattern;
		Metrics->m_Block = (A.m_MetricsBlock > 0) ? A.m_MetricsBlock : 1;
		const bool bFrameLockM = A.m_bCycleExact && FrameIntPeriod != 0 && Cpu.m_NextFrameIntCycle != UINT64_MAX;
		if (bFrameLockM)
		{
			Metrics->m_PeriodCycles = FrameIntPeriod;
			Metrics->m_NextEmitCycle = Cpu.m_NextFrameIntCycle;
		}
		else
		{
			const int Fps =
				(A.m_VideoFps > 0) ? A.m_VideoFps : ((A.m_Machine == MachineModel::kMachineModelNext60) ? 60 : 50);
			Metrics->m_PeriodCycles = static_cast<uint64_t>((A.m_Mhz * 1e6) / double(Fps) + 0.5);
			Metrics->m_NextEmitCycle = Cpu.m_Cycles + Metrics->m_PeriodCycles;
		}
		std::string MErr;
		if (!OpenMetrics(*Metrics, MErr))
		{
			fprintf(stderr, "%s\n", MErr.c_str());
			return 4;
		}
		fprintf(stderr, "metrics: %s%s%s (%dx%d block, 256x192 paper area)\n",
			A.m_MotionOutPath.empty() ? "" : A.m_MotionOutPath.c_str(),
			(!A.m_MotionOutPath.empty() && !A.m_HistogramOutPath.empty()) ? " + " : "",
			A.m_HistogramOutPath.empty() ? "" : A.m_HistogramOutPath.c_str(), Metrics->m_Block, Metrics->m_Block);
	}

	// A resumed recording carries on: the next frame number, the sample grid
	// (when the period is the same) and the motion history.
	if (bResuming && (Video || Metrics))
	{
		DumpCaptureState DC;
		std::string CErr;
		if (!LoadCaptureFromDump(DumpRoot, DC, CErr))
		{
			fprintf(stderr, "resume load error: %s\n", CErr.c_str());
			return 3;
		}
		if (Video && DC.m_bVideo)
		{
			Video->m_FrameCounter = DC.m_VideoFrames;
			if (DC.m_VideoPeriod == Video->m_PeriodCycles && DC.m_VideoNextEmit > Cpu.m_Cycles)
			{
				Video->m_NextEmitCycle = DC.m_VideoNextEmit;
			}
		}
		if (Metrics && DC.m_bMetrics)
		{
			Metrics->m_FrameCounter = DC.m_MetricsFrames;
			Metrics->m_History = DC.m_MetricsHistory;
			if (DC.m_MetricsPeriod == Metrics->m_PeriodCycles && DC.m_MetricsNextEmit > Cpu.m_Cycles)
			{
				Metrics->m_NextEmitCycle = DC.m_MetricsNextEmit;
			}
		}
	}
	IoContext Io{Mem, &Trd, InputPtr, Ay, Capture.get(), DotCmdPtr, DmaPtr, Sprites.get(), Video.get(),
		nullptr /* Beam, set below */, A.m_bAyEnabled, A.m_bInputLog, A.m_bResetTrap, A.m_bLenientUndocEd,
		A.m_bTraceDrip};
	Io.m_Cpu = &Cpu;
	if (bResuming)
	{
		// TurboSound: the chip the next register write goes to
		const JsonValue* Active = DumpRoot.Find("ay_active");
		uint64_t V = 0;
		if (Active && Active->TryAsUInt(V) && V <= 2)
		{
			Io.m_AyActive = uint8_t(V);
		}
	}
	Io.m_bUntilPc = A.m_bUntilPc; // --until-pc breakpoint
	Io.m_bForceFullDispatch = A.m_bDispatchFull; // --dispatch full
	// ULA /INT pulse: a frame interrupt the CPU cannot take inside it is dropped
	// (hardware). --int-window latch restores the legacy sticky request.
	// A resume keeps the window the dump recorded: it is scaled with turbo, so
	// the machine's base-clock value would be wrong after a speed change.
	const JsonValue* DumpCpu = bResuming ? DumpRoot.Find("cpu") : nullptr;
	if (!(DumpCpu && DumpCpu->Find("int_window") && !A.m_bIntWindowExplicit))
	{
		Cpu.m_IntWindow = (A.m_IntWindowMode < 0) ? IntWindowFor(A.m_Machine) : uint64_t(A.m_IntWindowMode);
	}
	Io.m_UntilPc = A.m_UntilPc;
	Io.m_UntilPcPage = A.m_UntilPcPage;
	if (bBeam)
	{
		Io.m_Beam = &Beam;
	} // --video-out emits the drawer's beam frames
	Io.m_Metrics = Metrics.get(); // --motion-out / --histogram-out
	if (!A.m_TapPath.empty())
	{
		Io.m_Tape = &Tape;
	} // arms the LD-BYTES ($0556) trap
	if (A.m_bTurboSound)
	{
		Io.m_Ay2 = &Ay2; // arms the $F8-$FF chip select in WriteIoPort
		Io.m_Capture2 = Capture2.get();
	}
	Io.m_BudgetPtr = &Budget;
	Io.m_UserSeconds = A.m_Seconds;
	Io.m_BaseMhz = A.m_Mhz;
	Io.m_CurrentMhz = A.m_Mhz;
	Io.m_LastTurboCheckpointCycle = Cpu.m_Cycles;
	Io.m_AccumulatedSeconds = 0.0;
	Io.m_bBudgetInCycles = A.m_bHaveCycles;
	// Integer-math wall-time cache: nanos-per-cycle in Q20 fixed point.
	// Per-cycle: 1e9 ns / (Mhz x 1e6 cycles/sec) = 1000/Mhz ns/cycle. In Q20
	// that's (1000 x 2^20) / Mhz. For 3.5 MHz: 299593145 / 2^20 ~ 285.7. For
	// 28 MHz: ~35.7. The turbo callback updates this whenever NR_07 changes.
	Io.m_NanosPerCycleQ20 = uint64_t((1000.0 * (1ull << 20)) / A.m_Mhz + 0.5);
	Io.m_Audio = Audio.get(); // unified audio render (nullable)
#if INSPECCY_TRACE_SUPPORT
	// .btr v4 video-state recording. Two hooks because the writes arrive by two
	// routes: I/O ports through the CPU's WriteIoPort, and NextRegs through the
	// memory bus - which is also where the COPPER writes them, bypassing the
	// CPU entirely. Binary traces only; a text trace has no record for these.
	if (bWantTrace && bWantTraceBin)
	{
		Io.m_VideoTrace = &Writer;
		Mem.SetVideoTrace(&Writer);
	}
#endif
	// The emulated wall clock carries on from the dumped run's, so audio
	// events keep their place on one timeline.
	Io.m_AccumulatedNanosAtCheckpoint = 0;
	if (bResuming)
	{
		const JsonValue* Wc = DumpRoot.Find("wall_clock");
		const JsonValue* N = (Wc && Wc->IsObject()) ? Wc->Find("checkpoint_nanos") : nullptr;
		const JsonValue* C = (Wc && Wc->IsObject()) ? Wc->Find("checkpoint_cycle") : nullptr;
		uint64_t Nanos = 0, Cycle = 0;
		if (N && C && N->TryAsUInt(Nanos) && C->TryAsUInt(Cycle) && Cycle <= Cpu.m_Cycles)
		{
			Io.m_AccumulatedNanosAtCheckpoint = Nanos;
			Io.m_LastTurboCheckpointCycle = Cycle;
			// --seconds counts this run only: start it below zero by the
			// stretch before the resume that the older checkpoint now spans.
			Io.m_AccumulatedSeconds = -double(Cpu.m_Cycles - Cycle) / (Io.m_CurrentMhz * 1.0e6);
		}
	}

	// Speed-switch writes (Next NR_07, Uno reg $0B, Evo #EFF7/#FF77) rescale
	// Budget and FrameIntPeriod so the user's --seconds remains real
	// wall-clock time across turbo flips. The bus computes the target MHz.
	// Device deadlines (CTC re-arm, copper start, ...): pull the dispatcher's
	// event horizon in when a device arms something nearer than it.
	// Frame-exact --bmp-out (default; --bmp-exact off disables): checkpoint the
	// whole machine every few frames from the stats tick, and at exit replay the
	// tail in a scratch copy with the beam drawer so the screenshot is a frame a
	// real display showed - whatever cycle and reason the run ended on. The
	// pixel-accurate / video paths are already exact and skip this; runs without
	// stats ticks (--no-status) have no frame cadence to hang it on.
	FrameExact FrameEx;
	const bool bFrameExact = A.m_bBmpExact && !A.m_bNoBmp && !bBeam && Cpu.m_StatsTickPeriod != 0;
	if (bFrameExact)
	{
		FrameExact::LiveRefs R;
		R.m_Mem = &Mem;
		R.m_Cpu = &Cpu;
		R.m_Sprites = Sprites.get();
		R.m_Dma = DmaPtr;
		R.m_Ay = &Ay;
		R.m_Ay2 = A.m_bTurboSound ? &Ay2 : nullptr;
		R.m_Tape = A.m_TapPath.empty() ? nullptr : &Tape;
		R.m_Input = InputPtr;
		R.m_Trd = &Trd;
		R.m_Vfs = &DotVfs;
		R.m_Io = &Io;
		R.m_LinesPerFrame = LinesPerFrameFor(A.m_Machine);
		R.m_TopBorderLines = TopBorderLinesFor(A.m_Machine);
		FrameEx.Configure(R);
		Io.m_OnFrameTickUser = &FrameEx;
		Io.m_OnFrameTick = [](void* U)
		{
			static_cast<FrameExact*>(U)->OnFrameTick();
		};
	}

	// Next UART: $133B..$163B talk to a loopback or a child process standing in for
	// the Pi (or ESP) on the far end of the wire. NR $A0 bit 4 gates the Pi link as
	// on hardware; a cold boot with a Pi link enables it the way NextZXOS would.
	NextUart Uart;
	if (!A.m_UartPi.empty() || !A.m_UartEsp.empty())
	{
		std::string UartErr;
		Uart.m_Ch[NextUart::kPi].m_Backend = MakeUartBackend(A.m_UartPi, UartErr);
		if (!A.m_UartPi.empty() && !Uart.m_Ch[NextUart::kPi].m_Backend)
		{
			fprintf(stderr, "--uart-pi: %s\n", UartErr.c_str());
			return 2;
		}
		Uart.m_Ch[NextUart::kEsp].m_Backend = MakeUartBackend(A.m_UartEsp, UartErr);
		if (!A.m_UartEsp.empty() && !Uart.m_Ch[NextUart::kEsp].m_Backend)
		{
			fprintf(stderr, "--uart-esp: %s\n", UartErr.c_str());
			return 2;
		}
		if (!Mem.IsNext())
		{
			fprintf(stderr, "--uart-pi/--uart-esp need a Next machine\n");
			return 2;
		}
		Io.m_Uart = &Uart;
		if (getenv("INSPECCY_UART_DEBUG") != nullptr) // where was the CPU when the Rx FIFO dropped a byte?
		{
			Io.m_Uart->m_OnOverflowUser = &Cpu;
			Io.m_Uart->m_OnOverflow = [](void* User, int Ch)
			{
				const Z80State& C = *static_cast<const Z80State*>(User);
				static int Shown = 0;
				if (Shown++ < 40)
				{
					fprintf(stderr, "uart%d overflow: cycle %llu pc $%04X iff1 %d halted %d\n", Ch, (unsigned long long)C.m_Cycles, C.PC,
						C.IFF1 ? 1 : 0, C.m_bHalted ? 1 : 0);
				}
			};
		}
		if (!A.m_UartPi.empty() && !bResuming)
		{
			const_cast<uint8_t*>(Mem.GetNextRegsArray())[0xA0] |= 0x30; // Pi UART enabled, Pi-facing Rx/Tx
		}
	}
	Mem.m_OnDeadlineUser = &Io;
	Mem.m_OnDeadline = [](void* User, uint64_t Cycle)
	{
		IoContext& IoRef = *static_cast<IoContext*>(User);
		if (IoRef.m_Cpu != nullptr && Cycle < IoRef.m_Cpu->m_NextEventCycle)
		{
			IoRef.m_Cpu->m_NextEventCycle = Cycle;
		}
	};
	// The untraced prefix is a separate RunUntil budget. Keep it in step with
	// turbo changes too, including a cycle-based start clamped by --seconds.
	struct TurboContext
	{
		IoContext& m_Io;
		const Args& m_Args;
		uint64_t* m_Prefix = nullptr;
		void UpdatePrefix()
		{
			if (m_Prefix == nullptr)
			{
				return;
			}
			const uint64_t Now = m_Io.m_Cpu->m_Cycles;
			const uint64_t End = *m_Io.m_BudgetPtr;
			*m_Prefix = std::min(m_Args.m_TraceStartCycle, End);
			if (m_Args.m_bTraceStartInSeconds)
			{
				const double Remaining =
					std::max(0.0, m_Args.m_TraceStartSeconds - double(m_Io.WallNanosNow(Now)) / 1.0e9);
				const double Cycles = Remaining * m_Io.m_CurrentMhz * 1.0e6 + 0.5;
				*m_Prefix = End <= Now || Cycles >= double(End - Now) ? End : Now + uint64_t(Cycles);
			}
		}
	} Turbo{Io, A};
	Mem.m_OnTurboChangeUser = &Turbo;
	Mem.m_OnTurboChange = [](void* User, double NewMhz)
	{
		auto& Context = *static_cast<TurboContext*>(User);
		IoContext& IoRef = Context.m_Io;
		const double OldMhz = IoRef.m_CurrentMhz;
		if (NewMhz == OldMhz)
		{
			return;
		}

		// Bank the wall time consumed at the OLD rate, then switch.
		const uint64_t DCycles = IoRef.m_Cpu->m_Cycles - IoRef.m_LastTurboCheckpointCycle;
		IoRef.m_AccumulatedSeconds += double(DCycles) / (OldMhz * 1.0e6);
		// Integer-math companion: advance AccumulatedNanos by (DCycles x
		// NanosPerCycleQ20) >> 20. The >> 20 happens AFTER the multiply to
		// keep Q20 precision through the addition. Keep both representations
		// in lock-step so the per-instruction TraceSnapshotPre can use the
		// integer one without falling out of sync with the budget-rescale
		// logic above.
		IoRef.m_AccumulatedNanosAtCheckpoint += (DCycles * IoRef.m_NanosPerCycleQ20) >> 20;
		IoRef.m_LastTurboCheckpointCycle = IoRef.m_Cpu->m_Cycles;
		IoRef.m_CurrentMhz = NewMhz;
		IoRef.m_NanosPerCycleQ20 = uint64_t((1000.0 * (1ull << 20)) / NewMhz + 0.5);

		// Rescale Budget so the dispatcher's exit condition stays at the
		// user's requested wall-clock second count. Skipped under --cycles
		// (the user asked for an exact cycle count, not wall time).
		if (!IoRef.m_bBudgetInCycles)
		{
			const double Remaining = (IoRef.m_UserSeconds > IoRef.m_AccumulatedSeconds)
										 ? (IoRef.m_UserSeconds - IoRef.m_AccumulatedSeconds)
										 : 0.0;
			*IoRef.m_BudgetPtr = IoRef.m_Cpu->m_Cycles + uint64_t(Remaining * NewMhz * 1.0e6 + 0.5);
		}
		Context.UpdatePrefix();

		// Rescale FrameIntPeriod so the 50 Hz interrupt cadence stays at
		// 50 Hz in wall-clock terms (which means more cycles between fires
		// when the CPU is faster). Re-arm the next deadline against the
		// new period, anchored at "now" so we don't immediately fire.
		const uint64_t OldFramePeriod = IoRef.m_Cpu->m_FrameIntPeriod;
		if (IoRef.m_Cpu->m_FrameIntPeriod > 0)
		{
			const uint64_t NewPeriod = uint64_t(double(IoRef.m_Cpu->m_FrameIntPeriod) * NewMhz / OldMhz + 0.5);
			IoRef.m_Cpu->m_FrameIntPeriod = NewPeriod;
			IoRef.m_Cpu->m_NextFrameIntCycle = IoRef.m_Cpu->m_Cycles + NewPeriod;
			// The beam drawer's frame boundaries (and the frame phase it sets
			// from them) move onto the new grid with the interrupt: left on
			// the old one they stepped by the new period from a stale anchor,
			// a fraction of a frame out, and the copper - realigned to that
			// phase - skipped its later WAITs every frame.
			if (IoRef.m_NextBeamFrameCycle != 0 && IoRef.m_NextBeamFrameCycle != UINT64_MAX)
			{
				IoRef.m_NextBeamFrameCycle = IoRef.m_Cpu->m_NextFrameIntCycle;
			}
			// The /INT pulse is ULA-timed, so in CPU T-states it scales with the clock too.
			IoRef.m_Cpu->m_IntWindow = uint64_t(double(IoRef.m_Cpu->m_IntWindow) * NewMhz / OldMhz + 0.5);
			// Keep the copper's / beam drawer's frame-origin grid in step with
			// the rescaled frame period.
			IoRef.m_Mem.SetFrameIntPeriodHint(NewPeriod);
			// Re-arm the line interrupt against the new period (line -> cycle
			// depends on cycles-per-line). Anchored at "now" so it doesn't fire
			// immediately.
			if (IoRef.m_Cpu->m_bLineIntEnabled)
			{
				IoRef.m_Cpu->m_NextLineIntCycle = IoRef.m_Mem.CycleOfRasterLine(IoRef.m_Cpu->m_LineIntLine,
					IoRef.m_Cpu->m_Cycles, IoRef.m_Cpu->m_NextFrameIntCycle, IoRef.m_Cpu->m_FrameIntPeriod);
			}
		}

		// Same rescale for the --video-out frame cadence: keep N frames per
		// emulated wall-clock second regardless of turbo.
		if (IoRef.m_Video != nullptr && IoRef.m_Video->m_PeriodCycles > 0)
		{
			const uint64_t DEmit = (IoRef.m_Video->m_NextEmitCycle > IoRef.m_Cpu->m_Cycles)
									   ? (IoRef.m_Video->m_NextEmitCycle - IoRef.m_Cpu->m_Cycles)
									   : 0;
			// a frame-locked capture stays locked: on the new frame grid
			const bool bLocked = OldFramePeriod != 0 && IoRef.m_Video->m_PeriodCycles == OldFramePeriod;
			IoRef.m_Video->m_PeriodCycles = uint64_t(double(IoRef.m_Video->m_PeriodCycles) * NewMhz / OldMhz + 0.5);
			IoRef.m_Video->m_NextEmitCycle = bLocked
												 ? IoRef.m_Cpu->m_NextFrameIntCycle
												 : IoRef.m_Cpu->m_Cycles + uint64_t(double(DEmit) * NewMhz / OldMhz + 0.5);
		}

		// ... and for the metrics sampler, for the same reason. Without it a
		// 28 MHz run samples eight times a frame and every per-frame figure
		// (motion delta especially) is eight times too small, which reads as
		// "the port moves less than the original" when it does not.
		if (IoRef.m_Metrics != nullptr && IoRef.m_Metrics->m_PeriodCycles > 0)
		{
			const uint64_t DEmit = (IoRef.m_Metrics->m_NextEmitCycle > IoRef.m_Cpu->m_Cycles)
									   ? (IoRef.m_Metrics->m_NextEmitCycle - IoRef.m_Cpu->m_Cycles)
									   : 0;
			const bool bLocked = OldFramePeriod != 0 && IoRef.m_Metrics->m_PeriodCycles == OldFramePeriod;
			IoRef.m_Metrics->m_PeriodCycles = uint64_t(double(IoRef.m_Metrics->m_PeriodCycles) * NewMhz / OldMhz + 0.5);
			IoRef.m_Metrics->m_NextEmitCycle =
				bLocked ? IoRef.m_Cpu->m_NextFrameIntCycle
						: IoRef.m_Cpu->m_Cycles + uint64_t(double(DEmit) * NewMhz / OldMhz + 0.5);
		}
		// A slowdown can move a run/trace deadline before the dispatcher's
		// cached next event. Re-evaluate that cache on the next instruction.
		IoRef.m_Cpu->m_NextEventCycle = IoRef.m_Cpu->m_Cycles;
	};
	if (A.m_Machine == MachineModel::kMachineModelZxEvo)
	{
		// Sync CurrentMhz to the #EFF7/#FF77 latches now the callback is live:
		// a no-op at cold boot (both say 7 MHz), but a resumed dump restores
		// the latches and the effective clock must follow them.
		Mem.FireEvoTurbo();
	}

	// NR $22/$23 line interrupt -> Z80State (mirrors OnTurboChange's shape). The
	// bus decodes the registers; here we store the state and arm the deadline.
	Mem.m_OnLineIntChangeUser = &Io;
	Mem.m_OnLineIntChange = [](void* User, bool bEnabled, uint32_t Line, bool bUlaMasked)
	{
		IoContext& IoRef = *static_cast<IoContext*>(User);
		Z80State& C = *IoRef.m_Cpu;
		C.m_bLineIntEnabled = bEnabled;
		C.m_LineIntLine = Line;
		C.m_bUlaIntMasked = bUlaMasked;
		if (bEnabled)
		{
			C.m_NextLineIntCycle =
				IoRef.m_Mem.CycleOfRasterLine(Line, C.m_Cycles, C.m_NextFrameIntCycle, C.m_FrameIntPeriod);
			IoRef.m_Mem.NotifyDeadline(C.m_NextLineIntCycle); // pull the event horizon in
		}
		else
		{
			C.m_NextLineIntCycle = UINT64_MAX;
		}
	};

	// ---- Contention. Now DEFAULT ON for every machine that actually has
	// contended RAM (48K / 128K / Timex): a demo developed against an
	// uncontended emulator silently overbudgets, so modelling it is the useful
	// default. `--contention off` opts out (e.g. for a clean cycle-count bug
	// hunt); the flag is inert on machines without contended RAM (Pentagon,
	// Next). The model is the exact ULA 6,5,4,3,2,1,0,0 pattern (see
	// memory.h ContendPattern), not the old flat average.
	// Live CPU clock for time-dependent NextReg read-backs (NR $1E/$1F) and the
	// classic floating bus.
	Mem.SetCpuClockPtrs(&Cpu.m_Cycles, &Cpu.m_NextFrameIntCycle, &Cpu.m_FrameIntPeriod);
	Mem.RebindContentionClock(&Cpu.m_NextFrameIntCycle, &Cpu.m_FrameIntPeriod);
	// Floating bus: 48K and 128K only (Pentagon has none; Timex/Next decode $FF).
	// The first paper fetch starts one cycle after the model's "first contended"
	// cycle (ContentionModelFor comments): 48K 14335, 128K 14361.
	const bool bFbMachine = A.m_Machine == MachineModel::kMachineModelSpectrum48K ||
							A.m_Machine == MachineModel::kMachineModelSpectrum128K ||
							A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A ||
							A.m_Machine == MachineModel::kMachineModelSpectrumPlus3;
	if (bFbMachine)
	{
		const bool bB128 = A.m_Machine != MachineModel::kMachineModelSpectrum48K; // 128K-class timing
		Mem.ConfigureFloatingBus(true, bB128 ? 14361u : 14335u, bB128 ? 228u : 224u);
	}

	const ContentionKind K = ContentionKindFor(A.m_Machine);
	const bool bWantContention = A.m_bContentionExplicit ? A.m_bContention : (K != ContentionKind::kContentionKindNone);
	if (bWantContention && K != ContentionKind::kContentionKindNone)
	{
		const ContentionModel kCM = ContentionModelFor(A.m_Machine);
		Mem.SetCyclePtr(&Cpu.m_Cycles);
		Mem.ConfigureContention(int(K), kCM.m_Line0, kCM.m_TperLine, &Cpu.m_NextFrameIntCycle, &Cpu.m_FrameIntPeriod);
	}
	// Next: the 28 MHz read wait state follows the CPU speed. NR $07 writes
	// keep it current; --mhz sets the start, and a resumed dump's bus block
	// has already restored the exact speed (7 and 14 MHz included).
	if (MachineHonoursMhzFlag(A.m_Machine) && !(bResuming && DumpRoot.Find("bus")))
	{
		Mem.SetNextCpuSpeed(A.m_Mhz >= 28.0 ? 3 : 0);
	}

	// --watch-phys: print PC + values whenever a write hits the watched byte.
	// We translate <bank16>:<offset> to <page8K>:<offset_in_page> here so the
	// hot path in Mem.Wr can do a single integer compare. The callback prints
	// Cpu.PC, which on most CPUs of interest is 1-3 bytes past the writing
	// op (Cpu.PC has been advanced by the time the write resolves) - the
	// user disassembles back from there.
	if (A.m_WatchBank16 >= 0)
	{
		Mem.m_WatchPage8K = A.m_WatchBank16 * 2 + (A.m_WatchBankOffset / 0x2000);
		Mem.m_WatchOffsetInPage = A.m_WatchBankOffset % 0x2000;
		// Pack Cpu + Mem into a small heap struct so the C-style callback can
		// reach both. (Captures-in-lambda is not allowed for function-pointer
		// callbacks; a struct + void* User is the canonical workaround.)
		struct WatchCtx
		{
			Z80State* m_Cpu;
			MemoryBus* m_Mem;
		};
		static WatchCtx kCtx;
		kCtx.m_Cpu = &Cpu;
		kCtx.m_Mem = &Mem;
		Mem.m_OnPhysicalWatchUser = &kCtx;
		Mem.m_OnPhysicalWatch = [](void* User, int Page8K, int OffsetInPage, uint8_t NewVal, uint8_t OldVal)
		{
			const WatchCtx& Ctx = *static_cast<WatchCtx*>(User);
			const Z80State& C = *Ctx.m_Cpu;
			const MemoryBus& M = *Ctx.m_Mem;
			const int Bank16 = Page8K / 2;
			const int InBank = (Page8K & 1) ? (0x2000 + OffsetInPage) : OffsetInPage;
			// Dump bytes around PC so the writing instruction is identifiable
			// even without the .lst. Cpu.PC is the byte AFTER the writing op,
			// so showing 8 bytes ending at PC covers any reasonable opcode.
			uint8_t Op[8] = {0};
			for (int IdxI = 0; IdxI < 8; ++IdxI)
			{
				Op[IdxI] = M.Rd(uint16_t(C.PC - 8 + IdxI));
			}
			fprintf(stderr,
				"WATCH: PC=$%04X (next instr; writer is just before)\n"
				"       bank%d+$%04X  $%02X -> $%02X  cyc=%llu\n"
				"       regs: HL=$%04X DE=$%04X BC=$%04X AF=$%04X "
				"HL'=$%04X DE'=$%04X BC'=$%04X\n"
				"       MMU NR_50..57 = "
				"%02X %02X %02X %02X %02X %02X %02X %02X "
				"(= banks %d %d %d %d  via half-pages)\n"
				"       bytes [PC-8..PC-1]: "
				"%02X %02X %02X %02X %02X %02X %02X %02X\n",
				C.PC, Bank16, InBank, OldVal, NewVal, static_cast<unsigned long long>(C.m_Cycles), C.HL, C.DE, C.BC,
				C.AF, C.HLp, C.DEp, C.BCp, M.GetNextReg(0x50), M.GetNextReg(0x51), M.GetNextReg(0x52),
				M.GetNextReg(0x53), M.GetNextReg(0x54), M.GetNextReg(0x55), M.GetNextReg(0x56), M.GetNextReg(0x57),
				// The four 16K banks visible at $0000/$4000/$8000/$C000 are
				// determined by the EVEN slot of each pair: NR_50/52/54/56.
				M.GetNextReg(0x50) / 2, M.GetNextReg(0x52) / 2, M.GetNextReg(0x54) / 2, M.GetNextReg(0x56) / 2, Op[0],
				Op[1], Op[2], Op[3], Op[4], Op[5], Op[6], Op[7]);
		};
		fprintf(stderr, "watching bank %d offset $%04X (= page %d offset $%04X)\n", A.m_WatchBank16,
			A.m_WatchBankOffset, Mem.m_WatchPage8K, Mem.m_WatchOffsetInPage);
	}

	// --invariants: load rule files, set up the Wr-hook callback.
	InvariantsState InvState;
	std::vector<MemoryBus::InvariantRange8K> InvRanges;
	if (!A.m_InvariantsPaths.empty())
	{
		for (const std::string& P : A.m_InvariantsPaths)
		{
			std::string Err;
			if (!LoadInvariantsFile(P.c_str(), InvState, Err))
			{
				fprintf(stderr, "%s\n", Err.c_str());
				return 3; // I/O error (bad/unreadable invariants file), not exit 1 = InvalidOpcode
			}
		}
		InvState.m_MaxViolations = A.m_MaxViolations;
		InvState.m_bExitOnViolation = A.m_bExitOnViolation;
		if (!A.m_ViolationsOutPath.empty() && A.m_ViolationsOutPath != "-")
		{
			InvState.m_ViolationsOut = fopen(A.m_ViolationsOutPath.c_str(), "wb");
			InvState.m_bOwnViolationsFh = true;
			if (InvState.m_ViolationsOut == nullptr)
			{
				fprintf(stderr, "cannot open --violations-out %s\n", A.m_ViolationsOutPath.c_str());
				return 3; // I/O error (can't open output), not exit 1 = InvalidOpcode
			}
		}
		else if (A.m_ViolationsOutPath == "-")
		{
			InvState.m_ViolationsOut = stdout;
		}
		// Copy parsed ranges into the Mem-side vector.
		InvRanges.reserve(InvState.m_ReadOnly.size());
		for (const auto& R : InvState.m_ReadOnly)
		{
			InvRanges.push_back({R.m_Page8KStart, R.m_OffsetStart, R.m_Page8KEnd, R.m_OffsetEnd});
		}
		Mem.m_InvariantReadOnly = &InvRanges;
		struct InvCtx
		{
			Z80State* m_Cpu;
			MemoryBus* m_Mem;
			InvariantsState* m_State;
		};
		static InvCtx kInvCtx;
		kInvCtx.m_Cpu = &Cpu;
		kInvCtx.m_Mem = &Mem;
		kInvCtx.m_State = &InvState;
		Mem.m_OnInvariantWriteUser = &kInvCtx;
		Mem.m_OnInvariantWrite =
			[](void* User, int Page8K, uint16_t OffsetInPage, uint8_t NewVal, uint8_t OldVal, int SlotIdx)
		{
			const InvCtx& Ctx = *static_cast<InvCtx*>(User);
			EmitInvariantViolation(*Ctx.m_State, Page8K, OffsetInPage, OldVal, NewVal, SlotIdx, *Ctx.m_Cpu, *Ctx.m_Mem);
		};
		fprintf(stderr, "invariants: %zu readonly rules loaded\n", InvState.m_ReadOnly.size());
	}

	// --coverage-out: caller-owned PHYSICAL byte-map (bit0=read/fetch,
	// bit1=executed) wired into the MemoryBus for the run and dumped raw after.
	// Keyed by MMU 8K page so paging can't alias -> parallels the 2 MiB RAM
	// array: byte index = page8k * 0x2000 + (addr & 0x1FFF). Allocated only when
	// enabled (2 MiB); otherwise the hot path stays a single not-taken branch.
	std::vector<uint8_t> CoverageMap;
	if (!A.m_CoverageOutPath.empty())
	{
		CoverageMap.assign(size_t(kRamPageCount) * kSlotSize, 0); // 2 MiB physical
		Mem.m_Coverage = CoverageMap.data();
		Mem.m_CoverageSize = CoverageMap.size();
		Mem.m_CoverageResetPC = A.m_CoverageResetPC; // one-shot wipe at the post-unpack entry, if set
		const JsonValue* DumpBus = bResuming ? DumpRoot.Find("bus") : nullptr;
		const JsonValue* DumpCov = (DumpBus && DumpBus->IsObject()) ? DumpBus->Find("coverage") : nullptr;
		if (DumpCov && DumpCov->IsObject())
		{
			// carry on marking the dumped run's map; a reset that already fired
			// there stays fired
			std::string B64, CovErr;
			std::vector<uint8_t> Prev;
			const JsonValue* Map = DumpCov->Find("map_b64");
			const JsonValue* ResetPc = DumpCov->Find("reset_pc");
			int64_t Pc = -1;
			if (!Map || !Map->IsString() || !DecodeBase64(Map->m_S, Prev, CovErr) || Prev.size() != CoverageMap.size() ||
				!ResetPc || !ResetPc->TryAsInt(Pc))
			{
				fprintf(stderr, "resume load error: invalid bus.coverage %s\n", CovErr.c_str());
				return 3;
			}
			CoverageMap = std::move(Prev);
			Mem.m_Coverage = CoverageMap.data();
			if (Pc < 0)
			{
				Mem.m_CoverageResetPC = -1;
			}
		}
	}

	// Re-arm the line interrupt from the (restored) NR $22/$23 before the run.
	// The Z80State scheduler fields (NextLineIntCycle et al.) are DERIVED from
	// those NextRegs rather than dumped, so a --resume needs this to pick the
	// line interrupt back up; on a cold boot $22 = 0 makes it a no-op.
	if (Mem.IsNext() && Mem.m_OnLineIntChange != nullptr)
	{
		const uint8_t kR22 = Mem.GetNextReg(0x22), kR23 = Mem.GetNextReg(0x23);
		Mem.m_OnLineIntChange(
			Mem.m_OnLineIntChangeUser, (kR22 & 0x02) != 0, (uint32_t(kR22 & 0x01) << 8) | kR23, (kR22 & 0x04) != 0);
	}

	const auto kT0 = std::chrono::steady_clock::now();
	if (!A.m_IrqDiagnosticsPath.empty())
	{
		Irq.InitialCycle = Cpu.m_Cycles;
		Io.m_IrqDiagnostics = &Irq;
	}
	RunSummary Sum{};
	Sum.m_Result = RunResult::kRunResultBudgetReached;
	Sum.m_Budget = Budget;
	Sum.m_LastPC = Cpu.PC;
	// --realtime: run in slices and sleep whenever emulated time (Io's turbo-aware
	// clock) is ahead of the wall clock, so a far end that lives in real time (the
	// Pi behind --uart-pi) sees the machine at its true speed. The plain run lets
	// the emulator sprint, HALT fast-forward most of all, which is right for a
	// test oracle and wrong for a peer that paces its own output.
	auto RunPaced = [&](TraceWriter* TracePtr) -> RunSummary
	{
		if (!A.m_bRealtime)
		{
			return RunUntil(Cpu, Io, Budget, TracePtr);
		}
		const uint64_t kEmulatedNs0 = Io.WallNanosNow(Cpu.m_Cycles);
		const auto kWall0 = std::chrono::steady_clock::now();
		RunSummary Total{};
		for (;;)
		{
			uint64_t Slice = std::min<uint64_t>(Budget, Cpu.m_Cycles + 140000); // 5 ms at 28 MHz, 40 ms at 3.5 MHz
			const RunSummary S = RunUntil(Cpu, Io, Slice, TracePtr);
			Total = S; // the summary's counters are running totals, not per-call deltas
			if (S.m_Result != RunResult::kRunResultBudgetReached || Cpu.m_Cycles >= Budget)
			{
				break;
			}
			const int64_t EmulatedNs = int64_t(Io.WallNanosNow(Cpu.m_Cycles) - kEmulatedNs0);
			const int64_t WallNs =
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kWall0).count();
			if (EmulatedNs > WallNs)
			{
				std::this_thread::sleep_for(std::chrono::nanoseconds(EmulatedNs - WallNs));
			}
		}
		if (getenv("INSPECCY_PACE_DEBUG") != nullptr)
		{
			fprintf(stderr, "pace: emulated %.3f s, wall %.3f s\n", double(Io.WallNanosNow(Cpu.m_Cycles) - kEmulatedNs0) * 1e-9,
				std::chrono::duration<double>(std::chrono::steady_clock::now() - kWall0).count());
		}
		Total.m_Budget = Budget;
		return Total;
	};
	int BatchExit = 0;
	if (!A.m_BatchPath.empty())
	{
#if INSPECCY_TRACE_SUPPORT
		BatchExit = RunBatch(A.m_BatchPath, A.m_BatchOutPath, Cpu, Io, Budget, Writer.IsActive() ? &Writer : nullptr,
			Sum, SldMap, BoundSymbols, InspectIdentity);
#else
		BatchExit = RunBatch(
			A.m_BatchPath, A.m_BatchOutPath, Cpu, Io, Budget, nullptr, Sum, SldMap, BoundSymbols, InspectIdentity);
#endif
	}
	else if (!A.m_bCreateInitialDump)
	{
#if INSPECCY_TRACE_SUPPORT
		TraceWriter* TracePtr = Writer.IsActive() ? &Writer : nullptr;
		// --trace-from-{cycles,seconds}: run the dispatch with the non-trace
		// template up to the start point, then switch to the trace template.
		// The non-trace path is meaningfully faster (no per-instruction emit
		// hooks), so this is the cheap way to skip a known-boring prefix.
		if (TracePtr != nullptr && A.m_bHaveTraceStart)
		{
			uint64_t Prefix = 0;
			Turbo.m_Prefix = &Prefix;
			Turbo.UpdatePrefix();
			Sum = RunUntil(Cpu, Io, Prefix, nullptr);
			Turbo.m_Prefix = nullptr;
			// Note Budget can be rescaled mid-run by NR_07 turbo writes -
			// re-read it (the turbo callback updated *BudgetPtr in place).
			if (Sum.m_Result == RunResult::kRunResultBudgetReached && Cpu.m_Cycles < Budget)
			{
				Sum = RunUntil(Cpu, Io, Budget, TracePtr);
			}
		}
		else
		{
			Sum = RunPaced(TracePtr);
		}
#else
		Sum = RunPaced(nullptr);
#endif
	}
	bool bOutputFailed = false;
	const auto kT1 = std::chrono::steady_clock::now();
	const double WallMs = std::chrono::duration<double, std::milli>(kT1 - kT0).count();
	Io.m_IrqDiagnostics = nullptr;
	if (Io.m_Uart != nullptr)
	{
		Io.m_Uart->Flush((Mem.GetNextReg(0xA0) & 0x10) != 0);
		for (int U = 0; U < 2; ++U)
		{
			const NextUartChannel& C = Io.m_Uart->m_Ch[U];
			if (C.m_Backend)
			{
				fprintf(stderr, "uart%d: %llu bytes out, %llu in, %llu dropped, %llu rx interrupts%s\n", U, (unsigned long long)C.m_WireBytesOut,
					(unsigned long long)C.m_WireBytesIn, (unsigned long long)C.m_Dropped, (unsigned long long)C.m_RxInts,
					C.m_Backend->IsAlive() ? "" : " (the far-end process is not running)");
			}
		}
	}
	if (!A.m_InspectOutPath.empty())
	{
		const char* Reason = A.m_bCreateInitialDump	  ? "InitialState"
							 : !A.m_BatchPath.empty() ? (BatchExit == 4 ? "BatchInvalid"
															: BatchExit ? "BatchFailed"
																		: "BatchComplete")
													  : ResultName(Sum.m_Result);
		const std::string Report = "{\"version\":1,\"stop\":" + DiagnosticJson::Quote(Reason) +
								   ",\"batch_exit\":" + std::to_string(BatchExit) +
								   ",\"state\":" + InspectionStateJson(Cpu, Io, SldMap, BoundSymbols, InspectIdentity) +
								   '}';
		if (!DiagnosticJson::Write(A.m_InspectOutPath, Report))
		{
			fprintf(stderr, "cannot write inspection: %s\n", A.m_InspectOutPath.c_str());
			bOutputFailed = true;
		}
	}
	if (!A.m_IrqDiagnosticsPath.empty() &&
		!DiagnosticJson::Write(A.m_IrqDiagnosticsOutPath, IrqReportJson(Irq, SldMap, BoundSymbols, InspectIdentity)))
	{
		fprintf(stderr, "cannot write IRQ diagnostics: %s\n", A.m_IrqDiagnosticsOutPath.c_str());
		bOutputFailed = true;
	}

	const int32_t CoverageResetPcAtEnd = Mem.m_CoverageResetPC; // for the dump: -1 once the reset fired
	if (!A.m_CoverageOutPath.empty())
	{
		Mem.m_Coverage = nullptr; // stop marking before teardown
		if (FILE* Cf = fopen(A.m_CoverageOutPath.c_str(), "wb"))
		{
			const size_t Wrote = fwrite(CoverageMap.data(), 1, CoverageMap.size(), Cf);
			if (fclose(Cf) != 0 || Wrote != CoverageMap.size())
			{
				bOutputFailed = true;
				fprintf(stderr, "coverage: failed to write %s\n", A.m_CoverageOutPath.c_str());
			}
			size_t Read = 0, Exec = 0;
			for (uint8_t B : CoverageMap)
			{
				if (B & 1)
				{
					++Read;
				}
				if (B & 2)
				{
					++Exec;
				}
			}
			fprintf(stderr, "coverage: %zu bytes read, %zu executed -> %s\n", Read, Exec, A.m_CoverageOutPath.c_str());
		}
		else
		{
			bOutputFailed = true;
			fprintf(stderr, "coverage: cannot open %s for writing\n", A.m_CoverageOutPath.c_str());
		}
	}

#if INSPECCY_TRACE_SUPPORT
	// Drain and stop the writer before printing the run summary so trace and
	// summary don't interleave on stdout.
	Writer.Stop();
	if (Writer.HasWriteFailed())
	{
		bOutputFailed = true;
		fprintf(stderr, "failed to write trace output or its index\n");
	}
	if (bWantPeephole)
	{
		// After Stop(): the consumer thread has drained, so every executed
		// instruction has reached the analyzer.
		if (!Peephole.WriteReport(A.m_OptimizeOutPath.c_str()))
		{
			bOutputFailed = true;
			fprintf(stderr, "failed to write peephole report: %s\n", A.m_OptimizeOutPath.c_str());
		}
		else
		{
			fprintf(stderr, "peephole: analysed %llu instructions -> %s\n",
				(unsigned long long)Peephole.InstructionsSeen(), A.m_OptimizeOutPath.c_str());
		}
	}
	if (SldMap.SymbolCount() > 0 && bWantTrace)
	{
		const uint64_t Total = SldMap.m_NumMatches + SldMap.m_NumMisses;
		fprintf(stderr, "sld: matched %llu/%llu trace lines (%llu symbols loaded)\n",
			(unsigned long long)SldMap.m_NumMatches, (unsigned long long)Total,
			(unsigned long long)SldMap.SymbolCount());
		if (SldMap.m_NumMatches == 0)
		{
			fprintf(stderr, "sld: zero matches - possible page-numbering "
							"mismatch between SLD and machine; check --machine and "
							"the sjasmplus DEVICE directive used at assembly time\n");
		}
	}
#endif

	// Finalise PSG capture BEFORE saving the SNA, so disk errors on .psg
	// surface immediately and don't get masked by a later .sna failure.
	if (Capture)
	{
		std::string PsgError;
		if (!Capture->Finalise(PsgError))
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", PsgError.c_str());
			// continue to print stats anyway
		}
	}
	if (Capture2)
	{
		std::string PsgError;
		if (!Capture2->Finalise(PsgError))
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", PsgError.c_str());
		}
	}

	// Finalise the unified audio render. Length = emulated wall time of the
	// whole run (turbo-exact via the Q20 clock); the AY chip clock is half
	// the machine's nominal CPU clock on every modelled machine.
	if (Audio)
	{
		const uint64_t TotalNanos = Io.WallNanosNow(Cpu.m_Cycles);
		// AY clock is NOT simply CPU-nominal/2 on every machine. The 128K family
		// derives it that way (3.54690 MHz -> 1.77345 MHz), and Pentagon's AY is
		// genuinely 1.75 MHz, but the ZX Next clocks its AY like the 128K
		// (1.7734 MHz) even though its base CPU nominal is 3.5 MHz -- so the
		// formula would put every AY-generated tone 1.3% flat.
		uint32_t AyClockHz = uint32_t(NominalMhzFor(A.m_Machine) * 500'000.0 + 0.5);
		if (A.m_Machine == MachineModel::kMachineModelNext50 || A.m_Machine == MachineModel::kMachineModelNext60)
		{
			AyClockHz = 1'773'400;
		}
		std::string AuErr;
		if (!Audio->Finalise(TotalNanos, AyClockHz, AuErr))
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", AuErr.c_str());
		}
	}

	// BMP screen capture (default on; --no-bmp suppresses).
	bool bBmpWritten = false;
	if (!A.m_bNoBmp)
	{
		std::string BmpError;
		// Finish the in-progress frame from the exit-cycle live state for a
		// single --bmp-out (the frame the display would show at exit), UNLESS
		// the copper is running - then its per-scanline register writes drive
		// the remaining scanlines, so the last fully-scanned frame is kept.
		// --video-out already emitted each frame at its boundary.
		if (bBeam && A.m_VideoOutPattern.empty() && !Mem.Copper().m_bRunning)
		{
			Beam.Finish(Mem, Sprites.get());
		}
		if (bBeam && !Beam.HasFrame())
		{
			fprintf(stderr, "warning: --pixel-accurate: the beam drawer produced no frame; "
							"writing the end-of-frame snapshot instead\n");
		}
		// Frame-exact default: replay the checkpointed tail and save the last
		// COMPLETED display frame; fall back to the exit-state snapshot (with a
		// stderr note) when that is impossible. See replay.h.
		if (bFrameExact && !A.m_bBmpPerLayer)
		{
			std::string Why;
			bBmpWritten = FrameEx.ReplayAndWriteBmp(A.m_BmpOutPath.c_str(), Why);
			if (!bBmpWritten)
			{
				fprintf(stderr, "note: --bmp-exact: %s; writing the exit-state snapshot instead\n", Why.c_str());
			}
		}
		if (!bBmpWritten)
		{
			bBmpWritten = (bBeam && Beam.HasFrame()) ? Beam.WriteBmp(A.m_BmpOutPath.c_str(), BmpError)
						  : A.m_bBmpPerLayer
							  ? WritePerLayerScreenBmp(Mem, A.m_BmpOutPath.c_str(), BmpError)
							  : WriteVisibleScreenBmp(Mem, Sprites.get(), A.m_BmpOutPath.c_str(), BmpError);
		}
		if (!bBmpWritten)
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", BmpError.c_str());
			// continue
		}
	}

	// Status JSON (default on; --no-status suppresses).
	bool bStatusWritten = false;
	if (!A.m_bNoStatus)
	{
		StatusReport SR;
		SR.m_ResultName = ResultName(Sum.m_Result);
		SR.m_ExitCode = (Sum.m_Result == RunResult::kRunResultBudgetReached)			  ? 0
						: (Sum.m_Result == RunResult::kRunResultInvalidOpcode)			  ? 1
						: (Sum.m_Result == RunResult::kRunResultHaltedInterruptsDisabled) ? 2
						: (Sum.m_Result == RunResult::kRunResultRestartTrap)			  ? 5
						: (Sum.m_Result == RunResult::kRunResultReachedPc)				  ? 8
																						  : 6;
		SR.m_DiToHaltCycles = Sum.m_DiToHaltCycles;
		SR.m_LastPC = Sum.m_LastPC;
		SR.m_LastOpcode = Sum.m_LastOpcode;
		SR.m_CyclesTotal = Sum.m_CyclesExecuted;
		SR.m_Budget = Sum.m_Budget;
		SR.m_Overshoot = int64_t(Sum.m_CyclesExecuted) - int64_t(Sum.m_Budget);
		SR.m_HaltCycles = Sum.m_HaltCycles;
		SR.m_PerFrameHalt = &Cpu.m_PerFrameHalt;
		SR.m_StatsTickPeriod = Cpu.m_StatsTickPeriod;
		SR.m_InterruptsAccepted = Sum.m_InterruptsAccepted;
		if (!A.m_IrqDiagnosticsPath.empty())
		{
			SR.m_IrqDiagnosticsJson = "{\"path\":" + DiagnosticJson::Quote(A.m_IrqDiagnosticsOutPath) +
									  ",\"summary\":" + IrqSummaryJson(Irq) + '}';
		}
		// Unsupported-feature histograms for compatibility sweeps.
		if (DotCmdPtr != nullptr)
		{
			SR.m_Rst08Calls = DotCmdPtr->Rst08Calls();
			SR.m_Rst08Unknown = DotCmdPtr->Rst08Unknown();
			SR.m_Rst08ByCode = DotCmdPtr->Rst08ByCode();
			SR.m_Rst08UnknownByCode = DotCmdPtr->Rst08UnknownByCode();
		}
		for (int IdxI = 0; IdxI < 256; ++IdxI)
		{
			if (Mem.m_NextRegWriteHist[IdxI])
			{
				SR.m_NextRegWrites[uint16_t(IdxI)] = Mem.m_NextRegWriteHist[IdxI];
			}
		}
		for (int IdxI = 0; IdxI < 256; ++IdxI)
		{
			if (Mem.m_NextRegReadHist[IdxI])
			{
				SR.m_NextRegReads[uint16_t(IdxI)] = Mem.m_NextRegReadHist[IdxI];
			}
		}
		SR.m_PortsUnhandledReads = Mem.m_UnhandledPortReads;
		SR.m_PortsUnhandledWrites = Mem.m_UnhandledPortWrites;
		SR.m_Layer2Port = Mem.m_Port123B;
		SR.m_Layer2WindowOpens = Mem.m_Layer2WindowOpens;
		SR.m_MissedInterrupts = Sum.m_MissedInterrupts;
		SR.m_LineIntsAccepted = Sum.m_LineIntsAccepted;
		SR.m_LineIntsMissed = Sum.m_LineIntsMissed;
		SR.m_LineIntLine = Cpu.m_LineIntLine;
		SR.m_bLineIntEnabled = Cpu.m_bLineIntEnabled;
		SR.m_FrameIntPeriod = Cpu.m_FrameIntPeriod;
		SR.m_AyWrites = Sum.m_AyWritesObserved;
		SR.m_RegistersTouched = Ay.m_RegistersTouched;
		SR.m_AyFinalLatch = Ay.m_Latch;
		SR.m_PsgFramesWritten = Sum.m_PsgFramesWritten;
		SR.m_PsgOutPath = A.m_PsgOutPath;
		FillVramAnalysis(Mem, SR);
		SR.m_OutputSna = A.m_OutPath;
		SR.m_OutputBmp = bBmpWritten ? A.m_BmpOutPath : std::string();
#if INSPECCY_TRACE_SUPPORT
		SR.m_OutputTrace = (bWantTrace && !A.m_TraceOutPath.empty()) ? A.m_TraceOutPath : std::string();
#endif
		SR.m_OutputStatus = A.m_StatusOutPath;
		SR.m_WallMs = WallMs;
		SR.m_Mhz = Io.m_CurrentMhz;
		SR.m_NominalMhz = NominalMhzFor(A.m_Machine);
		SR.m_ContentionX = Mem.ContentionOn() ? A.m_ContentionX : 0;

		std::string StatusError;
		bStatusWritten = WriteStatusJson(SR, A.m_StatusOutPath.c_str(), StatusError);
		if (!bStatusWritten)
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", StatusError.c_str());
		}
	}

	// Exact-state dump (default-on; --no-dump suppresses). MUST be written
	// BEFORE SaveSna because SaveSna mutates Cpu.SP / pushes PC on the stack
	// for 48K SNAs - we want the dump to reflect run-end state, not save-end.
	const char* MachineName = (A.m_Machine == MachineModel::kMachineModelSpectrum48K)	   ? "spectrum48k"
							  : (A.m_Machine == MachineModel::kMachineModelSpectrum128K)   ? "spectrum128k"
							  : (A.m_Machine == MachineModel::kMachineModelPentagon128)	   ? "pentagon128"
							  : (A.m_Machine == MachineModel::kMachineModelNext50)		   ? "next50"
							  : (A.m_Machine == MachineModel::kMachineModelNext60)		   ? "next60"
							  : (A.m_Machine == MachineModel::kMachineModelTs2068)		   ? "ts2068"
							  : (A.m_Machine == MachineModel::kMachineModelTc2068)		   ? "tc2068"
							  : (A.m_Machine == MachineModel::kMachineModelTc2048)		   ? "tc2048"
							  : (A.m_Machine == MachineModel::kMachineModelZxEvo)		   ? "zxevo"
							  : (A.m_Machine == MachineModel::kMachineModelZxUno)		   ? "zxuno"
							  : (A.m_Machine == MachineModel::kMachineModelSpectrumPlus2A) ? "spectrumplus2a"
							  : (A.m_Machine == MachineModel::kMachineModelSpectrumPlus3)  ? "spectrumplus3"
																						   : "unknown";

	// .SNA describes a 48K/128K Spectrum and nothing else: 16-bit CPU state,
	// the standard banks and $7FFD. There is no field for the Timex SCLD mode
	// or its DOCK/EXROM chunk map, nor for NextRAM, NextRegs, palettes or the
	// IDE_BANK pool. Writing one for those machines produces a file that loads
	// back as a DIFFERENT COMPUTER without saying so, which is worse than
	// writing nothing: it looks like a valid snapshot and quietly is not.
	//
	// The Timex case is not theoretical. A synthetic 48K image of a 2068
	// program contains none of the RAM the Home ROM sets up at boot - its bank
	// pager lives at $6000-$68xx - so a program that overwrites that region
	// runs perfectly from the snapshot and dies on hardware. The regression
	// test is tests/python/test_ts2068_boot_ram.py.
	//
	// Dumps preserve more machine state; peripheral resume limits are documented.
	const bool bSnaCapableMachine = A.m_Machine == MachineModel::kMachineModelSpectrum48K ||
									A.m_Machine == MachineModel::kMachineModelSpectrum128K ||
									A.m_Machine == MachineModel::kMachineModelPentagon128;

	bool bDumpWritten = false;
	if (!A.m_bNoDump)
	{
		DumpReport DR;
		DR.m_Cpu = &Cpu;
		DR.m_Mem = &Mem;
		DR.m_Ay = &Ay;
		DR.m_Capture = Capture.get();
		DR.m_Ay2 = A.m_bTurboSound ? &Ay2 : nullptr;
		DR.m_Capture2 = Capture2.get();
		DR.m_AyActive = Io.m_AyActive;
		DR.m_Input = InputPtr;
		DR.m_Trd = &Trd;
		DR.m_Tape = A.m_TapPath.empty() ? nullptr : &Tape;
		DR.m_DotVfs = DotCmdPtr;
		DR.m_Dma = DmaPtr;
		DR.m_Audio = Audio.get();
		if (!A.m_CoverageOutPath.empty())
		{
			DR.m_CoverageMap = &CoverageMap;
			DR.m_CoverageResetPc = CoverageResetPcAtEnd;
		}
		if (Video)
		{
			DR.m_Recording.m_bVideo = true;
			DR.m_Recording.m_VideoFrames = Video->m_FrameCounter;
			DR.m_Recording.m_VideoNextEmit = Video->m_NextEmitCycle;
			DR.m_Recording.m_VideoPeriod = Video->m_PeriodCycles;
		}
		if (Metrics)
		{
			DR.m_Recording.m_bMetrics = true;
			DR.m_Recording.m_MetricsFrames = Metrics->m_FrameCounter;
			DR.m_Recording.m_MetricsNextEmit = Metrics->m_NextEmitCycle;
			DR.m_Recording.m_MetricsPeriod = Metrics->m_PeriodCycles;
			DR.m_Recording.m_MetricsHistory = Metrics->m_History;
		}
		DR.m_Sprites = Sprites.get();
		DR.m_MachineName = MachineName;
		// Record the EFFECTIVE clock (= post-NR_07 turbo) so resume can
		// budget cycles correctly. Io.CurrentMhz tracks turbo flips; A.Mhz
		// is just the CLI default.
		DR.m_Mhz = Io.m_CurrentMhz;
		DR.m_NominalMhz = NominalMhzFor(A.m_Machine);
		DR.m_ContentionX = Mem.ContentionOn() ? A.m_ContentionX : 0;
		DR.m_FrameIntPeriod = Cpu.m_FrameIntPeriod;
		DR.m_StatsTickPeriod = Cpu.m_StatsTickPeriod;
		DR.m_WallNanos = Io.WallNanosNow(Cpu.m_Cycles);
		DR.m_WallCheckpointNanos = Io.m_AccumulatedNanosAtCheckpoint;
		DR.m_WallCheckpointCycle = Io.m_LastTurboCheckpointCycle;
		if (!A.m_DotPath.empty())
		{
			DR.m_DotCmd.m_DotCmdPath = A.m_DotPath;
			DR.m_DotCmd.m_ArgString = A.m_DotArg;
			DR.m_DotCmd.m_LoadAddr = kDotcmdLoadAddr;
			DR.m_DotCmd.m_BinarySize = bResuming ? DumpDot.m_BinarySize : DotCmdLoad.m_BinarySize;
			DR.m_DotCmd.m_Code8KPage = bResuming ? DumpDot.m_Code8KPage : DotCmdLoad.m_Code8KPage;
		}
		if (!A.m_NexPath.empty())
		{
			DR.m_Nex.m_NexPath = A.m_NexPath;
			DR.m_Nex.m_NexSha1 = LoadedNexSha;
			DR.m_Nex.m_IdentityOverride =
				A.m_AllowBuildMismatch.empty() ? DumpCfg.m_IdentityOverride : A.m_AllowBuildMismatch;
			DR.m_Nex.m_Version = NexLoad.m_Version;
			DR.m_Nex.m_NumBanks = NexLoad.m_NumBanks;
			DR.m_Nex.m_ScreenFlags = NexLoad.m_ScreenFlags;
			DR.m_Nex.m_Border = NexLoad.m_Border;
			DR.m_Nex.m_EntryBank = NexLoad.m_EntryBank;
			DR.m_Nex.m_InitialSP = NexLoad.m_InitialSP;
			DR.m_Nex.m_InitialPC = NexLoad.m_InitialPC;
			for (int IdxI = 0; IdxI < 3; ++IdxI)
			{
				DR.m_Nex.m_CoreVersion[IdxI] = NexLoad.m_CoreVersion[IdxI];
			}
			DR.m_Nex.m_bHasCopperCode = NexLoad.m_bHasCopperCode;
			DR.m_Nex.m_bHasLoadingScreen = NexLoad.m_bHasLoadingScreen;
			DR.m_Nex.m_bLoadingScreenApplied = NexLoad.m_bLoadingScreenApplied;
			DR.m_Nex.m_BanksOffset = NexLoad.m_BanksOffset;
			DR.m_Nex.m_BanksPresent = NexLoad.m_BanksPresent;
			if (bResuming)
			{
				// A resume does not parse the NEX again: report the header
				// facts the dump it came from recorded.
				const DumpNexInfo& Prev = DumpCfg.m_Nex;
				DR.m_Nex.m_Version = Prev.m_Version;
				DR.m_Nex.m_NumBanks = Prev.m_NumBanks;
				DR.m_Nex.m_ScreenFlags = Prev.m_ScreenFlags;
				DR.m_Nex.m_Border = Prev.m_Border;
				DR.m_Nex.m_EntryBank = Prev.m_EntryBank;
				DR.m_Nex.m_InitialSP = Prev.m_InitialSP;
				DR.m_Nex.m_InitialPC = Prev.m_InitialPC;
				for (int IdxI = 0; IdxI < 3; ++IdxI)
				{
					DR.m_Nex.m_CoreVersion[IdxI] = Prev.m_CoreVersion[IdxI];
				}
				DR.m_Nex.m_bHasCopperCode = Prev.m_bHasCopperCode;
				DR.m_Nex.m_bHasLoadingScreen = Prev.m_bHasLoadingScreen;
				DR.m_Nex.m_bLoadingScreenApplied = Prev.m_bLoadingScreenApplied;
				DR.m_Nex.m_BanksOffset = Prev.m_BanksOffset;
				DR.m_Nex.m_BanksPresent = Prev.m_BanksPresent;
			}
		}
		DR.m_Options.m_bResetTrap = A.m_bResetTrap;
		DR.m_Options.m_bLenientUndocEd = A.m_bLenientUndocEd;
		DR.m_Options.m_bNoCopper = A.m_bNoCopper;
		DR.m_Options.m_bPixelAccurate = A.m_bPixelAccurate;
		DR.m_Options.m_bAyEnabled = A.m_bAyEnabled;
		DR.m_Options.m_bTurboSound = A.m_bTurboSound;
		DR.m_Options.m_bVfsReadOnly = A.m_bVfsReadOnly;
		DR.m_Options.m_IntWindowMode = A.m_IntWindowMode;
		DR.m_Options.m_TapTypeFrame = A.m_TapTypeFrame;
		DR.m_Options.m_bTapTypeFrameSet = A.m_bTapTypeFrameSet;
		DR.m_Options.m_InputTapFrames = A.m_InputTapFrames;
		DR.m_Options.m_bInputLog = A.m_bInputLog;
		DR.m_ExternalRefs.m_Rom48Path = A.m_Rom48Path;
		DR.m_ExternalRefs.m_Rom128Path = A.m_Rom128Path;
		DR.m_ExternalRefs.m_RomTrdPath = A.m_RomTrdPath;
		DR.m_ExternalRefs.m_TrdDiskPath = A.m_TrdPath;
		DR.m_ExternalRefs.m_InputScriptPath = A.m_InputPath;
		DR.m_ExternalRefs.m_RomPlus3Path = A.m_RomPlus3Path;
		DR.m_ExternalRefs.m_DckPath = A.m_DckPath;
		DR.m_ExternalRefs.m_TapPath = A.m_TapPath;

		std::string DumpError;
		bDumpWritten = WriteDumpJson(DR, A.m_DumpOutPath.c_str(), DumpError);
		if (!bDumpWritten)
		{
			bOutputFailed = true;
			fprintf(stderr, "%s\n", DumpError.c_str());
			// continue to SaveSna anyway
		}
	}

	// Skip the .SNA write for dot-command and NEX runs: SNA only captures
	// the first 128 KB and the 16-bit Spectrum state, so it can't round-
	// trip NextRAM / NextRegs / palettes / IDE_BANK pool. Use the dump
	// (--dump-out, on by default) to resume those runs instead.
	bool bSaved = true;
	if (!bSnaCapableMachine && A.m_DotPath.empty() && A.m_NexPath.empty())
	{
		fprintf(stderr,
			"refusing to save .sna: the format cannot describe a %s - it holds "
			"48K/128K state only, so the file would load back as a different "
			"machine. Use --dump-out / --resume instead.\n",
			MachineName);
	}
	else if (A.m_DotPath.empty() && A.m_NexPath.empty())
	{
		std::string SaveError;
		bSaved = SaveSna(A.m_OutPath.c_str(), Load.m_Kind, Cpu, Mem, SaveError);
		if (!bSaved)
		{
			bOutputFailed = true;
			fprintf(stderr, "could not save output SNA: %s\n", SaveError.c_str());
			// continue to print stats anyway
		}
	}

	const long long Over = (long long)Sum.m_CyclesExecuted - (long long)Sum.m_Budget;
	if (Metrics)
	{
		CloseMetrics(*Metrics);
		if (Metrics->m_bWriteFailed)
		{
			bOutputFailed = true;
			fprintf(stderr, "failed to write metrics output\n");
		}
	}
	if (InvState.m_ViolationsOut != nullptr)
	{
		bool bFailed = ferror(InvState.m_ViolationsOut) != 0;
		bFailed = fflush(InvState.m_ViolationsOut) != 0 || bFailed;
		if (InvState.m_bOwnViolationsFh)
		{
			bFailed = fclose(InvState.m_ViolationsOut) != 0 || bFailed;
			InvState.m_ViolationsOut = nullptr;
			InvState.m_bOwnViolationsFh = false;
		}
		if (bFailed)
		{
			bOutputFailed = true;
			fprintf(stderr, "failed to write invariant violations\n");
		}
	}
	if (Video && Video->m_bWriteFailed)
	{
		bOutputFailed = true;
		fprintf(stderr, "failed to write video frame output\n");
	}

	// Append `videoframes=N` only when --video-out was active so the summary
	// schema stays the same for non-video runs (grep/awk consumers unaffected).
	char VideoField[64] = "";
	if (Video)
	{
		snprintf(VideoField, sizeof(VideoField), " videoframes=%llu",
			static_cast<unsigned long long>(Video->m_FrameCounter));
	}
	fprintf(stdout,
		"result=%s cycles=%llu budget=%llu overshoot=%lld lastPC=$%04X lastOp=$%02X ints=%llu missed=%llu "
		"lineints=%llu aywrites=%llu psgframes=%llu%s wall_ms=%.2f out=%s\n",
		ResultName(Sum.m_Result), (unsigned long long)Sum.m_CyclesExecuted, (unsigned long long)Sum.m_Budget, Over,
		static_cast<unsigned int>(Sum.m_LastPC), static_cast<unsigned int>(Sum.m_LastOpcode),
		(unsigned long long)Sum.m_InterruptsAccepted, (unsigned long long)Sum.m_MissedInterrupts,
		(unsigned long long)Sum.m_LineIntsAccepted, (unsigned long long)Sum.m_AyWritesObserved,
		(unsigned long long)Sum.m_PsgFramesWritten, VideoField, WallMs,
		// For dot-cmd and NEX runs the SNA write is skipped (SNA can't
		// round-trip NextRAM + NextRegs), so prefer the dump path - that's
		// what the user actually needs for resume. Fall back to OutPath
		// when --dump is also off.
		(((!A.m_DotPath.empty() || !A.m_NexPath.empty()) && !A.m_DumpOutPath.empty()) ? A.m_DumpOutPath : A.m_OutPath)
			.c_str());

	// Copy-pasteable ffmpeg invocation. Emitted on stderr (= doesn't pollute
	// the stdout summary that automation parses) only when --video-out
	// actually produced frames. Picks the source basename for the output
	// filename so a `--nex mydemo.nex` run suggests `mydemo.mp4`.
	if (Video && Video->m_FrameCounter > 0)
	{
		const int Fps =
			(A.m_VideoFps > 0) ? A.m_VideoFps : ((A.m_Machine == MachineModel::kMachineModelNext60) ? 60 : 50);
		// Derive output basename from whichever source was loaded.
		std::filesystem::path SrcPath;
		if (!A.m_NexPath.empty())
		{
			SrcPath = A.m_NexPath;
		}
		else if (!A.m_DotPath.empty())
		{
			SrcPath = A.m_DotPath;
		}
		else if (!A.m_SnaPath.empty())
		{
			SrcPath = A.m_SnaPath;
		}
		else if (!A.m_ResumePath.empty())
		{
			SrcPath = A.m_ResumePath;
		}
		else
		{
			SrcPath = "output";
		}
		const std::string OutMp4 = SrcPath.stem().string() + ".mp4";

		fprintf(stderr, "\nto encode %llu-frame video, run from this directory:\n",
			static_cast<unsigned long long>(Video->m_FrameCounter));
		if (!A.m_AudioOutPath.empty())
		{
			// The unified render carries the FULL soundtrack (AY + DAC +
			// beeper), so the suggested encode includes it as the audio track.
			fprintf(stderr,
				"  ffmpeg -framerate %d -i \"%s\" -i \"%s\" "
				"-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest -y \"%s\"\n",
				Fps, A.m_VideoOutPattern.c_str(), A.m_AudioOutPath.c_str(), OutMp4.c_str());
		}
		else
		{
			fprintf(stderr,
				"  ffmpeg -framerate %d -i \"%s\" "
				"-c:v libx264 -pix_fmt yuv420p -y \"%s\"\n",
				Fps, A.m_VideoOutPattern.c_str(), OutMp4.c_str());
		}
	}

	if (bOutputFailed)
	{
		return 3;
	}

	// Invariants summary + non-zero exit on violation if requested.
	if (!A.m_InvariantsPaths.empty())
	{
		fprintf(stderr, "invariants: %llu violation%s emitted\n",
			static_cast<unsigned long long>(InvState.m_ViolationsEmitted),
			InvState.m_ViolationsEmitted == 1 ? "" : "s");
		if (InvState.m_bAnyViolation && A.m_bExitOnViolation)
		{
			return 7;
		}
	}

	if (!A.m_BatchPath.empty())
	{
		return BatchExit;
	}
	switch (Sum.m_Result)
	{
	case RunResult::kRunResultBudgetReached:
		return 0;
	case RunResult::kRunResultInvalidOpcode:
		return 1;
	case RunResult::kRunResultHaltedInterruptsDisabled:
		return 2;
	case RunResult::kRunResultRestartTrap:
		return 5;
	case RunResult::kRunResultHaltedDeliberateStop:
		return 6;
	case RunResult::kRunResultReachedPc:
		return 8; // --until-pc target reached
	}
	return 0;
}
