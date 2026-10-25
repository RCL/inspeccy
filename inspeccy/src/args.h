#pragma once
// CLI surface for inspeccy. Args holds every flag value the parser produces;
// Usage() prints the help-and-error banner; ParseArgs() drives argv. The
// real bodies live in args.cpp - keeping them out of main.cpp keeps the
// run-loop logic readable.

#include "trdos.h"
#include "z80.h" // MachineModel

#include <stdint.h>
#include <string>
#include <vector>

struct Args
{
	std::string m_SnaPath;
	std::string m_Z80Path; // --z80 <path>; .z80 snapshot (load-only)
	std::string m_TapPath; // --tap <path>; .tap tape (cold-boot + auto LOAD "")
	uint64_t m_TapTypeFrame = 96; // --tap-type-frame N; frame to start auto-typing LOAD ""
	bool m_bTapTypeFrameSet = false; // explicit N is used verbatim; the default is scaled
									 // to the machine's frame length (the ROM boot takes a
									 // fixed CYCLE count, so 60Hz machines need more frames)
	std::string m_DckPath; // --dck <path>; Timex cartridge into the DOCK bank (2068)
	std::string m_TrdRunName; // --trd-run <name>; cold-boot + TR-DOS run of a named file
	std::string m_Rom48Path;
	std::string m_Rom128Path;
	std::string m_RomTrdPath;
	std::string m_RomPlus3Path; // --rom-plus3: 64 KB (four 16K ROMs) for +2A/+3
	std::string m_RomZxEvoPath; // --rom-zxevo <path>; ZX Evolution BaseConf ROM
	std::string m_RomZxUnoPath; // --rom-zxuno <path>; ZX-Uno core ROM
	std::string m_OutPath;
	double m_Seconds = 0.0;
	double m_Mhz = 3.5;
	bool m_bMhzExplicit = false; // user passed --mhz (vs default / nominal)
	bool m_bHaveSeconds = false;

	// --until-frame N - recorded here and converted to Seconds in main once
	// the machine (and so its nominal clock + frame rate) is final. The parse
	// still writes a provisional Seconds so ordering stays sane.
	double m_UntilFrame = 0.0;
	bool m_bHaveUntilFrame = false;

	// --trace-from-seconds - converted to TraceStartCycle in main once the
	// effective clock is known, then rescaled on turbo changes. This is an
	// absolute emulated time, including elapsed time restored from a dump.
	double m_TraceStartSeconds = 0.0;
	bool m_bTraceStartInSeconds = false;

	// --contention on|off (+ --contention-x N): coarse memory-contention
	// metadata. Effective contention defaults on for supported machines.
	bool m_bContention = false;
	bool m_bContentionExplicit = false;
	uint32_t m_ContentionX = 4;

	// --audio-out <wav>: unified audio render - all AY chips + DAC ports +
	// beeper, one WAV. --audio-rate sets the output sample rate (settable so
	// a render can exactly match a reference's rate); --audio-stereo picks
	// the AY panning (abc default).
	std::string m_AudioOutPath;
	uint32_t m_AudioRateHz = 44100;
	int m_AudioStereo = 0; // AudioRenderer::StereoMode value

	// --cycles N - set the budget directly in CPU cycles. Mutually exclusive
	// with --seconds / --until-frame. When set, the turbo-callback's budget
	// rescaling is skipped: cycles are cycles regardless of clock speed.
	uint64_t m_Cycles = 0;
	bool m_bHaveCycles = false;

	// --trace-from-cycles N or --trace-from-seconds N - defer trace emission
	// until Cpu.Cycles reaches this point. Implemented as two RunUntil calls:
	// a non-trace prefix (full speed) up to the start, then the templated
	// trace path from there to the budget. Lets you ignore the boring
	// start of a long run and only trace the interesting portion.
	uint64_t m_TraceStartCycle = 0;
	bool m_bHaveTraceStart = false;

	MachineModel m_Machine = MachineModel::kMachineModelSpectrum48K;
	bool m_bMachineExplicit = false;
	uint64_t m_FrameIntOverride = 0;
	bool m_bFrameIntExplicit = false;
	bool m_bNoInterrupts = false;

