#include "trace.h"

#if INSPECCY_TRACE_SUPPORT

#include "io.h"
#include "memory.h"
#include "peephole.h"
#include "z80.h"

#include <trace/disasm.h>
#include <trace/sld.h>
#include <trace/trace_binary.h>

#include <chrono>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{

constexpr size_t kMinQueueFrames = 1024;

// ftell returns a 32-bit long on Windows, so it overflows to -1 (which
// casts to the 0xFFFF...FF sentinel) once a trace passes 2 GB - corrupting the
// .fidx byte offsets. Use the platform's 64-bit file-position call instead.
uint64_t FileTell64(FILE* F)
{
#ifdef _WIN32
	return uint64_t(_ftelli64(F));
#else
	return uint64_t(ftello(F));
#endif
}

size_t NextPow2(size_t N)
{
	size_t P = 1;
	while (P < N)
	{
		P <<= 1;
	}
	return P;
}

// Binary trace encode helpers. The byte buffer being filled is a std::vector
// so we don't have to think about overflow inside the encoder; callers reserve
// a max-size up front and these append.

void PutU8(std::vector<uint8_t>& InOutBuf, uint8_t V)
{
	InOutBuf.push_back(V);
}

void PutU16(std::vector<uint8_t>& InOutBuf, uint16_t V)
{
	InOutBuf.push_back(uint8_t(V));
	InOutBuf.push_back(uint8_t(V >> 8));
}

void PutU32(std::vector<uint8_t>& InOutBuf, uint32_t V)
{
	InOutBuf.push_back(uint8_t(V));
	InOutBuf.push_back(uint8_t(V >> 8));
	InOutBuf.push_back(uint8_t(V >> 16));
	InOutBuf.push_back(uint8_t(V >> 24));
}

[[maybe_unused]] void PutU64(std::vector<uint8_t>& InOutBuf, uint64_t V)
{
	for (int IdxI = 0; IdxI < 8; ++IdxI)
	{
		InOutBuf.push_back(uint8_t(V >> (IdxI * 8)));
	}
}

void PutBytes(std::vector<uint8_t>& InOutBuf, const void* P, size_t N)
{
	const uint8_t* B = static_cast<const uint8_t*>(P);
	InOutBuf.insert(InOutBuf.end(), B, B + N);
}

// LEB128 unsigned varint. Used for cycle deltas and frame deltas in the
// instruction record - typical deltas are small (4-30 cycles, 0 frames) so
// almost always 1 byte, occasionally 2.
void PutVarint(std::vector<uint8_t>& InOutBuf, uint64_t V)
{
	while (V >= 0x80)
	{
		InOutBuf.push_back(uint8_t(V & 0x7F) | 0x80);
		V >>= 7;
	}
	InOutBuf.push_back(uint8_t(V));
}

uint8_t MachineCodeFor(MachineModel M)
{
	switch (M)
	{
	case MachineModel::kMachineModelSpectrum48K:
		return BinaryTrace::kMachine_Spectrum48K;
	case MachineModel::kMachineModelSpectrum128K:
		return BinaryTrace::kMachine_Spectrum128K;
	case MachineModel::kMachineModelPentagon128:
		return BinaryTrace::kMachine_Pentagon128;
	case MachineModel::kMachineModelNext50:
		return BinaryTrace::kMachine_Next50;
	case MachineModel::kMachineModelNext60:
		return BinaryTrace::kMachine_Next60;
	// Timex machines carry their own codes so viewers can render the
	// SCLD's visible bright black (timing is still Spectrum-class).
	case MachineModel::kMachineModelTs2068:
		return BinaryTrace::kMachine_Ts2068;
	case MachineModel::kMachineModelTc2068:
		return BinaryTrace::kMachine_Tc2068;
	case MachineModel::kMachineModelTc2048:
		return BinaryTrace::kMachine_Tc2048;
	case MachineModel::kMachineModelZxEvo:
		return BinaryTrace::kMachine_ZxEvo;
	case MachineModel::kMachineModelZxUno:
		return BinaryTrace::kMachine_ZxUno;
	}
	return 0;
}

} // namespace

TraceWriter::TraceWriter() = default;

TraceWriter::~TraceWriter()
{
	Stop();
}

bool TraceWriter::Open(const char* Path, const TraceFormatOptions& Opt, size_t QueueBytes, const SldSymbolMap* SymsIn,
	int Argc, char** Argv)
{
	m_Options = Opt;
	m_Symbols = SymsIn;

	if (Path == nullptr || Path[0] == '\0' || strcmp(Path, "-") == 0 || strcmp(Path, "stdout") == 0)
	{
		m_OutFile = stdout;
		m_bOwnsFile = false;
	}
	else
	{
		m_OutFile = fopen(Path, "wb");
		if (m_OutFile == nullptr)
		{
			return false;
		}
		m_bOwnsFile = true;
		std::setvbuf(m_OutFile, nullptr, _IOFBF, 1 << 16);
	}

	const size_t FrameSize = sizeof(TraceFrame);
	size_t Frames = QueueBytes / FrameSize;
	if (Frames < kMinQueueFrames)
	{
		Frames = kMinQueueFrames;
	}
	Frames = NextPow2(Frames);
	m_Ring.resize(Frames);
	m_RingMask = Frames - 1;

	m_Write.store(0, std::memory_order_relaxed);
	m_Read.store(0, std::memory_order_relaxed);
	m_bStop.store(false, std::memory_order_relaxed);

	// Header lines - invocation and column names. Written directly to OutFile
	// before the writer thread starts so they're guaranteed first in the stream.
	if (Argc > 0 && Argv != nullptr)
	{
		std::fputs("# ", m_OutFile);
		for (int IdxI = 0; IdxI < Argc; ++IdxI)
		{
			const char* Arg = Argv[IdxI];
			bool bNeedsQuoting = false;
			for (const char* P = Arg; *P; ++P)
			{
				if (*P == ' ' || *P == '\t' || *P == '"')
				{
					bNeedsQuoting = true;
					break;
				}
			}
			if (IdxI > 0)
			{
				std::fputc(' ', m_OutFile);
			}
			if (bNeedsQuoting)
			{
				std::fputc('"', m_OutFile);
				for (const char* P = Arg; *P; ++P)
				{
					if (*P == '"' || *P == '\\')
					{
						std::fputc('\\', m_OutFile);
					}
					std::fputc(*P, m_OutFile);
				}
				std::fputc('"', m_OutFile);
			}
			else
			{
				std::fputs(Arg, m_OutFile);
			}
		}
		std::fputc('\n', m_OutFile);
	}
	if (m_Symbols != nullptr)
	{
		std::fputs("# columns: cycles | frame | address | symbol | mnemonic | effects\n", m_OutFile);
	}
	else
	{
		std::fputs("# columns: cycles | frame | address | mnemonic | effects\n", m_OutFile);
	}

	m_bWriteFailed = false;
	m_bRunning = true;
	m_Thread = std::thread(
		[this]
		{
			RunLoop();
		});
	return true;
}

