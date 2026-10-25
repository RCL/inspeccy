#pragma once
// NextZXOS / esxDOS API subset for headless dot-command execution.
//
// Mechanism: when a dot command does RST $08 : db <API>, inspeccy's
// dispatcher (z80.cpp) traps at PC=$0008, pops the API-byte address from
// the stack, reads the API byte, advances PC past it, then calls
// NextZxosVfs::HandleRst08(api, cpu, mem). The handler reads register
// inputs, executes the API, sets register outputs (CF, A, BC, etc.),
// and returns. No Z80-level ROM stub is involved.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>

struct MemoryBus;
struct Z80State;

// One directory entry captured at F_OPENDIR time (a snapshot, so F_SEEKDIR /
// F_TELLDIR positions stay stable while the guest walks the listing).
struct VfsDirEntry
{
	std::string m_Name; // leaf name, as it appears on the host
	bool m_bDir = false; // FAT attr bit 4
	bool m_bReadOnly = false; // FAT attr bit 0
	uint32_t m_Size = 0; // 0 for directories
};

// One open handle. Indices 1..MAX_HANDLES-1 (0 reserved as "invalid").
// A handle is EITHER a file (File != nullptr) OR a directory (bDir, Dir/DirPos);
// esxDOS keeps both in one number space and F_CLOSE closes either.
struct VfsHandle
{
	FILE* m_File = nullptr;
	std::string m_ResolvedPath; // host path, for inspection/dump
	std::string m_OpenedAs; // path as the dot command opened it
	bool m_bWritable = false;

	bool m_bDir = false; // directory handle (File == nullptr)
	std::vector<VfsDirEntry> m_Dir; // snapshot listing (F_OPENDIR)
	size_t m_DirPos = 0; // next entry F_READDIR returns

	bool InUse() const
	{
		return m_File != nullptr || m_bDir;
	}
};

struct NextZxosVfs final
{
  public:
	NextZxosVfs();
	~NextZxosVfs(); // closes any FILE handles this instance owns

	// Frame-exact replay: become a copy of Src whose open handles are FRESH
	// read-only FILE*s positioned where Src's live files stand right now, so a
	// checkpoint replay re-reads the same bytes without moving the live file
	// positions. Returns false if any handle could not be reopened (that
	// handle is left empty; the replay self-check catches any use).
	bool CloneForReplay(const NextZxosVfs& Src);

	// Mount Path as the VFS root. DriveLetter is the letter under which the
	// VFS appears to the dot command ('*' wildcard resolves to this letter).
	// Returns false if Path doesn't exist or isn't a directory.
	bool Mount(const char* Path, char DriveLetter, std::string& OutError);

	bool IsMounted() const
	{
		return m_bMounted;
	}
	const std::string& Root() const
	{
		return m_RootPath;
	}
	char DriveLetter() const
	{
		return m_Drive;
	}

	// Register a host file as a pre-opened handle in the given slot. Used
	// for NEX streaming: the NEX loader leaves the .nex file open and the
	// demo reads from it via F_READ on this handle. Ownership of File
	// transfers to NextZxosVfs (closed in destructor / ReleaseHandle).
	// Returns false if the slot is invalid or already occupied.
	bool RegisterPreOpenedHandle(int Slot, FILE* File, const std::string& OpenedAs, const std::string& ResolvedPath);

	// RST $08 dispatcher. Api is the byte that followed the RST $08 opcode.
	// Cpu/Mem give the handler access to registers and memory.
	void HandleRst08(uint8_t Api, Z80State& InOutCpu, MemoryBus& InOutMem);

	// Calling-convention selector. esxDOS file APIs use HL for buffer
	// pointers when invoked from a dot command, IX when invoked from a
	// NEX program (dot commands pass HL, NEX programs IX -
	// both observed in the bundled test programs).
	// Defaults to HL (dot command); main.cpp flips to IX for --nex.
	void SetUseIxForBuffers(bool bB)
	{
		m_bUseIxForBuffers = bB;
	}

	// Mount-level write lock. Default off: an --nex game's own directory is its
	// sandbox and NextZXOS lets it save there. A compatibility sweep that must
	// not modify the collection turns it on; every write API then returns
	// A = VFS_ERR_READ_ONLY without touching the host filesystem.
	void SetReadOnly(bool bB)
	{
		m_bReadOnly = bB;
	}
	bool IsReadOnly() const
	{
		return m_bReadOnly;
	}

