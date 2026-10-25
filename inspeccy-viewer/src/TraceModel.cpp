#include "TraceModel.h"

#include "CpuSources.h"
#include "RecordWalker.h"

#include <QFileInfo>

#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <iterator>

namespace
{

// True iff Path names a regular file on the host filesystem. Refusing to
// mmap FIFOs / sockets / character devices (Linux /dev/zero, Windows CON /
// NUL) is a DoS guard: QFile::map() on an unbounded device would map a huge
// sparse region, and on a FIFO/pipe read() would block. QFileInfo::isFile()
// is the portable, Unicode-safe classifier - true only for a regular file
// (it follows symlinks, matching the old stat() path), false for directories,
// devices, pipes and non-existent paths, identically on every platform. The
// prior code used ::stat(toLocal8Bit()) which relied on POSIX S_ISREG (absent
// on MSVC) and mangled non-Latin paths through the ANSI CRT.
bool IsRegularFile(const QString& Path)
{
	return QFileInfo(Path).isFile();
}

} // namespace

TraceModel::TraceModel(QObject* Parent) : QObject(Parent), m_Sld(std::make_unique<SldSymbolMap>())
{
}

QString TraceModel::LoadSldFile(const QString& Path)
{
	std::string Err;
	if (!m_Sld->LoadFile(Path.toLocal8Bit().constData(), Err))
	{
		return QString::fromStdString(Err.empty() ? std::string("LoadFile reported failure") : Err);
	}
	m_UnresolvedSldPaths.removeAll(Path);
	emit SldChanged();
	return QString();
}

TraceModel::~TraceModel()
{
	if (m_BtrMap != nullptr)
	{
		m_BtrFile.unmap(m_BtrMap);
		m_BtrMap = nullptr;
	}
	// QFile dtor closes the handle, but be explicit in destruction order
	// so a future maintainer doesn't have to chase whether m_BtrFile is
	// still owned by the QObject parent / lambda capture / etc.
	if (m_BtrFile.isOpen())
	{
		m_BtrFile.close();
	}
	if (m_FidxFile.isOpen())
	{
		m_FidxFile.close();
	}
}