bool TraceWriter::OpenAnalysisOnly(const TraceFormatOptions& Opt, size_t QueueBytes)
{
	m_Options = Opt;
	m_Symbols = nullptr;
	m_bBinaryMode = false;
	m_OutFile = nullptr;
	m_bOwnsFile = false;

	const size_t FrameSize = sizeof(TraceFrame);
	size_t Frames = QueueBytes / FrameSize;
	if (Frames < kMinQueueFrames)
	{
		Frames = kMinQueueFrames;
	}
	Frames = NextPow2(Frames);
	m_Ring.resize(Frames);
	m_RingMask = Frames - 1;

	m_Write.store(0, std::memory_order_relaxed);
	m_Read.store(0, std::memory_order_relaxed);
	m_bStop.store(false, std::memory_order_relaxed);

	m_bWriteFailed = false;
	m_bRunning = true;
	m_Thread = std::thread(
		[this]
		{
			RunLoop();
		});
	return true;
}

bool TraceWriter::OpenBinary(const char* Path, const TraceFormatOptions& Opt, size_t QueueBytes,
	const SldSymbolMap* SymsIn, int Argc, char** Argv, const BinaryOpenContext& Ctx, const char* IndexPath)
{
	m_Options = Opt;
	m_Symbols = SymsIn;
	m_bBinaryMode = true;
	m_bHeaderWritten = false;
	m_bBuildIndex = (IndexPath != nullptr && IndexPath[0] != '\0');
	m_IndexFilePath = m_bBuildIndex ? std::string(IndexPath) : std::string();
	m_IndexEntryBytes.clear();
	if (m_bBuildIndex)
	{
		// Reserve for a typical 1-min budget; index entries are tiny
		// so this is mostly to avoid reallocations during the writer hot loop.
		m_IndexEntryBytes.reserve(sizeof(BinaryTrace::IndexEntry) * 4096);
	}

	if (Path == nullptr || Path[0] == '\0' || strcmp(Path, "-") == 0 || strcmp(Path, "stdout") == 0)
	{
		// Binary on stdout would mix with the run summary etc. - refuse.
		return false;
	}
	m_OutFile = fopen(Path, "wb");
	if (m_OutFile == nullptr)
	{
		return false;
	}
	m_bOwnsFile = true;
	std::setvbuf(m_OutFile, nullptr, _IOFBF, 1 << 16);

	// /dev/null and pipes aren't seekable, so we won't be able to patch the
	// header's EndCycle on Stop(). Detect early so the sentinel value stays
	// for downstream tools to recognise.
	const long Pos0 = ftell(m_OutFile);
	m_bSeekable = (Pos0 == 0) && (fseek(m_OutFile, 0, SEEK_SET) == 0);

	const size_t FrameSize = sizeof(TraceFrame);
	size_t Frames = QueueBytes / FrameSize;
	if (Frames < kMinQueueFrames)
	{
		Frames = kMinQueueFrames;
	}
	Frames = NextPow2(Frames);
	m_Ring.resize(Frames);
	m_RingMask = Frames - 1;

	m_Write.store(0, std::memory_order_relaxed);
	m_Read.store(0, std::memory_order_relaxed);
	m_bStop.store(false, std::memory_order_relaxed);

	// Build the on-disk header into a byte buffer NOW (captures the initial
	// CPU/MMU state before any execution can race it). We defer the actual
	// file write until the first record arrives, because that's when we know
	// the StartCycle to patch into the header.
	using namespace BinaryTrace;
	m_PendingHeaderBytes.clear();
	m_PendingHeaderBytes.reserve(sizeof(Header) + 4096);

	Header H{};
	H.m_Magic = kMagic;
	H.m_Version = kVersion;
	uint16_t Flags = 0;
	if (Opt.m_bVerbose)
	{
		Flags |= kFlag_Verbose;
	}
	if (Opt.m_bIncludeR)
	{
		Flags |= kFlag_IncludeR;
	}
	if (Opt.m_bIncludeMemPtr)
	{
		Flags |= kFlag_IncludeMemPtr;
	}
	if (Opt.m_bDiffOnly)
	{
		Flags |= kFlag_DiffOnly;
	}
	if (Opt.m_bB16KPageView)
	{
		Flags |= kFlag_16KPageView;
	}
	H.m_Flags = Flags;
	H.m_StartCycle = 0; // patched on first record (= EmitBinaryHeader)
	H.m_EndCycle = kSentinel;
	H.m_StartFrame = 0; // patched on first record
	H.m_EndFrame = kSentinel;
	H.m_StartTimeNanos = 0; // patched on first record
	H.m_EndTimeNanos = kSentinel;
	H.m_TotalRecordsHint = kSentinel;
	H.m_BaseMhzX1e6 = uint64_t(Ctx.m_BaseMhz * 1.0e6 + 0.5);
	H.m_FrameIntPeriod = uint32_t(Ctx.m_FrameIntPeriod);
	if (Ctx.m_InitCpu != nullptr)
	{
		H.m_InitPC = Ctx.m_InitCpu->PC;
		H.m_InitSP = Ctx.m_InitCpu->SP;
		H.m_InitAF = Ctx.m_InitCpu->AF;
		H.m_InitBC = Ctx.m_InitCpu->BC;
		H.m_InitDE = Ctx.m_InitCpu->DE;
		H.m_InitHL = Ctx.m_InitCpu->HL;
		H.m_InitIX = Ctx.m_InitCpu->IX;
		H.m_InitIY = Ctx.m_InitCpu->IY;
		H.m_InitAFp = Ctx.m_InitCpu->AFp;
		H.m_InitBCp = Ctx.m_InitCpu->BCp;
		H.m_InitDEp = Ctx.m_InitCpu->DEp;
		H.m_InitHLp = Ctx.m_InitCpu->HLp;
		H.m_InitI = Ctx.m_InitCpu->I;
		H.m_InitR = Ctx.m_InitCpu->R;
		H.m_InitIFF1 = Ctx.m_InitCpu->IFF1;
		H.m_InitIFF2 = Ctx.m_InitCpu->IFF2;
		H.m_InitIM = Ctx.m_InitCpu->IM;
		H.m_InitMemPtr = Ctx.m_InitCpu->m_MemPtr;
	}
	if (Ctx.m_InitMem != nullptr)
	{
		for (int IdxI = 0; IdxI < 8; ++IdxI)
		{
			H.m_InitNR5057[IdxI] = Ctx.m_InitMem->GetNextReg(uint8_t(0x50 + IdxI));
		}
		// v4: where the display started. Every later change to it arrives as a
		// kRecord_NextRegWrite / kRecord_PortWrite, so a consumer replaying
		// from here always knows the video mode at any cycle.
		H.m_InitBorder = Ctx.m_InitMem->GetBorderColour();
		H.m_InitPort7FFD = Ctx.m_InitMem->GetPort7FFD();
		H.m_InitScldMode = Ctx.m_InitMem->GetScldMode();
		H.m_InitEvoFf77 = Ctx.m_InitMem->GetEvoFf77();
		for (size_t IdxI = 0; IdxI < kDisplayRegCount; ++IdxI)
		{
			H.m_InitDisplayRegs[IdxI] = Ctx.m_InitMem->GetNextReg(kDisplayRegOrder[IdxI]);
		}
	}
	H.m_Machine = MachineCodeFor(Ctx.m_Machine);
	H.m_TrdosActive = Ctx.m_bTrdosActive ? 1 : 0;
	PutBytes(m_PendingHeaderBytes, &H, sizeof(H));

	// CLI args blob (single space-joined string, NUL-free).
	std::string CliBlob;
	for (int IdxI = 0; IdxI < Argc; ++IdxI)
	{
		if (IdxI > 0)
		{
			CliBlob += ' ';
		}
		CliBlob += Argv[IdxI];
	}
	if (CliBlob.size() > 0xFFFF)
	{
		CliBlob.resize(0xFFFF);
	}
	PutU16(m_PendingHeaderBytes, uint16_t(CliBlob.size()));
	PutBytes(m_PendingHeaderBytes, CliBlob.data(), CliBlob.size());

	// SLD paths blob - newline-separated, no trailing newline. Used by
	// downstream symbol-aware consumers (inspeccy) to auto-load
	// labels from the same .sld.txt files the writer was given.
	std::string SldBlob;
	for (size_t IdxI = 0; IdxI < Ctx.m_SldPaths.size(); ++IdxI)
	{
		if (IdxI > 0)
		{
			SldBlob += '\n';
		}
		SldBlob += Ctx.m_SldPaths[IdxI];
	}
	if (SldBlob.size() > 0xFFFF)
	{
		SldBlob.resize(0xFFFF);
	}
	PutU16(m_PendingHeaderBytes, uint16_t(SldBlob.size()));
	PutBytes(m_PendingHeaderBytes, SldBlob.data(), SldBlob.size());

	// CRC32 over everything written so far.
	const uint32_t Crc = Crc32(m_PendingHeaderBytes.data(), m_PendingHeaderBytes.size());
	PutU32(m_PendingHeaderBytes, Crc);

	m_bWriteFailed = false;
	m_bRunning = true;
	m_Thread = std::thread(
		[this]
		{
			RunLoop();
		});
	return true;
}

