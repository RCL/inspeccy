// inspeccy-emul: a small real-time SDL2 front end over inspeccy_core.  It
// boots a ZX Spectrum Next .nex without NextZXOS, exactly as the headless
// runner does (synthetic ROM, firmware palette defaults, the virtual SD card
// for esxDOS file calls), and then runs one frame at a time: keys and
// buttons into the keyboard matrix and the Kempston port, RunUntil for one
// frame interrupt period, the end-of-frame composite into an SDL texture,
// and the three AY chips' registers into a small synthesiser of our own.
//
// Uses the same core library the CLI links and its fast dispatcher (no
// trace or video capture), with all three Next AYs supplied. --pixel-accurate
// connects the same beam drawer used by the CLI instead of snapshot rendering.
//
//   inspeccy-emul game.nex [--hz 50|60] [--vfs dir] [--scale N] [--crop N]
//                          [--fullscreen] [--rate HZ] [--bench FRAMES]
//
// Desktop keys: the Spectrum's own letters, digits, ENTER, SPACE; Shift =
// CAPS SHIFT, Ctrl = SYMBOL SHIFT, Escape = EDIT (CAPS + 1), Backspace =
// DELETE; cursor keys and right Ctrl / right Alt / right Shift = Kempston 1
// directions and fire 1 / 2 / 3.  F10 quits, F11 toggles full screen.
// A game controller (muOS and the like) and the OpenDingux buttons, which
// arrive as keys, map to the Kempston port; see BindHandheld below.

#include <inspeccy/version.h>
#include <SDL.h>

#include "ay.h"
#include "ay_synth.h"
#include "bmp.h"
#include "dma.h"
#include "dotcmd.h"
#include "input.h"
#include "io.h"
#include "memory.h"
#include "nex.h"
#include "nextzxos.h"
#include "sprites.h"
#include "uart.h"
#include "z80.h"
#include "video.h"
#include "dac_stream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef _WIN32
#include <corecrt_io.h> // not <io.h>: the core has an io.h of its own on the include path
#include <direct.h>
#define access _access
#define W_OK 2
#define mkdir(Path, Mode) _mkdir(Path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// RunBench and RunInteractive are each called once, so a compiler happily
// inlines them back into main and optimises the result as one huge function,
// which costs the emulation about 10% (measured, on the path that does not
// even use the interactive loop).  Keeping them apart is the point.
#if defined(_MSC_VER)
#define INSPECCY_SEPARATE __declspec(noinline)
#else
#define INSPECCY_SEPARATE [[gnu::noinline]]
#endif

namespace
{

constexpr int kCanvasW = 320; // the machine's canvas, and the window's logical size
constexpr int kCanvasH = 256;
constexpr int kTexW = kCanvasW * 2; // the frame is drawn 640 wide so the 640x256 Layer 2 mode keeps every pixel
constexpr int kPitch = kTexW * 3;

struct Options
{
	std::string m_NexPath;
	std::string m_VfsPath;
	int m_Hz = 60;
	int m_Scale = 2;
	int m_Crop = 0; // rows off the top and the bottom
	bool m_bFullscreen = false;
	int m_RateHz = 44100;
	int m_BenchFrames = 0; // > 0: run this many frames flat out, print the rate, exit
	bool m_bNoAudio = false;
	bool m_bBenchNoCompose = false; // --bench: the emulation alone, no picture
	bool m_bNoRenderer = false; // --surface: blit into the window surface instead
	bool m_bPixelAccurate = false; // live display-write boundaries, including Copper splits
	std::string m_UartPi, m_UartEsp; // --uart-pi / --uart-esp: loop or a child process on the far end
	std::string m_InputPath; // --input FILE: the CLI's keyboard script, on top of the live keyboard
};

void Usage()
{
	fputs("inspeccy-emul " INSPECCY_VERSION "\n\n"
		  "usage: inspeccy-emul <program.nex> [--hz 50|60] [--vfs dir] [--scale N] [--crop N]\n"
		  "                     [--fullscreen] [--rate HZ] [--no-audio] [--surface] [--bench FRAMES]\n"
		  "                     [--pixel-accurate] [--uart-pi loop|COMMAND] [--uart-esp loop|COMMAND]\n"
		  "                     [--input SCRIPT]\n"
		  "       inspeccy-emul --version\n",
		stderr);
}

bool ParseArgs(int Argc, char** Argv, Options& Out)
{
	for (int I = 1; I < Argc; ++I)
	{
		const std::string A = Argv[I];
		auto Next = [&](int& Target) {
			if (I + 1 >= Argc)
			{
				return false;
			}
			Target = atoi(Argv[++I]);
			return true;
		};
		if (A == "--hz")
		{
			if (!Next(Out.m_Hz) || (Out.m_Hz != 50 && Out.m_Hz != 60))
			{
				return false;
			}
		}
		else if (A == "--scale")
		{
			if (!Next(Out.m_Scale) || Out.m_Scale < 1)
			{
				return false;
			}
		}
		else if (A == "--crop")
		{
			if (!Next(Out.m_Crop) || Out.m_Crop < 0 || Out.m_Crop > 64)
			{
				return false;
			}
		}
		else if (A == "--rate")
		{
			if (!Next(Out.m_RateHz) || Out.m_RateHz < 8000)
			{
				return false;
			}
		}
		else if (A == "--bench")
		{
			if (!Next(Out.m_BenchFrames) || Out.m_BenchFrames < 1)
			{
				return false;
			}
		}
		else if (A == "--uart-pi" || A == "--uart-esp")
		{
			if (I + 1 >= Argc)
			{
				return false;
			}
			(A == "--uart-pi" ? Out.m_UartPi : Out.m_UartEsp) = Argv[++I];
		}
		else if (A == "--input")
		{
			if (I + 1 >= Argc)
			{
				return false;
			}
			Out.m_InputPath = Argv[++I];
		}
		else if (A == "--vfs")
		{
			if (I + 1 >= Argc)
			{
				return false;
			}
			Out.m_VfsPath = Argv[++I];
		}
		else if (A == "--fullscreen")
		{
			Out.m_bFullscreen = true;
		}
		else if (A == "--no-audio")
		{
			Out.m_bNoAudio = true;
		}
		else if (A == "--no-compose")
		{
			Out.m_bBenchNoCompose = true;
		}
		else if (A == "--surface")
		{
			Out.m_bNoRenderer = true;
		}
		else if (A == "--pixel-accurate")
		{
			Out.m_bPixelAccurate = true;
		}
		else if (!A.empty() && A[0] == '-')
		{
			return false;
		}
		else if (Out.m_NexPath.empty())
		{
			Out.m_NexPath = A;
		}
		else
		{
			return false;
		}
	}
	// A beam renderer draws while the CPU runs; --no-compose would be a
	// misleading CPU-only benchmark in that mode.
	return !Out.m_NexPath.empty() && !(Out.m_bPixelAccurate && Out.m_bBenchNoCompose);
}

std::string DirectoryOf(const std::string& Path)
{
	const size_t Slash = Path.find_last_of("/\\");
	return Slash == std::string::npos ? std::string(".") : Path.substr(0, Slash);
}

std::string StemOf(const std::string& Path)
{
	const size_t Slash = Path.find_last_of("/\\");
	std::string Name = Slash == std::string::npos ? Path : Path.substr(Slash + 1);
	const size_t Dot = Name.find_last_of('.');
	return Dot == std::string::npos ? Name : Name.substr(0, Dot);
}

#ifdef __EMSCRIPTEN__
constexpr const char* kBrowserVfsPath = "/data";

// The emulated program writes its config and its results through the virtual
// SD card; the browser only keeps them if FS.syncfs is called.  Syncing on
// every write would be wasteful, so this watches the byte count and syncs a
// second after it has stopped growing.
void SyncFilesIfWritten(const NextZxosVfs& Vfs)
{
	static uint64_t LastBytes = 0;
	static double ChangedAt = 0.0;
	const uint64_t Now = Vfs.BytesWritten();
	const double T = emscripten_get_now();
	if (Now != LastBytes)
	{
		LastBytes = Now;
		ChangedAt = T;
		return;
	}
	if (ChangedAt != 0.0 && T - ChangedAt > 1000.0)
	{
		ChangedAt = 0.0;
		EM_ASM({
			if (Module['onSaveFiles']) Module['onSaveFiles']();
			else FS.syncfs(false, function(Err) { if (Err) console.warn('inspeccy-emul: syncfs', Err); });
		});
	}
}
#else
void SyncFilesIfWritten(const NextZxosVfs&)
{
}
#endif

// Where the program's files live: its own directory when that can be written
// to, else a folder of our own under the home directory (a .nex inside a
// read-only .opk, say, still keeps its settings and results).
std::string WritableVfsFor(const std::string& NexPath)
{
#ifdef __EMSCRIPTEN__
	(void)NexPath;
	return kBrowserVfsPath; // IndexedDB, mounted by the page
#else
	const std::string Dir = DirectoryOf(NexPath);
	if (access(Dir.c_str(), W_OK) == 0)
	{
		return Dir;
	}
	const char* Home = getenv("HOME");
	if (Home == nullptr || !*Home)
	{
		Home = getenv("USERPROFILE"); // Windows
	}
	std::string Base = (Home != nullptr && *Home) ? std::string(Home) : std::string(".");
	Base += "/.inspeccy-emul";
	mkdir(Base.c_str(), 0755);
	Base += "/" + StemOf(NexPath);
	mkdir(Base.c_str(), 0755);
	return Base;
#endif
}

// ---------------------------------------------------------------- the machine

struct Machine
{
	std::unique_ptr<MemoryBus> m_Mem;
	std::unique_ptr<SpritesState> m_Sprites;
	std::vector<uint8_t> m_Rom;
	Z80State m_Cpu;
	ZxnDmaState m_Dma;
	AyState m_Ay;
	AyState m_Ay2;
	AyState m_Ay3;
	InputState m_Input;
	NextZxosVfs m_Vfs;
	std::unique_ptr<IoContext> m_Io;
	NextUart m_Uart;
	bool m_bUart = false;
	bool m_bUartDeadReported[2] = {false, false};
	std::unique_ptr<BeamDrawer> m_Beam;
	VideoCapture m_Video;
	std::vector<uint8_t> m_Presented;
	uint64_t m_Budget = 0;
	int m_LastResult = 0; // for the bench report
	MachineModel m_Model = MachineModel::kMachineModelNext60;

