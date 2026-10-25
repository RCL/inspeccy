#include "nextzxos.h"

#include "memory.h"
#include "z80.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <string>

namespace
{

// esxDOS API codes used by real dot commands.
constexpr uint8_t kApiMDosversion = 0x88; // NextZXOS/esxDOS version query
constexpr uint8_t kApiMGetsetdrv = 0x89; // get/set default drive
constexpr uint8_t kApiFStat = 0xAC; // stat by name (F_FSTAT is by handle)
constexpr uint8_t kApiFOpen = 0x9A;
constexpr uint8_t kApiFClose = 0x9B;
constexpr uint8_t kApiFSync = 0x9C;
constexpr uint8_t kApiFRead = 0x9D;
constexpr uint8_t kApiFWrite = 0x9E;
constexpr uint8_t kApiFSeek = 0x9F;
constexpr uint8_t kApiFFgetpos = 0xA0;
constexpr uint8_t kApiFFstat = 0xA1;
constexpr uint8_t kApiFFtruncate = 0xA2;
constexpr uint8_t kApiFOpendir = 0xA3;
constexpr uint8_t kApiFReaddir = 0xA4;
constexpr uint8_t kApiFTelldir = 0xA5;
constexpr uint8_t kApiFSeekdir = 0xA6;
constexpr uint8_t kApiFRewinddir = 0xA7;
constexpr uint8_t kApiFGetcwd = 0xA8;
constexpr uint8_t kApiFChdir = 0xA9;
constexpr uint8_t kApiFMkdir = 0xAA;
constexpr uint8_t kApiFRmdir = 0xAB;
constexpr uint8_t kApiFUnlink = 0xAD;
constexpr uint8_t kApiFTruncate = 0xAE;
constexpr uint8_t kApiFChmod = 0xAF;
constexpr uint8_t kApiFRename = 0xB0;
constexpr uint8_t kApiFGetfree = 0xB1;
constexpr uint8_t kApiMP3dos = 0x94;

// esxDOS F_OPEN mode bits (esxdos.inc esx_mode_*).
constexpr uint8_t kEsxModeRead = 0x01;
constexpr uint8_t kEsxModeWrite = 0x02;
constexpr uint8_t kEsxModeOpenExist = 0x00; // open, must exist
constexpr uint8_t kEsxModeCreatNew = 0x04; // create, error if exists
constexpr uint8_t kEsxModeOpenCreat = 0x08; // open, create if missing
constexpr uint8_t kEsxModeCreatTrunc = 0x0C; // create/truncate
constexpr uint8_t kEsxModeCreateMask = 0x0C; // bits 2..3 select the create policy

// M_P3DOS function code for IDE_BANK.
constexpr uint16_t kMp3dIdeBank = 0x01BD;
constexpr uint8_t kRcBanktypeZx = 0; // 16 KB bank
constexpr uint8_t kRcBankAlloc = 1;
constexpr uint8_t kRcBankFree = 3;

inline void SetCF(Z80State& InOutCpu)
{
	InOutCpu.SetF(uint8_t(InOutCpu.F() | kFlagC));
}
inline void ClearCF(Z80State& InOutCpu)
{
	InOutCpu.SetF(uint8_t(InOutCpu.F() & ~kFlagC));
}

// Read a NUL-terminated string from emulator memory at Addr (max Limit bytes).
std::string ReadCString(const MemoryBus& Mem, uint16_t Addr, size_t Limit = 256)
{
	std::string Out;
	Out.reserve(64);
	for (size_t IdxI = 0; IdxI < Limit; ++IdxI)
	{
		const uint8_t C = Mem.Rd(uint16_t(Addr + IdxI));
		if (C == 0)
		{
			break;
		}
		if (C == 13)
		{
			break; // CR terminator (per ParseArg conventions)
		}
		if (C >= 128)
		{
			break; // high-bit-set marker (NextZXOS BASIC quirk)
		}
		Out += char(C);
	}
	return Out;
}

#if defined(_WIN32)
constexpr bool kPlatformCaseInsensitive = true;
#else
constexpr bool kPlatformCaseInsensitive = false;
#endif

// Walk one path segment with case-insensitive matching against the host
// directory's actual contents. Returns "" if no match.
std::string FindCaseInsensitive(const std::filesystem::path& Dir, const std::string& Name)
{
	std::error_code Ec;
	for (std::filesystem::directory_iterator It(Dir, Ec), End; !Ec && It != End; It.increment(Ec))
	// Use error-code iteration: unreadable host directories are VFS failures.
	{
		const std::string EntryName = It->path().filename().string();
		if (EntryName.size() != Name.size())
		{
			continue;
		}
		bool bMatch = true;
		for (size_t IdxI = 0; IdxI < Name.size(); ++IdxI)
		{
			if (tolower(uint8_t(EntryName[IdxI])) != tolower(uint8_t(Name[IdxI])))
			{
				bMatch = false;
				break;
			}
		}
		if (bMatch)
		{
			return EntryName;
		}
	}
	return "";
}

// Walk RelPath (already drive-stripped, '/'-separated, no "..") segment by
// segment under Root, matching case-insensitively when the host FS is
// case-sensitive. Returns the resolved host path if every segment exists, else
// "". An empty RelPath resolves to Root itself.
std::string WalkExisting(const std::string& Root, const std::string& RelPath, bool bHostCaseInsensitive)
{
	namespace fs = std::filesystem;
	std::error_code Ec;
	if (RelPath.empty())
	{
		return fs::exists(fs::path(Root), Ec) ? Root : std::string();
	}
	fs::path Direct = fs::path(Root) / RelPath;
	if (fs::exists(Direct, Ec))
	{
		return Direct.generic_string();
	}
	if (bHostCaseInsensitive)
	{
		return ""; // OS already case-folds; a miss is real
	}
	fs::path Cur(Root);
	size_t Pos = 0;
	while (Pos < RelPath.size())
	{
		const size_t NextSlash = RelPath.find('/', Pos);
		const std::string Seg =
			RelPath.substr(Pos, (NextSlash == std::string::npos ? RelPath.size() : NextSlash) - Pos);
		if (!Seg.empty() && Seg != ".")
		{
			const std::string Match = FindCaseInsensitive(Cur, Seg);
			if (Match.empty())
			{
				return "";
			}
			Cur /= Match;
		}
		if (NextSlash == std::string::npos)
		{
			break;
		}
		Pos = NextSlash + 1;
	}
	return fs::exists(Cur, Ec) ? Cur.generic_string() : std::string();
}

// Resolve links before comparing components. A lexical prefix alone does not
// constrain a symlink or Windows junction to the mounted directory.
bool IsInsideRoot(const std::string& Root, const std::string& Host)
{
	namespace fs = std::filesystem;
	std::error_code Ec;
	fs::path Candidate = fs::canonical(Host, Ec);
	if (Ec)
	{
		// Resolve the existing parent for creation; canonical() requires the
		// leaf to exist. Do not treat a dangling link as a missing leaf.
		const auto Status = fs::symlink_status(Host, Ec);
		if (Status.type() != fs::file_type::not_found)
		{
			return false;
		}
		Candidate = fs::canonical(fs::path(Host).parent_path(), Ec);
		if (Ec)
		{
			return false;
		}
		Candidate /= fs::path(Host).filename();
	}
	const fs::path RootPath(Root);
	auto Part = Candidate.begin();
	for (const auto& RootPart : RootPath)
	{
		if (Part == Candidate.end())
		{
			return false;
		}
		std::string Expected = RootPart.generic_string();
		std::string Actual = Part->generic_string();
		if (kPlatformCaseInsensitive)
		{
			for (char& C : Expected)
			{
				C = char(tolower(static_cast<unsigned char>(C)));
			}
			for (char& C : Actual)
			{
				C = char(tolower(static_cast<unsigned char>(C)));
			}
		}
		if (Expected != Actual)
		{
			return false;
		}
		++Part;
	}
	return true;
}

// Split "a/b/leaf" into ("a/b", "leaf"); a leaf-only path gives ("", "leaf").
void SplitParentLeaf(const std::string& Rel, std::string& OutParent, std::string& OutLeaf)
{
	const size_t Slash = Rel.find_last_of('/');
	if (Slash == std::string::npos)
	{
		OutParent.clear();
		OutLeaf = Rel;
	}
	else
	{
		OutParent = Rel.substr(0, Slash);
		OutLeaf = Rel.substr(Slash + 1);
	}
}

} // namespace