void TraceWriter::EmitBinaryHeader(uint64_t StartCycle)
{
	using namespace BinaryTrace;
	uint8_t* HB = m_PendingHeaderBytes.data();
	memcpy(HB + offsetof(Header, m_StartCycle), &StartCycle, sizeof(uint64_t));
	memcpy(HB + offsetof(Header, m_StartFrame), &m_BinaryStartFrame, sizeof(uint64_t));
	memcpy(HB + offsetof(Header, m_StartTimeNanos), &m_BinaryStartNanos, sizeof(uint64_t));
	const size_t CrcOffset = m_PendingHeaderBytes.size() - 4;
	const uint32_t Crc = Crc32(HB, CrcOffset);
	memcpy(HB + CrcOffset, &Crc, sizeof(uint32_t));
	// Stash for the .fidx sidecar - the parent CRC is how the index file
	// links back to a specific .btr without ambiguity. Note: this is the
	// CRC at first-record time; Stop() re-patches the header (EndCycle etc.)
	// and computes a NEW CRC. We use the FINAL CRC as the parent reference,
	// captured in WriteIndexSidecar() after Stop()'s header rewrite.
	m_ParentHeaderCrc = Crc;

	fwrite(HB, 1, m_PendingHeaderBytes.size(), m_OutFile);
	m_HeaderEndOffset = m_PendingHeaderBytes.size();
	m_bHeaderWritten = true;
}