	bool Boot(const Options& Opt, std::string& Err);
	// one frame interrupt period; false when the CPU stopped for good
	bool RunFrame();
	void DrawFrame(uint8_t* Out, int Pitch) const;
};

bool Machine::Boot(const Options& Opt, std::string& Err)
{
	m_Model = (Opt.m_Hz == 50) ? MachineModel::kMachineModelNext50 : MachineModel::kMachineModelNext60;
	m_Mem = std::make_unique<MemoryBus>();
	MemoryBus& Mem = *m_Mem;
	Mem.SetRamSizeMb(2);

	// no ROM files: 16K of $FF with a self-contained IM 1 handler at $0038,
	// as the runner installs when a .nex is run without a ROM
	m_Rom.assign(0x4000, 0xFF);
	static const uint8_t Im1[] = {0xE5, 0x2A, 0x78, 0x5C, 0x23, 0x22, 0x78, 0x5C, 0xE1, 0xFB, 0xED, 0x4D};
	memcpy(m_Rom.data() + 0x38, Im1, sizeof Im1);
	Mem.SetRom48(m_Rom.data());
	Mem.SetRom128(m_Rom.data());

	NexLoadResult Nex = LoadNex(Opt.m_NexPath.c_str(), m_Cpu, Mem, Err);
	if (!Nex.m_bOk)
	{
		if (Err.empty())
		{
			Err = Nex.m_ErrorMessage;
		}
		return false;
	}

	// what the firmware leaves behind: the timing register, the ULA palette
	// and the transparency colour (main.cpp does the same)
	{
		uint8_t* Regs = const_cast<uint8_t*>(Mem.GetNextRegsArray());
		Regs[0x05] = (m_Model == MachineModel::kMachineModelNext60) ? 0x04 : 0x00;
		if (!Opt.m_UartPi.empty()) Regs[0xA0] |= 0x30; // Pi UART enabled, Pi-facing Rx/Tx, as NextZXOS leaves it
		static constexpr uint8_t Ula[16][2] = {
			{0x00, 0}, {0x03, 0}, {0xC0, 0}, {0xC3, 0}, {0x18, 0}, {0x1B, 0}, {0xD8, 0}, {0xDB, 0},
			{0x00, 0}, {0x03, 1}, {0xE0, 0}, {0xE3, 1}, {0x1C, 0}, {0x1F, 1}, {0xFC, 0}, {0xFF, 1},
		};
		uint8_t* Pal = Mem.MutablePalette(0);
		for (int I = 0; I < 16; ++I)
		{
			Pal[I * 2 + 0] = Ula[I][0];
			Pal[I * 2 + 1] = Ula[I][1];
		}
		Regs[0x14] = 0xE3;
	}

	m_Sprites = std::make_unique<SpritesState>();
	Mem.SetSprites(m_Sprites.get());
	Mem.Copper().m_bPresent = true;
	Mem.Copper().m_LinesPerFrame = LinesPerFrameFor(m_Model);
	Mem.Copper().m_TopBorderLines = TopBorderLinesFor(m_Model);

	// the virtual SD card: the .nex's own directory unless told otherwise;
	// writable, so a game keeps its settings and results
	const std::string VfsPath = Opt.m_VfsPath.empty() ? WritableVfsFor(Opt.m_NexPath) : Opt.m_VfsPath;
	fprintf(stderr, "inspeccy-emul: files in %s\n", VfsPath.c_str());
	if (!m_Vfs.Mount(VfsPath.c_str(), 'C', Err))
	{
		return false;
	}
	m_Vfs.SetReadOnly(false);
	m_Vfs.SetUseIxForBuffers(true);
	if (Nex.m_OpenFile != nullptr)
	{
		constexpr int kNexHandle = 1;
		m_Vfs.RegisterPreOpenedHandle(kNexHandle, Nex.m_OpenFile, Opt.m_NexPath, Opt.m_NexPath);
		if (Nex.m_FileHandleCfg >= 0x4000)
		{
			Mem.Wr(uint16_t(Nex.m_FileHandleCfg), kNexHandle);
		}
		else if (Nex.m_FileHandleCfg != 0)
		{
			m_Cpu.BC = kNexHandle;
		}
		InstallNextZxosIm1Stub(Mem);
	}

	// the port intercept only runs while the input has a script: one event
	// that never comes keeps it on, and the live state goes in by hand
	{
		InputEvent Never;
		Never.m_Frame = UINT64_MAX;
		Never.m_bPress = false;
		Never.m_Name = "SPACE";
		Never.m_Targets.push_back(KeyTarget{false, 7, 0});
		m_Input.AddEvents({Never});
		if (!Opt.m_InputPath.empty())
		{
			std::string ScriptErr;
			if (!m_Input.LoadScript(Opt.m_InputPath.c_str(), 2, ScriptErr))
			{
				Err = "--input: " + ScriptErr;
				return false;
			}
			m_Input.AddEvents({Never}); // LoadScript replaces the list; keep the live-keyboard sentinel
		}
	}

	// the clock: a Next boots at 3.5 MHz and turbos itself; the period and
	// the interrupt window follow NR $07 through the turbo callback
	const double Mhz = NominalMhzFor(m_Model);
	const uint64_t Period = FrameIntPeriodFor(m_Model, Mhz);
	EnableFrameInterrupts(m_Cpu, Period); // after LoadNex, which clears the scheduler
	m_Cpu.m_IntWindow = IntWindowFor(m_Model);

	m_Io = std::make_unique<IoContext>(IoContext{Mem, nullptr, &m_Input, m_Ay, nullptr, &m_Vfs, &m_Dma, m_Sprites.get(),
		nullptr, nullptr, /*bAy*/ true, /*bInputLog*/ false, /*bResetTrap*/ false, /*bLenientUndocEd*/ false,
		/*bTraceDrip*/ false});
	IoContext& Io = *m_Io;
	Io.m_Cpu = &m_Cpu;
	if (!Opt.m_UartPi.empty() || !Opt.m_UartEsp.empty())
	{
		std::string UartErr;
		m_Uart.m_Ch[NextUart::kPi].m_Backend = MakeUartBackend(Opt.m_UartPi, UartErr);
		if (!Opt.m_UartPi.empty() && !m_Uart.m_Ch[NextUart::kPi].m_Backend)
		{
			Err = "--uart-pi: " + UartErr;
			return false;
		}
		m_Uart.m_Ch[NextUart::kEsp].m_Backend = MakeUartBackend(Opt.m_UartEsp, UartErr);
		if (!Opt.m_UartEsp.empty() && !m_Uart.m_Ch[NextUart::kEsp].m_Backend)
		{
			Err = "--uart-esp: " + UartErr;
			return false;
		}
		Io.m_Uart = &m_Uart;
		m_bUart = true;
		if (getenv("INSPECCY_UART_DEBUG") != nullptr) // where was the CPU when the Rx FIFO dropped a byte?
		{
			m_Uart.m_OnOverflowUser = this;
			m_Uart.m_OnOverflow = [](void* User, int Ch)
			{
				Machine& M = *static_cast<Machine*>(User);
				static int Shown = 0;
				if (Shown++ < 40)
				{
					fprintf(stderr, "uart%d overflow: cycle %llu pc $%04X iff1 %d halted %d\n", Ch,
						(unsigned long long)M.m_Cpu.m_Cycles, M.m_Cpu.PC, M.m_Cpu.IFF1 ? 1 : 0, M.m_Cpu.m_bHalted ? 1 : 0);
				}
			};
		}
	}
	Io.m_Ay2 = &m_Ay2; // TurboSound's second chip on $F8..$FF
	Io.m_Ay3 = &m_Ay3; // Next's third chip on $FD
	Io.m_BudgetPtr = &m_Budget;
	Io.m_bBudgetInCycles = true;
	Io.m_BaseMhz = Mhz;
	Io.m_CurrentMhz = Mhz;
	Io.m_LastTurboCheckpointCycle = m_Cpu.m_Cycles;
	Io.m_NanosPerCycleQ20 = uint64_t((1000.0 * (1ull << 20)) / Mhz + 0.5);

	Mem.m_OnDeadlineUser = &Io;
	Mem.m_OnDeadline = [](void* User, uint64_t Cycle) {
		IoContext& R = *static_cast<IoContext*>(User);
		if (R.m_Cpu != nullptr && Cycle < R.m_Cpu->m_NextEventCycle)
		{
			R.m_Cpu->m_NextEventCycle = Cycle;
		}
	};
	Mem.m_OnTurboChangeUser = &Io;
	Mem.m_OnTurboChange = [](void* User, double NewMhz) {
		IoContext& R = *static_cast<IoContext*>(User);
		const double OldMhz = R.m_CurrentMhz;
		if (NewMhz == OldMhz)
		{
			return;
		}
		const uint64_t DCycles = R.m_Cpu->m_Cycles - R.m_LastTurboCheckpointCycle;
		R.m_AccumulatedSeconds += double(DCycles) / (OldMhz * 1.0e6);
		R.m_AccumulatedNanosAtCheckpoint += (DCycles * R.m_NanosPerCycleQ20) >> 20;
		R.m_LastTurboCheckpointCycle = R.m_Cpu->m_Cycles;
		R.m_CurrentMhz = NewMhz;
		R.m_NanosPerCycleQ20 = uint64_t((1000.0 * (1ull << 20)) / NewMhz + 0.5);
		const uint64_t OldFramePeriod = R.m_Cpu->m_FrameIntPeriod;
		if (R.m_Cpu->m_FrameIntPeriod > 0)
		{
			const uint64_t NewPeriod = uint64_t(double(R.m_Cpu->m_FrameIntPeriod) * NewMhz / OldMhz + 0.5);
			R.m_Cpu->m_FrameIntPeriod = NewPeriod;
			R.m_Cpu->m_NextFrameIntCycle = R.m_Cpu->m_Cycles + NewPeriod;
			// the beam drawer's frame boundaries follow the interrupt onto the
			// new grid (see the CLI's turbo callback)
			if (R.m_NextBeamFrameCycle != 0 && R.m_NextBeamFrameCycle != UINT64_MAX)
			{
				R.m_NextBeamFrameCycle = R.m_Cpu->m_NextFrameIntCycle;
			}
			R.m_Cpu->m_IntWindow = uint64_t(double(R.m_Cpu->m_IntWindow) * NewMhz / OldMhz + 0.5);
			R.m_Mem.SetFrameIntPeriodHint(NewPeriod);
			if (R.m_Cpu->m_bLineIntEnabled)
			{
				R.m_Cpu->m_NextLineIntCycle = R.m_Mem.CycleOfRasterLine(
					R.m_Cpu->m_LineIntLine, R.m_Cpu->m_Cycles, R.m_Cpu->m_NextFrameIntCycle, R.m_Cpu->m_FrameIntPeriod);
			}
		}
		if (R.m_Video)
		{
			const uint64_t Left =
				R.m_Video->m_NextEmitCycle > R.m_Cpu->m_Cycles ? R.m_Video->m_NextEmitCycle - R.m_Cpu->m_Cycles : 0;
			const bool bLocked = OldFramePeriod != 0 && R.m_Video->m_PeriodCycles == OldFramePeriod;
			R.m_Video->m_PeriodCycles = uint64_t(double(R.m_Video->m_PeriodCycles) * NewMhz / OldMhz + 0.5);
			R.m_Video->m_NextEmitCycle = bLocked ? R.m_Cpu->m_NextFrameIntCycle
												 : R.m_Cpu->m_Cycles + uint64_t(double(Left) * NewMhz / OldMhz + 0.5);
		}
		// the frame we are in is being re-timed: what is left of it, at the new rate
		if (R.m_BudgetPtr != nullptr && *R.m_BudgetPtr > R.m_Cpu->m_Cycles)
		{
			const uint64_t Left = *R.m_BudgetPtr - R.m_Cpu->m_Cycles;
			*R.m_BudgetPtr = R.m_Cpu->m_Cycles + uint64_t(double(Left) * NewMhz / OldMhz + 0.5);
		}
	};
	Mem.m_OnLineIntChangeUser = &Io;
	Mem.m_OnLineIntChange = [](void* User, bool bEnabled, uint32_t Line, bool bUlaMasked) {
		IoContext& R = *static_cast<IoContext*>(User);
		Z80State& C = *R.m_Cpu;
		C.m_bLineIntEnabled = bEnabled;
		C.m_LineIntLine = Line;
		C.m_bUlaIntMasked = bUlaMasked;
		if (bEnabled)
		{
			C.m_NextLineIntCycle = R.m_Mem.CycleOfRasterLine(Line, C.m_Cycles, C.m_NextFrameIntCycle, C.m_FrameIntPeriod);
			R.m_Mem.NotifyDeadline(C.m_NextLineIntCycle);
		}
		else
		{
			C.m_NextLineIntCycle = UINT64_MAX;
		}
	};

	Mem.SetCpuClockPtrs(&m_Cpu.m_Cycles, &m_Cpu.m_NextFrameIntCycle, &m_Cpu.m_FrameIntPeriod);
	Mem.RebindContentionClock(&m_Cpu.m_NextFrameIntCycle, &m_Cpu.m_FrameIntPeriod);
	{
		const ContentionModel CM = ContentionModelFor(m_Model);
		Mem.SetCyclePtr(&m_Cpu.m_Cycles);
		Mem.ConfigureContention(int(ContentionKindFor(m_Model)), CM.m_Line0, CM.m_TperLine, &m_Cpu.m_NextFrameIntCycle,
			&m_Cpu.m_FrameIntPeriod);
	}
	Mem.SetNextCpuSpeed(0);
	Mem.SetFrameIntPeriodHint(Period);
	if (Opt.m_bPixelAccurate)
	{
		Mem.SetPixelAccurate(true);
		Mem.SetFrameIntPhase(m_Cpu.m_NextFrameIntCycle % Period);
		m_Beam = std::make_unique<BeamDrawer>();
		m_Beam->Configure(Period, LinesPerFrameFor(m_Model), TopBorderLinesFor(m_Model));
		Mem.SetBeamBoundaryHook([](void* User, uint64_t Cycle) {
			Machine& M = *static_cast<Machine*>(User);
			M.m_Beam->FlushTo(*M.m_Mem, M.m_Sprites.get(), Cycle);
		}, this);
		Mem.SetBeamArmCycle(0);
		Io.m_Beam = m_Beam.get();
		// use the CLI's capture point. Reading the beam after an arbitrary
		// RunUntil budget can select a different completed frame, particularly
		// across turbo changes and during a long DMA instruction.
		m_Presented.assign(kPitch * kCanvasH, 0);
		m_Video.m_PeriodCycles = Period;
		m_Video.m_NextEmitCycle = m_Cpu.m_NextFrameIntCycle;
		m_Video.m_OnFrameUser = this;
		m_Video.m_OnFrame = [](void* InOutUser, const MemoryBus& Bus, const SpritesState* Spr, const BeamDrawer* Beam)
		{
			auto& M = *static_cast<Machine*>(InOutUser);
			if (Beam && Beam->HasFrame())
			{
				// the beam renderer draws the 320 canvas: double it into the 640 frame
				static std::vector<uint8_t> Narrow(size_t(kCanvasW) * 3 * kCanvasH);
				Beam->CopyCaptureBgr(Narrow.data(), kCanvasW * 3);
				for (int Y = 0; Y < kCanvasH; ++Y)
				{
					const uint8_t* In = Narrow.data() + size_t(Y) * kCanvasW * 3;
					uint8_t* Out = M.m_Presented.data() + size_t(Y) * kPitch;
					for (int X = 0; X < kCanvasW; ++X, In += 3, Out += 6)
					{
						Out[0] = Out[3] = In[0]; Out[1] = Out[4] = In[1]; Out[2] = Out[5] = In[2];
					}
				}
			}
			else
			{
				RenderCompositeBgrHires(Bus, Spr, M.m_Presented.data(), kPitch);
			}
		};
		Io.m_Video = &m_Video;
	}
	fprintf(stderr, "inspeccy-emul: renderer %s\n", m_Beam ? "pixel-accurate beam" : "end-of-frame snapshot");
	return true;
}

bool Machine::RunFrame()
{
	// carry the scheduled boundary forward. Starting at the CPU's reached
	// cycle would accumulate instruction overshoot and eventually drop a
	// displayed frame. Turbo changes rescale the remaining budget in Boot.
	// run beyond the boundary: HALT/DMA can stop exactly on a budget
	// without re-entering the dispatch loop that emits the video frame.
	if (m_Beam)
	{
		m_Budget = m_Video.m_NextEmitCycle + 1;
	}
	else
	{
		m_Budget += m_Cpu.m_FrameIntPeriod;
	}
	const uint64_t PreviousFrame = m_Video.m_FrameCounter;
	RunSummary S = RunUntil(m_Cpu, *m_Io, m_Budget, nullptr);
	// some fast-forward paths return directly at the budget. Re-enter once
	// to service the capture deadline before exposing this frame to SDL.
	if (m_Beam && m_Video.m_FrameCounter == PreviousFrame && S.m_Result == RunResult::kRunResultBudgetReached)
	{
		S = RunUntil(m_Cpu, *m_Io, m_Budget, nullptr);
	}
	m_LastResult = int(S.m_Result);
	if (m_bUart)
	{
		m_Uart.Advance(m_Io->WallNanosNow(m_Cpu.m_Cycles), (m_Mem->GetNextReg(0xA0) & 0x10) != 0);
		for (int U = 0; U < 2; ++U) // a dead far end is the first thing to know about
		{
			const UartBackend* B = m_Uart.m_Ch[U].m_Backend.get();
			if (B && !B->IsAlive() && !m_bUartDeadReported[U])
			{
				m_bUartDeadReported[U] = true;
				fprintf(stderr, "inspeccy-emul: the --uart-%s process is not running (exited or failed to start)\n", U ? "pi" : "esp");
			}
		}
	}
	switch (S.m_Result)
	{
	case RunResult::kRunResultBudgetReached:
	case RunResult::kRunResultHaltedInterruptsDisabled: // HALT with nothing to wake it: still a frame
		return true;
	default:
		return false;
	}
}

void Machine::DrawFrame(uint8_t* Out, int Pitch) const
{
	if (m_Beam)
	{
		for (int IdxRow = 0; IdxRow < kCanvasH; ++IdxRow)
		{
			memcpy(Out + IdxRow * Pitch, m_Presented.data() + IdxRow * kPitch, kPitch);
		}
	}
	else
	{
		RenderCompositeBgrHires(*m_Mem, m_Sprites.get(), Out, Pitch);
	}
}

// ------------------------------------------------------------------- the AY
// A plain AY-3-8910 rendered from the register file once a frame: three tone
// generators, the noise, the envelope and the logarithmic volume steps; no
// intra-frame register timing, which is enough for effects and most tunes.

// ------------------------------------------------------------------ the keys
// Our own picture of the matrix and the joystick, pushed into the core once a
// frame.  Rows are active low; the Kempston byte is active high, and its
// bits 5 and 6 (fire 3 and 2) pass through the port read as they are.

struct Keys
{
	uint8_t m_Rows[8];
	uint8_t m_Kempston = 0;
	Keys()
	{
		memset(m_Rows, 0xFF, sizeof m_Rows);
	}
	void Matrix(int Row, int Bit, bool bDown)
	{
		if (bDown)
		{
			m_Rows[Row] &= uint8_t(~(1u << Bit));
		}
		else
		{
			m_Rows[Row] |= uint8_t(1u << Bit);
		}
	}
	void Joy(int Bit, bool bDown)
	{
		if (bDown)
		{
			m_Kempston |= uint8_t(1u << Bit);
		}
		else
		{
			m_Kempston &= uint8_t(~(1u << Bit));
		}
	}
};

enum KempstonBit
{
	kJoyRight = 0,
	kJoyLeft = 1,
	kJoyDown = 2,
	kJoyUp = 3,
	kJoyFire1 = 4,
	kJoyFire3 = 5,
	kJoyFire2 = 6,
};

// the matrix: row, bit
struct MatrixKey
{
	SDL_Scancode m_Code;
	uint8_t m_Row, m_Bit;
};
const MatrixKey kMatrix[] = {
	{SDL_SCANCODE_LSHIFT, 0, 0}, {SDL_SCANCODE_Z, 0, 1}, {SDL_SCANCODE_X, 0, 2}, {SDL_SCANCODE_C, 0, 3}, {SDL_SCANCODE_V, 0, 4},
	{SDL_SCANCODE_A, 1, 0}, {SDL_SCANCODE_S, 1, 1}, {SDL_SCANCODE_D, 1, 2}, {SDL_SCANCODE_F, 1, 3}, {SDL_SCANCODE_G, 1, 4},
	{SDL_SCANCODE_Q, 2, 0}, {SDL_SCANCODE_W, 2, 1}, {SDL_SCANCODE_E, 2, 2}, {SDL_SCANCODE_R, 2, 3}, {SDL_SCANCODE_T, 2, 4},
	{SDL_SCANCODE_1, 3, 0}, {SDL_SCANCODE_2, 3, 1}, {SDL_SCANCODE_3, 3, 2}, {SDL_SCANCODE_4, 3, 3}, {SDL_SCANCODE_5, 3, 4},
	{SDL_SCANCODE_0, 4, 0}, {SDL_SCANCODE_9, 4, 1}, {SDL_SCANCODE_8, 4, 2}, {SDL_SCANCODE_7, 4, 3}, {SDL_SCANCODE_6, 4, 4},
	{SDL_SCANCODE_P, 5, 0}, {SDL_SCANCODE_O, 5, 1}, {SDL_SCANCODE_I, 5, 2}, {SDL_SCANCODE_U, 5, 3}, {SDL_SCANCODE_Y, 5, 4},
	{SDL_SCANCODE_RETURN, 6, 0}, {SDL_SCANCODE_L, 6, 1}, {SDL_SCANCODE_K, 6, 2}, {SDL_SCANCODE_J, 6, 3}, {SDL_SCANCODE_H, 6, 4},
	{SDL_SCANCODE_SPACE, 7, 0}, {SDL_SCANCODE_LCTRL, 7, 1}, {SDL_SCANCODE_M, 7, 2}, {SDL_SCANCODE_N, 7, 3}, {SDL_SCANCODE_B, 7, 4},
};

bool g_bQuitRequested = false; // read by the browser's frame callback

bool QuitRequested()
{
	return g_bQuitRequested;
}

struct Frontend
{
	Keys m_Keys;
	bool m_bQuit = false;
	bool m_bToggleFullscreen = false;
	bool m_SelectHeld = false, m_StartHeld = false;