QString TraceModel::Open(const QString& BtrPath)
{
	using namespace BinaryTrace;

	// Always reset member state at the top - previously skipped on
	// first-time open after a prior failed open, leaving stale handles.
	if (m_BtrMap != nullptr)
	{
		m_BtrFile.unmap(m_BtrMap);
		m_BtrMap = nullptr;
	}
	if (m_BtrFile.isOpen())
	{
		m_BtrFile.close();
	}
	if (m_FidxFile.isOpen())
	{
		m_FidxFile.close();
	}
	m_Index.clear();
	m_CpuUtilisation.clear();
	m_SourceIds.clear();
	m_SourceUtil.clear();
	m_Header = nullptr;
	m_BtrSize = 0;

	m_BtrPath = BtrPath;

	// Refuse non-regular files BEFORE QFile::open() - mapping /dev/zero
	// would allocate a sparse mapping of effectively unbounded size, and
	// reading a FIFO would block forever. Refuse symlink-to-directory,
	// sockets, etc. for the same reason.
	if (!IsRegularFile(BtrPath))
	{
		return QStringLiteral("not a regular file: %1").arg(BtrPath);
	}

	m_BtrFile.setFileName(BtrPath);
	if (!m_BtrFile.open(QIODevice::ReadOnly))
	{
		return QStringLiteral("cannot open %1: %2").arg(BtrPath, m_BtrFile.errorString());
	}
	m_BtrSize = m_BtrFile.size();
	// Minimum legal trace: fixed Header + CliArgsLen(u16) + SldRefsLen(u16) + CRC(u32) = sizeof(Header) + 8.
	if (m_BtrSize < qint64(sizeof(Header) + 8))
	{
		return QStringLiteral("file too small to be a .btr (%1 bytes; need at least %2)")
			.arg(m_BtrSize)
			.arg(qint64(sizeof(Header) + 8));
	}

	m_BtrMap = m_BtrFile.map(0, m_BtrSize);
	if (m_BtrMap == nullptr)
	{
		return QStringLiteral("mmap failed: %1").arg(m_BtrFile.errorString());
	}

	// Header validation.
	const Header* H = reinterpret_cast<const Header*>(m_BtrMap);
	if (H->m_Magic != kMagic)
	{
		return QStringLiteral("not a .btr file (magic = 0x%1)").arg(H->m_Magic, 8, 16, QChar('0'));
	}
	if (H->m_Version != kVersion)
	{
		return QStringLiteral("unsupported .btr version %1 (this viewer expects %2)").arg(H->m_Version).arg(kVersion);
	}
	// Sanity: blob lengths fit in file before the CRC.
	const qint64 FixedEnd = qint64(sizeof(Header));
	if (m_BtrSize < FixedEnd + 2)
	{
		return QStringLiteral("truncated header (no CliArgs length)");
	}
	quint16 CliLen = 0;
	memcpy(&CliLen, m_BtrMap + FixedEnd, 2);
	const qint64 SldLenOff = FixedEnd + 2 + CliLen;
	if (m_BtrSize < SldLenOff + 2)
	{
		return QStringLiteral("truncated header (CliArgs runs past EOF)");
	}
	quint16 SldLen = 0;
	memcpy(&SldLen, m_BtrMap + SldLenOff, 2);
	const qint64 CrcOff = SldLenOff + 2 + SldLen;
	if (m_BtrSize < CrcOff + 4)
	{
		return QStringLiteral("truncated header (SldRefs runs past EOF)");
	}
	// CRC validation: covers all bytes from offset 0 up to crcOff.
	quint32 StoredCrc = 0;
	memcpy(&StoredCrc, m_BtrMap + CrcOff, 4);
	const quint32 Computed = Crc32(m_BtrMap, size_t(CrcOff));
	if (Computed != StoredCrc)
	{
		return QStringLiteral("header CRC32 mismatch (stored 0x%1, computed 0x%2)")
			.arg(StoredCrc, 8, 16, QChar('0'))
			.arg(Computed, 8, 16, QChar('0'));
	}

	m_Header = H;

	// Auto-load SLD paths from the header's SldRefs blob. The blob is
	// newline-separated. Missing files go on m_UnresolvedSldPaths so the
	// UI can surface a "Locate symbols..." affordance.
	m_UnresolvedSldPaths.clear();
	// Re-construct SldSymbolMap so a previous trace's symbols don't leak in.
	m_Sld = std::make_unique<SldSymbolMap>();
	if (SldLen > 0)
	{
		const QByteArray Blob(reinterpret_cast<const char*>(m_BtrMap + SldLenOff + 2), SldLen);
		for (const QByteArray& Line : Blob.split('\n'))
		{
			if (Line.isEmpty())
			{
				continue;
			}
			const QString Path = QString::fromLocal8Bit(Line);
			std::string Err;
			if (!m_Sld->LoadFile(Path.toLocal8Bit().constData(), Err))
			{
				m_UnresolvedSldPaths.append(Path);
				qInfo("inspeccy-viewer: SLD path from header not loadable: %s", qPrintable(Path));
			}
		}
	}

	// .fidx sidecar (optional). Parse what we can; report SPECIFIC reasons
	// when something doesn't validate so the user knows whether to
	// re-emit the trace or rebuild the index.
	const QString FidxPath = BtrPath + QStringLiteral(".fidx");
	m_FidxFile.setFileName(FidxPath);
	if (!IsRegularFile(FidxPath))
	{
		// Either missing or wrong file type. Common case - quiet log.
		qInfo("inspeccy-viewer: no .fidx sidecar at %s; "
			  "navigation will be slower.",
			qPrintable(FidxPath));
	}
	else if (!m_FidxFile.open(QIODevice::ReadOnly))
	{
		qWarning("inspeccy-viewer: .fidx exists but can't be opened: %s", qPrintable(m_FidxFile.errorString()));
	}
	else
	{
		const qint64 FidxSize = m_FidxFile.size();
		QByteArray Bytes;
		if (FidxSize < qint64(sizeof(IndexHeader)))
		{
			qWarning(
				"inspeccy-viewer: .fidx too small (%lld bytes; need >= %zu)", (long long)FidxSize, sizeof(IndexHeader));
		}
		else if ((Bytes = m_FidxFile.readAll()).size() != FidxSize)
		{
			qWarning("inspeccy-viewer: .fidx short read");
		}
		else
		{
			const IndexHeader* kIH = reinterpret_cast<const IndexHeader*>(Bytes.constData());
			const auto Fail = [&](const char* Why)
			{
				qWarning("inspeccy-viewer: .fidx rejected - %s", Why);
			};
			if (kIH->m_Magic != kIndexMagic)
			{
				Fail("magic mismatch");
			}
			else if (kIH->m_Version != kVersion)
			{
				Fail("version mismatch");
			}
			else if (kIH->m_ParentMagic != kMagic)
			{
				Fail("ParentMagic doesn't reference an INTB trace");
			}
			else if (kIH->m_ParentHeaderCrc != StoredCrc)
			{
				Fail("ParentHeaderCrc doesn't match this .btr - stale .fidx?");
			}
			else
			{
				const qint64 EntriesStart = sizeof(IndexHeader);
				const qint64 Expected = EntriesStart + qint64(kIH->m_EntryCount) * qint64(sizeof(IndexEntry));
				if (FidxSize != Expected)
				{
					qWarning("inspeccy-viewer: .fidx size %lld != expected %lld "
							 "(%u entries x %zu + %zu header)",
						(long long)FidxSize, (long long)Expected, unsigned(kIH->m_EntryCount), sizeof(IndexEntry),
						sizeof(IndexHeader));
				}
				else
				{
					// Read entries provisionally, then sanity-check.
					QVector<IndexEntry> Tentative(int(kIH->m_EntryCount));
					memcpy(Tentative.data(), Bytes.constData() + EntriesStart,
						size_t(kIH->m_EntryCount) * sizeof(IndexEntry));
					bool bSane = true;
					uint32_t PrevFrame = 0;
					uint64_t PrevCycle = 0;
					uint64_t PrevOff = uint64_t(CrcOff + 4); // header end
					for (int IdxI = 0; bSane && IdxI < Tentative.size(); ++IdxI)
					{
						const auto& E = Tentative[IdxI];
						// Each entry's byte offset must point into the .btr
						// body (post-header). Monotonic forward.
						if (E.m_FirstByteOffset < PrevOff || E.m_FirstByteOffset >= uint64_t(m_BtrSize))
						{
							qWarning("inspeccy-viewer: .fidx entry %d has bad "
									 "FirstByteOffset %llu (file size %lld)",
								IdxI, (unsigned long long)E.m_FirstByteOffset, (long long)m_BtrSize);
							bSane = false;
							break;
						}
						if (IdxI > 0)
						{
							if (E.m_FrameNumber < PrevFrame || E.m_StartCycle < PrevCycle)
							{
								qWarning("inspeccy-viewer: .fidx entry %d not "
										 "monotonic (frame %u < prev %u, "
										 "or cycle %llu < prev %llu)",
									IdxI, E.m_FrameNumber, PrevFrame, (unsigned long long)E.m_StartCycle,
									(unsigned long long)PrevCycle);
								bSane = false;
								break;
							}
						}
						PrevFrame = E.m_FrameNumber;
						PrevCycle = E.m_StartCycle;
						PrevOff = E.m_FirstByteOffset;
					}
					if (bSane)
					{
						m_Index = std::move(Tentative);
					}
				}
			}
		}
	}
	if (m_Index.isEmpty())
	{
		qInfo("inspeccy-viewer: trace loaded without usable index - "
			  "cursor seek and OverviewWidget will be limited.");
	}

	return QString(); // success
}