	// F_READ time-slice state (chunked by the z80 dispatcher so nested
	// CTC0 ISRs can fire mid-read, like real hw). DoFRead reads the bytes
	// from the host file upfront into Buf, then sets bActive - the
	// dispatcher's F_READ chunker writes Buf to memory in event-bounded
	// chunks, charging cycles per byte, and finalises (sets BC, advances
	// HL/IX, clears CF) when Done == Total.
	bool FReadActive() const
	{
		return m_FRead.m_bActive;
	}
	uint8_t FReadDepth() const
	{
		return m_FRead.m_IsrDepthAtStart;
	}
	// Step at most MaxBytes of the pending F_READ into Mem and return how
	// many cycles to charge for those bytes. The proportional charge keeps
	// the cumulative cost equal to TotalCharge once Done == Total, so
	// tests pinning read_cycles_charged stay deterministic.
	uint32_t StepFRead(MemoryBus& InOutMem, uint32_t MaxBytes, uint64_t& OutCharge);
	// Called when Done == Total; commits the return values to Cpu and
	// clears the pending state.
	void FinaliseFRead(Z80State& InOutCpu);
	// Per-byte cycle cost (approximate, only used to bound the chunk size
	// by the next-event deadline). Final accounting uses the proportional
	// formula in StepFRead.
	uint64_t FReadPerByteCycles() const
	{
		return m_FRead.m_Total > 0 ? (m_FRead.m_TotalCharge + m_FRead.m_Total / 2) / m_FRead.m_Total : 0;
	}
	bool FReadComplete() const
	{
		return m_FRead.m_bActive && m_FRead.m_Done >= m_FRead.m_Total;
	}

	// Inspection counters (read at exit for status JSON).
	uint64_t FilesOpened() const
	{
		return m_Stats.m_FilesOpened;
	}
	uint64_t BytesRead() const
	{
		return m_Stats.m_BytesRead;
	}
	uint64_t BytesWritten() const
	{
		return m_Stats.m_BytesWritten;
	}
	uint64_t IdeBankAllocs() const
	{
		return m_Stats.m_IdeBankAllocs;
	}
	uint64_t IdeBankFrees() const
	{
		return m_Stats.m_IdeBankFrees;
	}
	uint64_t DirOpens() const
	{
		return m_Stats.m_DirOpens;
	}
	uint64_t Rst08Calls() const
	{
		return m_Stats.m_Rst08Calls;
	}
	uint64_t Rst08Unknown() const
	{
		return m_Stats.m_Rst08Unknown;
	}
	uint64_t ReadCyclesCharged() const
	{
		return m_Stats.m_ReadCyclesCharged;
	}

	// Per-handle iteration for dump emit.
	struct HandleSnapshot
	{
		int m_Slot;
		std::string m_OpenedAs;
		std::string m_ResolvedPath;
		long m_Position;
		bool m_bWritable;
	};
	std::vector<HandleSnapshot> SnapshotHandles() const;

	// Everything a resume needs to carry on as the dumped run would: every
	// open file and directory handle, the working directory and drive, the
	// read-only flag, an F_READ still delivering bytes, and the statistics.
	// The mount itself (root, drive letter) is the caller's.
	struct ResumeHandle
	{
		int m_Slot = 0;
		bool m_bDir = false;
		std::string m_ResolvedPath; // host path
		std::string m_OpenedAs;
		bool m_bWritable = false;
		long m_Position = 0;
		std::vector<VfsDirEntry> m_Dir;
		size_t m_DirPos = 0;
	};
	struct ResumeState
	{
		std::string m_Cwd;
		uint8_t m_DefaultDriveId = 0x43;
		bool m_bReadOnly = false;
		bool m_bUseIxForBuffers = false;
		std::vector<ResumeHandle> m_Handles;
		bool m_bFReadActive = false;
		uint16_t m_FReadTotal = 0, m_FReadDone = 0, m_FReadDest = 0;
		bool m_bFReadUseIx = false;
		uint8_t m_FReadIsrDepth = 0;
		uint64_t m_FReadTotalCharge = 0, m_FReadChargedSoFar = 0;
		std::vector<uint8_t> m_FReadBuf;
		uint64_t m_FilesOpened = 0, m_BytesRead = 0, m_BytesWritten = 0;
		uint64_t m_IdeBankAllocs = 0, m_IdeBankFrees = 0, m_DirOpens = 0;
		uint64_t m_Rst08Calls = 0, m_Rst08Unknown = 0, m_ReadCyclesCharged = 0;
		std::map<uint8_t, uint64_t> m_Rst08ByCode;
		std::map<uint8_t, uint64_t> m_Rst08UnknownByCode;
	};
	void ExportResumeState(ResumeState& Out) const;
	// Re-opens every file handle at its position (writable ones for update,
	// so nothing written is lost); false names the first that failed.
	bool ImportResumeState(const ResumeState& In, std::string& OutError);