void TraceWriter::AppendIndexEntry(const TraceFrame& Frame, uint64_t ByteOffset)
{
	using namespace BinaryTrace;
	if (!m_bBuildIndex)
	{
		return;
	}
	IndexEntry E{};
	E.m_FrameNumber = uint32_t(Frame.m_Frame);
	E.m_StartCycle = Frame.m_StartCycles;
	E.m_StartTimeNanos = Frame.m_EmuWallNanos;
	E.m_FirstByteOffset = ByteOffset;
	// Register snapshot is the PRE state of the first instruction of this
	// frame - i.e., the state the decoder lands in when it seeks to this
	// entry's offset and starts walking forward through delta records.
	E.PC = Frame.m_PrePC;
	E.AF = Frame.m_PreAF;
	E.BC = Frame.m_PreBC;
	E.DE = Frame.m_PreDE;
	E.HL = Frame.m_PreHL;
	E.IX = Frame.m_PreIX;
	E.IY = Frame.m_PreIY;
	E.SP = Frame.m_PreSP;
	const uint8_t* P = reinterpret_cast<const uint8_t*>(&E);
	m_IndexEntryBytes.insert(m_IndexEntryBytes.end(), P, P + sizeof(E));
}

void TraceWriter::WriteIndexSidecar()
{
	using namespace BinaryTrace;
	if (!m_bBuildIndex || m_IndexEntryBytes.empty() || m_IndexFilePath.empty())
	{
		return;
	}
	FILE* IF = fopen(m_IndexFilePath.c_str(), "wb");
	if (IF == nullptr)
	{
		m_bWriteFailed = true;
		fprintf(stderr, "trace: failed to write index sidecar %s\n", m_IndexFilePath.c_str());
		return;
	}
	IndexHeader IH{};
	IH.m_Magic = kIndexMagic;
	IH.m_Version = kVersion;
	IH.m_ParentMagic = kMagic;
	IH.m_ParentHeaderCrc = m_ParentHeaderCrc;
	IH.m_StartFrame = m_BinaryStartFrame;
	IH.m_EndFrame = m_BinaryLastFrame;
	const size_t NumEntries = m_IndexEntryBytes.size() / sizeof(IndexEntry);
	IH.m_EntryCount = uint32_t(NumEntries);
	fwrite(&IH, 1, sizeof(IH), IF);
	fwrite(m_IndexEntryBytes.data(), 1, m_IndexEntryBytes.size(), IF);
	const bool bError = ferror(IF) != 0;
	if (fclose(IF) != 0 || bError)
	{
		m_bWriteFailed = true;
	}
}

TraceFrame* TraceWriter::BeginFrame()
{
	const uint64_t W = m_Write.load(std::memory_order_relaxed);
	const uint64_t Capacity = uint64_t(m_Ring.size());
	for (;;)
	{
		const uint64_t R = m_Read.load(std::memory_order_acquire);
		if (W - R < Capacity)
		{
			break;
		}
		std::unique_lock<std::mutex> L(m_Lock);
		m_CondNotFull.wait(L,
			[&]
			{
				return (m_Write.load(std::memory_order_relaxed) - m_Read.load(std::memory_order_acquire)) < Capacity ||
					   m_bStop.load(std::memory_order_relaxed);
			});
		if (m_bStop.load(std::memory_order_relaxed))
		{
			// Producer is being torn down; return a throwaway slot anyway -
			// CommitFrame's publish will fall through and the writer thread
			// exits without consuming.
			break;
		}
	}
	TraceFrame* F = &m_Ring[size_t(W & m_RingMask)];
	*F = TraceFrame{};
	m_CurrentProducerFrame = F;
	return F;
}

void TraceWriter::CommitFrame()
{
	const uint64_t W = m_Write.load(std::memory_order_relaxed);
	// Snapshot consumer's read position BEFORE the publish. If R == W the
	// consumer either has nothing or is about to sleep - we MUST notify.
	// If R < W the consumer is still draining earlier items and will pick
	// this one up without help; skip the notify and save a mutex lock plus
	// condvar signal that perf attributed ~23% of total wall time to.
	//
	// Race we accept: consumer can finish its drain (advance R to W) and
	// decide to sleep AFTER our load but BEFORE our store. We then skip
	// notify and the consumer's existing 5 ms wait_for timeout caps the
	// worst-case latency at 5 ms - fine for trace workloads.
	const uint64_t R = m_Read.load(std::memory_order_acquire);
	m_Write.store(W + 1, std::memory_order_release);
	m_CurrentProducerFrame = nullptr;
	if (R == W)
	{
		std::lock_guard<std::mutex> L(m_Lock);
		m_CondNotEmpty.notify_one();
	}

	// Flush any video-state events raised during the frame just published.
	// The guard stops the BeginFrame/CommitFrame pairs below from recursing
	// back into this drain.
	if (!m_PendingStates.empty() && !m_bFlushingStates)
	{
		m_bFlushingStates = true;
		for (const PendingState& P : m_PendingStates)
		{
			TraceFrame* F = BeginFrame();
			F->m_Flags = TraceFrame::kFlagStateWrite;
			F->m_StartCycles = P.m_Cycle;
			F->m_Frame = P.m_FrameNo;
			F->m_StateKind = P.m_Kind;
			F->m_StateAddr = P.m_Addr;
			F->m_StateValue = P.m_Value;
			F->m_StateSource = P.m_Source;
			CommitFrame();
		}
		m_PendingStates.clear();
		m_bFlushingStates = false;
	}
}