NextZxosVfs::NextZxosVfs()
{
	for (int IdxI = 0; IdxI < kMaxHandles; ++IdxI)
	{
		m_Handles[IdxI] = {};
	}
}

std::vector<NextZxosVfs::HandleSnapshot> NextZxosVfs::SnapshotHandles() const
{
	std::vector<HandleSnapshot> Out;
	for (int IdxI = 1; IdxI < kMaxHandles; ++IdxI)
	{
		const VfsHandle& H = m_Handles[IdxI];
		if (H.m_File == nullptr)
		{
			continue;
		}
		HandleSnapshot S;
		S.m_Slot = IdxI;
		S.m_OpenedAs = H.m_OpenedAs;
		S.m_ResolvedPath = H.m_ResolvedPath;
		S.m_Position = ftell(H.m_File);
		S.m_bWritable = H.m_bWritable;
		Out.push_back(std::move(S));
	}
	return Out;
}

bool NextZxosVfs::RegisterPreOpenedHandle(
	int Slot, FILE* File, const std::string& OpenedAs, const std::string& ResolvedPath)
{
	if (Slot <= 0 || Slot >= kMaxHandles)
	{
		return false;
	}
	if (m_Handles[Slot].m_File != nullptr)
	{
		return false;
	}
	m_Handles[Slot].m_File = File;
	m_Handles[Slot].m_ResolvedPath = ResolvedPath;
	m_Handles[Slot].m_OpenedAs = OpenedAs;
	m_Handles[Slot].m_bWritable = false;
	++m_Stats.m_FilesOpened;
	return true;
}

void NextZxosVfs::ExportResumeState(ResumeState& Out) const
{
	Out.m_Cwd = m_Cwd;
	Out.m_DefaultDriveId = m_DefaultDriveId;
	Out.m_bReadOnly = m_bReadOnly;
	Out.m_bUseIxForBuffers = m_bUseIxForBuffers;
	Out.m_Handles.clear();
	for (int IdxI = 1; IdxI < kMaxHandles; ++IdxI)
	{
		const VfsHandle& H = m_Handles[IdxI];
		if (!H.InUse())
		{
			continue;
		}
		ResumeHandle R;
		R.m_Slot = IdxI;
		R.m_bDir = H.m_bDir;
		R.m_ResolvedPath = H.m_ResolvedPath;
		R.m_OpenedAs = H.m_OpenedAs;
		R.m_bWritable = H.m_bWritable;
		R.m_Position = H.m_File ? ftell(H.m_File) : 0;
		R.m_Dir = H.m_Dir;
		R.m_DirPos = H.m_DirPos;
		Out.m_Handles.push_back(std::move(R));
	}
	Out.m_bFReadActive = m_FRead.m_bActive;
	Out.m_FReadTotal = m_FRead.m_Total;
	Out.m_FReadDone = m_FRead.m_Done;
	Out.m_FReadDest = m_FRead.m_Dest;
	Out.m_bFReadUseIx = m_FRead.m_bUseIxOutput;
	Out.m_FReadIsrDepth = m_FRead.m_IsrDepthAtStart;
	Out.m_FReadTotalCharge = m_FRead.m_TotalCharge;
	Out.m_FReadChargedSoFar = m_FRead.m_ChargedSoFar;
	Out.m_FReadBuf = m_FRead.m_Buf;
	Out.m_FilesOpened = m_Stats.m_FilesOpened;
	Out.m_BytesRead = m_Stats.m_BytesRead;
	Out.m_BytesWritten = m_Stats.m_BytesWritten;
	Out.m_IdeBankAllocs = m_Stats.m_IdeBankAllocs;
	Out.m_IdeBankFrees = m_Stats.m_IdeBankFrees;
	Out.m_DirOpens = m_Stats.m_DirOpens;
	Out.m_Rst08Calls = m_Stats.m_Rst08Calls;
	Out.m_Rst08Unknown = m_Stats.m_Rst08Unknown;
	Out.m_ReadCyclesCharged = m_Stats.m_ReadCyclesCharged;
	Out.m_Rst08ByCode = m_Stats.m_Rst08ByCode;
	Out.m_Rst08UnknownByCode = m_Stats.m_Rst08UnknownByCode;
}

bool NextZxosVfs::ImportResumeState(const ResumeState& In, std::string& OutError)
{
	for (VfsHandle& H : m_Handles)
	{
		if (H.m_File != nullptr)
		{
			fclose(H.m_File);
		}
		H = VfsHandle{};
	}
	for (const ResumeHandle& R : In.m_Handles)
	{
		if (R.m_Slot <= 0 || R.m_Slot >= kMaxHandles)
		{
			OutError = "vfs handle slot out of range";
			return false;
		}
		VfsHandle& H = m_Handles[R.m_Slot];
		H.m_ResolvedPath = R.m_ResolvedPath;
		H.m_OpenedAs = R.m_OpenedAs;
		H.m_bWritable = R.m_bWritable;
		H.m_bDir = R.m_bDir;
		H.m_Dir = R.m_Dir;
		H.m_DirPos = R.m_DirPos;
		if (R.m_bDir)
		{
			continue;
		}
		// "r+b" keeps what the first half wrote and lets writes continue.
		H.m_File = fopen(R.m_ResolvedPath.c_str(), R.m_bWritable ? "r+b" : "rb");
		if (H.m_File == nullptr || fseek(H.m_File, R.m_Position, SEEK_SET) != 0)
		{
			OutError = "cannot re-open VFS handle " + std::to_string(R.m_Slot) + ": " + R.m_ResolvedPath;
			if (H.m_File != nullptr)
			{
				fclose(H.m_File);
			}
			H = VfsHandle{};
			return false;
		}
	}
	m_Cwd = In.m_Cwd;
	m_DefaultDriveId = In.m_DefaultDriveId;
	m_bReadOnly = In.m_bReadOnly;
	m_bUseIxForBuffers = In.m_bUseIxForBuffers;
	m_FRead.m_bActive = In.m_bFReadActive;
	m_FRead.m_Total = In.m_FReadTotal;
	m_FRead.m_Done = In.m_FReadDone;
	m_FRead.m_Dest = In.m_FReadDest;
	m_FRead.m_bUseIxOutput = In.m_bFReadUseIx;
	m_FRead.m_IsrDepthAtStart = In.m_FReadIsrDepth;
	m_FRead.m_TotalCharge = In.m_FReadTotalCharge;
	m_FRead.m_ChargedSoFar = In.m_FReadChargedSoFar;
	m_FRead.m_Buf = In.m_FReadBuf;
	m_Stats.m_FilesOpened = In.m_FilesOpened;
	m_Stats.m_BytesRead = In.m_BytesRead;
	m_Stats.m_BytesWritten = In.m_BytesWritten;
	m_Stats.m_IdeBankAllocs = In.m_IdeBankAllocs;
	m_Stats.m_IdeBankFrees = In.m_IdeBankFrees;
	m_Stats.m_DirOpens = In.m_DirOpens;
	m_Stats.m_Rst08Calls = In.m_Rst08Calls;
	m_Stats.m_Rst08Unknown = In.m_Rst08Unknown;
	m_Stats.m_ReadCyclesCharged = In.m_ReadCyclesCharged;
	m_Stats.m_Rst08ByCode = In.m_Rst08ByCode;
	m_Stats.m_Rst08UnknownByCode = In.m_Rst08UnknownByCode;
	return true;
}