	std::string m_TrdPath; // --trd <path>
	TrdosMode m_TrdosModeFlag = TrdosMode::kTrdosModeAuto; // --trdos auto|on|off|sna
	bool m_bTrdosModeExplicit = false; // named on the command line (so it wins over a resumed dump)
	bool m_bResetTrap = true; // --reset-trap on|off (default on)
	bool m_bResetTrapExplicit = false; // named on the command line (so it wins over a resumed dump)
	bool m_bLenientUndocEd = false; // --lenient-undoc-ed: NOP undocumented ED nn instead of trapping
	bool m_bNoCopper = false; // --no-copper: suppress the Next Copper (isolate its cost)
	bool m_bPixelAccurate = false; // --pixel-accurate: sample per-line NextReg state (copper effects)
	bool m_bCycleExact = true; // --cycle-exact on|off (default on): frame-locked video sampling
							   //   Disable to sample the current state without the frame lock.
	bool m_bCycleExactExplicit = false; // user named --cycle-exact (so it survives resume)
	bool m_bDispatchFull = false; // --dispatch full: force the general dispatcher (equivalence tests)
	int64_t m_IntWindowMode = -1; // --int-window: -1 auto (per machine), 0 latch (legacy), N = T-states
	bool m_bIntWindowExplicit = false; // named on the command line (so it wins over a resumed dump)
	bool m_bUntilPc = false; // --until-pc <addr>: stop when PC (+page) reached
	uint16_t m_UntilPc = 0; //   the 16-bit Z80 PC to break on
	int32_t m_UntilPcPage = -1; //   8K physical page to also match, or -1 = any (16-bit form)

	std::string m_InputPath; // --input <path>
	bool m_bInputExplicit = false; // --input given on this command line (not resolved from a dump)
	int m_InputTapFrames = 2; // --input-tap-frames N
	bool m_bInputLog = true; // --input-log on|off (default on with --input)

	std::string m_PsgOutPath; // --psg-out <path>
	int m_PsgRate = 50; // --psg-rate 50|60 (file header byte)
	std::string m_VideoOutPattern; // --video-out <printf-pattern>; one composite BMP per emulated frame
	std::string m_MotionOutPath; // --motion-out <path.csv>; per-frame motion signature
	std::string m_HistogramOutPath; // --histogram-out <path.csv>; per-frame brightness histogram
	std::string m_PaperOutPattern; // --paper-out <printf-pattern>; block-averaged paper BMP per frame
	int m_MetricsBlock = 1; // --metrics-block N; average NxN before measuring
	std::string m_CoverageOutPath; // --coverage-out <path>; 2MB physical byte-map: bit0=read(incl fetch), bit1=executed
	// Peephole optimisation report. Entirely opt-in: with OptimizeOutPath
	// empty nothing is analysed and the run costs exactly what it did before.
	std::string m_OptimizeOutPath; // --optimize-out <path>
	std::string m_OptimizeProfile = "all"; // --optimize-profile speed|size|all
	double m_OptimizeMinRatio = 1.0; // --optimize-min-ratio (1.0 = always-true only)
	int32_t m_CoverageResetPC =
		-1; // --coverage-reset-at-pc <addr>; zero the map at first fetch there (skip depacker); -1=off
	int m_VideoFps = 0; // --video-fps N; 0 = auto-pick from --machine (50 default, 60 for next60)
	bool m_bAyEnabled = true; // --no-ay disables AY emulation entirely
	bool m_bTurboSound = false; // --ts: TurboSound (2nd AY + NedoPC $F8-$FF chip select);
								//   with --psg-out the 2nd chip lands in <base>.2.psg

	// --watch-phys <bank16>:<offsetInBank> -- prints to stderr every time a
	// CPU/DMA write resolves to that physical byte. <bank16> is the 16K-bank
	// index as displayed in the dump (bank_N); offsetInBank is hex ($xxxx)
	// or decimal. -1 = disabled.
	int m_WatchBank16 = -1;
	int m_WatchBankOffset = -1;