	void Key(SDL_Scancode Code, bool bDown)
	{
		for (const MatrixKey& K : kMatrix)
		{
			if (K.m_Code == Code)
			{
				m_Keys.Matrix(K.m_Row, K.m_Bit, bDown);
				return;
			}
		}
		switch (Code)
		{
		case SDL_SCANCODE_ESCAPE: // EDIT: CAPS + 1
			m_Keys.Matrix(0, 0, bDown);
			m_Keys.Matrix(3, 0, bDown);
			break;
		case SDL_SCANCODE_BACKSPACE: // DELETE: CAPS + 0
			m_Keys.Matrix(0, 0, bDown);
			m_Keys.Matrix(4, 0, bDown);
			break;
		case SDL_SCANCODE_RSHIFT:
			m_Keys.Joy(kJoyFire3, bDown);
			break;
		case SDL_SCANCODE_RCTRL:
			m_Keys.Joy(kJoyFire1, bDown);
			break;
		case SDL_SCANCODE_RALT:
			m_Keys.Joy(kJoyFire2, bDown);
			break;
		case SDL_SCANCODE_UP:
			m_Keys.Joy(kJoyUp, bDown);
			break;
		case SDL_SCANCODE_DOWN:
			m_Keys.Joy(kJoyDown, bDown);
			break;
		case SDL_SCANCODE_LEFT:
			m_Keys.Joy(kJoyLeft, bDown);
			break;
		case SDL_SCANCODE_RIGHT:
			m_Keys.Joy(kJoyRight, bDown);
			break;
		case SDL_SCANCODE_F10:
			if (bDown)
			{
				m_bQuit = g_bQuitRequested = true;
			}
			break;
		case SDL_SCANCODE_F11:
			if (bDown)
			{
				m_bToggleFullscreen = true;
			}
			break;
		default:
			break;
		}
	}

#if INSPECCY_OPENDINGUX
	// OpenDingux delivers the buttons as keys: D-pad = cursor keys, A = left
	// Ctrl, B = left Alt, X = space, Y = left Shift, L = Tab, R = Backspace,
	// START = Return, SELECT = Escape, POWER = Home.  Kempston for play, with
	// the menus' fire on A as well; SELECT is EDIT (pause / quit dialog),
	// START is ENTER, SELECT + START together leave the emulator.
	bool BindHandheld(SDL_Scancode Code, bool bDown)
	{
		switch (Code)
		{
		case SDL_SCANCODE_UP: m_Keys.Joy(kJoyUp, bDown); return true;
		case SDL_SCANCODE_DOWN: m_Keys.Joy(kJoyDown, bDown); return true;
		case SDL_SCANCODE_LEFT: m_Keys.Joy(kJoyLeft, bDown); return true;
		case SDL_SCANCODE_RIGHT: m_Keys.Joy(kJoyRight, bDown); return true;
		case SDL_SCANCODE_LCTRL: m_Keys.Joy(kJoyFire1, bDown); return true; // A
		case SDL_SCANCODE_LALT: m_Keys.Joy(kJoyFire2, bDown); return true; // B
		case SDL_SCANCODE_LSHIFT: m_Keys.Joy(kJoyFire3, bDown); return true; // Y
		case SDL_SCANCODE_SPACE: m_Keys.Matrix(7, 0, bDown); return true; // X
		case SDL_SCANCODE_RETURN: // START
			m_StartHeld = bDown;
			m_Keys.Matrix(6, 0, bDown);
			if (bDown && m_SelectHeld) m_bQuit = g_bQuitRequested = true;
			return true;
		case SDL_SCANCODE_ESCAPE: // SELECT: EDIT
			m_SelectHeld = bDown;
			m_Keys.Matrix(0, 0, bDown);
			m_Keys.Matrix(3, 0, bDown);
			if (bDown && m_StartHeld) m_bQuit = g_bQuitRequested = true;
			return true;
		case SDL_SCANCODE_HOME: // POWER
			if (bDown) m_bQuit = g_bQuitRequested = true;
			return true;
		default:
			return false;
		}
	}
#endif