void TraceModel::ComputeMetrics()
{
	using namespace BinaryTrace;

	m_bCancelMetrics.store(false);
	m_CpuUtilisation.clear();
	m_SourceIds.clear();
	m_SourceUtil.clear();
	m_VideoEvents.clear();
	if (m_Header == nullptr || m_Index.isEmpty() || m_BtrMap == nullptr)
	{
		emit MetricsReady(false);
		return;
	}
	m_CpuUtilisation.resize(m_Index.size());

	// Per-frame time split by source, accumulated in a fixed 16-wide row and
	// compacted to the buckets actually used once the walk is done.
	QVector<float> WideUtil(m_Index.size() * CpuSource::kCount, 0.0f);

	emit ProgressUpdate(0, tr("Computing per-frame utilisation..."));

	const uint8_t* Base = m_BtrMap;

	// Per-frame walk: between this entry's FirstByteOffset and the next
	// entry's, accumulate CPU-active cycles using the plan's "gap cap 100"
	// heuristic - any inter-record gap > 100 cycles is overwhelmingly a
	// HALT or DMA-stall window, so cap each gap's contribution at 100.
	// Average instructions are 4-23 cycles; 100 is ~5x that.
	//
	// Progress: 0% -> 33% across this loop. Cancel check fires per entry -
	// O(index.size()) checks, plenty fine even for 21k-frame traces.
	const int TotalEntries = m_Index.size();
	int LastPctReported = -1;

	// Interrupt-context stack, deliberately carried ACROSS frames: an ISR that
	// straddles a frame boundary must keep being charged to its own source. A
	// context closes when SP rises back above the value it had just after the
	// interrupt pushed its return address - the same test CallGraphBuilder uses,
	// which handles RET, RETI/RETN and jumping out of a handler alike.
	struct IntCtx
	{
		int m_Source;
		uint16_t m_SpAtAccept;
	};
	IntCtx CtxStack[16];
	int CtxDepth = 0;
	// Bucket that owned the PREVIOUS record. A record's cost is the gap to the
	// NEXT record, so attribution always lags one record - which is what makes
	// an ISR's final RET count against the ISR rather than its caller.
	int CtxPrev = CpuSource::kMain;
	uint16_t RunSP = m_Index.first().SP;

	for (int IdxI = 0; IdxI < m_Index.size(); ++IdxI)
	{
		if (m_bCancelMetrics.load())
		{
			emit MetricsReady(true);
			return;
		}
		const int Pct = TotalEntries > 0 ? (IdxI * 33) / TotalEntries : 0;
		if (Pct != LastPctReported)
		{
			emit ProgressUpdate(Pct, QString());
			LastPctReported = Pct;
		}
		const auto& E = m_Index[IdxI];

		// End-of-frame byte boundary: next entry's start, or EOF for the
		// tail frame. We rely on the .fidx invariant that FirstByteOffset
		// is monotonic and bounded (validated at open()).
		const qint64 StartByte = qint64(E.m_FirstByteOffset);
		const qint64 EndByte = (IdxI + 1 < m_Index.size()) ? qint64(m_Index[IdxI + 1].m_FirstByteOffset) : m_BtrSize;

		// End-of-frame cycle boundary: next entry's StartCycle, or the
		// header's EndCycle if the trace was finalised, else the last
		// record cycle we observe in the walk.
		uint64_t EndCycle = E.m_StartCycle;
		bool bEndCycleKnown = false;
		if (IdxI + 1 < m_Index.size())
		{
			EndCycle = m_Index[IdxI + 1].m_StartCycle;
			bEndCycleKnown = true;
		}
		else if (m_Header->m_EndCycle != kSentinel)
		{
			EndCycle = m_Header->m_EndCycle;
			bEndCycleKnown = true;
		}

		if (StartByte >= EndByte || StartByte < 0 || EndByte > m_BtrSize)
		{
			m_CpuUtilisation[IdxI] = 0.0f;
			continue;
		}

		const uint8_t* Cursor = Base + StartByte;
		const uint8_t* Eof = Base + EndByte;

		uint64_t RunCycle = E.m_StartCycle;
		uint64_t RunFrame = E.m_FrameNumber;
		uint64_t PrevCycle = E.m_StartCycle;
		uint64_t Active = 0;
		uint64_t LastRecCycle = E.m_StartCycle;
		uint64_t Bucket[CpuSource::kCount] = {};

		for (;;)
		{
			ViewerWalk::DecodedRecord Rec;
			if (!ViewerWalk::DecodeOne(Cursor, Eof, RunCycle, RunFrame, Rec))
			{
				// EOF or corrupt record - accept what we've accumulated so
				// far. A truncated tail frame just gets a partial picture.
				break;
			}
			if (ViewerWalk::IsStateRecord(Rec.m_Tag))
			{
				// Collect rather than discard: this is the one full walk of the
				// record stream, so the video timeline is built here for free.
				m_VideoEvents.append(
					{Rec.m_Cycle, Rec.m_StateAddr, Rec.m_StateValue, uint8_t(Rec.m_Tag == kRecord_PortWrite ? 1 : 0)});
				continue;
			}
			// Only "instr-like" tags consume CPU cycles. FrameBoundary
			// contributes no active time (it's a zero-width landmark).
			if (Rec.m_Tag != kRecord_FrameBoundary)
			{
				const uint64_t Gap = Rec.m_Cycle > PrevCycle ? Rec.m_Cycle - PrevCycle : 0;
				const uint64_t Capped = std::min<uint64_t>(Gap, 100);
				Active += Capped;
				Bucket[CtxPrev] += Capped;
				LastRecCycle = Rec.m_Cycle;

				if (Rec.m_Tag == kRecord_Interrupt)
				{
					// Diff_SP on an acceptance record is already the post-push
					// value, so it is the watermark the matching return crosses.
					if (Rec.m_Mask & kRegDiffBitDiffSp)
					{
						RunSP = Rec.m_PostSP;
					}
					if (CtxDepth < int(std::size(CtxStack)))
					{
						CtxStack[CtxDepth].m_Source = CpuSource::FromIntAck(Rec.m_Opcodes);
						CtxStack[CtxDepth].m_SpAtAccept = RunSP;
						++CtxDepth;
					}
					// Charge the acknowledge cycles to the handler being entered
					// rather than to whatever it interrupted.
					CtxPrev = CtxStack[CtxDepth - 1].m_Source;
				}
				else
				{
					// The record runs in whatever context is open at its entry...
					CtxPrev = (CtxDepth > 0) ? CtxStack[CtxDepth - 1].m_Source : CpuSource::kMain;
					// ...and only then may its own SP effect close that context.
					if (Rec.m_Mask & kRegDiffBitDiffSp)
					{
						RunSP = Rec.m_PostSP;
						while (CtxDepth > 0 && RunSP > CtxStack[CtxDepth - 1].m_SpAtAccept)
						{
							--CtxDepth;
						}
					}
				}
			}
			PrevCycle = Rec.m_Cycle;
		}

		if (!bEndCycleKnown)
		{
			EndCycle = LastRecCycle;
		}

		// Use the recorded span: turbo changes the cycles per video frame,
		// and an index entry may cover a partial frame or several frames.
		// The header's initial period would inflate 28 MHz use by up to 8x.
		const uint64_t Total = EndCycle > E.m_StartCycle ? EndCycle - E.m_StartCycle : 0;
		float U = (Total > 0) ? float(double(Active) / double(Total)) : 0.0f;
		if (U < 0.0f)
		{
			U = 0.0f;
		}
		if (U > 1.0f)
		{
			U = 1.0f;
		}
		m_CpuUtilisation[IdxI] = U;

		if (Total > 0)
		{
			float* Row = WideUtil.data() + qsizetype(IdxI) * CpuSource::kCount;
			for (int S = 0; S < CpuSource::kCount; ++S)
			{
				Row[S] = float(double(Bucket[S]) / double(Total));
			}
		}
	}

	if (m_bCancelMetrics.load())
	{
		emit MetricsReady(true);
		return;
	}

	// Compact the 16-wide accumulator down to the buckets this trace actually
	// uses. Main flow is kept even when a frame never leaves an ISR, so the
	// legend and the stacking order never change shape mid-trace.
	{
		bool bUsed[CpuSource::kCount] = {};
		bUsed[CpuSource::kMain] = true;
		for (qsizetype K = 0; K < WideUtil.size(); ++K)
		{
			if (WideUtil[K] > 0.0f)
			{
				bUsed[K % CpuSource::kCount] = true;
			}
		}
		for (int S = 0; S < CpuSource::kCount; ++S)
		{
			if (bUsed[S])
			{
				m_SourceIds.append(quint8(S));
			}
		}
		const int W = m_SourceIds.size();
		m_SourceUtil.resize(qsizetype(m_Index.size()) * W);
		for (int IdxI = 0; IdxI < m_Index.size(); ++IdxI)
		{
			const float* Src = WideUtil.constData() + qsizetype(IdxI) * CpuSource::kCount;
			float* Dst = m_SourceUtil.data() + qsizetype(IdxI) * W;
			for (int C = 0; C < W; ++C)
			{
				Dst[C] = Src[m_SourceIds[C]];
			}
		}

		// Whole-trace totals, for checking the split against something you can
		// compute independently (an ISR's length times its interrupt count).
		// Set INSPECCY_OVERVIEW_DEBUG to a file path - the viewer is a GUI
		// subsystem binary on Windows, so qInfo() has nowhere to go.
		if (const char* DbgPath = getenv("INSPECCY_OVERVIEW_DEBUG"))
		{
			if (FILE* F = fopen(DbgPath, "w"))
			{
				double Grand = 0.0;
				for (qsizetype K = 0; K < m_SourceUtil.size(); ++K)
				{
					Grand += double(m_SourceUtil[K]);
				}
				fprintf(F, "overview: %d source buckets over %d frames\n", W, int(m_Index.size()));
				for (int C = 0; C < W; ++C)
				{
					double Sum = 0.0;
					for (int IdxI = 0; IdxI < m_Index.size(); ++IdxI)
					{
						Sum += double(m_SourceUtil[qsizetype(IdxI) * W + C]);
					}
					fprintf(F, "  id %2d %-34s %6.2f%% of busy CPU, mean %5.1f%% of a frame\n", int(m_SourceIds[C]),
						qPrintable(CpuSource::Name(m_SourceIds[C])), Grand > 0.0 ? 100.0 * Sum / Grand : 0.0,
						m_Index.isEmpty() ? 0.0 : 100.0 * Sum / double(m_Index.size()));
				}
				// v4 video state: the resolved display state at a few points,
				// so the recorded stream can be checked against what the
				// program is known to do.
				fprintf(F, "\nvideo events: %d\n", int(m_VideoEvents.size()));
				const uint64_t First = m_Index.first().m_StartCycle;
				const uint64_t Last = m_Index.last().m_StartCycle;
				for (int K = 0; K <= 4; ++K)
				{
					const uint64_t C = First + (Last - First) * uint64_t(K) / 4;
					const VideoState V = VideoStateAtCycle(C);
					fprintf(F,
						"  cycle %-12llu border=%u 7ffd=$%02X scld=$%02X "
						"NR68=$%02X NR15=$%02X NR69=$%02X NR70=$%02X\n",
						(unsigned long long)C, V.m_Border, V.m_Port7FFD, V.m_ScldMode, V.NextReg(0x68), V.NextReg(0x15),
						V.NextReg(0x69), V.NextReg(0x70));
				}
				fclose(F);
			}
		}
	}

	emit ProgressUpdate(33, tr("Reconstructing call graph..."));
	// Build the call graph in the same worker pass - both consume the
	// memory-mapped record stream once, so we don't re-fault pages by
	// staggering them. CallGraphBuilder::build is O(records); cheap relative
	// to the per-frame utilisation walk which already touched everything.
	m_CallGraph.Build(*this);

	if (m_bCancelMetrics.load())
	{
		emit MetricsReady(true);
		return;
	}
	emit ProgressUpdate(66, tr("Saving memory snapshots..."));

	// Memory snapshots: walk the WHOLE trace once, applying MemWrites to a
	// shadow 64 KB buffer; save a copy at every Nth index entry's frame
	// boundary. Snapshot count capped at kMaxSnapshots so a 21k-frame
	// trace doesn't allocate >8 MB of frozen state.
	constexpr int kMaxSnapshots = 128;
	m_MemSnapshots.clear();
	m_MemSnapIndexEntry.clear();
	m_MemSnapBank7FFD.clear();
	m_MemSnapNextRegs.clear();
	m_MemSnapBank7.clear();
	if (!m_Index.isEmpty())
	{
		const int SnapEvery = std::max(1, int((m_Index.size() + kMaxSnapshots - 1) / kMaxSnapshots));

		QByteArray Shadow(64 * 1024, '\0');
		// Bank 7 (alt-screen) shadow. Only updated when bank 7 is mapped
		// at $C000 (i.e., $7FFD & 0x07 == 7). Lets the ULA Screen 1 /
		// Visible-when-bit3=1 render real bytes once the program has
		// touched the alt screen.
		QByteArray Bank7Shadow(16 * 1024, '\0');

		const uint8_t* StreamBase = m_BtrMap;
		const uint8_t* Cur = StreamBase + m_Index.first().m_FirstByteOffset;
		const uint8_t* Eof = StreamBase + m_BtrSize;

		uint64_t RunCycle = m_Index.first().m_StartCycle;
		uint64_t RunFrame = m_Index.first().m_FrameNumber;
		// Running A and BC (high+low) are needed to interpret OUT writes.
		// Initial values come from the first index entry's snapshot. The
		// header doesn't store $7FFD so we assume 0 (post-reset default);
		// demos that depend on a specific initial mapping should write it
		// explicitly during init (most do).
		uint8_t RunA = uint8_t(m_Index.first().AF >> 8);
		uint16_t RunBC = m_Index.first().BC;
		uint8_t RunBank7FFD = 0;
		// Initial NextReg state from the header. Header captures slots 0-7
		// at the moment the trace started - covers SNA / NEX / dot-cmd
		// boot. We update from explicit writes (Z80N opcodes + port $253B)
		// as we walk; $7FFD-driven changes are not reconstructed here.
		QByteArray RunNextRegs(reinterpret_cast<const char*>(m_Header->m_InitNR5057), 8);
		uint8_t SelectedNextReg = 0; // last value written to port $243B

		// Snapshot the (zero) state at frame 0 BEFORE the first record
		// executes. Anchors "memory state at trace start" so a cursor at
		// the very beginning shows the initial baseline.
		m_MemSnapshots.append(Shadow);
		m_MemSnapIndexEntry.append(0);
		m_MemSnapBank7FFD.append(RunBank7FFD);
		m_MemSnapNextRegs.append(RunNextRegs);
		m_MemSnapBank7.append(Bank7Shadow);

		int NextSnapEntry = SnapEvery; // index entry to snapshot at next
		int CurEntry = 0; // most recent entry we've crossed

		while (Cur < Eof)
		{
			ViewerWalk::DecodedRecord Rec;
			if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
			{
				break;
			}
			if (ViewerWalk::IsStateRecord(Rec.m_Tag))
			{
				continue; // v4 video event
			}
			if (Rec.m_Tag == kRecord_FrameBoundary)
			{
				// Find the index entry that names this frame and snapshot
				// if we've reached the next-snapshot threshold. Index is
				// sorted; advance curEntry forward to track it.
				while (CurEntry + 1 < m_Index.size() && m_Index[CurEntry + 1].m_FrameNumber <= Rec.m_NewFrame)
				{
					++CurEntry;
				}
				if (CurEntry >= NextSnapEntry && CurEntry < m_Index.size())
				{
					if (m_bCancelMetrics.load())
					{
						emit MetricsReady(true);
						return;
					}
					m_MemSnapshots.append(Shadow);
					m_MemSnapIndexEntry.append(CurEntry);
					m_MemSnapBank7FFD.append(RunBank7FFD);
					m_MemSnapNextRegs.append(RunNextRegs);
					m_MemSnapBank7.append(Bank7Shadow);
					// Re-anchor running A / BC from the index entry's
					// snapshot to limit drift accumulated from records that
					// change A/BC without setting Diff_AF/Diff_BC. Rare in
					// practice but cheap to handle.
					RunA = uint8_t(m_Index[CurEntry].AF >> 8);
					RunBC = m_Index[CurEntry].BC;
					NextSnapEntry = CurEntry + SnapEvery;
					const int Pct = TotalEntries > 0 ? 66 + (CurEntry * 34) / TotalEntries : 66;
					emit ProgressUpdate(std::min(99, Pct), QString());
				}
				continue;
			}
			// Detect bank-affecting writes BEFORE updating runA/runBC -
			// we need the values that were on the bus at the moment OUT
			// executed. Patterns:
			//   $ED $79          OUT (C),A   port = BC, val = A
			//   $D3 nn           OUT (nn),A  port = (A<<8) | nn, val = A
			//   $ED $91 n m      nextreg n,m (Z80N)
			//   $ED $92 n        nextreg n,a (Z80N)
			if (Rec.m_Tag == kRecord_Instr)
			{
				const uint8_t kB0 = Rec.m_Opcodes[0];
				const uint8_t kB1 = Rec.m_Opcodes[1];
				const uint8_t kB2 = Rec.m_Opcodes[2];
				const uint8_t kB3 = Rec.m_Opcodes[3];

				auto StoreNextReg = [&](uint8_t Reg, uint8_t Value)
				{
					if (Reg >= 0x50 && Reg <= 0x57)
					{
						RunNextRegs[Reg - 0x50] = char(Value);
					}
				};

				// Helper: apply Map128K's derivation when $7FFD changes -
				// mirrors emulator/src/memory.cpp's MemoryBus::Map128K().
				// Only NR_56/57 (the dynamic slot 3 bank) change per write;
				// NR_50/51 stay at $FF (sentinel for ROM in slot 0) and
				// NR_52..55 keep the fixed bank-5/bank-2 layout that was
				// captured in the trace header's InitNR50_57.
				auto On7FFDWrite = [&](uint8_t NewValue)
				{
					RunBank7FFD = NewValue;
					const uint8_t Bank3 = NewValue & 0x07;
					RunNextRegs[6] = char(Bank3 * 2); // NR_56
					RunNextRegs[7] = char(Bank3 * 2 + 1); // NR_57
				};

				if (kB0 == 0xED && kB1 == 0x79)
				{
					// OUT (C), A. Port = BC.
					if (RunBC == 0x7FFD)
					{
						On7FFDWrite(RunA);
					}
					else if (RunBC == 0x243B)
					{
						SelectedNextReg = RunA;
					}
					else if (RunBC == 0x253B)
					{
						StoreNextReg(SelectedNextReg, RunA);
					}
				}
				else if (kB0 == 0xD3 && kB1 == 0xFD && RunA == 0x7F)
				{
					// OUT ($FD),A with A=$7F -> port $7FFD. Value written
					// is A ($7F here) - the OUT-immediate form can only
					// set $7FFD to $7F. Rare in practice but kept for
					// completeness.
					On7FFDWrite(RunA);
				}
				else if (kB0 == 0xED && kB1 == 0x91)
				{
					StoreNextReg(kB2, kB3);
				}
				else if (kB0 == 0xED && kB1 == 0x92)
				{
					StoreNextReg(kB2, RunA);
				}
			}
			// Update running registers from this record's diff. Done AFTER
			// OUT detection so the OUT path uses pre-instruction values.
			if (Rec.m_Mask & kRegDiffBitDiffAf)
			{
				RunA = uint8_t(Rec.m_PostAF >> 8);
			}
			if (Rec.m_Mask & kRegDiffBitDiffBc)
			{
				RunBC = Rec.m_PostBC;
			}
			// Apply mem writes to shadow AND, when bank 7 is the slot-3
			// mapping, mirror the upper-16K writes into the bank-7 shadow
			// so the ULA-Screen-1 / Visible-when-bit3=1 view can render.
			const bool bBank7AtC000 = (RunBank7FFD & 0x07) == 7;
			for (uint8_t IdxI = 0; IdxI < Rec.m_NumMemWrites; ++IdxI)
			{
				const uint16_t Addr = Rec.m_MemAddr[IdxI];
				const uint8_t Val = Rec.m_MemVal[IdxI];
				Shadow[Addr] = char(Val);
				if (bBank7AtC000 && Addr >= 0xC000)
				{
					Bank7Shadow[Addr - 0xC000] = char(Val);
				}
			}
		}
	}

	emit ProgressUpdate(100, tr("Ready."));
	emit MetricsReady(false);
}