bool NextZxosVfs::Mount(const char* Path, char DriveLetter, std::string& OutError)
{
	namespace fs = std::filesystem;
	std::error_code Ec;
	fs::path P(Path);
	if (!fs::exists(P, Ec) || !fs::is_directory(P, Ec))
	{
		OutError = std::string("--vfs path is not a directory: ") + Path;
		return false;
	}
	const fs::path Root = fs::canonical(P, Ec);
	if (Ec)
	{
		OutError = "cannot resolve VFS root: " + Ec.message();
		return false;
	}
	m_RootPath = Root.generic_string();
	m_Drive = char(toupper(uint8_t(DriveLetter)));
	m_bMounted = true;
	return true;
}

int NextZxosVfs::AllocHandle()
{
	for (int IdxI = 1; IdxI < kMaxHandles; ++IdxI)
	{
		if (!m_Handles[IdxI].InUse())
		{
			return IdxI;
		}
	}
	return 0;
}

VfsHandle* NextZxosVfs::GetHandle(uint8_t Index)
{
	if (Index == 0 || Index >= kMaxHandles)
	{
		return nullptr;
	}
	VfsHandle& H = m_Handles[Index];
	if (H.m_File == nullptr) // dir handles (File == nullptr) are not file handles
	{
		return nullptr;
	}
	return &H;
}

VfsHandle* NextZxosVfs::GetDirHandle(uint8_t Index)
{
	if (Index == 0 || Index >= kMaxHandles)
	{
		return nullptr;
	}
	VfsHandle& H = m_Handles[Index];
	return H.m_bDir ? &H : nullptr;
}

void NextZxosVfs::ReleaseHandle(uint8_t Index)
{
	if (Index == 0 || Index >= kMaxHandles)
	{
		return;
	}
	VfsHandle& H = m_Handles[Index];
	if (H.m_File)
	{
		fclose(H.m_File);
		H.m_File = nullptr;
	}
	H.m_ResolvedPath.clear();
	H.m_OpenedAs.clear();
	H.m_bWritable = false;
	H.m_bDir = false;
	H.m_Dir.clear();
	H.m_DirPos = 0;
}

std::string NextZxosVfs::ResolveToHostPath(const std::string& VfsPath, bool& OutbOk, ResolveMode Mode)
{
	OutbOk = false;
	if (!m_bMounted)
	{
		return "";
	}

	// Strip drive prefix ("C:", "*:", "*"+nothing handled by caller passing
	// bare names). Accept the configured drive letter and the '*' wildcard.
	std::string Path = VfsPath;
	bool bDriveAbsolute = false;
	if (Path.size() >= 2 && Path[1] == ':')
	{
		const char Letter = char(toupper(uint8_t(Path[0])));
		if (Letter != m_Drive && Path[0] != '*')
		{
			return "";
		}
		Path.erase(0, 2);
		bDriveAbsolute = true; // "C:foo" is relative to root, not to the cwd
	}

	// Convert backslashes to forward slashes.
	for (char& C : Path)
	{
		if (C == ':' || C == '\0')
		{
			return ""; // No host drive prefixes or NTFS alternate streams.
		}
		if (C == '\\')
		{
			C = '/';
		}
	}

	// A leading slash (or a drive prefix) means "from the mount root"; anything
	// else is relative to the current working directory.
	const bool bRootAbsolute = bDriveAbsolute || (!Path.empty() && Path.front() == '/');
	while (!Path.empty() && Path.front() == '/')
	{
		Path.erase(0, 1);
	}
	if (!bRootAbsolute && !m_Cwd.empty())
	{
		Path = Path.empty() ? m_Cwd : (m_Cwd + "/" + Path);
	}

	// Reject ".." segments for traversal safety.
	{
		size_t Pos = 0;
		while (Pos < Path.size())
		{
			const size_t NextSlash = Path.find('/', Pos);
			const std::string Seg = Path.substr(Pos, (NextSlash == std::string::npos ? Path.size() : NextSlash) - Pos);
			if (Seg == "..")
			{
				return "";
			}
			if (NextSlash == std::string::npos)
			{
				break;
			}
			Pos = NextSlash + 1;
		}
	}

	if (Mode == ResolveMode::kResolveModeExisting)
	{
		const std::string Host = WalkExisting(m_RootPath, Path, kPlatformCaseInsensitive);
		if (!Host.empty() && IsInsideRoot(m_RootPath, Host))
		{
			OutbOk = true;
			return Host;
		}
		// Not found: hand back the direct join so a caller's diagnostic (and a
		// subsequent fopen) reports a sensible host path.
		return (std::filesystem::path(m_RootPath) / Path).generic_string();
	}

	// ForCreate: the parent directory must already resolve; the leaf may be new.
	std::string Parent, Leaf;
	SplitParentLeaf(Path, Parent, Leaf);
	if (Leaf.empty() || Leaf == ".")
	{
		return ""; // no file component to create
	}
	const std::string ParentHost = WalkExisting(m_RootPath, Parent, kPlatformCaseInsensitive);
	if (ParentHost.empty())
	{
		return ""; // parent directory does not exist -> caller returns NOT_DIR/NO_FILE
	}
	// Reuse an existing leaf's real casing if present (so a re-open matches).
	const std::string LeafHost = WalkExisting(ParentHost, Leaf, kPlatformCaseInsensitive);
	const std::string Host = LeafHost.empty() ? (std::filesystem::path(ParentHost) / Leaf).generic_string() : LeafHost;
	if (!IsInsideRoot(m_RootPath, Host))
	{
		return "";
	}
	OutbOk = true;
	return Host;
}

// ============================ RST $08 dispatch ============================