	// a game controller (muOS presents the buttons this way)
	void Button(SDL_GameControllerButton B, bool bDown)
	{
		switch (B)
		{
		case SDL_CONTROLLER_BUTTON_DPAD_UP: m_Keys.Joy(kJoyUp, bDown); break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: m_Keys.Joy(kJoyDown, bDown); break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: m_Keys.Joy(kJoyLeft, bDown); break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: m_Keys.Joy(kJoyRight, bDown); break;
		case SDL_CONTROLLER_BUTTON_A: m_Keys.Joy(kJoyFire1, bDown); break;
		case SDL_CONTROLLER_BUTTON_B: m_Keys.Joy(kJoyFire2, bDown); break;
		case SDL_CONTROLLER_BUTTON_Y: m_Keys.Joy(kJoyFire3, bDown); break;
		case SDL_CONTROLLER_BUTTON_X: m_Keys.Matrix(7, 0, bDown); break;
		case SDL_CONTROLLER_BUTTON_START:
			m_StartHeld = bDown;
			m_Keys.Matrix(6, 0, bDown);
			if (bDown && m_SelectHeld) m_bQuit = g_bQuitRequested = true;
			break;
		case SDL_CONTROLLER_BUTTON_BACK:
			m_SelectHeld = bDown;
			m_Keys.Matrix(0, 0, bDown);
			m_Keys.Matrix(3, 0, bDown);
			if (bDown && m_StartHeld) m_bQuit = g_bQuitRequested = true;
			break;
		case SDL_CONTROLLER_BUTTON_GUIDE:
			if (bDown) m_bQuit = g_bQuitRequested = true;
			break;
		default:
			break;
		}
	}