	// --invariants <file> (repeatable). Per .inv file: parse readonly rules,
	// emit JSONL violation events when writes hit a protected byte.
	std::vector<std::string> m_InvariantsPaths;
	std::string m_ViolationsOutPath; // --violations-out; empty = stderr
	bool m_bExitOnViolation = false;
	uint64_t m_MaxViolations = 1000;

	// --ram-mb N -- physical RAM size in MB. 1 = stock 1 MB Next;
	// 2 = 2 MB Next (default - matches the upgrade most demo authors target).
	// Pages outside the effective range route to open bus (reads = $FF,
	// writes ignored) rather than aliasing onto low pages.
	int m_RamMb = 2;
	bool m_bRamMbExplicit = false;

	std::string m_StatusOutPath; // --status-out <path>; empty = auto-derive
	bool m_bNoStatus = false; // --no-status suppresses
	std::string m_BmpOutPath; // --bmp-out <path>; empty = auto-derive
	bool m_bRealtime = false; // --realtime: pace emulated time to the wall clock (never run ahead, HALT included)
	bool m_bNoBmp = false; // --no-bmp suppresses
	bool m_bBmpPerLayer = false; // --bmp-per-layer: use per-layer debug encoder (default = composite)
	bool m_bBmpExact = true; // --bmp-exact on|off: replay the checkpointed tail so --bmp-out is a completed display
							 // frame          // --bmp-per-layer: use per-layer debug encoder (default = composite)
	std::string m_DumpOutPath; // --dump-out <path>; empty = auto-derive
	bool m_bNoDump = false; // --no-dump suppresses
	std::string m_ResumePath; // --resume <path>; replaces --sna
	std::string m_BuildManifestPath;
	std::string m_AllowBuildMismatch; // explicit reason, retained in subsequent dumps
	std::string m_BatchPath; // --batch <JSON>: one loaded machine, ordered steps
	std::string m_BatchOutPath; // --batch-out <JSON>: compact observations/result
	std::string m_InspectOutPath;
	std::string m_IrqDiagnosticsPath;
	std::string m_IrqDiagnosticsOutPath;

	std::string m_DotPath; // --dot <path>; NextZXOS dot command
	std::string m_NexPath; // --nex <path>; Spectrum Next .NEX file
	std::string m_DotArg; // --dot-arg "<string>"
	std::string m_VfsPath; // --vfs <host dir>
	char m_VfsDrive = 'C'; // --vfs-drive (default C)
	bool m_bVfsReadOnly = false; // --vfs-readonly (refuse guest writes)
	std::string m_UartPi; // --uart-pi loop|COMMAND: Next UART 1 (the Pi link) backend
	std::string m_UartEsp; // --uart-esp loop|COMMAND: Next UART 0 (the ESP link) backend
	bool m_bCreateInitialDump = false; // --create-initial-dump (skip RunUntil)

	bool m_bTrace = false; // --trace
	std::string m_TraceOutPath; // --trace-out <path>
	std::string m_TraceOutBinPath; // --trace-out-bin <path>: binary trace format (see trace_binary.h)
	std::string m_TraceDecodePath; // --trace-decode <path>: decode a .btr to text on stdout and exit
	std::string m_TraceIndexOutPath; // --trace-index-out <path>: override .fidx destination (default: <btr>.fidx)
	bool m_bNoTraceIndex = false; // --no-trace-index: skip .fidx emission entirely
	bool m_bTraceVerbose = false; // --trace-format verbose
	bool m_bTraceR = false; // --trace-r
	bool m_bTraceMemPtr = false; // --trace-memptr
	bool m_bTraceDrip = false; // --trace-drip - log every drip-instr that runs during a DMA window
	bool m_bTraceDiffOnly = false; // --trace-diff-only (suppress dest-default emission)
	int m_TraceQueueMb = 16; // --trace-queue-mb
	std::vector<std::string> m_SldPaths; // --sld <path> (repeatable)
};

// Returns the exit code (4 for "bad args"). Also when invoked for --help.
int Usage();

// Parses argv into Out. Returns false on any failure (caller should then
// return Usage()'s exit code). The parser is tolerant about flag ordering
// and validates inter-flag constraints at the end (e.g. --cycles and
// --seconds are mutually exclusive).
bool ParseArgs(int Argc, char** Argv, Args& Out);
