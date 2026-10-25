#pragma once
#include "CallGraphBuilder.h"
#include "Cursor.h"

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>
#include <QVector>

#include <trace/sld.h>
#include <trace/trace_binary.h>

#include <atomic>
#include <stdint.h>
#include <memory>

// Document model. Memory-maps the .btr and (when present) the .fidx
// sidecar; validates magic + version + CRC; exposes the in-memory index
// + cached per-frame derived metrics that the widgets paint from.
//
// Loading is constructor-cheap (just mmap + header parse). The expensive
// per-frame utilisation scan runs as a separate computeMetrics() call,
// suitable for QtConcurrent dispatch from MainWindow.

struct TraceModel : public QObject
{
	Q_OBJECT
  public:
	explicit TraceModel(QObject* Parent = nullptr);
	~TraceModel() override;

	// open() returns a human-readable error message on failure, or an
	// empty string on success. On success: m_Header is set, m_Index is
	// populated, m_BtrSize is the file's size, m_BtrMap points at the
	// mmap'd .btr.
	QString Open(const QString& BtrPath);
	bool IsLoaded() const
	{
		return m_BtrMap != nullptr;
	}

	// Computed once at open via QtConcurrent::run, or synchronously by
	// computeMetrics(). Length == m_Index.size() once populated.
	const QVector<float>& CpuUtilisationPerFrame() const
	{
		return m_CpuUtilisation;
	}
	void ComputeMetrics(); // synchronous; safe to call once on a worker

	// The same per-frame CPU time, split by where it was spent: main flow, or
	// one identified interrupt's service routine (see CpuSources.h).
	//
	// sourceIds() lists only the buckets that occur somewhere in this trace, in
	// ascending id order - which is also the order the overview stacks them,
	// main flow at the bottom. Main flow is always present.
	//
	// sourceUtilisation() is a flat row-major matrix, one row of
	// sourceIds().size() fractions per index entry. Each fraction is of that
	// frame's whole cycle span, so a row sums to cpuUtilisationPerFrame().
	const QVector<quint8>& SourceIds() const
	{
		return m_SourceIds;
	}
	const QVector<float>& SourceUtilisation() const
	{
		return m_SourceUtil;
	}
	float SourceUtilisationAt(int Frame, int Col) const
	{
		const int W = m_SourceIds.size();
		const int At = Frame * W + Col;
		return (W > 0 && Col >= 0 && Col < W && At >= 0 && At < m_SourceUtil.size()) ? m_SourceUtil[At] : 0.0f;
	}

	// Resolved video state at a cycle (.btr v4). Starts from the header's
	// initial state and applies every recorded change up to `cycle`, so it is
	// exact rather than inferred - the previous approach re-disassembled OUT
	// instructions and only recognised two of the eight OUT forms.
	struct VideoState
	{
		uint8_t m_Border = 0;
		uint8_t m_Port7FFD = 0; // bit 3 selects the displayed screen
		uint8_t m_ScldMode = 0; // Timex / ZX-Uno port $FF
		uint8_t m_EvoFf77 = 0;
		uint8_t m_DisplayRegs[BinaryTrace::kDisplayRegCount] = {};

		// NextReg by its real number; 0 for anything outside the display set.
		uint8_t NextReg(uint8_t Idx) const
		{
			const int Slot = BinaryTrace::DisplayRegSlot(Idx);
			return (Slot >= 0) ? m_DisplayRegs[Slot] : 0;
		}
	};
	VideoState VideoStateAtCycle(uint64_t Cycle) const;
	// How many video-change events the trace holds (0 = nothing recorded,
	// e.g. a run where the display was never touched).
	int VideoEventCount() const
	{
		return m_VideoEvents.size();
	}

	// Call graph reconstruction, built alongside per-frame metrics.
	const CallGraphBuilder& CallGraph() const
	{
		return m_CallGraph;
	}

	// Memory snapshots. Each entry is a 64KB byte array taken at the
	// frame-boundary record that corresponds to m_MemSnapIndexEntry[i].
	// Snapshot count is capped (~128) so even multi-GB traces stay under
	// 8MB of cached memory state.
	int MemSnapshotCount() const
	{
		return m_MemSnapshots.size();
	}
	const QByteArray& MemSnapshotAt(int IdxI) const
	{
		return m_MemSnapshots[IdxI];
	}
	int MemSnapshotIndexEntry(int IdxI) const
	{
		return m_MemSnapIndexEntry[IdxI];
	}
	uint8_t MemSnapshotBank7FFDAt(int IdxI) const
	{
		return m_MemSnapBank7FFD[IdxI];
	}