	// a joystick SDL could not map as a controller: the hat and the first
	// two axes steer; buttons 0..3 are fire 1, fire 2, fire 3 and SPACE in
	// the order the handhelds usually report them (A, B, X, Y as 0..3 is
	// common, but not universal: see inspeccy.gptk for the mapped path)
	void JoyButton(int Button, bool bDown)
	{
		switch (Button)
		{
		case 0: m_Keys.Joy(kJoyFire1, bDown); break;
		case 1: m_Keys.Joy(kJoyFire2, bDown); break;
		case 2: m_Keys.Joy(kJoyFire3, bDown); break;
		case 3: m_Keys.Matrix(7, 0, bDown); break;
		case 6: // SELECT-like: EDIT
			m_SelectHeld = bDown;
			m_Keys.Matrix(0, 0, bDown);
			m_Keys.Matrix(3, 0, bDown);
			if (bDown && m_StartHeld) m_bQuit = g_bQuitRequested = true;
			break;
		case 7: // START-like: ENTER
			m_StartHeld = bDown;
			m_Keys.Matrix(6, 0, bDown);
			if (bDown && m_SelectHeld) m_bQuit = g_bQuitRequested = true;
			break;
		default:
			break;
		}
	}

	void JoyHat(uint8_t Value)
	{
		m_Keys.Joy(kJoyUp, (Value & SDL_HAT_UP) != 0);
		m_Keys.Joy(kJoyDown, (Value & SDL_HAT_DOWN) != 0);
		m_Keys.Joy(kJoyLeft, (Value & SDL_HAT_LEFT) != 0);
		m_Keys.Joy(kJoyRight, (Value & SDL_HAT_RIGHT) != 0);
	}