Cursor TraceModel::CursorAtCycle(uint64_t Cycle) const
{
	Cursor C{};
	if (m_Index.isEmpty())
	{
		return C;
	}
	// Binary search for largest entry with StartCycle <= cycle.
	int Lo = 0;
	int Hi = m_Index.size() - 1;
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi + 1) / 2;
		if (m_Index[Mid].m_StartCycle <= Cycle)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid - 1;
		}
	}
	const auto& E = m_Index[Lo];
	C.m_Cycle = Cycle < E.m_StartCycle ? E.m_StartCycle : Cycle;
	C.m_Frame = E.m_FrameNumber;
	C.m_ByteOffset = E.m_FirstByteOffset;
	C.m_TimeNanos = E.m_StartTimeNanos;
	return C;
}

bool TraceModel::IsNextMachine() const
{
	using namespace BinaryTrace;
	if (m_Header == nullptr)
	{
		return false;
	}
	return m_Header->m_Machine == kMachine_Next50 || m_Header->m_Machine == kMachine_Next60;
}

bool TraceModel::IsTimexMachine() const
{
	using namespace BinaryTrace;
	if (m_Header == nullptr)
	{
		return false;
	}
	return m_Header->m_Machine == kMachine_Ts2068 || m_Header->m_Machine == kMachine_Tc2068 ||
		   m_Header->m_Machine == kMachine_Tc2048;
}

