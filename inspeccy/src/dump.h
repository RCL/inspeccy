#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "nextzxos.h"

struct MemoryBus;
struct PsgCapture;
struct InputState;
struct Trdos;
struct NextZxosVfs;
struct TapePlayer;
struct AyState;
struct Z80State;

// Dot-command metadata captured at launch time and round-tripped through the
// dump so resume / inspection tools know how the run was started. All fields
// optional - the writer skips the block entirely when DotCmdPath is empty.
struct DumpDotCmdInfo
{
	std::string m_DotCmdPath; // file path to the .dot binary
	std::string m_ArgString; // --dot-arg payload (may be empty)
	uint16_t m_LoadAddr = 0; // typically $2000
	uint64_t m_BinarySize = 0;
	int m_Code8KPage = -1; // 8K RAM page mapped at slot 1
};

// NEX metadata, populated when the run was started with --nex. Writer
// skips the block entirely when NexPath is empty.
struct DumpNexInfo
{
	std::string m_NexPath;
	std::string m_NexSha1;
	std::string m_IdentityOverride;
	std::string m_Version;
	uint8_t m_NumBanks = 0;
	uint8_t m_ScreenFlags = 0;
	uint8_t m_Border = 0;
	uint8_t m_EntryBank = 0;
	uint16_t m_InitialSP = 0;
	uint16_t m_InitialPC = 0;
	uint8_t m_CoreVersion[3] = {0, 0, 0};
	bool m_bHasCopperCode = false;
	bool m_bHasLoadingScreen = false;
	bool m_bLoadingScreenApplied = false; // Layer 2 screen blitted + enabled at load
	uint32_t m_BanksOffset = 512;
	std::vector<int> m_BanksPresent;
};

// The run's options that change how the machine behaves (not what the run
// writes out), so a resumed run behaves as the original: a resume adopts
// each one unless the command line names it again.
struct DumpRunOptions
{
	bool m_bPresent = false; // the dump had the block (older dumps do not)
	bool m_bResetTrap = true;
	bool m_bLenientUndocEd = false;
	bool m_bNoCopper = false;
	bool m_bPixelAccurate = false;
	bool m_bAyEnabled = true;
	bool m_bTurboSound = false;
	bool m_bVfsReadOnly = false;
	int64_t m_IntWindowMode = -1;
	uint64_t m_TapTypeFrame = 96;
	bool m_bTapTypeFrameSet = false;
	int m_InputTapFrames = 2;
	bool m_bInputLog = true;
};

// Where the recordings stood (--video-out, --motion-out / --histogram-out /
// --paper-out), so a resumed run's frames and rows carry on from the dumped
// run's: the next file number, the next sample cycle, the motion history.
struct DumpCaptureState
{
	bool m_bVideo = false;
	uint64_t m_VideoFrames = 0, m_VideoNextEmit = 0, m_VideoPeriod = 0;
	bool m_bMetrics = false;
	uint64_t m_MetricsFrames = 0, m_MetricsNextEmit = 0, m_MetricsPeriod = 0;
	std::vector<std::vector<uint8_t>> m_MetricsHistory;
};

// External-file references stored in the dump so resume can re-mount them
// without the user re-specifying every path. SHA1 fields are filled by main
// at dump time; the loader compares them on resume (DUMP2). Empty string =
// not mounted in this run.
struct DumpExternalRefs
{
	std::string m_Rom48Path;
	std::string m_Rom128Path;
	std::string m_RomTrdPath;
	std::string m_TapPath; // reserved for TAP plan
	std::string m_TrdDiskPath;
	std::string m_InputScriptPath;
	std::string m_RomPlus3Path; // --rom-plus3
	std::string m_DckPath; // --dck
};

// Snapshot of every reference the dump writer needs. Built once in main.cpp
// after RunUntil returns and the per-component getters are stable.
struct ZxnDmaState;
struct AudioRenderer;
struct SpritesState;