	// Dump-load helper: restore counters (handles are NOT round-tripped in v1
	// - resume cannot re-open mid-stream files identically). Caller still has
	// to call Mount() separately.
	void RestoreStatsFromDump(uint64_t FilesOp, uint64_t BytesRd, uint64_t BytesWr, uint64_t IdeAlloc, uint64_t IdeFree,
		uint64_t Rst08All, uint64_t Rst08Unk)
	{
		m_Stats.m_FilesOpened = FilesOp;
		m_Stats.m_BytesRead = BytesRd;
		m_Stats.m_BytesWritten = BytesWr;
		m_Stats.m_IdeBankAllocs = IdeAlloc;
		m_Stats.m_IdeBankFrees = IdeFree;
		m_Stats.m_Rst08Calls = Rst08All;
		m_Stats.m_Rst08Unknown = Rst08Unk;
	}

  private:
	uint8_t m_DefaultDriveId = 0x43; // 'C' (esxDOS drive id as games pass it around)
	// esxDOS file APIs.
	void DoFOpen(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoMDosVersion(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoMGetSetDrv(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFStatByName(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFClose(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFRead(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFWrite(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFSync(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFSeek(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFGetPos(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFStat(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFTruncate(Z80State& InOutCpu, MemoryBus& InOutMem, bool bByHandle);

	// Directory + filesystem-mutation APIs (esxDOS $A2..$B1).
	void DoFOpenDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFReadDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFTellDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFSeekDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFRewindDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFGetCwd(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFChDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFMkDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFRmDir(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFUnlink(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFRename(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFGetFree(Z80State& InOutCpu, MemoryBus& InOutMem);
	void DoFChmod(Z80State& InOutCpu, MemoryBus& InOutMem);

	// NextZXOS M_P3DOS sub-APIs (DE selects which).
	void DoMP3Dos(Z80State& InOutCpu, MemoryBus& InOutMem);

	// Translate a NextZXOS path (may have drive letter, may use \ or /)
	// into a host filesystem path. Relative paths join onto Cwd_. Returns ""
	// on rejection (path traversal, wrong drive letter, etc.).
	//   Existing  - the target must exist (bOk = false otherwise);
	//   ForCreate - resolve the parent directory case-insensitively and allow
	//               a missing leaf (for F_OPEN create modes, F_MKDIR, F_RENAME
	//               destination, ...). bOk = false only if the PARENT is bad.
	enum ResolveMode
	{
		kResolveModeExisting,
		kResolveModeForCreate
	};
	std::string ResolveToHostPath(
		const std::string& VfsPath, bool& OutbOk, ResolveMode Mode = ResolveMode::kResolveModeExisting);

	// Current working directory, VFS-relative (empty = mount root). F_CHDIR
	// sets it (validated existing dir); F_GETCWD reports it; relative paths in
	// every API join onto it. Never escapes the mount (".." still rejected).
	std::string m_Cwd;
	bool m_bReadOnly = false;

	// Handle table. Index 0 reserved as "invalid".
	static constexpr int kMaxHandles = 16;
	VfsHandle m_Handles[kMaxHandles];
	int AllocHandle();
	VfsHandle* GetHandle(uint8_t Index); // FILE handles only
	VfsHandle* GetDirHandle(uint8_t Index); // directory handles only
	void ReleaseHandle(uint8_t Index);

	bool m_bMounted = false;
	bool m_bUseIxForBuffers = false; // false = HL (dot-cmd), true = IX (NEX)
	std::string m_RootPath;
	char m_Drive = 'C';

	struct FReadStateImpl
	{
		bool m_bActive = false;
		uint16_t m_Total = 0; // bytes scheduled (= host fread Got)
		uint16_t m_Done = 0; // bytes written to memory so far
		uint16_t m_Dest = 0; // starting dest addr captured at DoFRead time
		bool m_bUseIxOutput = false; // which reg gets the advanced dest at finalise
		uint8_t m_IsrDepthAtStart = 0; // chunker only steps when Cpu.IsrDepth == this
		uint64_t m_TotalCharge = 0; // pre-computed at DoFRead (NR_07 snapshot)
		uint64_t m_ChargedSoFar = 0; // sum of Step charges paid out so far
		std::vector<uint8_t> m_Buf; // bytes already read from host file
	} m_FRead;

	struct
	{
		uint64_t m_FilesOpened = 0;
		uint64_t m_BytesRead = 0;
		uint64_t m_BytesWritten = 0;
		uint64_t m_IdeBankAllocs = 0;
		uint64_t m_IdeBankFrees = 0;
		uint64_t m_DirOpens = 0;
		uint64_t m_Rst08Calls = 0;
		uint64_t m_Rst08Unknown = 0;
		// Total Z80 cycles charged to the running CPU by F_READ to model
		// SD-card transfer time (~5 KB ~ one 50 Hz frame at 3.5 MHz base,
		// scaled by NR_07 turbo). Lets tests assert the I/O-cost model
		// actually fired without depending on derived timing of unrelated
		// counters.
		uint64_t m_ReadCyclesCharged = 0;
		// Per-API-code histograms: every RST 8 call by function code, and
		// the subset inspeccy does not implement (returned as an error to the
		// program). This is what tells a compatibility sweep WHICH NextZXOS
		// service a program wanted.
		std::map<uint8_t, uint64_t> m_Rst08ByCode;
		std::map<uint8_t, uint64_t> m_Rst08UnknownByCode;
	} m_Stats;

  public:
	const std::map<uint8_t, uint64_t>& Rst08ByCode() const
	{
		return m_Stats.m_Rst08ByCode;
	}
	const std::map<uint8_t, uint64_t>& Rst08UnknownByCode() const
	{
		return m_Stats.m_Rst08UnknownByCode;
	}

  private:
};

// esxDOS error codes returned in A on CF=1 (file APIs). These are the CANONICAL
// values from the NextZXOS/esxDOS API document (esxDOS-compatible error codes):
//   0 ok  1 eok  2 nonsense  3 estend  4 ewrtype  5 enoent  6 eio  7 einval
//   8 eacces  9 enospc  10 enxio  11 enodrv  12 enfile  13 ebadf  14 enodev
//   15 eoverflow  16 eisdir  17 enotdir  18 eexist  19 epath  20 esys ...
//   24 erdonly. Most guests branch only on the carry flag, but the ones that
// inspect A (or feed it to M_GETERR for a BASIC report) need the real numbers.
constexpr uint8_t kVfsErrNone = 0; // esx_ok
constexpr uint8_t kVfsErrNoFile = 5; // esx_enoent  (no such file/dir)
constexpr uint8_t kVfsErrGeneric = 6; // esx_eio     (host I/O failure)
constexpr uint8_t kVfsErrInvalid = 7; // esx_einval
constexpr uint8_t kVfsErrAccess = 8; // esx_eacces  (write to read-only file/handle)
constexpr uint8_t kVfsErrDiskFull = 9; // esx_enospc  (no space / write failed)
constexpr uint8_t kVfsErrTooMany = 12; // esx_enfile  (too many open handles)
constexpr uint8_t kVfsErrBadHandle = 13; // esx_ebadf
constexpr uint8_t kVfsErrNotDir = 17; // esx_enotdir (not a directory / no such dir)
constexpr uint8_t kVfsErrExist = 18; // esx_eexist  (create-new on existing)
constexpr uint8_t kVfsErrReadOnly = 24; // esx_erdonly (mount is read-only)