QByteArray TraceModel::NextRegs5057AtCycle(uint64_t Cycle) const
{
	using namespace BinaryTrace;
	if (m_MemSnapNextRegs.isEmpty() || m_BtrMap == nullptr || m_Header == nullptr)
	{
		return QByteArray(8, '\0');
	}
	const auto& Index = m_Index;
	if (Index.isEmpty())
	{
		return m_MemSnapNextRegs.first();
	}

	int Chosen = 0;
	for (int IdxI = 0; IdxI < m_MemSnapNextRegs.size(); ++IdxI)
	{
		const int E = m_MemSnapIndexEntry[IdxI];
		if (E < 0 || E >= Index.size())
		{
			break;
		}
		if (Index[E].m_StartCycle <= Cycle)
		{
			Chosen = IdxI;
		}
		else
		{
			break;
		}
	}
	QByteArray Regs = m_MemSnapNextRegs[Chosen];

	const int StartEntry = m_MemSnapIndexEntry[Chosen];
	if (StartEntry < 0 || StartEntry >= Index.size())
	{
		return Regs;
	}
	const uint8_t* Base = m_BtrMap;
	const uint8_t* Cur = Base + Index[StartEntry].m_FirstByteOffset;
	const uint8_t* Eof = Base + m_BtrSize;
	uint64_t RunCycle = Index[StartEntry].m_StartCycle;
	uint64_t RunFrame = Index[StartEntry].m_FrameNumber;
	uint8_t RunA = uint8_t(Index[StartEntry].AF >> 8);
	uint16_t RunBC = Index[StartEntry].BC;
	uint8_t SelectedNextReg = 0;

	auto StoreReg = [&](uint8_t Reg, uint8_t Value)
	{
		if (Reg >= 0x50 && Reg <= 0x57)
		{
			Regs[Reg - 0x50] = char(Value);
		}
	};
	// Apply Map128K-style derivation on $7FFD writes; keep this walker
	// consistent with computeMetrics's main pass.
	auto On7FFDWrite = [&](uint8_t V)
	{
		const uint8_t Bank3 = V & 0x07;
		Regs[6] = char(Bank3 * 2);
		Regs[7] = char(Bank3 * 2 + 1);
	};

	while (Cur < Eof)
	{
		ViewerWalk::DecodedRecord Rec;
		if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
		{
			break;
		}
		if (ViewerWalk::IsStateRecord(Rec.m_Tag))
		{
			continue; // v4 video event
		}
		if (Rec.m_Cycle > Cycle)
		{
			break;
		}
		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			continue;
		}
		if (Rec.m_Tag == kRecord_Instr)
		{
			const uint8_t kB0 = Rec.m_Opcodes[0];
			const uint8_t kB1 = Rec.m_Opcodes[1];
			const uint8_t kB2 = Rec.m_Opcodes[2];
			const uint8_t kB3 = Rec.m_Opcodes[3];
			if (kB0 == 0xED && kB1 == 0x79)
			{
				if (RunBC == 0x7FFD)
				{
					On7FFDWrite(RunA);
				}
				else if (RunBC == 0x243B)
				{
					SelectedNextReg = RunA;
				}
				else if (RunBC == 0x253B)
				{
					StoreReg(SelectedNextReg, RunA);
				}
			}
			else if (kB0 == 0xD3 && kB1 == 0xFD && RunA == 0x7F)
			{
				On7FFDWrite(RunA);
			}
			else if (kB0 == 0xED && kB1 == 0x91)
			{
				StoreReg(kB2, kB3);
			}
			else if (kB0 == 0xED && kB1 == 0x92)
			{
				StoreReg(kB2, RunA);
			}
		}
		if (Rec.m_Mask & kRegDiffBitDiffAf)
		{
			RunA = uint8_t(Rec.m_PostAF >> 8);
		}
		if (Rec.m_Mask & kRegDiffBitDiffBc)
		{
			RunBC = Rec.m_PostBC;
		}
	}
	return Regs;
}