struct DumpReport
{
	const Z80State* m_Cpu = nullptr;
	const MemoryBus* m_Mem = nullptr;
	const AyState* m_Ay = nullptr;
	const PsgCapture* m_Capture = nullptr; // null when --no PSG capture
	const AyState* m_Ay2 = nullptr; // TurboSound second chip (--ts)
	const PsgCapture* m_Capture2 = nullptr; // and its capture
	uint8_t m_AyActive = 0; // the chip register writes go to
	const InputState* m_Input = nullptr; // null when no --input script
	const Trdos* m_Trd = nullptr; // present always; emitted only if IsSupportActive
	const TapePlayer* m_Tape = nullptr; // non-null only when --tap
	const NextZxosVfs* m_DotVfs = nullptr; // non-null only when --dot / --nex streaming
	const ZxnDmaState* m_Dma = nullptr; // non-null on Next-family machines
	const AudioRenderer* m_Audio = nullptr; // non-null when --audio-out
	const SpritesState* m_Sprites = nullptr; // non-null on Next-family machines
	DumpDotCmdInfo m_DotCmd; // emitted only when DotCmdPath set
	DumpNexInfo m_Nex; // emitted only when NexPath set
	std::string m_MachineName; // "spectrum48k" / "spectrum128k" / ...
	double m_Mhz = 3.5; // effective clock at save time
	double m_NominalMhz = 3.5; // the machine's real stock clock
	uint32_t m_ContentionX = 0; // 0 = contention off; else the per-access penalty
	uint64_t m_FrameIntPeriod = 0;
	uint64_t m_StatsTickPeriod = 0;
	uint64_t m_WallNanos = 0; // emulated wall clock at save time (audio event time)
	uint64_t m_WallCheckpointNanos = 0; // and the checkpoint it advances from
	uint64_t m_WallCheckpointCycle = 0;
	DumpExternalRefs m_ExternalRefs;
	DumpRunOptions m_Options;
	DumpCaptureState m_Recording;
	// --coverage-out: the map (marking has stopped by dump time) and its
	// one-shot reset address, -1 once fired
	const std::vector<uint8_t>* m_CoverageMap = nullptr;
	int32_t m_CoverageResetPc = -1;
};

// Write the dump as inspeccy's textual snapshot format to Path. Returns
// false and sets OutError on file-open or write failure.
bool WriteDumpJson(const DumpReport& R, const char* Path, std::string& OutError);

// --- Resume side (DUMP2) ---

struct JsonValue;

// Read + parse the dump file at Path into Root. Validates dump_version == 1.
bool ReadAndParseDump(const char* Path, JsonValue& OutRoot, std::string& OutError);

// Resolve a stored external_refs.path relative to the dump file's directory.
// Absolute paths in the dump are returned as-is (with forward slashes).
// Relative paths are joined with the dump's directory and lexically normalised.
// Returns an empty string when StoredPath is empty.
std::string ResolveRefPathFromDump(const std::string& StoredPath, const std::string& DumpPath);

// External-file references resolved from the dump.
struct ParsedExternalRefs
{
	std::string m_Rom48Path, m_Rom48Sha1;
	std::string m_Rom128Path, m_Rom128Sha1;
	std::string m_RomTrdPath, m_RomTrdSha1;
	std::string m_TapPath, m_TapSha1;
	std::string m_TrdDiskPath, m_TrdDiskSha1;
	std::string m_InputScriptPath, m_InputScriptSha1;
	std::string m_RomPlus3Path, m_RomPlus3Sha1;
	std::string m_DckPath, m_DckSha1;
};
bool LoadExternalRefsFromDump(const JsonValue& Root, ParsedExternalRefs& Out, std::string& OutError);

// Convenience: the run-config bits the dump carries that main.cpp needs.
struct DumpRunConfig
{
	std::string m_Machine; // "spectrum48k" / "spectrum128k" / ...
	double m_Mhz = 3.5;
	int m_RamMb = 2;
	uint32_t m_ContentionX = 0; // 0 = contention was off
	uint64_t m_FrameIntPeriod = 0;
	bool m_bAyEnabled = true;
	bool m_bHasPsg = false;
	std::string m_PsgOutPath;
	int m_PsgRate = 50;
	bool m_bHasInput = false;
	int m_InputTapFrames = 2;
	bool m_bInputLog = true;
	bool m_bTrdosSupportActive = false;

	// NEX runs: the originally-loaded .nex file (so resume can re-open
	// it for streaming F_READ) + the saved position of VFS handle 1
	// (so the demo's next F_READ continues from where it left off).
	std::string m_NexPath;
	std::string m_NexSha1;
	std::string m_IdentityOverride;
	bool m_bHasNexHandle = false;
	long m_NexHandlePosition = 0;
	DumpRunOptions m_Options;
	DumpNexInfo m_Nex; // the dump's nex block, for the next dump of a resumed run
};
bool LoadRunConfigFromDump(const JsonValue& Root, DumpRunConfig& Out, std::string& OutError);