	void JoyAxis(int Axis, int16_t Value)
	{
		constexpr int kDead = 12000;
		if (Axis == 0)
		{
			m_Keys.Joy(kJoyLeft, Value < -kDead);
			m_Keys.Joy(kJoyRight, Value > kDead);
		}
		else if (Axis == 1)
		{
			m_Keys.Joy(kJoyUp, Value < -kDead);
			m_Keys.Joy(kJoyDown, Value > kDead);
		}
	}

	void Axis(SDL_GameControllerAxis A, int16_t Value)
	{
		constexpr int kDead = 12000;
		if (A == SDL_CONTROLLER_AXIS_LEFTX)
		{
			m_Keys.Joy(kJoyLeft, Value < -kDead);
			m_Keys.Joy(kJoyRight, Value > kDead);
		}
		else if (A == SDL_CONTROLLER_AXIS_LEFTY)
		{
			m_Keys.Joy(kJoyUp, Value < -kDead);
			m_Keys.Joy(kJoyDown, Value > kDead);
		}
	}
};

// The --bench path: as many frames as asked for, flat out and with no
// window, then the rate and the last frame.  Its own function so the
// compiler optimises it without the interactive loop in the way.
INSPECCY_SEPARATE int RunBench(Machine& InOutMachine, const Options& Opt, std::vector<uint8_t>& OutFrame)
{
	DacStream Dac;
	InOutMachine.m_Io->m_OnDacWriteUser = &Dac;
	InOutMachine.m_Io->m_OnDacWrite = [](void* InOutUser, uint8_t Mask, uint8_t Value, uint64_t Ns)
	{
		static_cast<DacStream*>(InOutUser)->Write(Mask, Value, Ns);
	};
	std::vector<int16_t> DacSamples(1762);
	uint64_t DacWrites = 0;
	const Uint64 T0 = SDL_GetPerformanceCounter();
	int Frames = 0;
	for (; Frames < Opt.m_BenchFrames; ++Frames)
	{
		uint8_t Rows[8];
		memset(Rows, 0xFF, sizeof Rows);
		InOutMachine.m_Input.RestoreFromDump(Rows, 0, 0);
		if (!InOutMachine.RunFrame())
		{
			break;
		}
		if (!Opt.m_bBenchNoCompose)
		{
			InOutMachine.DrawFrame(OutFrame.data(), kPitch);
#ifdef __EMSCRIPTEN__
			// optional lossless frame sink for native/WASM parity tests. The
			// host must copy the BGR bytes before this callback returns.
			EM_ASM(
				{
					if (Module['onBenchFrame'])
					{
						Module['onBenchFrame']($0, HEAPU8.subarray($1, $1 + $2));
					}
				},
				Frames, OutFrame.data(), OutFrame.size());
#endif
		}
		DacWrites += Dac.m_Events.size();
		std::fill(DacSamples.begin(), DacSamples.end(), 0);
		Dac.Mix(DacSamples, int(DacSamples.size() / 2), InOutMachine.m_Io->WallNanosNow(InOutMachine.m_Cpu.m_Cycles));
#ifdef __EMSCRIPTEN__
		EM_ASM(
			{
				if (Module['onBenchDac'])
				{
					Module['onBenchDac'](HEAP16.subarray($0 >> 1, ($0 >> 1) + $1));
				}
			},
			DacSamples.data(), DacSamples.size());
#endif
	}
	InOutMachine.m_Io->m_OnDacWrite = nullptr;
	InOutMachine.m_Io->m_OnDacWriteUser = nullptr;
	printf("DAC writes: %llu\n", (unsigned long long)DacWrites);
	printf("video frames: %llu; cycles: %llu\n", (unsigned long long)InOutMachine.m_Video.m_FrameCounter,
		(unsigned long long)InOutMachine.m_Cpu.m_Cycles);
	const double Seconds = double(SDL_GetPerformanceCounter() - T0) / double(SDL_GetPerformanceFrequency());
	printf("%d frames in %.2f s: %.1f frames/s (%.0f%% of %d Hz), %.1f MHz emulated\n", Frames, Seconds,
		Frames / Seconds, 100.0 * Frames / Seconds / Opt.m_Hz, Opt.m_Hz, InOutMachine.m_Io->m_CurrentMhz);
	{
		const uint8_t* Regs = InOutMachine.m_Mem->GetNextRegsArray();
		printf("last result %d, PC $%04x; NR15 $%02x NR69 $%02x NR07 $%02x; period %llu\n", InOutMachine.m_LastResult,
			InOutMachine.m_Cpu.PC, Regs[0x15], Regs[0x69], Regs[0x07],
			(unsigned long long)InOutMachine.m_Cpu.m_FrameIntPeriod);
	}
	// and the last frame, to see that it got somewhere
	if (FILE* Fp = fopen("inspeccy-emul-bench.ppm", "wb"))
	{
		fprintf(Fp, "P6\n%d %d\n255\n", kTexW, kCanvasH);
		for (size_t IdxPixel = 0; IdxPixel < OutFrame.size(); IdxPixel += 3)
		{
			const uint8_t Rgb[3] = {OutFrame[IdxPixel + 2], OutFrame[IdxPixel + 1], OutFrame[IdxPixel]};
			fwrite(Rgb, 1, 3, Fp);
		}
		fclose(Fp);
		Fp = nullptr;
	}
	// and the reference compositor's picture of the same state, to compare
	std::string BmpErr;
	WriteCompositeBmp(*InOutMachine.m_Mem, InOutMachine.m_Sprites.get(), "inspeccy-emul-bench-ref.bmp", BmpErr);
	return 0;
}

// Everything the interactive run needs: the window, the sound, the input
// and the frame loop.  Likewise separate: main() holding all of this made
// the file too big for the compiler to inline the hot calls anywhere in it.
INSPECCY_SEPARATE int RunInteractive(Machine& M, Options& Opt, std::vector<uint8_t>& Frame, const char* ProgramPath)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
	{
		fprintf(stderr, "inspeccy-emul: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	{
		SDL_version Linked;
		SDL_GetVersion(&Linked);
		fprintf(stderr, "inspeccy-emul: SDL %u.%u.%u, video driver %s, drivers:", Linked.major, Linked.minor,
			Linked.patch, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none yet)");
		for (int I = 0, N = SDL_GetNumVideoDrivers(); I < N; ++I)
		{
			fprintf(stderr, " %s", SDL_GetVideoDriver(I));
		}
		fprintf(stderr, "\n");
	}
	const int ViewH = kCanvasH - 2 * Opt.m_Crop;
	Uint32 WindowFlags = SDL_WINDOW_SHOWN;
#if INSPECCY_OPENDINGUX
	// a window the size of the canvas: the handheld's scaler fills the screen
	WindowFlags |= SDL_WINDOW_FULLSCREEN;
	const int WinW = kCanvasW, WinH = ViewH;
#else
	if (Opt.m_bFullscreen)
	{
		WindowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}
	const int WinW = kCanvasW * Opt.m_Scale, WinH = ViewH * Opt.m_Scale;
#endif
	SDL_Window* Window = SDL_CreateWindow("inspeccy-emul " INSPECCY_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WinW, WinH, WindowFlags);
	if (Window == nullptr)
	{
		fprintf(stderr, "inspeccy-emul: SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Renderer* Renderer = nullptr;
	if (!Opt.m_bNoRenderer)
	{
		Renderer = SDL_CreateRenderer(Window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (Renderer == nullptr)
		{
			fprintf(stderr, "inspeccy-emul: no accelerated vsync renderer (%s), trying any\n", SDL_GetError());
			Renderer = SDL_CreateRenderer(Window, -1, 0);
		}
	}
	if (Renderer == nullptr)
	{
		fprintf(stderr, "inspeccy-emul: no renderer at all (%s), blitting into the window surface\n", SDL_GetError());
	}
	if (Renderer != nullptr)
	{
		SDL_RenderSetLogicalSize(Renderer, kCanvasW, ViewH);
		int OutW = 0, OutH = 0;
		SDL_GetRendererOutputSize(Renderer, &OutW, &OutH);
		const bool bWhole = OutW >= 2 * kCanvasW && OutH >= 2 * ViewH;
		SDL_RenderSetIntegerScale(Renderer, bWhole ? SDL_TRUE : SDL_FALSE);
		fprintf(stderr, "inspeccy-emul: output %dx%d, %s scaling\n", OutW, OutH, bWhole ? "whole-number" : "free");
	}
	if (Renderer != nullptr)
	{
		SDL_RendererInfo Info;
		if (SDL_GetRendererInfo(Renderer, &Info) == 0)
		{
			fprintf(stderr, "inspeccy-emul: renderer %s, flags %x, %u texture formats\n", Info.name, Info.flags, Info.num_texture_formats);
		}
	}
	// 32-bit: the format every renderer has; the composite's BGR triples are
	// widened into it each frame (bytes B, G, R, X in memory)
	SDL_Texture* Texture = nullptr;
	if (Renderer != nullptr)
	{
		Texture = SDL_CreateTexture(Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kTexW, kCanvasH);
		if (Texture == nullptr)
		{
			fprintf(stderr, "inspeccy-emul: no texture (%s), blitting into the window surface\n", SDL_GetError());
			SDL_DestroyRenderer(Renderer);
			Renderer = nullptr;
		}
	}
	// the surface path: one 32-bit surface of the canvas, blitted and scaled
	// into the window's surface, for a driver with no working renderer
	SDL_Surface* CanvasSurface = nullptr;
	if (Renderer == nullptr)
	{
		CanvasSurface = SDL_CreateRGBSurfaceWithFormat(0, kTexW, kCanvasH, 32, SDL_PIXELFORMAT_ARGB8888);
		if (CanvasSurface == nullptr)
		{
			fprintf(stderr, "inspeccy-emul: no window-surface path either: %s\n", SDL_GetError());
			return 1;
		}
	}
	std::vector<uint8_t> Frame32(size_t(kTexW) * 4 * kCanvasH, 0);
	fprintf(stderr, "inspeccy-emul: window %dx%d, drawing through %s\n", WinW, WinH,
		Renderer != nullptr ? "a renderer" : "the window surface");
	const SDL_Rect Source = {0, Opt.m_Crop, kTexW, ViewH};

	// audio: a frame's worth of the three chips a frame, kept a few frames deep
	SDL_AudioDeviceID Audio = 0;
	AySynth Ay1, Ay2, Ay3;
	DacStream Dac;
	std::vector<int16_t> Samples;
	if (!Opt.m_bNoAudio)
	{
		SDL_AudioSpec Want = {}, Have = {};
		Want.freq = Opt.m_RateHz;
		Want.format = AUDIO_S16SYS;
		Want.channels = 2;
		Want.samples = 1024;
		Audio = SDL_OpenAudioDevice(nullptr, 0, &Want, &Have, 0);
		if (Audio == 0)
		{
			fprintf(stderr, "inspeccy-emul: no audio: %s\n", SDL_GetError());
		}
		else
		{
			Ay1.Init(uint32_t(Have.freq));
			Ay2.Init(uint32_t(Have.freq));
			Ay3.Init(uint32_t(Have.freq));
			Opt.m_RateHz = Have.freq;
			M.m_Io->m_OnDacWriteUser = &Dac;
			M.m_Io->m_OnDacWrite = [](void* InOutUser, uint8_t Mask, uint8_t Value, uint64_t Ns)
			{
				static_cast<DacStream*>(InOutUser)->Write(Mask, Value, Ns);
			};
			SDL_PauseAudioDevice(Audio, 0);
		}
	}
	const double FrameHz = (Opt.m_Hz == 50) ? 50.0801 : 59.6373; // the Next's own rates
	const int SamplesPerFrame = int(Opt.m_RateHz / FrameHz + 0.5);
	const Uint32 MaxQueuedBytes = Uint32(SamplesPerFrame * 4 * 5);

	// controller mappings: SDL's own, SDL_GAMECONTROLLERCONFIG from the
	// environment (PortMaster sets it), and a database beside the program
	{
		const std::string Db = DirectoryOf(ProgramPath) + "/gamecontrollerdb.txt";
		const int Added = SDL_GameControllerAddMappingsFromFile(Db.c_str());
		if (Added > 0)
		{
			fprintf(stderr, "inspeccy-emul: %d controller mappings from %s\n", Added, Db.c_str());
		}
	}
	SDL_GameController* Pad = nullptr;
	SDL_Joystick* RawJoy = nullptr;
	auto OpenInput = [&](int Index) {
		if (Pad != nullptr || RawJoy != nullptr)
		{
			return;
		}
		if (SDL_IsGameController(Index))
		{
			Pad = SDL_GameControllerOpen(Index);
			fprintf(stderr, "inspeccy-emul: controller %s\n", SDL_GameControllerName(Pad));
		}
		else
		{
			RawJoy = SDL_JoystickOpen(Index);
			if (RawJoy != nullptr)
			{
				fprintf(stderr, "inspeccy-emul: joystick %s without a controller mapping: %d buttons, %d hats, %d axes\n",
					SDL_JoystickName(RawJoy), SDL_JoystickNumButtons(RawJoy), SDL_JoystickNumHats(RawJoy),
					SDL_JoystickNumAxes(RawJoy));
			}
		}
	};
	for (int I = 0; I < SDL_NumJoysticks(); ++I)
	{
		OpenInput(I);
	}

	Frontend Fe;
	const double Freq = double(SDL_GetPerformanceFrequency());
	const double FrameSeconds = 1.0 / FrameHz;
	double Due = double(SDL_GetPerformanceCounter()) / Freq;
	int Skipped = 0;
	int Shown = 0;
	double ReportedAt = double(SDL_GetPerformanceCounter()) / Freq;
#ifdef __EMSCRIPTEN__
	bool WasPaused = false;
#endif
	// One frame: the events, the emulation, the sound and the picture.  A
	// browser calls this from requestAnimationFrame; everywhere else the
	// plain loop below does.  It returns early where a loop would `continue`.
	auto RunOneFrame = [&]() {
#ifdef __EMSCRIPTEN__
		const double FrameStarted = double(SDL_GetPerformanceCounter()) / Freq;
		const bool Paused = EM_ASM_INT({ return !!(document.hidden || Module['paused']); });
		if (Paused != WasPaused)
		{
			Fe = Frontend(); // no stuck keys after losing focus / stopping
			if (Audio) { SDL_ClearQueuedAudio(Audio); SDL_PauseAudioDevice(Audio, Paused); }
			WasPaused = Paused;
		}
		if (Paused) { Due = FrameStarted; Shown = 0; ReportedAt = FrameStarted; return; }
		// rAF follows the monitor, not the emulated machine. In particular,
		// a 50 Hz game must not run at 60/120/144 Hz on those displays.
		if (FrameStarted < Due) return;
#endif
		SDL_Event E;
		while (SDL_PollEvent(&E))
		{
			switch (E.type)
			{
			case SDL_QUIT:
				Fe.m_bQuit = g_bQuitRequested = true;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				if (E.key.repeat)
				{
					break;
				}
#if INSPECCY_OPENDINGUX
				if (Fe.BindHandheld(E.key.keysym.scancode, E.type == SDL_KEYDOWN))
				{
					break;
				}
#endif
				Fe.Key(E.key.keysym.scancode, E.type == SDL_KEYDOWN);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				Fe.Button(SDL_GameControllerButton(E.cbutton.button), E.type == SDL_CONTROLLERBUTTONDOWN);
				break;
			case SDL_CONTROLLERAXISMOTION:
				Fe.Axis(SDL_GameControllerAxis(E.caxis.axis), E.caxis.value);
				break;
			case SDL_JOYDEVICEADDED:
				OpenInput(E.jdevice.which);
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				if (RawJoy != nullptr)
				{
					Fe.JoyButton(E.jbutton.button, E.type == SDL_JOYBUTTONDOWN);
				}
				break;
			case SDL_JOYHATMOTION:
				if (RawJoy != nullptr)
				{
					Fe.JoyHat(E.jhat.value);
				}
				break;
			case SDL_JOYAXISMOTION:
				if (RawJoy != nullptr)
				{
					Fe.JoyAxis(E.jaxis.axis, E.jaxis.value);
				}
				break;
			default:
				break;
			}
		}
		if (Fe.m_bToggleFullscreen)
		{
			Fe.m_bToggleFullscreen = false;
			const bool bFull = (SDL_GetWindowFlags(Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
			SDL_SetWindowFullscreen(Window, bFull ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
		}

		// one emulated frame, or two when a frame behind (never more: a slow
		// machine then simply runs slow rather than falling ever further back)
		M.m_Input.RestoreFromDump(Fe.m_Keys.m_Rows, Fe.m_Keys.m_Kempston, 0);
		if (!M.RunFrame())
		{
			fprintf(stderr, "inspeccy-emul: the program stopped\n");
			Fe.m_bQuit = g_bQuitRequested = true;
			return;
		}
		if (Audio != 0)
		{
			Samples.assign(size_t(SamplesPerFrame) * 2, 0);
			const bool Acb = (M.m_Mem->GetNextReg(0x08) & 0x20) != 0;
			const uint8_t Mono = M.m_Mem->GetNextReg(0x09);
			Ay1.Render(M.m_Ay, Samples.data(), SamplesPerFrame, Acb, (Mono & 0x20) != 0);
			Ay2.Render(M.m_Ay2, Samples.data(), SamplesPerFrame, Acb, (Mono & 0x40) != 0);
			Ay3.Render(M.m_Ay3, Samples.data(), SamplesPerFrame, Acb, (Mono & 0x80) != 0);
			Dac.Mix(Samples, SamplesPerFrame, M.m_Io->WallNanosNow(M.m_Cpu.m_Cycles));
			if (SDL_GetQueuedAudioSize(Audio) < MaxQueuedBytes)
			{
				SDL_QueueAudio(Audio, Samples.data(), Uint32(Samples.size() * sizeof(int16_t)));
			}
		}
		Due += FrameSeconds;
		const double Now = double(SDL_GetPerformanceCounter()) / Freq;
		if (Now > Due + FrameSeconds && Skipped < 1)
		{
			++Skipped; // behind: the next frame runs before this one is shown
#ifdef __EMSCRIPTEN__
			EM_ASM({ if (Module['onFrame']) Module['onFrame']($0, false); },
				(double(SDL_GetPerformanceCounter()) / Freq - FrameStarted) * 1000.0);
#endif
			return;
		}
		Skipped = 0;
		if (Now > Due + 4 * FrameSeconds)
		{
			Due = Now; // hopelessly behind (a pause, a slow machine): start afresh
		}

		M.DrawFrame(Frame.data(), kPitch);
		{
			const uint8_t* In = Frame.data();
			uint32_t* Out32 = reinterpret_cast<uint32_t*>(Frame32.data());
			for (size_t I = 0, N = size_t(kTexW) * kCanvasH; I < N; ++I, In += 3)
			{
				Out32[I] = 0xFF000000u | (uint32_t(In[2]) << 16) | (uint32_t(In[1]) << 8) | In[0];
			}
		}
		if (Renderer != nullptr)
		{
			SDL_UpdateTexture(Texture, nullptr, Frame32.data(), kTexW * 4);
			SDL_RenderClear(Renderer);
			SDL_RenderCopy(Renderer, Texture, &Source, nullptr);
			SDL_RenderPresent(Renderer);
		}
		else if (SDL_Surface* WindowSurface = SDL_GetWindowSurface(Window))
		{
			memcpy(CanvasSurface->pixels, Frame32.data(), size_t(kTexW) * 4 * kCanvasH);
			SDL_FillRect(WindowSurface, nullptr, 0);
			SDL_BlitScaled(CanvasSurface, const_cast<SDL_Rect*>(&Source), WindowSurface, nullptr);
			SDL_UpdateWindowSurface(Window);
		}

#ifndef __EMSCRIPTEN__
		// pace to the Next's frame rate when vsync does not (or runs faster);
		// in a browser requestAnimationFrame does the waiting
		const double After = double(SDL_GetPerformanceCounter()) / Freq;
		if (Due > After)
		{
			const double Wait = Due - After;
			if (Wait > 0.002)
			{
				SDL_Delay(Uint32((Wait - 0.001) * 1000.0));
			}
		}
#endif
		SyncFilesIfWritten(M.m_Vfs);
#ifdef __EMSCRIPTEN__
		EM_ASM({ if (Module['onFrame']) Module['onFrame']($0, true); },
			(double(SDL_GetPerformanceCounter()) / Freq - FrameStarted) * 1000.0);
#endif

		++Shown;
		const double Since = double(SDL_GetPerformanceCounter()) / Freq - ReportedAt;
		if (Since >= 5.0)
		{
			fprintf(stderr, "inspeccy-emul: %.1f frames a second (%.0f%% of %.1f Hz)\n", Shown / Since,
				100.0 * (Shown / Since) / FrameHz, FrameHz);
			Shown = 0;
			ReportedAt = double(SDL_GetPerformanceCounter()) / Freq;
		}
	};

#ifdef __EMSCRIPTEN__
	// The browser drives it: 0 = requestAnimationFrame, and the 1 keeps
	// main's stack (and so everything the lambda captured) alive.
	emscripten_set_main_loop_arg(
		[](void* User) {
			auto& Frame = *static_cast<decltype(RunOneFrame)*>(User);
			Frame();
			if (QuitRequested())
			{
				emscripten_cancel_main_loop();
			}
		},
		&RunOneFrame, 0, 1);
#else
	while (!Fe.m_bQuit)
	{
		RunOneFrame();
	}
#endif

	if (Pad != nullptr)
	{
		SDL_GameControllerClose(Pad);
	}
	if (RawJoy != nullptr)
	{
		SDL_JoystickClose(RawJoy);
	}
	if (Audio != 0)
	{
		SDL_CloseAudioDevice(Audio);
	}
	if (CanvasSurface != nullptr)
	{
		SDL_FreeSurface(CanvasSurface);
	}
	if (Texture != nullptr)
	{
		SDL_DestroyTexture(Texture);
	}
	if (Renderer != nullptr)
	{
		SDL_DestroyRenderer(Renderer);
	}
	SDL_DestroyWindow(Window);
	SDL_Quit();
	return 0;
}

} // namespace

int main(int Argc, char** Argv)
{
	if (Argc == 2 && strcmp(Argv[1], "--version") == 0)
	{
		puts("inspeccy-emul " INSPECCY_VERSION);
		return 0;
	}
	if (Argc == 2 && (strcmp(Argv[1], "--help") == 0 || strcmp(Argv[1], "-h") == 0))
	{
		Usage();
		return 0;
	}

	Options Opt;
	if (!ParseArgs(Argc, Argv, Opt))
	{
		Usage();
		return 2;
	}

	Machine M;
	std::string Err;
	if (!M.Boot(Opt, Err))
	{
		fprintf(stderr, "inspeccy-emul: %s\n", Err.c_str());
		return 1;
	}
	std::vector<uint8_t> Frame(size_t(kPitch) * kCanvasH, 0);

	if (Opt.m_BenchFrames > 0)
	{
		return RunBench(M, Opt, Frame);
	}
	return RunInteractive(M, Opt, Frame, Argv[0]);
}