void TraceWriter::NoteStateWrite(
	uint8_t Kind, uint16_t Addr, uint8_t Value, uint8_t Source, uint64_t Cycle, uint64_t FrameNo)
{
	if (m_CurrentProducerFrame != nullptr)
	{
		m_PendingStates.push_back({Cycle, FrameNo, Addr, Value, Kind, Source});
		return;
	}
	TraceFrame* F = BeginFrame();
	F->m_Flags = TraceFrame::kFlagStateWrite;
	F->m_StartCycles = Cycle;
	F->m_Frame = FrameNo;
	F->m_StateKind = Kind;
	F->m_StateAddr = Addr;
	F->m_StateValue = Value;
	F->m_StateSource = Source;
	CommitFrame();
}

void TraceWriter::MemWriteHook(void* User, uint16_t Addr, uint8_t Value, uint8_t PageTag)
{
	auto* Self = static_cast<TraceWriter*>(User);
	TraceFrame* F = Self->m_CurrentProducerFrame;
	if (F == nullptr)
	{
		return;
	}
	if (F->m_NumMemWrites < 4)
	{
		F->m_MemWrites[F->m_NumMemWrites].m_Addr = Addr;
		F->m_MemWrites[F->m_NumMemWrites].m_Value = Value;
		F->m_MemWrites[F->m_NumMemWrites].m_PageTag = PageTag;
		++F->m_NumMemWrites;
	}
}

void TraceWriter::Stop()
{
	if (!m_bRunning)
	{
		return;
	}

	m_bStop.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> L(m_Lock);
		m_CondNotEmpty.notify_all();
		m_CondNotFull.notify_all();
	}
	if (m_Thread.joinable())
	{
		m_Thread.join();
	}

	if (m_OutFile != nullptr)
	{
		// Patch the binary header's EndCycle + TotalRecordsHint + CRC if we
		// wrote any records AND the file is seekable. Drops cleanly when on
		// stdout / /dev/null / pipes (bSeekable = false) so the sentinel
		// stays and tools can detect "not finalised" downstream.
		//
		// We update the in-memory PendingHeaderBytes (which holds the same
		// bytes we initially wrote), patch the two fields + recompute the
		// CRC from that buffer, then fseek back to byte 0 and overwrite the
		// whole header in one fwrite. Reading bytes back from disk between
		// a write and CRC compute is hazardous under stdio buffering rules
		// (fseek mostly handles it, but "mostly" is the wrong word here).
		if (m_bBinaryMode && m_bHeaderWritten && m_bSeekable && m_BinaryRecordCount > 0)
		{
			using namespace BinaryTrace;
			uint8_t* HB = m_PendingHeaderBytes.data();
			const size_t HSize = m_PendingHeaderBytes.size();
			memcpy(HB + offsetof(Header, m_EndCycle), &m_BinaryLastCycle, sizeof(uint64_t));
			memcpy(HB + offsetof(Header, m_EndFrame), &m_BinaryLastFrame, sizeof(uint64_t));
			memcpy(HB + offsetof(Header, m_EndTimeNanos), &m_BinaryLastNanos, sizeof(uint64_t));
			memcpy(HB + offsetof(Header, m_TotalRecordsHint), &m_BinaryRecordCount, sizeof(uint64_t));
			const uint32_t Crc = Crc32(HB, HSize - 4);
			memcpy(HB + (HSize - 4), &Crc, sizeof(uint32_t));
			// Update the parent-CRC reference for the sidecar to the FINAL
			// CRC. The index file points at this trace, and the loader
			// verifies the .btr it pairs with carries this exact header CRC.
			m_ParentHeaderCrc = Crc;
			fflush(m_OutFile);
			if (fseek(m_OutFile, 0, SEEK_SET) == 0)
			{
				fwrite(HB, 1, HSize, m_OutFile);
			}
			else
			{
				m_bWriteFailed = true;
			}
		}
		if (fflush(m_OutFile) != 0 || ferror(m_OutFile))
		{
			m_bWriteFailed = true;
		}
		if (m_bOwnsFile && fclose(m_OutFile) != 0)
		{
			m_bWriteFailed = true;
		}
		// Flush the index sidecar AFTER the .btr is closed so the parent
		// file is on disk by the time anyone opens the .fidx and follows
		// its parent CRC reference.
		WriteIndexSidecar();
	}
	m_OutFile = nullptr;
	m_bRunning = false;
}

void TraceWriter::RunLoop()
{
	while (true)
	{
		uint64_t R = m_Read.load(std::memory_order_relaxed);
		uint64_t W = m_Write.load(std::memory_order_acquire);

		if (R == W)
		{
			if (m_bStop.load(std::memory_order_acquire))
			{
				// One more check before exit (producer may have raced).
				W = m_Write.load(std::memory_order_acquire);
				if (R == W)
				{
					return;
				}
			}
			else
			{
				std::unique_lock<std::mutex> L(m_Lock);
				m_CondNotEmpty.wait_for(L, std::chrono::milliseconds(5),
					[&]
					{
						return m_Read.load(std::memory_order_relaxed) < m_Write.load(std::memory_order_acquire) ||
							   m_bStop.load(std::memory_order_relaxed);
					});
				continue;
			}
		}

		while (R < W)
		{
			const TraceFrame& F = m_Ring[size_t(R & m_RingMask)];
			if (m_Analyzer != nullptr)
			{
				m_Analyzer->Feed(F);
			}
			if (m_OutFile != nullptr)
			{
				if (m_bBinaryMode)
				{
					FormatAndWriteBinary(F);
				}
				else
				{
					FormatAndWrite(F);
				}
			}
			++R;
		}
		m_Read.store(R, std::memory_order_release);

		// Allow producer to make progress if it was waiting on a full ring.
		std::lock_guard<std::mutex> L(m_Lock);
		m_CondNotFull.notify_one();
	}
}