void NextZxosVfs::HandleRst08(uint8_t Api, Z80State& InOutCpu, MemoryBus& InOutMem)
{
	++m_Stats.m_Rst08Calls;
	++m_Stats.m_Rst08ByCode[Api];
	switch (Api)
	{
	case kApiFOpen:
		DoFOpen(InOutCpu, InOutMem);
		break;
	case kApiFClose:
		DoFClose(InOutCpu, InOutMem);
		break;
	case kApiFSync:
		DoFSync(InOutCpu, InOutMem);
		break;
	case kApiFRead:
		DoFRead(InOutCpu, InOutMem);
		break;
	case kApiFWrite:
		DoFWrite(InOutCpu, InOutMem);
		break;
	case kApiFSeek:
		DoFSeek(InOutCpu, InOutMem);
		break;
	case kApiFFgetpos:
		DoFGetPos(InOutCpu, InOutMem);
		break;
	case kApiFFstat:
		DoFStat(InOutCpu, InOutMem);
		break;
	case kApiFFtruncate:
		DoFTruncate(InOutCpu, InOutMem, /*bByHandle=*/true);
		break;
	case kApiFTruncate:
		DoFTruncate(InOutCpu, InOutMem, /*bByHandle=*/false);
		break;
	case kApiFOpendir:
		DoFOpenDir(InOutCpu, InOutMem);
		break;
	case kApiFReaddir:
		DoFReadDir(InOutCpu, InOutMem);
		break;
	case kApiFTelldir:
		DoFTellDir(InOutCpu, InOutMem);
		break;
	case kApiFSeekdir:
		DoFSeekDir(InOutCpu, InOutMem);
		break;
	case kApiFRewinddir:
		DoFRewindDir(InOutCpu, InOutMem);
		break;
	case kApiFGetcwd:
		DoFGetCwd(InOutCpu, InOutMem);
		break;
	case kApiFChdir:
		DoFChDir(InOutCpu, InOutMem);
		break;
	case kApiFMkdir:
		DoFMkDir(InOutCpu, InOutMem);
		break;
	case kApiFRmdir:
		DoFRmDir(InOutCpu, InOutMem);
		break;
	case kApiFUnlink:
		DoFUnlink(InOutCpu, InOutMem);
		break;
	case kApiFRename:
		DoFRename(InOutCpu, InOutMem);
		break;
	case kApiFGetfree:
		DoFGetFree(InOutCpu, InOutMem);
		break;
	case kApiFChmod:
		DoFChmod(InOutCpu, InOutMem);
		break;
	case kApiMP3dos:
		DoMP3Dos(InOutCpu, InOutMem);
		break;
	case kApiMDosversion:
		DoMDosVersion(InOutCpu, InOutMem);
		break;
	case kApiMGetsetdrv:
		DoMGetSetDrv(InOutCpu, InOutMem);
		break;
	case kApiFStat:
		DoFStatByName(InOutCpu, InOutMem);
		break;
	default:
		++m_Stats.m_Rst08Unknown;
		++m_Stats.m_Rst08UnknownByCode[Api];
		InOutCpu.SetA(kVfsErrInvalid);
		SetCF(InOutCpu);
		break;
	}
	// Sweep diagnostics: every API call that returns an error, with its inputs.
	// (esxDOS convention: Fc=1 on error, A = error code.) F_READ is delivered
	// time-sliced - FinaliseFRead sets its flags later - so the caller's stale
	// carry means nothing while a read is in flight.
	if ((InOutCpu.F() & kFlagC) && !m_FRead.m_bActive)
	{
		fprintf(stderr, "vfs: API $%02X -> error A=$%02X (BC=$%04X DE=$%04X HL=$%04X IX=$%04X)\n", Api,
			unsigned(InOutCpu.A()), unsigned(InOutCpu.BC), unsigned(InOutCpu.DE), unsigned(InOutCpu.HL),
			unsigned(InOutCpu.IX));
	}
}

// ============================ esxDOS file APIs ============================

// esxDOS/NextZXOS pass the filename pointer in IX for ordinary programs and in
// HL only from inside a dot command. Take whichever
// holds a plausible ASCIIZ name.
static bool PlausibleName(const std::string& N)
{
	if (N.empty() || N.size() > 260)
	{
		return false;
	}
	for (unsigned char C : N)
	{
		if (C < 0x20 || C > 0x7E)
		{
			return false;
		}
	}
	return true;
}
static std::string ReadNameHLorIX(MemoryBus& InOutMem, Z80State& InOutCpu, const char** Which)
{
	std::string N = ReadCString(InOutMem, InOutCpu.HL);
	if (PlausibleName(N))
	{
		*Which = "HL";
		return N;
	}
	N = ReadCString(InOutMem, InOutCpu.IX);
	*Which = "IX";
	return PlausibleName(N) ? N : std::string();
}

