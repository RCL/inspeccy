#pragma once
#if INSPECCY_TRACE_SUPPORT

#include <atomic>
#include <condition_variable>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// TraceFrame + TraceFormatOptions + FormatTextLine live in the shared
// trace/ library so the standalone binary->text decoder (which doesn't link
// against the emulator) can use the same definitions and the same text
// formatter as the live writer thread does.
#include <trace/trace_binary.h>
#include <trace/trace_frame.h>
#include <trace/text_format.h>

struct Z80State;
struct MemoryBus;
struct SldSymbolMap;
struct PeepholeAnalyzer;
enum MachineModel : int;

// Extra per-run context needed when opening a binary trace. The text format
// doesn't need any of these because they don't appear in its output; the
// binary header captures them so a later reader has the full provenance.
struct BinaryOpenContext
{
	const Z80State* m_InitCpu = nullptr; // captures initial register state
	const MemoryBus* m_InitMem = nullptr; // captures initial NR_50..57
	double m_BaseMhz = 3.5;
	uint64_t m_FrameIntPeriod = 0; // nominal T-states/frame (0 = none)
	MachineModel m_Machine = MachineModel(0);
	bool m_bTrdosActive = false;
	// Newline-joined .sld.txt paths; serialised into the .btr header's
	// SldRefs blob so downstream tools (notably inspeccy) can
	// auto-load symbols without needing the original CLI re-typed.
	std::vector<std::string> m_SldPaths;
};

struct TraceWriter final
{
  public:
	TraceWriter();
	~TraceWriter();

	// Path "-" or "" or "stdout" -> stdout. Otherwise opens the named file.
	// Symbols (optional) is consulted by the formatter to add a "Symbol" column
	// between the address and mnemonic when a (page, address) hit is found.
	// Argc/Argv (optional) are used to emit a "# inspeccy ..." invocation
	// header at the top of the trace.
	bool Open(const char* Path, const TraceFormatOptions& Options, size_t QueueBytes,
		const SldSymbolMap* Symbols = nullptr, int Argc = 0, char** Argv = nullptr);

	// Opens in binary mode (".btr" format - see trace_binary.h). The full
	// header is written when the first record arrives so it can capture the
	// actual start cycle; EndCycle is patched on Stop() via fseek. If the
	// output isn't seekable (stdout, /dev/null, pipes) the EndCycle stays at
	// the sentinel value, and downstream tools treat the trace as truncated.
	//
	// IndexPath: where to write the .fidx frame-index sidecar at Stop(). Pass
	// nullptr or an empty string to skip index emission entirely (e.g., for
	// the perf benchmark with --trace-out-bin /dev/null where we don't want
	// a "/dev/null.fidx" artefact to materialise). Otherwise write goes to
	// the named path.
	bool OpenBinary(const char* Path, const TraceFormatOptions& Options, size_t QueueBytes, const SldSymbolMap* Symbols,
		int Argc, char** Argv, const BinaryOpenContext& Ctx, const char* IndexPath);

	// Producer interface. Always returns a valid slot; if the queue is full,
	// blocks until the writer thread drains a slot.
	TraceFrame* BeginFrame();

	// Publishes the slot returned by BeginFrame.
	void CommitFrame();

	// Attach a peephole analyzer. Set before Open*; every committed frame is
	// fed to it on the consumer thread, in addition to whatever file output is
	// configured. With OpenAnalysisOnly there is no file output at all.
	void SetPeepholeAnalyzer(PeepholeAnalyzer* A)
	{
		m_Analyzer = A;
	}

	// Opens the producer/consumer machinery with NO output file: the analyzer
	// is the only consumer. Lets --optimize-out run without also paying for a
	// trace on disk, while reusing the proven ring + per-instruction snapshot.
	bool OpenAnalysisOnly(const TraceFormatOptions& Options, size_t QueueBytes);

	// Record a video-state change (.btr v4): NextReg (Kind 0) or I/O port
	// (Kind 1). Safe to call at ANY point, including from inside an
	// instruction's fill window - which is the normal case, since an OUT is
	// itself an instruction and the copper runs mid-instruction.
	//
	// It must not simply call BeginFrame: the ring is single-producer with one
	// open slot, so a nested BeginFrame hands back the SAME slot and zeroes the
	// half-filled instruction frame under it. That corrupted the instruction
	// record (frame/cycle reverted to 0) and appended a non-monotonic .fidx
	// entry, which made the sidecar fail validation. Events raised while a
	// frame is open are queued and flushed right after it commits.
	void NoteStateWrite(uint8_t Kind, uint16_t Addr, uint8_t Value, uint8_t Source, uint64_t Cycle, uint64_t FrameNo);