QByteArray TraceModel::Bank7AtCycle(uint64_t Cycle) const
{
	using namespace BinaryTrace;
	if (m_MemSnapBank7.isEmpty() || m_BtrMap == nullptr || m_Header == nullptr)
	{
		return QByteArray(16 * 1024, '\0');
	}
	const auto& Index = m_Index;
	if (Index.isEmpty())
	{
		return m_MemSnapBank7.first();
	}

	int Chosen = 0;
	for (int IdxI = 0; IdxI < m_MemSnapBank7.size(); ++IdxI)
	{
		const int E = m_MemSnapIndexEntry[IdxI];
		if (E < 0 || E >= Index.size())
		{
			break;
		}
		if (Index[E].m_StartCycle <= Cycle)
		{
			Chosen = IdxI;
		}
		else
		{
			break;
		}
	}
	QByteArray Bank7 = m_MemSnapBank7[Chosen];

	const int StartEntry = m_MemSnapIndexEntry[Chosen];
	if (StartEntry < 0 || StartEntry >= Index.size())
	{
		return Bank7;
	}
	const uint8_t* Base = m_BtrMap;
	const uint8_t* Cur = Base + Index[StartEntry].m_FirstByteOffset;
	const uint8_t* Eof = Base + m_BtrSize;
	uint64_t RunCycle = Index[StartEntry].m_StartCycle;
	uint64_t RunFrame = Index[StartEntry].m_FrameNumber;
	uint8_t RunA = uint8_t(Index[StartEntry].AF >> 8);
	uint16_t RunBC = Index[StartEntry].BC;
	uint8_t Bank7FFD = m_MemSnapBank7FFD[Chosen];

	while (Cur < Eof)
	{
		ViewerWalk::DecodedRecord Rec;
		if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
		{
			break;
		}
		if (ViewerWalk::IsStateRecord(Rec.m_Tag))
		{
			continue; // v4 video event
		}
		if (Rec.m_Cycle > Cycle)
		{
			break;
		}
		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			continue;
		}
		if (Rec.m_Tag == kRecord_Instr)
		{
			const uint8_t kB0 = Rec.m_Opcodes[0];
			const uint8_t kB1 = Rec.m_Opcodes[1];
			if (kB0 == 0xED && kB1 == 0x79 && RunBC == 0x7FFD)
			{
				Bank7FFD = RunA;
			}
			else if (kB0 == 0xD3 && kB1 == 0xFD && RunA == 0x7F)
			{
				Bank7FFD = RunA;
			}
		}
		if ((Bank7FFD & 0x07) == 7)
		{
			for (uint8_t IdxI = 0; IdxI < Rec.m_NumMemWrites; ++IdxI)
			{
				const uint16_t Addr = Rec.m_MemAddr[IdxI];
				if (Addr >= 0xC000)
				{
					Bank7[Addr - 0xC000] = char(Rec.m_MemVal[IdxI]);
				}
			}
		}
		if (Rec.m_Mask & kRegDiffBitDiffAf)
		{
			RunA = uint8_t(Rec.m_PostAF >> 8);
		}
		if (Rec.m_Mask & kRegDiffBitDiffBc)
		{
			RunBC = Rec.m_PostBC;
		}
	}
	return Bank7;
}