void NextZxosVfs::DoFOpen(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = drive ('*' = wildcard), B = mode, HL = filename ptr (NUL-term).
	const uint8_t Mode = uint8_t(InOutCpu.BC >> 8);
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	if (Name.empty())
	{
		fprintf(stderr, "vfs: F_OPEN with no plausible name in HL or IX -> NO_FILE\n");
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}

	const bool bWrite = (Mode & kEsxModeWrite) != 0;
	const uint8_t Create = Mode & kEsxModeCreateMask; // $00/$04/$08/$0C
	const bool bMayCreate = (Create != kEsxModeOpenExist);

	// The mount can be write-locked (sweep mode) - refuse anything that would
	// touch the host filesystem before resolving.
	if (m_bReadOnly && (bWrite || bMayCreate))
	{
		fprintf(stderr, "vfs: F_OPEN '%s' mode=$%02X -> READ_ONLY (mount is read-only)\n", Name.c_str(), Mode);
		InOutCpu.SetA(kVfsErrReadOnly);
		SetCF(InOutCpu);
		return;
	}

	bool bResolveOk = false;
	const NextZxosVfs::ResolveMode RMode =
		bMayCreate ? ResolveMode::kResolveModeForCreate : ResolveMode::kResolveModeExisting;
	const std::string HostPath = ResolveToHostPath(Name, bResolveOk, RMode);
	if (!bResolveOk)
	{
		fprintf(stderr, "vfs: F_OPEN '%s' mode=$%02X -> NO_FILE (path does not resolve under the mount)\n",
			Name.c_str(), Mode);
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}

	std::error_code Ec;
	const bool bExists = std::filesystem::exists(HostPath, Ec);

	// Map the esxDOS create policy to a stdio open mode.
	const char* FMode = nullptr;
	if (!bWrite && !bMayCreate)
	{
		FMode = "rb"; // plain read (must exist)
	}
	else if (Create == kEsxModeCreatNew)
	{
		if (bExists) // create-new but the file is there
		{
			fprintf(stderr, "vfs: F_OPEN '%s' -> EXIST (creat_noexist on existing file)\n", Name.c_str());
			InOutCpu.SetA(kVfsErrExist);
			SetCF(InOutCpu);
			return;
		}
		FMode = "w+b";
	}
	else if (Create == kEsxModeCreatTrunc)
	{
		FMode = "w+b"; // create / truncate
	}
	else if (Create == kEsxModeOpenCreat)
	{
		FMode = bExists ? "r+b" : "w+b"; // open if present else create
	}
	else // OPEN_EXIST with write
	{
		FMode = "r+b"; // must exist, read/write in place
	}

	FILE* F = fopen(HostPath.c_str(), FMode);
	if (!F)
	{
		fprintf(stderr, "vfs: F_OPEN '%s' mode=$%02X (%s) -> NO_FILE (host: %s)\n", Name.c_str(), Mode, FMode,
			HostPath.c_str());
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}

	const int Slot = AllocHandle();
	if (Slot == 0)
	{
		fclose(F);
		InOutCpu.SetA(kVfsErrTooMany);
		SetCF(InOutCpu);
		return;
	}

	m_Handles[Slot].m_File = F;
	m_Handles[Slot].m_ResolvedPath = HostPath;
	m_Handles[Slot].m_OpenedAs = Name;
	m_Handles[Slot].m_bWritable = bWrite || bMayCreate;
	++m_Stats.m_FilesOpened;

	InOutCpu.SetA(uint8_t(Slot));
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFClose(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	const uint8_t Handle = uint8_t(InOutCpu.A());
	if (!GetHandle(Handle) && !GetDirHandle(Handle)) // F_CLOSE closes files or dirs
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	ReleaseHandle(Handle);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFRead(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const uint16_t Count = InOutCpu.BC;
	const uint16_t Dest = m_bUseIxForBuffers ? InOutCpu.IX : InOutCpu.HL;

	// Read the host file synchronously - only the *memory transfer* + cycle
	// charge gets time-sliced. Real hw spreads both the SD read and the
	// byte writes over Z80-instruction time; we approximate by spreading
	// the writes only (the host fread is essentially instant).
	m_FRead.m_Buf.assign(Count, 0);
	const size_t Got = (Count > 0) ? fread(m_FRead.m_Buf.data(), 1, Count, H->m_File) : 0;
	m_FRead.m_Buf.resize(Got);

	// SD-card transfer time - empirical anchor: 28 MHz turbo, ~5 KB ~ one
	// 50 Hz frame (= 560 000 Z80 cyc). The formula
	// `(Got * 70000) << NR_07[1:0] / 5120` matches the legacy single-shot
	// charge; the dispatcher's chunker pays it out proportionally so the
	// cumulative charge after Done == Total equals this total exactly.
	const uint8_t Nr07 = InOutMem.GetNextReg(0x07);
	const uint64_t TurboShift = uint64_t(Nr07 & 0x03);
	const uint64_t TotalCharge = ((uint64_t(Got) * 70000ull) << TurboShift) / 5120ull;

	if (Got == 0)
	{
		// EOF semantics: CF=0, BC=0, DE=0 (per esxDOS). Finalise inline -
		// there's nothing to time-slice.
		InOutCpu.BC = 0;
		InOutCpu.DE = 0;
		ClearCF(InOutCpu);
		m_FRead.m_bActive = false;
		return;
	}

	m_FRead.m_bActive = true;
	m_FRead.m_Total = uint16_t(Got);
	m_FRead.m_Done = 0;
	m_FRead.m_Dest = Dest;
	m_FRead.m_bUseIxOutput = m_bUseIxForBuffers;
	m_FRead.m_IsrDepthAtStart = InOutCpu.m_IsrDepth;
	m_FRead.m_TotalCharge = TotalCharge;
	m_FRead.m_ChargedSoFar = 0;

	m_Stats.m_BytesRead += Got;
}

uint32_t NextZxosVfs::StepFRead(MemoryBus& InOutMem, uint32_t MaxBytes, uint64_t& OutCharge)
{
	OutCharge = 0;
	if (!m_FRead.m_bActive)
	{
		return 0;
	}
	const uint32_t Remaining = uint32_t(m_FRead.m_Total - m_FRead.m_Done);
	const uint32_t Step = (MaxBytes < Remaining) ? MaxBytes : Remaining;
	for (uint32_t IdxI = 0; IdxI < Step; ++IdxI)
	{
		InOutMem.Wr(uint16_t(m_FRead.m_Dest + m_FRead.m_Done + IdxI), m_FRead.m_Buf[m_FRead.m_Done + IdxI]);
	}
	m_FRead.m_Done = uint16_t(m_FRead.m_Done + Step);
	// Proportional charge: total after Done bytes = Done * TotalCharge / Total.
	// Subtract what we've already paid out to get this step's charge.
	const uint64_t ChargedAfter = (uint64_t(m_FRead.m_Done) * m_FRead.m_TotalCharge) / m_FRead.m_Total;
	OutCharge = ChargedAfter - m_FRead.m_ChargedSoFar;
	m_FRead.m_ChargedSoFar = ChargedAfter;
	m_Stats.m_ReadCyclesCharged += OutCharge;
	return Step;
}

void NextZxosVfs::FinaliseFRead(Z80State& InOutCpu)
{
	if (!m_FRead.m_bActive)
	{
		return;
	}
	InOutCpu.BC = m_FRead.m_Total;
	if (m_FRead.m_bUseIxOutput)
	{
		InOutCpu.IX = uint16_t(m_FRead.m_Dest + m_FRead.m_Total);
	}
	else
	{
		InOutCpu.HL = uint16_t(m_FRead.m_Dest + m_FRead.m_Total);
	}
	ClearCF(InOutCpu);
	m_FRead.m_bActive = false;
	m_FRead.m_Buf.clear();
	m_FRead.m_Buf.shrink_to_fit();
}

void NextZxosVfs::DoFSeek(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const uint32_t Offset = (uint32_t(InOutCpu.BC) << 16) | uint32_t(InOutCpu.DE);
	// Mode arrives in IXL from a program, in L only from a dot command (same
	// split as the F_OPEN name and F_READ buffer). Prefer IXL when it is a
	// valid mode, else L.
	const uint8_t IxMode = uint8_t(InOutCpu.IX & 0xFF);
	const uint8_t Mode = (IxMode <= 2) ? IxMode : uint8_t(InOutCpu.HL & 0xFF);
	int Origin;
	switch (Mode)
	{
	case 0:
		Origin = SEEK_SET;
		break;
	case 1:
		Origin = SEEK_CUR;
		break;
	case 2:
		Origin = SEEK_END;
		break;
	default:
		InOutCpu.SetA(kVfsErrInvalid);
		SetCF(InOutCpu);
		return;
	}
	if (fseek(H->m_File, long(Offset), Origin) != 0)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

// M_DOSVERSION ($88): identify the DOS. NextZXOS answers Fc=0, B='N' C='X',
// DE = BCD NextZXOS version (2.08), HL = 0; A = 0. Games use it to refuse to
// run on plain esxDOS or on too-old NextZXOS.
void NextZxosVfs::DoMDosVersion(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	InOutCpu.SetA(0);
	InOutCpu.BC = 0x4E58; // 'N','X'
	InOutCpu.DE = 0x0208; // NextZXOS 2.08
	InOutCpu.HL = 0x0000;
	ClearCF(InOutCpu);
}

// M_GETSETDRV ($89): A=0 -> get default drive (returns A = drive id), else set
// it. We have one mounted drive; report it and accept any set.
void NextZxosVfs::DoMGetSetDrv(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	const uint8_t Want = uint8_t(InOutCpu.A());
	if (Want != 0)
	{
		m_DefaultDriveId = Want;
	}
	InOutCpu.SetA(m_DefaultDriveId);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFGetPos(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const long Pos = ftell(H->m_File);
	if (Pos < 0)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	InOutCpu.BC = uint16_t(uint32_t(Pos) >> 16);
	InOutCpu.DE = uint16_t(uint32_t(Pos) & 0xFFFF);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFStat(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = handle, HL = stat buffer pointer.
	// Bytes 0..6: drive/device/attributes/date/time (zero-filled).
	// Bytes 7..10: file size, little-endian.
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const long Cur = ftell(H->m_File);
	fseek(H->m_File, 0, SEEK_END);
	const long Size = ftell(H->m_File);
	fseek(H->m_File, Cur, SEEK_SET);
	if (Size < 0)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	const uint16_t Buf = m_bUseIxForBuffers ? InOutCpu.IX : InOutCpu.HL; // IX from a program, HL from a dot command
	for (int IdxI = 0; IdxI < 11; ++IdxI)
	{
		InOutMem.Wr(uint16_t(Buf + IdxI), 0);
	}
	InOutMem.Wr(uint16_t(Buf + 7), uint8_t(uint32_t(Size) & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 8), uint8_t((uint32_t(Size) >> 8) & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 9), uint8_t((uint32_t(Size) >> 16) & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 10), uint8_t((uint32_t(Size) >> 24) & 0xFF));
	ClearCF(InOutCpu);
}

// Write an ASCIIZ string to guest memory (name buffers, F_GETCWD).
static void WriteCString(MemoryBus& InOutMem, uint16_t Addr, const std::string& S, size_t Limit = 256)
{
	size_t IdxI = 0;
	for (; IdxI < S.size() && IdxI + 1 < Limit; ++IdxI)
	{
		InOutMem.Wr(uint16_t(Addr + IdxI), uint8_t(S[IdxI]));
	}
	InOutMem.Wr(uint16_t(Addr + IdxI), 0);
}

void NextZxosVfs::DoFWrite(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = handle, BC = byte count, IX/HL = source buffer.
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	if (!H->m_bWritable)
	{
		fprintf(stderr, "vfs: F_WRITE on read-only handle %u -> ACCESS\n", Handle);
		InOutCpu.SetA(kVfsErrAccess);
		SetCF(InOutCpu);
		return;
	}
	const uint16_t Count = InOutCpu.BC;
	const uint16_t Src = m_bUseIxForBuffers ? InOutCpu.IX : InOutCpu.HL;
	std::vector<uint8_t> Buf(Count);
	for (uint16_t IdxI = 0; IdxI < Count; ++IdxI)
	{
		Buf[IdxI] = InOutMem.Rd(uint16_t(Src + IdxI));
	}
	const size_t Wrote = (Count > 0) ? fwrite(Buf.data(), 1, Count, H->m_File) : 0;
	m_Stats.m_BytesWritten += Wrote;
	// Return: BC = bytes written, buffer pointer advanced, CF = 0. A partial
	// write (disk full) still reports what landed, like esxDOS.
	InOutCpu.BC = uint16_t(Wrote);
	const uint16_t NewPtr = uint16_t(Src + Wrote);
	if (m_bUseIxForBuffers)
	{
		InOutCpu.IX = NewPtr;
	}
	else
	{
		InOutCpu.HL = NewPtr;
	}
	if (Wrote < Count)
	{
		InOutCpu.SetA(kVfsErrDiskFull);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFSync(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	fflush(H->m_File);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFTruncate(Z80State& InOutCpu, MemoryBus& InOutMem, bool bByHandle)
{
	// by handle: A = handle, BCDE = new size. by name: IX/HL = name, BCDE = size.
	const uint32_t Size = (uint32_t(InOutCpu.BC) << 16) | uint32_t(InOutCpu.DE);
	std::string HostPath;
	if (bByHandle)
	{
		VfsHandle* H = GetHandle(uint8_t(InOutCpu.A()));
		if (!H)
		{
			InOutCpu.SetA(kVfsErrBadHandle);
			SetCF(InOutCpu);
			return;
		}
		if (!H->m_bWritable)
		{
			InOutCpu.SetA(kVfsErrAccess);
			SetCF(InOutCpu);
			return;
		}
		fflush(H->m_File);
		HostPath = H->m_ResolvedPath;
	}
	else
	{
		if (m_bReadOnly)
		{
			InOutCpu.SetA(kVfsErrReadOnly);
			SetCF(InOutCpu);
			return;
		}
		const char* Which = "HL";
		const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
		bool bOk = false;
		HostPath = Name.empty() ? std::string() : ResolveToHostPath(Name, bOk);
		if (!bOk)
		{
			InOutCpu.SetA(kVfsErrNoFile);
			SetCF(InOutCpu);
			return;
		}
	}
	std::error_code Ec;
	std::filesystem::resize_file(HostPath, Size, Ec);
	if (Ec)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFOpenDir(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = drive, IX/HL = path (ASCIIZ, may be empty/"." for the cwd), B = flags.
	const char* Which = "HL";
	std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	if (Name == ".")
	{
		Name.clear();
	}
	bool bOk = false;
	const std::string HostPath = ResolveToHostPath(Name, bOk); // "" resolves to cwd/root
	std::error_code Ec;
	if (!bOk || !std::filesystem::is_directory(HostPath, Ec))
	{
		fprintf(stderr, "vfs: F_OPENDIR '%s' -> NOT_DIR\n", Name.c_str());
		InOutCpu.SetA(kVfsErrNotDir);
		SetCF(InOutCpu);
		return;
	}
	const int Slot = AllocHandle();
	if (Slot == 0)
	{
		InOutCpu.SetA(kVfsErrTooMany);
		SetCF(InOutCpu);
		return;
	}

	VfsHandle& H = m_Handles[Slot];
	H = VfsHandle{};
	H.m_bDir = true;
	H.m_ResolvedPath = HostPath;
	H.m_OpenedAs = Name;
	for (std::filesystem::directory_iterator It(HostPath, Ec), End; !Ec && It != End; It.increment(Ec))
	// Do not let filesystem exceptions escape an emulated API call.
	{
		const auto& Ent = *It;
		const std::string Leaf = Ent.path().filename().string();
		if (Leaf == "." || Leaf == "..")
		{
			continue;
		}
		VfsDirEntry E;
		E.m_Name = Leaf;
		E.m_bDir = Ent.is_directory(Ec);
		if (Ec)
		{
			break;
		}
		const uintmax_t Size = E.m_bDir ? 0 : std::filesystem::file_size(Ent.path(), Ec);
		if (Ec)
		{
			break;
		}
		E.m_Size = uint32_t(std::min<uintmax_t>(Size, UINT32_MAX));
		auto Perms = Ent.status(Ec).permissions();
		if (Ec)
		{
			break;
		}
		E.m_bReadOnly = (Perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
		H.m_Dir.push_back(std::move(E));
	}
	if (Ec)
	{
		H = VfsHandle{};
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	++m_Stats.m_DirOpens;
	InOutCpu.SetA(uint8_t(Slot));
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFReadDir(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = dir handle, IX/HL = buffer. Fills one entry and returns A = 1; at the
	// end of the listing returns A = 0, CF = 0. Entry layout (esxDOS + the
	// NextZXOS size fields): attr(1), ASCIIZ name, time(2)=0, date(2)=0, size(4).
	const uint8_t Handle = uint8_t(InOutCpu.A());
	VfsHandle* H = GetDirHandle(Handle);
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	if (H->m_DirPos >= H->m_Dir.size())
	{
		InOutCpu.SetA(0); // no more entries
		ClearCF(InOutCpu);
		return;
	}
	const VfsDirEntry& E = H->m_Dir[H->m_DirPos++];
	const uint16_t Buf = m_bUseIxForBuffers ? InOutCpu.IX : InOutCpu.HL;
	const uint8_t Attr = uint8_t((E.m_bDir ? 0x10 : 0x00) | (E.m_bReadOnly ? 0x01 : 0x00));
	InOutMem.Wr(Buf, Attr);
	size_t IdxI = 0;
	for (; IdxI < E.m_Name.size() && IdxI < 254; ++IdxI)
	{
		InOutMem.Wr(uint16_t(Buf + 1 + IdxI), uint8_t(E.m_Name[IdxI]));
	}
	InOutMem.Wr(uint16_t(Buf + 1 + IdxI), 0);
	const uint16_t Tail = uint16_t(Buf + 1 + IdxI + 1);
	InOutMem.Wr(uint16_t(Tail + 0), 0);
	InOutMem.Wr(uint16_t(Tail + 1), 0); // time (unmodelled)
	InOutMem.Wr(uint16_t(Tail + 2), 0);
	InOutMem.Wr(uint16_t(Tail + 3), 0); // date
	InOutMem.Wr(uint16_t(Tail + 4), uint8_t(E.m_Size & 0xFF));
	InOutMem.Wr(uint16_t(Tail + 5), uint8_t((E.m_Size >> 8) & 0xFF));
	InOutMem.Wr(uint16_t(Tail + 6), uint8_t((E.m_Size >> 16) & 0xFF));
	InOutMem.Wr(uint16_t(Tail + 7), uint8_t((E.m_Size >> 24) & 0xFF));
	InOutCpu.SetA(1);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFTellDir(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	VfsHandle* H = GetDirHandle(uint8_t(InOutCpu.A()));
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const uint32_t Pos = uint32_t(H->m_DirPos);
	InOutCpu.BC = uint16_t(Pos >> 16);
	InOutCpu.DE = uint16_t(Pos & 0xFFFF);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFSeekDir(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	VfsHandle* H = GetDirHandle(uint8_t(InOutCpu.A()));
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	const uint32_t Pos = (uint32_t(InOutCpu.BC) << 16) | uint32_t(InOutCpu.DE);
	H->m_DirPos = (Pos <= H->m_Dir.size()) ? size_t(Pos) : H->m_Dir.size();
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFRewindDir(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	VfsHandle* H = GetDirHandle(uint8_t(InOutCpu.A()));
	if (!H)
	{
		InOutCpu.SetA(kVfsErrBadHandle);
		SetCF(InOutCpu);
		return;
	}
	H->m_DirPos = 0;
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFGetCwd(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = drive, IX/HL = buffer. Return NextZXOS-form ASCIIZ "<drive>:/<cwd>".
	const uint16_t Buf = m_bUseIxForBuffers ? InOutCpu.IX : InOutCpu.HL;
	std::string Out;
	Out += m_Drive;
	Out += ":/";
	Out += m_Cwd;
	WriteCString(InOutMem, Buf, Out);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFChDir(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	const char* Which = "HL";
	std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	if (Name == ".")
	{
		ClearCF(InOutCpu);
		return;
	}
	bool bOk = false;
	const std::string HostPath = ResolveToHostPath(Name, bOk);
	std::error_code Ec;
	if (!bOk || !std::filesystem::is_directory(HostPath, Ec))
	{
		fprintf(stderr, "vfs: F_CHDIR '%s' -> NOT_DIR\n", Name.c_str());
		InOutCpu.SetA(kVfsErrNotDir);
		SetCF(InOutCpu);
		return;
	}
	// Derive the new cwd as a VFS-relative path by stripping the mount root.
	std::string Rel = HostPath;
	if (Rel.compare(0, m_RootPath.size(), m_RootPath) == 0)
	{
		Rel.erase(0, m_RootPath.size());
	}
	while (!Rel.empty() && Rel.front() == '/')
	{
		Rel.erase(0, 1);
	}
	while (!Rel.empty() && Rel.back() == '/')
	{
		Rel.pop_back();
	}
	m_Cwd = Rel;
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFMkDir(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	if (m_bReadOnly)
	{
		InOutCpu.SetA(kVfsErrReadOnly);
		SetCF(InOutCpu);
		return;
	}
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	bool bOk = false;
	const std::string HostPath =
		Name.empty() ? std::string() : ResolveToHostPath(Name, bOk, ResolveMode::kResolveModeForCreate);
	if (!bOk)
	{
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	std::error_code Ec;
	if (std::filesystem::exists(HostPath, Ec))
	{
		InOutCpu.SetA(kVfsErrExist);
		SetCF(InOutCpu);
		return;
	}
	if (!std::filesystem::create_directory(HostPath, Ec) || Ec)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFRmDir(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	if (m_bReadOnly)
	{
		InOutCpu.SetA(kVfsErrReadOnly);
		SetCF(InOutCpu);
		return;
	}
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	bool bOk = false;
	const std::string HostPath = Name.empty() ? std::string() : ResolveToHostPath(Name, bOk);
	std::error_code Ec;
	if (!bOk || !std::filesystem::is_directory(HostPath, Ec))
	{
		InOutCpu.SetA(kVfsErrNotDir);
		SetCF(InOutCpu);
		return;
	}
	if (!std::filesystem::is_empty(HostPath, Ec) || Ec)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	if (!std::filesystem::remove(HostPath, Ec) || Ec)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFUnlink(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	if (m_bReadOnly)
	{
		InOutCpu.SetA(kVfsErrReadOnly);
		SetCF(InOutCpu);
		return;
	}
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	bool bOk = false;
	const std::string HostPath = Name.empty() ? std::string() : ResolveToHostPath(Name, bOk);
	std::error_code Ec;
	if (!bOk || std::filesystem::is_directory(HostPath, Ec))
	{
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	if (!std::filesystem::remove(HostPath, Ec) || Ec)
	{
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFRename(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	if (m_bReadOnly)
	{
		InOutCpu.SetA(kVfsErrReadOnly);
		SetCF(InOutCpu);
		return;
	}
	// Old name in IX/HL, new name in DE (ASCIIZ).
	const char* Which = "HL";
	const std::string Old = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	const std::string New = ReadCString(InOutMem, InOutCpu.DE);
	bool bOldOk = false, bNewOk = false;
	const std::string OldHost = Old.empty() ? std::string() : ResolveToHostPath(Old, bOldOk);
	const std::string NewHost =
		New.empty() ? std::string() : ResolveToHostPath(New, bNewOk, ResolveMode::kResolveModeForCreate);
	if (!bOldOk)
	{
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	if (!bNewOk)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	std::error_code Ec;
	std::filesystem::rename(OldHost, NewHost, Ec);
	if (Ec)
	{
		InOutCpu.SetA(kVfsErrGeneric);
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFGetFree(Z80State& InOutCpu, MemoryBus& /*Mem*/)
{
	// Exit: BCDE = number of 512-byte blocks free (per the NextZXOS API doc),
	// NOT bytes. Clamp to 32-bit.
	std::error_code Ec;
	const auto Space = std::filesystem::space(m_RootPath, Ec);
	uint64_t Blocks = Ec ? 0 : (Space.available / 512ull);
	if (Blocks > 0xFFFFFFFFull)
	{
		Blocks = 0xFFFFFFFFull;
	}
	InOutCpu.BC = uint16_t(uint32_t(Blocks) >> 16);
	InOutCpu.DE = uint16_t(uint32_t(Blocks) & 0xFFFF);
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoFChmod(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// A = drive, IX/HL = name, B = attributes (FAT: bit 0 = read-only). Best
	// effort: toggle the host write bit; never fail the guest over chmod.
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	bool bOk = false;
	const std::string HostPath = Name.empty() ? std::string() : ResolveToHostPath(Name, bOk);
	if (!bOk)
	{
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	if (!m_bReadOnly)
	{
		const bool bWantReadOnly = (uint8_t(InOutCpu.BC >> 8) & 0x01) != 0;
		std::error_code Ec;
		std::filesystem::permissions(HostPath, std::filesystem::perms::owner_write,
			bWantReadOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add, Ec);
	}
	ClearCF(InOutCpu);
}

// ============================ NextZXOS M_P3DOS ============================

// F_STAT ($AC): A = drive, IX (HL) = filename, DE = 11-byte buffer, same
// layout as F_FSTAT: +0 drive, +1 device, +2 attributes (bit 4 = directory),
// +3..+6 timestamp (unmodelled, 0), +7..+10 size.
void NextZxosVfs::DoFStatByName(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	const char* Which = "HL";
	const std::string Name = ReadNameHLorIX(InOutMem, InOutCpu, &Which);
	bool bResolveOk = false;
	const std::string HostPath = Name.empty() ? std::string() : ResolveToHostPath(Name, bResolveOk);
	if (!bResolveOk)
	{
		fprintf(stderr, "vfs: F_STAT '%s' -> NO_FILE\n", Name.c_str());
		InOutCpu.SetA(kVfsErrNoFile);
		SetCF(InOutCpu);
		return;
	}
	std::error_code Ec;
	const bool bDir = std::filesystem::is_directory(HostPath, Ec);
	const uint64_t Size = bDir ? 0 : uint64_t(std::filesystem::file_size(HostPath, Ec));
	const uint16_t Buf = InOutCpu.DE;
	for (int IdxI = 0; IdxI < 11; ++IdxI)
	{
		InOutMem.Wr(uint16_t(Buf + IdxI), 0);
	}
	InOutMem.Wr(uint16_t(Buf + 0), uint8_t(m_Drive));
	InOutMem.Wr(uint16_t(Buf + 2), uint8_t(bDir ? 0x10 : 0x00));
	InOutMem.Wr(uint16_t(Buf + 7), uint8_t(Size & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 8), uint8_t((Size >> 8) & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 9), uint8_t((Size >> 16) & 0xFF));
	InOutMem.Wr(uint16_t(Buf + 10), uint8_t((Size >> 24) & 0xFF));
	ClearCF(InOutCpu);
}

void NextZxosVfs::DoMP3Dos(Z80State& InOutCpu, MemoryBus& InOutMem)
{
	// M_P3DOS IDE_BANK: main DE=$01BD, C=7; alternate H'=0 selects
	// ZX RAM, L'=1 allocates or L'=3 frees the bank in E'.
	// Allocation returns the page in main E. CF=1 means success here,
	// which is the opposite convention to esxDOS file calls.
	const uint16_t FnCode = InOutCpu.DE;
	if (FnCode != kMp3dIdeBank)
	{
		ClearCF(InOutCpu);
		return;
	}
	const uint8_t Subcall = uint8_t(InOutCpu.HLp & 0xFF);
	const uint8_t BankType = uint8_t(InOutCpu.HLp >> 8);
	if (BankType != kRcBanktypeZx)
	{
		ClearCF(InOutCpu);
		return;
	}
	if (Subcall == kRcBankAlloc)
	{
		const int Page8K = InOutMem.AllocBank8K();
		if (Page8K < 0)
		{
			ClearCF(InOutCpu);
			return;
		}
		++m_Stats.m_IdeBankAllocs;
		// Return 8K page ID in main E (caller reads with `ld a, e`).
		InOutCpu.DE = uint16_t((InOutCpu.DE & 0xFF00) | uint16_t(Page8K & 0xFF));
		SetCF(InOutCpu);
		return;
	}
	if (Subcall == kRcBankFree)
	{
		// 8K page ID to free is in E' (alt E).
		const int Page8K = int(uint8_t(InOutCpu.DEp & 0xFF));
		if (!InOutMem.IsPage8KAllocated(Page8K))
		{
			ClearCF(InOutCpu);
			return;
		}
		InOutMem.FreeBank8K(Page8K);
		++m_Stats.m_IdeBankFrees;
		SetCF(InOutCpu);
		return;
	}
	ClearCF(InOutCpu);
}

NextZxosVfs::~NextZxosVfs()
{
	for (VfsHandle& H : m_Handles)
	{
		if (H.m_File != nullptr)
		{
			fclose(H.m_File);
			H.m_File = nullptr;
		}
	}
}

bool NextZxosVfs::CloneForReplay(const NextZxosVfs& Src)
{
	// Live positions must be read BEFORE the memberwise copy aliases the FILE*s.
	long Pos[kMaxHandles];
	for (int IdxI = 0; IdxI < kMaxHandles; ++IdxI)
	{
		Pos[IdxI] = (Src.m_Handles[IdxI].m_File != nullptr) ? ftell(Src.m_Handles[IdxI].m_File) : -1;
	}
	for (VfsHandle& H : m_Handles) // close what this clone owned from a previous checkpoint
	{
		if (H.m_File != nullptr)
		{
			fclose(H.m_File);
			H.m_File = nullptr;
		}
	}
	*this = Src; // every member (stats, FRead buffer, mount state...)
	bool bOk = true;
	for (int IdxI = 0; IdxI < kMaxHandles; ++IdxI)
	{
		VfsHandle& H = m_Handles[IdxI];
		if (H.m_File == nullptr)
		{
			continue; // aliased Src pointer only marks presence
		}
		H.m_File = fopen(H.m_ResolvedPath.c_str(), "rb"); // replay never writes
		if (H.m_File == nullptr || Pos[IdxI] < 0 || fseek(H.m_File, Pos[IdxI], SEEK_SET) != 0)
		{
			if (H.m_File != nullptr)
			{
				fclose(H.m_File);
			}
			H = VfsHandle{};
			bOk = false;
		}
	}
	return bOk;
}