	// 128K-style $7FFD value at the given cycle. Starts from the nearest
	// snapshot's saved value and walks forward applying any OUT writes
	// up to cycle. Linear walk; per scrub is fine.
	uint8_t Bank7FFDAtCycle(uint64_t Cycle) const;

	// NextReg $50-$57 (8K-slot bank mapping on the Next) at the given cycle.
	// Returns array of 8 bytes; slot N is at array index N.
	QByteArray NextRegs5057AtCycle(uint64_t Cycle) const;
	bool IsNextMachine() const;
	bool IsTimexMachine() const;

	// Bank 7 (alternate ULA screen) contents at the given cycle. 16KB array;
	// only the first $1B00 bytes are meaningful for the ULA. Initially
	// all-zero: bank 7's pre-trace contents are not captured.
	QByteArray Bank7AtCycle(uint64_t Cycle) const;

	const BinaryTrace::Header* GetHeader() const
	{
		return m_Header;
	}
	// Nominal T-states per frame from the .btr header (0 = unknown / no
	// interrupts). This is the authoritative per-frame budget - used instead of
	// the .fidx index-entry cycle delta, which collapses to the whole trace when
	// there are few frame boundaries.
	uint32_t FrameIntPeriod() const
	{
		return m_Header != nullptr ? m_Header->m_FrameIntPeriod : 0;
	}
	const QVector<BinaryTrace::IndexEntry>& Index() const
	{
		return m_Index;
	}
	qint64 BtrSize() const
	{
		return m_BtrSize;
	}
	const uchar* BtrData() const
	{
		return m_BtrMap;
	}
	const QString& BtrPath() const
	{
		return m_BtrPath;
	}

	// Resolve a cycle into (Frame, ByteOffset, TimeNanos) via the index.
	// Clamps to the trace window if out of range.
	Cursor CursorAtCycle(uint64_t Cycle) const;
	Cursor CursorAtFrame(uint64_t Frame) const;

	// Allow the UI to interrupt a long parse. The flag is read between
	// major phases AND between index entries in the heaviest walk.
	void RequestCancelMetrics()
	{
		m_bCancelMetrics.store(true);
	}
	bool CancelRequested() const
	{
		return m_bCancelMetrics.load();
	}

	// SLD symbol-map access. open() auto-loads any paths embedded in the
	// header's SldRefs blob; missing paths are listed in unresolvedSldPaths()
	// for the UI to surface a "Locate symbols..." prompt. Manual addition via
	// loadSldFile() merges into the same map.
	const SldSymbolMap& Sld() const
	{
		return *m_Sld;
	}
	QStringList UnresolvedSldPaths() const
	{
		return m_UnresolvedSldPaths;
	}
	QString LoadSldFile(const QString& Path); // "" on success

  signals:
	void ProgressUpdate(int Percent, QString Status);
	void MetricsReady(bool bCancelled);
	void SldChanged(); // fired whenever the symbol map gains entries

  private:
	QString m_BtrPath;
	QFile m_BtrFile;
	QFile m_FidxFile;
	uchar* m_BtrMap = nullptr;
	qint64 m_BtrSize = 0;
	const BinaryTrace::Header* m_Header = nullptr;

	QVector<BinaryTrace::IndexEntry> m_Index;
	QVector<float> m_CpuUtilisation; // one entry per index entry
	QVector<quint8> m_SourceIds; // buckets present, ascending
	// v4 video-change events, in cycle order, collected during computeMetrics.
	struct VideoEvent
	{
		uint64_t m_Cycle;
		uint16_t m_Addr;
		uint8_t m_Value;
		uint8_t m_Kind;
	};
	QVector<VideoEvent> m_VideoEvents;
	QVector<float> m_SourceUtil; // index entries x m_SourceIds.size()
	CallGraphBuilder m_CallGraph;
	QVector<QByteArray> m_MemSnapshots; // 64KB each - the CPU's view at snapshot
	QVector<int> m_MemSnapIndexEntry;
	QVector<uint8_t> m_MemSnapBank7FFD; // $7FFD value at each snapshot
	QVector<QByteArray> m_MemSnapNextRegs; // 8 bytes each - NextRegs $50-$57
	QVector<QByteArray> m_MemSnapBank7; // 16KB each - bank 7 contents (alt
										// ULA screen). Initially zeroed;
										// populated only by writes that
										// happened while bank 7 was
										// mapped at $C000.

	std::atomic<bool> m_bCancelMetrics{false};

	std::unique_ptr<SldSymbolMap> m_Sld; // never null (constructed in ctor)
	QStringList m_UnresolvedSldPaths;
};