// Restore CTC state so resumed timer and audio interrupts retain their schedule.
bool LoadCtcFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
bool LoadCopperFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
bool LoadBusExtrasFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
bool LoadDmaFromDump(const JsonValue& Root, struct ZxnDmaState& Out, std::string& OutError);
bool LoadAudioFromDump(const JsonValue& Root, struct AudioRenderer& Out, std::string& OutError);
bool LoadCaptureFromDump(const JsonValue& Root, DumpCaptureState& Out, std::string& OutError);
bool LoadSlotsFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
// The sprite attributes, pattern memory and port cursors; a missing block
// (a classic machine, an older dump) leaves the state at reset.
bool LoadSpritesFromDump(const JsonValue& Root, struct SpritesState& Out, std::string& OutError);

// Per-component restore functions. Each is idempotent on a fresh component
// instance. Order in main.cpp:
//   1. parse dump
//   2. load external_refs, load ROMs (with CLI overrides applied)
//   3. construct Z80State / MemoryBus / AyState
//   4. LoadCpuFromDump, LoadMemoryFromDump (sets paging mode), LoadRamFromDump
//   5. LoadSchedulerFromDump, LoadStatsFromDump
//   6. LoadAyFromDump
//   7. construct PsgCapture (if dump has psg) -> LoadPsgFromDump
//   8. construct Trdos -> if support_active in dump, mount disk + LoadTrdosFromDump
//   9. construct InputState -> if HasInput, LoadScript + LoadInputFromDump
bool LoadCpuFromDump(const JsonValue& Root, struct Z80State& Out, std::string& OutError);
bool LoadMemoryFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
bool LoadRamFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);
bool LoadAyFromDump(const JsonValue& Root, struct AyState& Out, std::string& OutError, const char* Key = "ay");
bool LoadPsgFromDump(
	const JsonValue& Root, struct PsgCapture& InOutCapture, std::string& OutError, const char* Key = "psg");
bool LoadTrdosFromDump(const JsonValue& Root, struct Trdos& InOutTrd, std::string& OutError);
bool LoadInputFromDump(const JsonValue& Root, struct InputState& InOutInput, std::string& OutError);
// A resume given a different --input script than the dump ran with: the
// keys the dump holds stay held, and the script's events from the dump's
// frame on replace the dump's own. True when it replaced them.
bool TakeInputScriptOnResume(const JsonValue& Root, struct InputState& InOutInput,
	const std::vector<struct InputEvent>& Script, std::string& OutError);
bool LoadSchedulerFromDump(const JsonValue& Root, struct Z80State& Out, std::string& OutError);
bool LoadStatsFromDump(const JsonValue& Root, struct Z80State& Out, std::string& OutError);

// New blocks for Next/dot-cmd/VFS - all tolerant of missing sections so they
// can be invoked unconditionally during resume of any dump.
bool LoadNextFromDump(const JsonValue& Root, struct MemoryBus& Out, std::string& OutError);

// Resolved on the load side. ResolvedRoot is set only when the dump's vfs
// block carries a path; main.cpp passes it through to NextZxosVfs.Mount.
struct ParsedDotCmdInfo
{
	bool m_bPresent = false;
	std::string m_Path;
	std::string m_ArgString;
	uint16_t m_LoadAddr = 0;
	uint64_t m_BinarySize = 0;
	int m_Code8KPage = -1;
};
struct ParsedVfsInfo
{
	bool m_bPresent = false;
	std::string m_RootPath;
	char m_Drive = 'C';
	uint64_t m_FilesOpened = 0, m_BytesRead = 0, m_BytesWritten = 0;
	uint64_t m_IdeBankAllocs = 0, m_IdeBankFrees = 0;
	uint64_t m_Rst08Calls = 0, m_Rst08Unknown = 0;
};
bool LoadDotCmdFromDump(const JsonValue& Root, ParsedDotCmdInfo& Out, std::string& OutError);
bool LoadVfsFromDump(const JsonValue& Root, ParsedVfsInfo& Out, std::string& OutError);
bool LoadVfsResumeFromDump(const JsonValue& Root, const std::string& DumpPath, NextZxosVfs::ResumeState& Out,
	bool& OutbPresent, std::string& OutRootPath, char& OutDrive, std::string& OutError);