	// Stops the writer thread, drains the queue, flushes the output.
	// Idempotent.
	void Stop();
	// Read after Stop(), which joins the consumer before checking stdio errors.
	bool HasWriteFailed() const
	{
		return m_bWriteFailed;
	}

	// Memory-write hook entry point; registered into MemoryBus::OnWrite.
	static void MemWriteHook(void* User, uint16_t Addr, uint8_t Value, uint8_t PageTag);

	bool IsActive() const
	{
		return m_bRunning;
	}

  private:
	void RunLoop();
	void FormatAndWrite(const TraceFrame& Frame);
	void FormatAndWriteBinary(const TraceFrame& Frame);
	void EmitBinaryHeader(uint64_t StartCycle);
	void EmitFrameBoundaryRecord(uint64_t CycleDelta, uint64_t FrameNumber);
	void AppendIndexEntry(const TraceFrame& Frame, uint64_t ByteOffset);
	void WriteIndexSidecar();

	FILE* m_OutFile = nullptr;
	bool m_bOwnsFile = false;
	bool m_bWriteFailed = false;
	bool m_bRunning = false;

	TraceFormatOptions m_Options;
	const SldSymbolMap* m_Symbols = nullptr;
	uint64_t m_LastEmittedFrame = UINT64_MAX; // for inter-frame divider emission

	// Binary-mode state. Inactive in text mode (bBinaryMode = false).
	bool m_bBinaryMode = false;
	PeepholeAnalyzer* m_Analyzer = nullptr;
	bool m_bHeaderWritten = false; // deferred to first record
	bool m_bSeekable = false; // can we fseek to update EndCycle?
	uint64_t m_BinaryLastCycle = 0; // for cycle-delta encoding
	uint64_t m_BinaryLastFrame = 0; // tracked for FrameBoundary emission
	uint64_t m_BinaryLastNanos = 0; // last seen Frame.EmuWallNanos
	uint64_t m_BinaryStartCycle = 0; // captured at first record (header patch)
	uint64_t m_BinaryStartFrame = 0;
	uint64_t m_BinaryStartNanos = 0;
	uint64_t m_BinaryRecordCount = 0;
	// .fidx sidecar state - built incrementally in memory, flushed in Stop().
	// One entry per FrameBoundary emission. Disabled by passing an empty
	// path / nullptr to OpenBinary's IndexPath; or via the user's
	// `--no-trace-index` flag (which translates to empty path in main.cpp).
	bool m_bBuildIndex = false;
	std::string m_IndexFilePath;
	uint32_t m_ParentHeaderCrc = 0; // captured at header finalisation;
									// written into .fidx so a stray
									// index can be linked back to a
									// missing .btr
	std::vector<uint8_t> m_IndexEntryBytes; // raw IndexEntry array; grows
											// one entry per frame transition
	uint64_t m_HeaderEndOffset = 0; // file offset just after the header
	// The deferred header content: cached at OpenBinary() so the worker thread
	// can emit it without re-fetching from Cpu/Mem (which the worker thread
	// doesn't own and which would race otherwise).
	std::vector<uint8_t> m_PendingHeaderBytes;

	// Producer-mutated index of the slot currently owned by the producer
	// (set by BeginFrame, published by CommitFrame).
	TraceFrame* m_CurrentProducerFrame = nullptr;

	// Producer-thread only. Video-state events raised while an instruction
	// frame is open, flushed by CommitFrame once that frame is published.
	struct PendingState
	{
		uint64_t m_Cycle;
		uint64_t m_FrameNo;
		uint16_t m_Addr;
		uint8_t m_Value;
		uint8_t m_Kind;
		uint8_t m_Source;
	};
	std::vector<PendingState> m_PendingStates;
	bool m_bFlushingStates = false;

	// Ring buffer.
	std::vector<TraceFrame> m_Ring;
	size_t m_RingMask = 0;
	std::atomic<uint64_t> m_Write{0};
	std::atomic<uint64_t> m_Read{0};

	std::mutex m_Lock;
	std::condition_variable m_CondNotFull;
	std::condition_variable m_CondNotEmpty;
	std::thread m_Thread;
	std::atomic<bool> m_bStop{false};
};

struct IoContext;

// Helper used by the dispatcher to fill a pre-state snapshot from a Z80State.
// IoContext is consulted for the emulated wall-clock time (AccumulatedSeconds
// + current-MHz-adjusted delta since the last turbo checkpoint).
void TraceSnapshotPre(const Z80State& Cpu, const MemoryBus& Mem, const IoContext& Io, TraceFrame& InOutFrame);

// Helper used by the dispatcher to fill a post-state snapshot from a Z80State.
void TraceSnapshotPost(const Z80State& Cpu, TraceFrame& InOutFrame);

#endif // INSPECCY_TRACE_SUPPORT