TraceModel::VideoState TraceModel::VideoStateAtCycle(uint64_t Cycle) const
{
	using namespace BinaryTrace;
	VideoState S;
	if (m_Header == nullptr)
	{
		return S;
	}

	S.m_Border = m_Header->m_InitBorder;
	S.m_Port7FFD = m_Header->m_InitPort7FFD;
	S.m_ScldMode = m_Header->m_InitScldMode;
	S.m_EvoFf77 = m_Header->m_InitEvoFf77;
	for (size_t IdxI = 0; IdxI < kDisplayRegCount; ++IdxI)
	{
		S.m_DisplayRegs[IdxI] = m_Header->m_InitDisplayRegs[IdxI];
	}

	// Events are in cycle order; replay to the cursor. A copper-driven trace
	// holds ~15k events/second, so this is a linear scan of a compact array
	// rather than a re-walk of the record stream.
	for (const VideoEvent& E : m_VideoEvents)
	{
		if (E.m_Cycle > Cycle)
		{
			break;
		}
		if (E.m_Kind == 0)
		{
			const int Slot = DisplayRegSlot(uint8_t(E.m_Addr));
			if (Slot >= 0)
			{
				S.m_DisplayRegs[Slot] = E.m_Value;
			}
			continue;
		}
		switch (E.m_Addr)
		{
		case kVideoPort_UlaFE:
			S.m_Border = uint8_t(E.m_Value & 0x07);
			break;
		case kVideoPort_Scld:
			S.m_ScldMode = E.m_Value;
			break;
		case kVideoPort_7FFD:
			S.m_Port7FFD = E.m_Value;
			break;
		case kVideoPort_EvoFF77:
			S.m_EvoFf77 = E.m_Value;
			break;
		default:
			break; // Uno register file is a select/data pair, not resolved here
		}
	}
	return S;
}