void TraceWriter::FormatAndWrite(const TraceFrame& Frame)
{
	FormatTextLine(Frame, m_Options, m_Symbols, m_OutFile, m_LastEmittedFrame);
}

void TraceSnapshotPre(const Z80State& Cpu, const MemoryBus& Mem, const IoContext& Io, TraceFrame& InOutF)
{
	InOutF.m_StartCycles = Cpu.m_Cycles;
	InOutF.m_Frame = (Cpu.m_FrameIntPeriod > 0) ? (Cpu.m_Cycles / Cpu.m_FrameIntPeriod) : 0;
	// Emulated wall-clock at this cycle, in nanoseconds. Pure integer math
	// via the IoContext's Q20 nanos-per-cycle cache (updated in the NR_07
	// turbo callback). Previously this was a 3-double-op + cvttsd2si chain
	// showing 12% of total time in perf annotate; the int version is one
	// multiply + add + shift and disappears from the profile.
	const uint64_t DCycles = Cpu.m_Cycles - Io.m_LastTurboCheckpointCycle;
	InOutF.m_EmuWallNanos = Io.m_AccumulatedNanosAtCheckpoint + ((DCycles * Io.m_NanosPerCycleQ20) >> 20);
	InOutF.m_PrePC = Cpu.PC;
	InOutF.m_PreAF = Cpu.AF;
	InOutF.m_PreBC = Cpu.BC;
	InOutF.m_PreDE = Cpu.DE;
	InOutF.m_PreHL = Cpu.HL;
	InOutF.m_PreIX = Cpu.IX;
	InOutF.m_PreIY = Cpu.IY;
	InOutF.m_PreSP = Cpu.SP;
	InOutF.m_PreAFp = Cpu.AFp;
	InOutF.m_PreBCp = Cpu.BCp;
	InOutF.m_PreDEp = Cpu.DEp;
	InOutF.m_PreHLp = Cpu.HLp;
	InOutF.m_PreMemPtr = Cpu.m_MemPtr;
	InOutF.m_PreI = Cpu.I;
	InOutF.m_PreR = Cpu.R;
	InOutF.m_PreIFF1 = Cpu.IFF1;
	InOutF.m_PreIFF2 = Cpu.IFF2;
	InOutF.m_PreHalted = Cpu.m_bHalted ? 1 : 0;

	// A peek, not the CPU's read: kAccessFull would charge the Next's wait
	// states (and mark coverage) for these four bytes on every traced
	// instruction, so a traced run drifted from an untraced one by a few T a
	// step and took different interrupt timing - a crash reproduced untraced
	// never happened with --trace-out on.
	using A = MemoryBus::Access;
	InOutF.m_OpcodeBytes[0] = Mem.Rd<A::kAccessFast>(Cpu.PC);
	InOutF.m_OpcodeBytes[1] = Mem.Rd<A::kAccessFast>(uint16_t(Cpu.PC + 1));
	InOutF.m_OpcodeBytes[2] = Mem.Rd<A::kAccessFast>(uint16_t(Cpu.PC + 2));
	InOutF.m_OpcodeBytes[3] = Mem.Rd<A::kAccessFast>(uint16_t(Cpu.PC + 3));

	const MemoryBus::Page8KInfo P = Mem.Page8KAt(Cpu.PC);
	InOutF.m_PrePageTag = BinaryTrace::MakePageTag(P.m_bIsRom, P.m_Index);
}

void TraceSnapshotPost(const Z80State& Cpu, TraceFrame& InOutF)
{
	InOutF.m_EndCycles = Cpu.m_Cycles;
	InOutF.m_PostPC = Cpu.PC;
	InOutF.m_PostAF = Cpu.AF;
	InOutF.m_PostBC = Cpu.BC;
	InOutF.m_PostDE = Cpu.DE;
	InOutF.m_PostHL = Cpu.HL;
	InOutF.m_PostIX = Cpu.IX;
	InOutF.m_PostIY = Cpu.IY;
	InOutF.m_PostSP = Cpu.SP;
	InOutF.m_PostAFp = Cpu.AFp;
	InOutF.m_PostBCp = Cpu.BCp;
	InOutF.m_PostDEp = Cpu.DEp;
	InOutF.m_PostHLp = Cpu.HLp;
	InOutF.m_PostMemPtr = Cpu.m_MemPtr;
	InOutF.m_PostI = Cpu.I;
	InOutF.m_PostR = Cpu.R;
	InOutF.m_PostIFF1 = Cpu.IFF1;
	InOutF.m_PostIFF2 = Cpu.IFF2;
	InOutF.m_PostHalted = Cpu.m_bHalted ? 1 : 0;
}