uint8_t TraceModel::Bank7FFDAtCycle(uint64_t Cycle) const
{
	using namespace BinaryTrace;
	if (m_MemSnapshots.isEmpty() || m_BtrMap == nullptr)
	{
		return 0;
	}

	// Pick the latest snapshot whose StartCycle is <= cycle.
	const auto& Index = m_Index;
	if (Index.isEmpty())
	{
		return 0;
	}
	int Chosen = 0;
	for (int IdxI = 0; IdxI < m_MemSnapshots.size(); ++IdxI)
	{
		const int E = m_MemSnapIndexEntry[IdxI];
		if (E < 0 || E >= Index.size())
		{
			break;
		}
		if (Index[E].m_StartCycle <= Cycle)
		{
			Chosen = IdxI;
		}
		else
		{
			break;
		}
	}

	uint8_t Bank = m_MemSnapBank7FFD[Chosen];

	const int StartEntry = m_MemSnapIndexEntry[Chosen];
	if (StartEntry < 0 || StartEntry >= Index.size())
	{
		return Bank;
	}
	const uint8_t* Base = m_BtrMap;
	const uint8_t* Cur = Base + Index[StartEntry].m_FirstByteOffset;
	const uint8_t* Eof = Base + m_BtrSize;
	uint64_t RunCycle = Index[StartEntry].m_StartCycle;
	uint64_t RunFrame = Index[StartEntry].m_FrameNumber;
	uint8_t RunA = uint8_t(Index[StartEntry].AF >> 8);
	uint16_t RunBC = Index[StartEntry].BC;

	while (Cur < Eof)
	{
		ViewerWalk::DecodedRecord Rec;
		if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
		{
			break;
		}
		if (ViewerWalk::IsStateRecord(Rec.m_Tag))
		{
			continue; // v4 video event
		}
		if (Rec.m_Cycle > Cycle)
		{
			break;
		}
		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			continue;
		}
		if (Rec.m_Tag == kRecord_Instr)
		{
			const uint8_t kB0 = Rec.m_Opcodes[0];
			const uint8_t kB1 = Rec.m_Opcodes[1];
			if (kB0 == 0xED && kB1 == 0x79 && RunBC == 0x7FFD)
			{
				Bank = RunA;
			}
			else if (kB0 == 0xD3 && kB1 == 0xFD && RunA == 0x7F)
			{
				Bank = RunA; // (A<<8|nn) == $7FFD -> value is A itself
				// The above is incorrect - value written IS A, but here
				// A is both the port high byte AND the value. Programs
				// using $D3 $FD always set A first, so the stored value
				// is whatever A was set to before. This case is rare so
				// accept the limitation.
			}
		}
		if (Rec.m_Mask & kRegDiffBitDiffAf)
		{
			RunA = uint8_t(Rec.m_PostAF >> 8);
		}
		if (Rec.m_Mask & kRegDiffBitDiffBc)
		{
			RunBC = Rec.m_PostBC;
		}
	}
	return Bank;
}

Cursor TraceModel::CursorAtFrame(uint64_t Frame) const
{
	Cursor C{};
	if (m_Index.isEmpty())
	{
		return C;
	}
	// Match cursorAtCycle's convention: largest entry with FrameNumber <=
	// frame. Previously this was lower_bound (smallest >= frame), which
	// disagreed with the cycle path - a click that landed between two
	// entries jumped FORWARD with cursorAtFrame but BACKWARD with
	// cursorAtCycle. Pin to a consistent direction.
	if (Frame < m_Index.first().m_FrameNumber)
	{
		// Cursor before trace start: clamp to first entry.
		const auto& E = m_Index.first();
		C.m_Cycle = E.m_StartCycle;
		C.m_Frame = E.m_FrameNumber;
		C.m_ByteOffset = E.m_FirstByteOffset;
		C.m_TimeNanos = E.m_StartTimeNanos;
		return C;
	}
	int Lo = 0;
	int Hi = m_Index.size() - 1;
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi + 1) / 2;
		if (m_Index[Mid].m_FrameNumber <= Frame)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid - 1;
		}
	}
	const auto& E = m_Index[Lo];
	C.m_Cycle = E.m_StartCycle;
	C.m_Frame = E.m_FrameNumber;
	C.m_ByteOffset = E.m_FirstByteOffset;
	C.m_TimeNanos = E.m_StartTimeNanos;
	return C;
}