// Binary record encoder. One call per TraceFrame; emits a single record
// using the format defined in trace_binary.h. The encoder is intentionally
// straightforward - no varint for fields that are usually small (PC, regs)
// because the worker thread has plenty of CPU budget and explicit fixed
// layout makes the reader easier to write later. Cycle and frame deltas
// DO use varint because those are usually 1 byte each.
void TraceWriter::FormatAndWriteBinary(const TraceFrame& Frame)
{
	using namespace BinaryTrace;

	// v4 video-state events are zero-width: they carry no register snapshot
	// and, crucially, they must NOT take part in frame bookkeeping. Boot-time
	// register initialisation arrives with cycle 0, and letting that drive the
	// frame counter emitted a FrameBoundary back to frame 0 mid-stream and
	// appended a non-monotonic .fidx entry, which made the whole sidecar fail
	// validation ("0 frames"). Frames belong to the instruction stream.
	if (Frame.m_Flags & TraceFrame::kFlagStateWrite)
	{
		// Nothing to anchor to before the header exists; a state write that
		// early is part of machine setup, and the header's Init* fields
		// already describe that starting state.
		if (!m_bHeaderWritten)
		{
			return;
		}

		thread_local std::vector<uint8_t> SBuf;
		SBuf.clear();
		// Never go backwards: an out-of-order event contributes a 0 delta
		// rather than a huge unsigned wrap.
		const uint64_t Delta =
			(Frame.m_StartCycles > m_BinaryLastCycle) ? (Frame.m_StartCycles - m_BinaryLastCycle) : 0;
		if (Frame.m_StateKind == 0)
		{
			PutU8(SBuf, kRecord_NextRegWrite);
			PutVarint(SBuf, Delta);
			PutU8(SBuf, uint8_t(Frame.m_StateAddr & 0xFF));
			PutU8(SBuf, Frame.m_StateValue);
			PutU8(SBuf, Frame.m_StateSource);
		}
		else
		{
			PutU8(SBuf, kRecord_PortWrite);
			PutVarint(SBuf, Delta);
			PutU16(SBuf, Frame.m_StateAddr);
			PutU8(SBuf, Frame.m_StateValue);
		}
		m_BinaryLastCycle += Delta;
		fwrite(SBuf.data(), 1, SBuf.size(), m_OutFile);
		++m_BinaryRecordCount;
		return;
	}

	if (!m_bHeaderWritten)
	{
		// First record: patch the header's StartCycle / StartFrame /
		// StartTimeNanos with the values from this record, then flush the
		// header. After this point the file has a stable layout the reader
		// can validate.
		m_BinaryStartCycle = Frame.m_StartCycles;
		m_BinaryStartFrame = Frame.m_Frame;
		m_BinaryStartNanos = Frame.m_EmuWallNanos;
		EmitBinaryHeader(Frame.m_StartCycles);
		m_BinaryLastCycle = Frame.m_StartCycles;
		m_BinaryLastFrame = Frame.m_Frame;
		m_BinaryLastNanos = Frame.m_EmuWallNanos;
		// Emit an explicit FrameBoundary right after the header so decoders
		// never have to read the header's StartFrame to know the frame number
		// of the first instruction record. (They still SHOULD read the header
		// for partial-window detection, but the stream is also self-sufficient.)
		EmitFrameBoundaryRecord(0 /*cycle delta from header - there's no
									 prior record*/
			,
			Frame.m_Frame);
		// Index entry for this starting frame. ByteOffset = position AFTER
		// the FrameBoundary marker, i.e., where the first Instr record
		// begins. Decoder seeking to this entry reads the Instr right away.
		AppendIndexEntry(Frame, FileTell64(m_OutFile));
	}

	// Reuse a single thread-local buffer to avoid per-record allocations.
	// Verbose-mode instruction with 4 mem writes fits in ~50 bytes.
	thread_local std::vector<uint8_t> Buf;
	Buf.clear();
	Buf.reserve(96);

	// If the dispatcher has advanced the frame counter, emit a
	// kRecord_FrameBoundary marker BEFORE the instruction record. The marker
	// carries its own cycle_delta (cycles from prior record to the frame
	// transition); the following Instr record's cycle_delta is then the
	// small remainder (often 0) from boundary to first instruction.
	if (Frame.m_Frame != m_BinaryLastFrame)
	{
		EmitFrameBoundaryRecord(Frame.m_StartCycles - m_BinaryLastCycle, Frame.m_Frame);
		AppendIndexEntry(Frame, FileTell64(m_OutFile));
		m_BinaryLastCycle = Frame.m_StartCycles;
		m_BinaryLastFrame = Frame.m_Frame;
	}

	// Pick the record tag. The dispatcher's Flag bits map to the v1 tag set;
	// anything we don't recognise falls back to plain Instr.
	uint8_t Tag = kRecord_Instr;
	if (Frame.m_Flags & TraceFrame::kFlagInvalid)
	{
		Tag = kRecord_InvalidOpcode;
	}
	else if (Frame.m_Flags & TraceFrame::kFlagResetTrap)
	{
		Tag = kRecord_RestartTrap;
	}
	else if (Frame.m_Flags & TraceFrame::kFlagInterrupt)
	{
		Tag = kRecord_Interrupt;
	}
	else if (Frame.m_Flags & TraceFrame::kFlagDma)
	{
		Tag = kRecord_DmaDrip;
	}
	PutU8(Buf, Tag);

	PutVarint(Buf, Frame.m_StartCycles - m_BinaryLastCycle);

	// Address column (PrePC + page tag).
	PutU16(Buf, Frame.m_PrePC);
	PutU8(Buf, Frame.m_PrePageTag);

	// Opcode bytes (4 fixed; decoder disassembles to find real length).
	PutBytes(Buf, Frame.m_OpcodeBytes, 4);

	// Delta-encoded register state. One bit per field that changed; emit
	// Post values for set bits only. The decoder maintains running state
	// initialised from the header's Init* fields and applies these deltas.
	uint16_t Mask = 0;
	Mask |= (Frame.m_PostPC != Frame.m_PrePC) ? kRegDiffBitDiffPc : 0;
	Mask |= (Frame.m_PostAF != Frame.m_PreAF) ? kRegDiffBitDiffAf : 0;
	Mask |= (Frame.m_PostBC != Frame.m_PreBC) ? kRegDiffBitDiffBc : 0;
	Mask |= (Frame.m_PostDE != Frame.m_PreDE) ? kRegDiffBitDiffDe : 0;
	Mask |= (Frame.m_PostHL != Frame.m_PreHL) ? kRegDiffBitDiffHl : 0;
	Mask |= (Frame.m_PostIX != Frame.m_PreIX) ? kRegDiffBitDiffIx : 0;
	Mask |= (Frame.m_PostIY != Frame.m_PreIY) ? kRegDiffBitDiffIy : 0;
	Mask |= (Frame.m_PostSP != Frame.m_PreSP) ? kRegDiffBitDiffSp : 0;
	if (m_Options.m_bVerbose)
	{
		// Verbose mode requires alt set to always be visible - emit even
		// when unchanged so the decoder's running state is refreshable from
		// any record. Negligible size cost; matches text-writer behaviour.
		Mask |= (kRegDiffBitDiffAFp | kRegDiffBitDiffBCp | kRegDiffBitDiffDEp | kRegDiffBitDiffHLp);
	}
	Mask |= (m_Options.m_bIncludeMemPtr && Frame.m_PostMemPtr != Frame.m_PreMemPtr) ? kRegDiffBitDiffMemPtr : 0;
	Mask |= (Frame.m_PostI != Frame.m_PreI) ? kRegDiffBitDiffI : 0;
	Mask |= (m_Options.m_bIncludeR && Frame.m_PostR != Frame.m_PreR) ? kRegDiffBitDiffR : 0;
	Mask |= (Frame.m_PostIFF1 != Frame.m_PreIFF1 || Frame.m_PostIFF2 != Frame.m_PreIFF2 ||
				Frame.m_PostHalted != Frame.m_PreHalted)
				? kRegDiffBitDiffIFFs
				: 0;

	PutU16(Buf, Mask);
	if (Mask & kRegDiffBitDiffPc)
	{
		PutU16(Buf, Frame.m_PostPC);
	}
	if (Mask & kRegDiffBitDiffAf)
	{
		PutU16(Buf, Frame.m_PostAF);
	}
	if (Mask & kRegDiffBitDiffBc)
	{
		PutU16(Buf, Frame.m_PostBC);
	}
	if (Mask & kRegDiffBitDiffDe)
	{
		PutU16(Buf, Frame.m_PostDE);
	}
	if (Mask & kRegDiffBitDiffHl)
	{
		PutU16(Buf, Frame.m_PostHL);
	}
	if (Mask & kRegDiffBitDiffIx)
	{
		PutU16(Buf, Frame.m_PostIX);
	}
	if (Mask & kRegDiffBitDiffIy)
	{
		PutU16(Buf, Frame.m_PostIY);
	}
	if (Mask & kRegDiffBitDiffSp)
	{
		PutU16(Buf, Frame.m_PostSP);
	}
	if (Mask & kRegDiffBitDiffAFp)
	{
		PutU16(Buf, Frame.m_PostAFp);
	}
	if (Mask & kRegDiffBitDiffBCp)
	{
		PutU16(Buf, Frame.m_PostBCp);
	}
	if (Mask & kRegDiffBitDiffDEp)
	{
		PutU16(Buf, Frame.m_PostDEp);
	}
	if (Mask & kRegDiffBitDiffHLp)
	{
		PutU16(Buf, Frame.m_PostHLp);
	}
	if (Mask & kRegDiffBitDiffMemPtr)
	{
		PutU16(Buf, Frame.m_PostMemPtr);
	}
	if (Mask & kRegDiffBitDiffI)
	{
		PutU8(Buf, Frame.m_PostI);
	}
	if (Mask & kRegDiffBitDiffR)
	{
		PutU8(Buf, Frame.m_PostR);
	}
	if (Mask & kRegDiffBitDiffIFFs)
	{
		PutU8(Buf, uint8_t((Frame.m_PostIFF1 & 1) | ((Frame.m_PostIFF2 & 1) << 1) | ((Frame.m_PostHalted & 1) << 7)));
	}

	PutU8(Buf, Frame.m_NumMemWrites);
	for (int IdxI = 0; IdxI < Frame.m_NumMemWrites; ++IdxI)
	{
		PutU16(Buf, Frame.m_MemWrites[IdxI].m_Addr);
		PutU8(Buf, Frame.m_MemWrites[IdxI].m_Value);
	}

	fwrite(Buf.data(), 1, Buf.size(), m_OutFile);
	m_BinaryLastCycle = Frame.m_StartCycles;
	m_BinaryLastNanos = Frame.m_EmuWallNanos;
	++m_BinaryRecordCount;
}

void TraceWriter::EmitFrameBoundaryRecord(uint64_t CycleDelta, uint64_t FrameNumber)
{
	uint8_t Buf[24];
	size_t Pos = 0;
	Buf[Pos++] = BinaryTrace::kRecord_FrameBoundary;
	// varint cycle_delta
	uint64_t V = CycleDelta;
	while (V >= 0x80)
	{
		Buf[Pos++] = uint8_t(V & 0x7F) | 0x80;
		V >>= 7;
	}
	Buf[Pos++] = uint8_t(V);
	// varint frame_number
	V = FrameNumber;
	while (V >= 0x80)
	{
		Buf[Pos++] = uint8_t(V & 0x7F) | 0x80;
		V >>= 7;
	}
	Buf[Pos++] = uint8_t(V);
	fwrite(Buf, 1, Pos, m_OutFile);
}

#endif // INSPECCY_TRACE_SUPPORT
