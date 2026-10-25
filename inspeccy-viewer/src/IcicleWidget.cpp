#include "IcicleWidget.h"

#include "CallGraphBuilder.h"
#include "TraceModel.h"

#include <trace/trace_binary.h>

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <math.h>
#include <stdlib.h>

namespace
{

// Per-depth bar fill colour. Distinct neighbours, but the palette doesn't
// need to encode any semantic - depth is conveyed by Y position.
QColor BarColour(uint32_t Depth)
{
	static const QColor Table[] = {
		QColor(0x4d, 0x9b, 0xff), // deep 0  cool blue
		QColor(0x6c, 0xbf, 0x84), // 1       teal-green
		QColor(0xff, 0xb1, 0x4d), // 2       amber
		QColor(0xd5, 0x77, 0xff), // 3       violet
		QColor(0xff, 0x6b, 0x6b), // 4       coral
		QColor(0x4d, 0xc9, 0xff), // 5       cyan
		QColor(0xc9, 0xff, 0x4d), // 6       lime
		QColor(0xff, 0x9b, 0xd5), // 7       pink
	};
	return Table[Depth % (sizeof(Table) / sizeof(Table[0]))];
}

QString HexAddr(uint16_t V)
{
	return QStringLiteral("$%1").arg(V, 4, 16, QChar('0')).toUpper();
}

// Compact cycle count for the bar overlay. Adds K/M suffixes so bars wider
// than a few dozen pixels can still fit. Tooltip shows the precise number.
QString FormatCyclesCompact(uint64_t Cyc)
{
	if (Cyc < 1000)
	{
		return QStringLiteral("%1c").arg(Cyc);
	}
	if (Cyc < 100'000)
	{
		return QStringLiteral("%1c").arg(Cyc);
	}
	if (Cyc < 10'000'000)
	{
		return QStringLiteral("%1Kc").arg(Cyc / 1000);
	}
	return QStringLiteral("%1Mc").arg(Cyc / 1'000'000);
}

// Convert cycles -> milliseconds at the trace's base CPU clock. Stored as
// MHz x 1e6 in the header so the integer divide stays lossless.
double CyclesToMs(uint64_t Cyc, uint64_t BaseMhzX1e6)
{
	if (BaseMhzX1e6 == 0)
	{
		return 0.0;
	}
	return double(Cyc) * 1000.0 / double(BaseMhzX1e6);
}

// "0:30.940" - minutes:seconds.milliseconds. The trace covers wall-clock
// emulated time, capped at minutes for display fit.
QString FormatTime(uint64_t Nanos)
{
	const double Sec = double(Nanos) / 1.0e9;
	const int Mins = int(Sec) / 60;
	const double Rem = Sec - 60.0 * Mins;
	return QStringLiteral("%1:%2").arg(Mins, 2, 10, QChar('0')).arg(Rem, 6, 'f', 3, QChar('0'));
}

// "1,082,400,000" - commas every three digits. Cycle counts hit 10^10 in
// long traces; thousands-separated keeps them visually parseable.
QString FormatCycle(uint64_t C)
{
	QString S = QString::number(C);
	int IdxI = S.size() - 3;
	while (IdxI > 0)
	{
		S.insert(IdxI, QChar(','));
		IdxI -= 3;
	}
	return S;
}

} // namespace

IcicleWidget::IcicleWidget(QWidget* Parent) : QWidget(Parent)
{
	setMinimumHeight(220);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(true);
	setFocusPolicy(Qt::WheelFocus); // accept wheel events without click
}

void IcicleWidget::SetModel(TraceModel* M)
{
	m_Model = M;
	m_CyclesPerPixel = 1.0; // reset -> fit applied on next paint
	m_LeftCycle = 0;
	update();
}

void IcicleWidget::SetCursor(const Cursor& C)
{
	m_Cursor = C;
	update();
}

void IcicleWidget::SetViewportCycles(uint64_t LeftCycle, double CyclesPerPixel)
{
	m_FittedExecution = -1;
	m_LeftCycle = LeftCycle;
	m_CyclesPerPixel = (CyclesPerPixel > 0.0) ? CyclesPerPixel : 1.0;
	emit ViewportChanged(m_LeftCycle, XToCycle(width()));
	update();
}

uint64_t IcicleWidget::XToCycle(int X) const
{
	const double C = double(m_LeftCycle) + m_CyclesPerPixel * double(X);
	if (C < 0.0)
	{
		return 0;
	}
	return uint64_t(C);
}

int IcicleWidget::CycleToX(uint64_t C) const
{
	const double Dx = (double(C) - double(m_LeftCycle)) / m_CyclesPerPixel;
	if (Dx < -1e9)
	{
		return -1000000; // clamp to a sentinel "off-screen"
	}
	if (Dx > 1e9)
	{
		return 1000000;
	}
	return int(std::round(Dx));
}

void IcicleWidget::FitWholeTraceIfUnset()
{
	using namespace BinaryTrace;
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return;
	}
	const Header* H = m_Model->GetHeader();
	if (H == nullptr)
	{
		return;
	}

	// First view-state setup AFTER a load. m_CyclesPerPixel == 1.0 and
	// m_LeftCycle == 0 are the "uninitialised" sentinels; we leave any
	// user-driven view state alone on subsequent updates.
	if (m_CyclesPerPixel != 1.0 || m_LeftCycle != 0)
	{
		return;
	}

	const auto& Index = m_Model->Index();
	uint64_t StartCycle = (Index.isEmpty()) ? H->m_StartCycle : Index.first().m_StartCycle;
	uint64_t EndCycle = H->m_EndCycle;
	if (EndCycle == kSentinel || EndCycle <= StartCycle)
	{
		// Truncated trace; pick a sensible window so things don't divide by 0.
		if (!m_Model->CallGraph().Frames().isEmpty())
		{
			EndCycle = m_Model->CallGraph().Frames().last().m_EndCycle;
		}
		else
		{
			EndCycle = StartCycle + 1000;
		}
	}
	const int InnerW = std::max(1, width());
	m_LeftCycle = StartCycle;
	m_CyclesPerPixel = double(EndCycle - StartCycle) / double(InnerW);
	if (m_CyclesPerPixel <= 0.0)
	{
		m_CyclesPerPixel = 1.0;
	}
	// Tell listeners (overview viewport box, instruction TopFunctions
	// aggregator) what the initial visible range is. queued so we don't
	// emit from within the same paint event chain that's running.
	QMetaObject::invokeMethod(
		this,
		[this]
		{
			emit ViewportChanged(m_LeftCycle, XToCycle(width()));
		},
		Qt::QueuedConnection);
}

void IcicleWidget::ZoomAroundX(int X, double Factor)
{
	m_FittedExecution = -1;
	// Keep the cycle currently under x stationary as the cpp scales.
	const uint64_t PivotCycle = XToCycle(X);
	m_CyclesPerPixel *= Factor;
	// Clamp to a sane range: too small and integer math breaks; too large
	// and we can't see anything anyway.
	if (m_CyclesPerPixel < 0.001)
	{
		m_CyclesPerPixel = 0.001;
	}
	if (m_CyclesPerPixel > 1e9)
	{
		m_CyclesPerPixel = 1e9;
	}

	// newLeft = pivotCycle - cpp * x. Floor at 0.
	const double NewLeft = double(PivotCycle) - m_CyclesPerPixel * double(X);
	m_LeftCycle = (NewLeft < 0.0) ? 0 : uint64_t(NewLeft);
}

void IcicleWidget::FitWholeTrace()
{
	m_FittedExecution = -1;
	m_LeftCycle = 0;
	m_CyclesPerPixel = 1.0;
	FitWholeTraceIfUnset();
	update();
}

void IcicleWidget::FitExecution(int Index)
{
	if (!m_Model || Index < 0 || Index >= m_Model->CallGraph().Frames().size())
	{
		return;
	}
	const auto& F = m_Model->CallGraph().Frames()[Index];
	const uint64_t Duration = std::max<uint64_t>(1, F.m_EndCycle - F.m_StartCycle);
	const uint64_t Pad = std::max<uint64_t>(1, Duration / 30);
	const uint64_t Left = F.m_StartCycle > Pad ? F.m_StartCycle - Pad : 0;
	SetViewportCycles(Left, double(F.m_EndCycle - Left + Pad) / std::max(1, width()));
	m_FittedExecution = Index;
}

void IcicleWidget::resizeEvent(QResizeEvent*)
{
	if (m_FittedExecution >= 0)
	{
		FitExecution(m_FittedExecution);
	}
}

void IcicleWidget::paintEvent(QPaintEvent*)
{
	QPainter P(this);
	P.fillRect(rect(), QColor(0x14, 0x14, 0x14));

	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		P.setPen(QColor(0x90, 0x90, 0x90));
		P.drawText(rect(), Qt::AlignCenter, tr("Open a .btr file (File -> Open)"));
		return;
	}

	// INSPECCY_PERF=1: time each repaint and report how many call frames the
	// draw loop scanned vs actually drew. The headless --bench (main.cpp) sets
	// it and forces paints via icicle()->grab(). Costs nothing when unset.
	static const bool bPerf = getenv("INSPECCY_PERF") != nullptr;
	QElapsedTimer PerfTimer;
	if (bPerf)
	{
		PerfTimer.start();
	}

	FitWholeTraceIfUnset();

	const int W = width();
	const int H = height();
	const int kRH = RulerHeight();

	// Ruler band background + divider; per-frame text + ticks drawn by
	// paintRuler() after we know the visible cycle range.
	P.fillRect(0, 0, W, kRH, QColor(0x20, 0x20, 0x20));

	// Visible window in cycles.
	const uint64_t LeftC = m_LeftCycle;
	const uint64_t RightC = XToCycle(W);

	P.save();
	PaintRuler(P, W, kRH, LeftC, RightC);
	P.restore(); // ruler's small font must not leak into call labels

	P.setPen(QColor(0x40, 0x40, 0x40));
	P.drawLine(0, kRH, W, kRH);

	// Synthetic root row at depth 0 - represents "the CPU is executing
	// SOMETHING here even if we don't have a CALL frame tracking it".
	// Real depth-0 frames overwrite this on top below; we draw the root
	// FIRST so the segmentation by ROM/RAM provenance shows through where
	// no frame exists. ROM segments come from the per-cycle PageTag trail
	// (covers JP-into-ROM cases like TR-DOS's $3Dxx auto-page-in).
	{
		const int RowY = kRH + 0;
		const int RowH = BarHeight() - 2;
		// Base "main" colour - neutral grey.
		P.fillRect(0, RowY, W, RowH, QColor(0x55, 0x55, 0x55));
		// Overlay ROM segments in a distinct purple-ish tint.
		const auto& Roms = m_Model->CallGraph().RomIntervals();
		for (const auto& Iv : Roms)
		{
			if (Iv.m_EndCycle <= LeftC)
			{
				continue;
			}
			if (Iv.m_StartCycle > RightC)
			{
				break;
			}
			const int Rx0 = std::clamp(CycleToX(Iv.m_StartCycle), -2, W + 2);
			const int Rx1 = std::clamp(CycleToX(Iv.m_EndCycle), -2, W + 2);
			const int Rw = std::max(1, Rx1 - Rx0);
			P.fillRect(Rx0, RowY, Rw, RowH, QColor(0x8a, 0x5a, 0xc8));
		}
		// Label only if there's empty space (no depth-0 frame spanning the
		// whole row). Heuristic: if the leftmost couple of pixels are not
		// covered by a depth-0 frame, label there.
		P.setPen(QColor(0xd0, 0xd0, 0xd0));
		P.drawText(QRect(4, RowY, 60, RowH), Qt::AlignVCenter | Qt::AlignLeft, tr("[main]"));
	}

	// Call frames are sorted by StartCycle. We CANNOT binary-search by
	// EndCycle to find the first visible frame because deeply-nested
	// frames with low StartCycles often have high EndCycles (they're
	// long-running outer frames). Instead: find the LAST frame whose
	// StartCycle <= rightC (everything past that has zero overlap with
	// the viewport), then linear-scan from 0 filtering by EndCycle > leftC.
	// O(visible-prefix) instead of O(visible), but for our trace sizes
	// (<= tens of thousands of frames) the linear pass is sub-millisecond.
	const auto& Frames = m_Model->CallGraph().Frames();
	const int N = Frames.size();
	int Lo = 0, Hi = N;
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi) / 2;
		if (Frames[Mid].m_StartCycle > RightC)
		{
			Hi = Mid;
		}
		else
		{
			Lo = Mid + 1;
		}
	}
	const int EndIdx = Lo; // exclusive - scan [0, endIdx)

	// Pre-compute highlight match decision. When anyHighlight is set, frames
	// that don't satisfy at least one active highlight are dimmed instead
	// of skipped - keeps the call hierarchy readable while drawing attention
	// to the highlighted code.
	const bool bAnyHighlight = m_HighlightMask != 0;

	int Drawn = 0;
	for (int IdxI = 0; IdxI < EndIdx; ++IdxI)
	{
		const auto& F = Frames[IdxI];
		if (F.m_EndCycle <= LeftC)
		{
			continue; // finished before viewport
		}

		const int kX0 = std::clamp(CycleToX(F.m_StartCycle), -2, W + 2);
		const int kX1 = std::clamp(CycleToX(F.m_EndCycle), -2, W + 2);
		int BarW = kX1 - kX0;
		if (BarW < 1)
		{
			BarW = 1;
		}
		const int Y = kRH + int(F.m_Depth) * BarHeight();
		if (Y >= H)
		{
			continue;
		}
		const int BarH = std::min(BarHeight() - 2, H - Y);

		// Highlight-driven dimming. Frame matches a highlight iff at least
		// one of its tag bits is selected. With no active highlight, all
		// frames render at full saturation. DMA isn't tracked yet; it
		// just means "no frame matches DMA" and acts as identity.
		// ROM highlight now ALSO uses the per-cycle ROM trail (covers
		// TR-DOS / JP-into-ROM regions that don't get a CallFrame).
		bool bMatches = !bAnyHighlight;
		if (bAnyHighlight)
		{
			if ((m_HighlightMask & kHighlightBitHlInterrupt) && F.m_bIsInInterrupt)
			{
				bMatches = true;
			}
			if (m_HighlightMask & kHighlightBitHlRom)
			{
				// Frame-level: a frame whose target is in ROM. Trail-level:
				// any portion of the frame's lifetime overlapping a ROM
				// interval. We use the StartCycle as a cheap proxy - a
				// ROM-call frame whose body ended up in RAM is rare.
				if (F.m_bIsInRom || m_Model->CallGraph().IsCycleInRom(F.m_StartCycle))
				{
					bMatches = true;
				}
			}
			// HL_Dma: deferred to Batch 4.
		}
		QColor Fill = BarColour(F.m_Depth);
		if (!bMatches)
		{
			// Dim to ~30% intensity. Keeps depth structure visible without
			// competing with the highlighted frames for attention.
			Fill = QColor(Fill.red() / 3, Fill.green() / 3, Fill.blue() / 3);
		}
		P.fillRect(kX0, Y, BarW, BarH, Fill);
		if (IdxI == m_FittedExecution)
		{
			P.setPen(QColor(255, 240, 150));
			P.drawRect(kX0, Y, BarW - 1, BarH - 1);
		}

		// Label + cycle-count overlay. Symbol resolution prefers SLD if
		// any labels are loaded; falls back to $PC. Cycle count shown on
		// the right when the bar is wide enough.
		const uint64_t DurCycles = (F.m_EndCycle > F.m_StartCycle) ? (F.m_EndCycle - F.m_StartCycle) : 0;
		if (BarW >= 36)
		{
			QString Label = HexAddr(F.m_TargetPC);
			// Resolve against the callee's ACTUAL native 8K page (not a
			// hardcoded guess) - the map is keyed by (page, addr). RAM only:
			// sjasmplus rarely emits ROM labels. 256-byte window matches the
			// instruction list's FormatRow so both views resolve identically.
			if (m_Model->Sld().SymbolCount() > 0 && !BinaryTrace::PageTagIsRom(F.m_TargetPageTag) &&
				F.m_TargetPC >= 0x4000)
			{
				std::string Lbl;
				if (m_Model->Sld().LookupNearest(F.m_TargetPageTag, F.m_TargetPC, 256, Lbl))
				{
					Label = QString::fromStdString(Lbl);
				}
			}
			P.setPen(bMatches ? QColor(0x10, 0x10, 0x10) : QColor(0x60, 0x60, 0x60));
			// Label on the left.
			const QString CyclesText = FormatCyclesCompact(DurCycles);
			const int CountWidth = P.fontMetrics().horizontalAdvance(CyclesText) + 8;
			const bool ShowCount = BarW >= CountWidth + 80;
			const int LabelWidth = std::max(0, BarW - 6 - (ShowCount ? CountWidth : 0));
			P.drawText(QRect(kX0 + 3, Y, LabelWidth, BarH), Qt::AlignVCenter | Qt::AlignLeft,
				P.fontMetrics().elidedText(Label, Qt::ElideRight, LabelWidth));
			// Cycle count on the right when bar has room (>= 70 px lets
			// the address label and the cycles count coexist without
			// visual collision).
			if (ShowCount)
			{
				P.drawText(QRect(kX0 + 3, Y, BarW - 6, BarH), Qt::AlignVCenter | Qt::AlignRight,
					FormatCyclesCompact(DurCycles));
			}
		}
		++Drawn;
	}

	// Cursor line: vertical, golden, through ruler + body.
	const int XCursor = CycleToX(m_Cursor.m_Cycle);
	if (XCursor >= 0 && XCursor < W)
	{
		P.setPen(QColor(0xff, 0xd0, 0x40));
		P.drawLine(XCursor, 0, XCursor, H);
	}

	// Zoom HUD pinned to bottom-right so it doesn't fight the ruler labels.
	// Drawn last (overlays everything beneath).
	P.setPen(QColor(0x99, 0x99, 0x99));
	P.drawText(QRect(0, H - 16, W - 6, 14), Qt::AlignRight | Qt::AlignVCenter,
		tr("zoom %1 cyc/px / visible %2 / %3 frames").arg(m_CyclesPerPixel, 0, 'f', 2).arg(Drawn).arg(N));

	if (bPerf)
	{
		qInfo("[perf] icicle paint %lld ms  frames=%d scanned=%d drawn=%d  left=%llu cpp=%.1f",
			(long long)PerfTimer.elapsed(), N, EndIdx, Drawn, (unsigned long long)LeftC, m_CyclesPerPixel);
	}
}

void IcicleWidget::PaintRuler(QPainter& InOutP, int W, int RH, uint64_t LeftC, uint64_t RightC)
{
	using namespace BinaryTrace;
	const Header* H = m_Model->GetHeader();
	if (H == nullptr)
	{
		return;
	}
	const auto& Index = m_Model->Index();
	if (Index.isEmpty())
	{
		return;
	}

	// Layout - ruler is now 110 px and carries:
	//   yFrame (4)   frame-number labels
	//   yTime  (18)  time labels
	//   yCycle (32)  cycle labels
	//   yReadout(50) golden cursor: frame / time / cycle / in symbol(depth)
	//   yReg1  (68)  PC SP AF BC DE HL IX IY  <- was RegisterView
	//   yReg2  (82)  AF' BC' DE' HL' I R IM IFFs
	//   yBank  (96)  bank/slot mapping (placeholder until Batch 4)
	constexpr int YFrame = 4;
	constexpr int YTime = 18;
	constexpr int YCycle = 32;
	constexpr int YReadout = 50;
	constexpr int YReg1 = 68;
	constexpr int YReg2 = 82;
	constexpr int YBank = 96;

	QFont SmallFont = InOutP.font();
	SmallFont.setPointSize(std::max(7, SmallFont.pointSize() - 2));
	InOutP.setFont(SmallFont);
	const QFontMetrics Fm(SmallFont);

	// Pass 1: ticks. One vertical line per index entry that lands in view.
	// Lines extend from the ruler base through the call-graph body - frame
	// boundaries are time landmarks, not call-graph artefacts.
	InOutP.setPen(QColor(0x2a, 0x2a, 0x2a));
	for (int IdxI = 0; IdxI < Index.size(); ++IdxI)
	{
		const auto& Entry = Index[IdxI];
		if (Entry.m_StartCycle < LeftC)
		{
			continue;
		}
		if (Entry.m_StartCycle > RightC)
		{
			break;
		}
		const int X = CycleToX(Entry.m_StartCycle);
		if (X < 0 || X > W)
		{
			continue;
		}
		InOutP.drawLine(X, RH, X, height()); // through body
	}

	// INT/RETI band. For each Interrupt CallFrame visible in the window,
	// draw a hatched red horizontal stripe at the bottom edge of the ruler
	// spanning [StartCycle, EndCycle]. The hatching gives the "------"
	// look the user asked for; the stripe sits just under the ruler base
	// line so it stays adjacent to the ticks above and the call bars below.
	const auto& CgFrames = m_Model->CallGraph().Frames();
	// Thin INT band hugging the ruler base - keep it out of the "banks"
	// text row above. 2 px is enough to be visible as hatched red.
	const int YIntBand = RH - 2;
	const int YIntBandH = 2;
	for (const auto& F : CgFrames)
	{
		if (F.m_KindV != CallFrame::kKindInterrupt)
		{
			continue;
		}
		if (F.m_EndCycle <= LeftC)
		{
			continue;
		}
		if (F.m_StartCycle > RightC)
		{
			break; // frames are sorted by StartCycle
		}
		int X0 = std::clamp(CycleToX(F.m_StartCycle), -2, W + 2);
		int X1 = std::clamp(CycleToX(F.m_EndCycle), -2, W + 2);
		if (X1 < X0)
		{
			X1 = X0;
		}
		// Hatched fill - alternating dark and bright red across 2-px
		// segments. Reads as "this region is inside an ISR" even at
		// sub-pixel widths because the brightest pixel always wins.
		for (int X = X0; X <= X1; X += 2)
		{
			const QColor C = (X & 2) ? QColor(0xff, 0x40, 0x40) : QColor(0x80, 0x00, 0x00);
			InOutP.fillRect(X, YIntBand, std::min(2, X1 - X + 1), YIntBandH, C);
		}
		// Cap marks at both ends. The INT mark (start) is a small green
		// triangle to mark "interrupt accepted here"; the RETI mark (end)
		// is a yellow triangle. Both stand out against the hatched red.
		if (X0 >= 0 && X0 < W)
		{
			QPolygon Tri;
			Tri << QPoint(X0, YIntBand) << QPoint(X0 - 3, YIntBand - 5) << QPoint(X0 + 3, YIntBand - 5);
			InOutP.setBrush(QColor(0x60, 0xff, 0x60));
			InOutP.setPen(Qt::NoPen);
			InOutP.drawPolygon(Tri);
		}
		if (X1 >= 0 && X1 < W && X1 != X0)
		{
			QPolygon Tri;
			Tri << QPoint(X1, YIntBand) << QPoint(X1 - 3, YIntBand - 5) << QPoint(X1 + 3, YIntBand - 5);
			InOutP.setBrush(QColor(0xff, 0xff, 0x60));
			InOutP.setPen(Qt::NoPen);
			InOutP.drawPolygon(Tri);
		}
	}
	InOutP.setPen(QColor(0x60, 0x60, 0x60)); // restore for tick pass below
	InOutP.setPen(QColor(0x60, 0x60, 0x60));
	for (int IdxI = 0; IdxI < Index.size(); ++IdxI)
	{
		const auto& Entry = Index[IdxI];
		if (Entry.m_StartCycle < LeftC)
		{
			continue;
		}
		if (Entry.m_StartCycle > RightC)
		{
			break;
		}
		const int X = CycleToX(Entry.m_StartCycle);
		if (X < 0 || X > W)
		{
			continue;
		}
		InOutP.drawLine(X, RH - 8, X, RH); // tick on ruler
	}

	// Pass 2: labels. Skip entries whose tick is too close to the previous
	// labelled tick (keep >= ~120 px between labels to fit three text rows).
	InOutP.setPen(QColor(0xcc, 0xcc, 0xcc));
	int LastLabelX = -1000;
	for (int IdxI = 0; IdxI < Index.size(); ++IdxI)
	{
		const auto& Entry = Index[IdxI];
		if (Entry.m_StartCycle < LeftC)
		{
			continue;
		}
		if (Entry.m_StartCycle > RightC)
		{
			break;
		}
		const int X = CycleToX(Entry.m_StartCycle);
		if (X < 0 || X > W)
		{
			continue;
		}
		if (X - LastLabelX < 120)
		{
			continue;
		}
		LastLabelX = X;

		const QString FStr = QStringLiteral("F %1").arg(Entry.m_FrameNumber);
		const QString TStr = FormatTime(Entry.m_StartTimeNanos);
		const QString CStr = FormatCycle(Entry.m_StartCycle);
		InOutP.drawText(X + 2, YFrame + Fm.ascent(), FStr);
		InOutP.drawText(X + 2, YTime + Fm.ascent(), TStr);
		InOutP.drawText(X + 2, YCycle + Fm.ascent(), CStr);
	}

	// Cursor readout - golden line. Always shown when the model is loaded.
	{
		QString Readout = tr("cursor: frame %1  /  %2  /  cycle %3")
							  .arg(m_Cursor.m_Frame)
							  .arg(FormatTime(m_Cursor.m_TimeNanos))
							  .arg(FormatCycle(m_Cursor.m_Cycle));
		const int CgIdx = m_Model->CallGraph().FrameAtCycle(m_Cursor.m_Cycle);
		if (CgIdx >= 0)
		{
			const auto& Cf = m_Model->CallGraph().Frames()[CgIdx];
			Readout += tr("  /  in %1 (depth %2)").arg(HexAddr(Cf.m_TargetPC)).arg(Cf.m_Depth);
		}
		InOutP.setPen(QColor(0xff, 0xd0, 0x40));
		InOutP.drawText(4, YReadout + Fm.ascent(), Readout);
	}

	// CPU state - was the standalone RegisterView pane; lives here now.
	// Snapshot fields come from the nearest index entry whose StartCycle
	// is <= cursor.Cycle. Per-instruction running state would need a walk
	// from that snapshot, which is overkill for the at-frame-start view.
	{
		// Binary search for entry covering cursor.Cycle.
		int Lo = 0, Hi = Index.size() - 1;
		while (Lo < Hi)
		{
			const int Mid = (Lo + Hi + 1) / 2;
			if (Index[Mid].m_StartCycle <= m_Cursor.m_Cycle)
			{
				Lo = Mid;
			}
			else
			{
				Hi = Mid - 1;
			}
		}
		const auto& E = Index[Lo];
		InOutP.setPen(QColor(0xe0, 0xe0, 0xe0));
		const QString Reg1 = tr("PC=%1  SP=%2  AF=%3  BC=%4  DE=%5  HL=%6  IX=%7  IY=%8")
								 .arg(HexAddr(E.PC))
								 .arg(HexAddr(E.SP))
								 .arg(HexAddr(E.AF))
								 .arg(HexAddr(E.BC))
								 .arg(HexAddr(E.DE))
								 .arg(HexAddr(E.HL))
								 .arg(HexAddr(E.IX))
								 .arg(HexAddr(E.IY));
		InOutP.drawText(4, YReg1 + Fm.ascent(), Reg1);

		// The frame index lacks I/R/IFF/IM; identify these header values as
		// initial state rather than attributing them to the selected frame.
		const QString Reg2 = tr("I=%1  R=%2  IM=%3  IFF=%4/%5  (trace initial)")
								 .arg(H->m_InitI, 2, 16, QChar('0'))
								 .toUpper()
								 .arg(H->m_InitR, 2, 16, QChar('0'))
								 .arg(H->m_InitIM)
								 .arg(H->m_InitIFF1 ? '1' : '0')
								 .arg(H->m_InitIFF2 ? '1' : '0');
		InOutP.setPen(QColor(0xb0, 0xb0, 0xb0));
		InOutP.drawText(4, YReg2 + Fm.ascent(), Reg2);

		// Row 3 - bank mapping at cursor. On 48K/128K/Pentagon we report
		// the four 16K slots derived from $7FFD. On the Next we report
		// the eight 8K NextReg-driven slots - much finer-grained and
		// closer to what the running code actually sees. $FF in any NextReg
		// slot marks ROM; otherwise the value is the 8K page index.
		InOutP.setPen(QColor(0x90, 0x90, 0x90));
		if (m_Model->IsNextMachine())
		{
			const QByteArray Nrs = m_Model->NextRegs5057AtCycle(m_Cursor.m_Cycle);
			QString S = tr("banks (8K):  ");
			for (int IdxI = 0; IdxI < 8; ++IdxI)
			{
				const uint8_t V = uint8_t(Nrs[IdxI]);
				S += (V == 0xFF) ? QStringLiteral("ROM ") : QStringLiteral("%1 ").arg(V);
				if (IdxI < 7)
				{
					S += QStringLiteral("| ");
				}
			}
			InOutP.drawText(4, YBank + Fm.ascent(), S);
		}
		else
		{
			const uint8_t B7ffd = m_Model->Bank7FFDAtCycle(m_Cursor.m_Cycle);
			const int CBank3 = (B7ffd & 0x07);
			const int CRom = (B7ffd & 0x10) ? 1 : 0;
			const bool bScrAlt = (B7ffd & 0x08) != 0;
			InOutP.drawText(4, YBank + Fm.ascent(),
				tr("banks: ROM%1 | 5 | 2 | %2   $7FFD=$%3   screen=%4")
					.arg(CRom)
					.arg(CBank3)
					.arg(B7ffd, 2, 16, QChar('0'))
					.toUpper()
					.arg(bScrAlt ? tr("alt (bank 7)") : tr("normal (bank 5)")));
		}
	}
}

void IcicleWidget::mousePressEvent(QMouseEvent* Ev)
{
	// Middle-button OR Shift+LMB: enter pan mode.
	if (Ev->button() == Qt::MiddleButton || (Ev->button() == Qt::LeftButton && (Ev->modifiers() & Qt::ShiftModifier)))
	{
		m_bPanning = true;
		m_DragLastPos = Ev->pos();
		m_DragLeftStart = m_LeftCycle;
		QWidget::setCursor(Qt::ClosedHandCursor);
		return;
	}
	// Plain LMB: scrub cursor - fire on press AND keep firing while moving.
	if (Ev->button() == Qt::LeftButton)
	{
		m_bScrubbing = true;
		emit CursorRequested(XToCycle(Ev->pos().x()));
	}
}

void IcicleWidget::mouseMoveEvent(QMouseEvent* Ev)
{
	if (m_bPanning)
	{
		m_FittedExecution = -1;
		const int Dx = Ev->pos().x() - m_DragLastPos.x();
		const double kDC = -double(Dx) * m_CyclesPerPixel; // drag right -> pan left
		const double NewLeft = double(m_DragLeftStart) + kDC;
		m_LeftCycle = (NewLeft < 0.0) ? 0 : uint64_t(NewLeft);
		emit ViewportChanged(m_LeftCycle, XToCycle(width()));
		update();
		return;
	}
	if (m_bScrubbing && (Ev->buttons() & Qt::LeftButton))
	{
		// Keep emitting cursorRequested while the user drags the mouse
		// with LMB held - same UX as the overview track.
		emit CursorRequested(XToCycle(Ev->pos().x()));
	}
}

void IcicleWidget::mouseReleaseEvent(QMouseEvent* Ev)
{
	if (m_bPanning && (Ev->button() == Qt::MiddleButton || Ev->button() == Qt::LeftButton))
	{
		m_bPanning = false;
		QWidget::unsetCursor();
	}
	if (Ev->button() == Qt::LeftButton)
	{
		m_bScrubbing = false;
	}
}

void IcicleWidget::NavigateToCall(int Direction)
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return;
	}
	const auto& Frames = m_Model->CallGraph().Frames();
	if (Frames.isEmpty())
	{
		return;
	}

	// Frames are sorted by StartCycle. Binary search either:
	//   direction > 0 -> smallest StartCycle > cursor
	//   direction < 0 -> largest  StartCycle < cursor
	if (Direction > 0)
	{
		int Lo = 0, Hi = Frames.size();
		while (Lo < Hi)
		{
			const int Mid = (Lo + Hi) / 2;
			if (Frames[Mid].m_StartCycle > m_Cursor.m_Cycle)
			{
				Hi = Mid;
			}
			else
			{
				Lo = Mid + 1;
			}
		}
		if (Lo < Frames.size())
		{
			emit CursorRequested(Frames[Lo].m_StartCycle);
		}
	}
	else
	{
		int Lo = 0, Hi = Frames.size();
		while (Lo < Hi)
		{
			const int Mid = (Lo + Hi) / 2;
			if (Frames[Mid].m_StartCycle < m_Cursor.m_Cycle)
			{
				Lo = Mid + 1;
			}
			else
			{
				Hi = Mid;
			}
		}
		if (Lo > 0)
		{
			emit CursorRequested(Frames[Lo - 1].m_StartCycle);
		}
	}
}

void IcicleWidget::keyPressEvent(QKeyEvent* Ev)
{
	if (Ev->key() == Qt::Key_W || Ev->key() == Qt::Key_S)
	{
		ZoomAroundX(width() / 2, Ev->key() == Qt::Key_W ? 0.5 : 2.0);
		emit ViewportChanged(m_LeftCycle, XToCycle(width()));
		update();
		Ev->accept();
		return;
	}
	if (Ev->key() == Qt::Key_A || Ev->key() == Qt::Key_D)
	{
		const uint64_t Step = std::max<uint64_t>(1, uint64_t(m_CyclesPerPixel * width() / 4));
		SetViewportCycles(Ev->key() == Qt::Key_A ? (m_LeftCycle > Step ? m_LeftCycle - Step : 0) : m_LeftCycle + Step,
			m_CyclesPerPixel);
		Ev->accept();
		return;
	}
	if (Ev->key() == Qt::Key_Right)
	{
		NavigateToCall(+1);
		Ev->accept();
		return;
	}
	if (Ev->key() == Qt::Key_Left)
	{
		NavigateToCall(-1);
		Ev->accept();
		return;
	}
	QWidget::keyPressEvent(Ev);
}

void IcicleWidget::contextMenuEvent(QContextMenuEvent* Ev)
{
	QMenu Menu(this);
	auto* ActNext = Menu.addAction(tr("Next call  (->)"));
	auto* ActPrev = Menu.addAction(tr("Previous call  (<-)"));
	Menu.addSeparator();

	auto* ActHlInt = Menu.addAction(tr("Highlight interrupt code"));
	ActHlInt->setCheckable(true);
	ActHlInt->setChecked(m_HighlightMask & kHighlightBitHlInterrupt);

	auto* ActHlRom = Menu.addAction(tr("Highlight ROM code"));
	ActHlRom->setCheckable(true);
	ActHlRom->setChecked(m_HighlightMask & kHighlightBitHlRom);

	auto* ActHlDma = Menu.addAction(tr("Highlight DMA transfers"));
	ActHlDma->setCheckable(true);
	ActHlDma->setEnabled(false);
	ActHlDma->setToolTip(tr("DMA tracking arrives in Batch 4."));

	QAction* Picked = Menu.exec(Ev->globalPos());
	if (Picked == ActNext)
	{
		NavigateToCall(+1);
	}
	else if (Picked == ActPrev)
	{
		NavigateToCall(-1);
	}
	else if (Picked == ActHlInt)
	{
		if (ActHlInt->isChecked())
		{
			m_HighlightMask |= kHighlightBitHlInterrupt;
		}
		else
		{
			m_HighlightMask &= ~kHighlightBitHlInterrupt;
		}
		update();
	}
	else if (Picked == ActHlRom)
	{
		if (ActHlRom->isChecked())
		{
			m_HighlightMask |= kHighlightBitHlRom;
		}
		else
		{
			m_HighlightMask &= ~kHighlightBitHlRom;
		}
		update();
	}
	Ev->accept();
}

bool IcicleWidget::event(QEvent* Ev)
{
	if (Ev->type() != QEvent::ToolTip)
	{
		return QWidget::event(Ev);
	}
	auto* He = static_cast<QHelpEvent*>(Ev);
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		QToolTip::hideText();
		Ev->ignore();
		return true;
	}
	// Resolve mouse position -> (cycle, depth).
	const int kRH = RulerHeight();
	if (He->pos().y() < kRH)
	{
		QToolTip::hideText();
		Ev->ignore();
		return true;
	}
	const uint64_t Cycle = XToCycle(He->pos().x());
	const int Depth = (He->pos().y() - kRH) / BarHeight();

	// Find the CallFrame whose [StartCycle, EndCycle) covers cycle AT this
	// depth. Frames are sorted by StartCycle; linear scan since tooltips
	// are infrequent.
	const auto& Frames = m_Model->CallGraph().Frames();
	const CallFrame* Hit = nullptr;
	for (const auto& F : Frames)
	{
		if (F.m_StartCycle > Cycle)
		{
			break;
		}
		if (int(F.m_Depth) != Depth)
		{
			continue;
		}
		if (F.m_EndCycle <= Cycle)
		{
			continue;
		}
		Hit = &F;
	}

	const uint64_t Mhz = m_Model->GetHeader() ? m_Model->GetHeader()->m_BaseMhzX1e6 : 3'500'000ull;
	QString Tip;
	QLocale Loc;
	if (Hit != nullptr)
	{
		const uint64_t Dur = Hit->m_EndCycle - Hit->m_StartCycle;
		const double Ms = CyclesToMs(Dur, Mhz);
		const char* Kind = "CALL";
		switch (Hit->m_KindV)
		{
		case CallFrame::kKindZ80Rst:
			Kind = "RST";
			break;
		case CallFrame::kKindInterrupt:
			Kind = "INT";
			break;
		case CallFrame::kKindTrappedCall:
			Kind = "TRAP";
			break;
		default:
			break;
		}
		Tip = tr("%1 -> %2\n%3 cycles  /  %4 ms\nstart cycle %5\ndepth %6")
				  .arg(QString::fromLatin1(Kind))
				  .arg(HexAddr(Hit->m_TargetPC))
				  .arg(Loc.toString(qulonglong(Dur)))
				  .arg(QString::number(Ms, 'f', 3))
				  .arg(Loc.toString(qulonglong(Hit->m_StartCycle)))
				  .arg(Hit->m_Depth);
	}
	else if (Depth == 0)
	{
		// Synthetic root: report the cycle position and ROM/RAM segment.
		const bool bInRom = m_Model->CallGraph().IsCycleInRom(Cycle);
		Tip = tr("[main]  /  cycle %1  /  %2")
				  .arg(Loc.toString(qulonglong(Cycle)))
				  .arg(bInRom ? tr("ROM execution") : tr("RAM execution"));
	}
	if (Tip.isEmpty())
	{
		QToolTip::hideText();
		Ev->ignore();
		return true;
	}
	QToolTip::showText(He->globalPos(), Tip, this);
	return true;
}

void IcicleWidget::wheelEvent(QWheelEvent* Ev)
{
	// angleDelta in 1/8th degrees; one click = 120 (= 15 degrees). Each click zooms
	// by 1.25x (in or out). Ctrl modifier not required - wheel-zoom is the
	// standard convention; the surrounding scroll-wheel-pan use case is
	// handled by the bottom-row panes' own scrolling.
	const int Dy = Ev->angleDelta().y();
	if (Dy == 0)
	{
		return;
	}
	const double Steps = double(Dy) / 120.0;
	const double Factor = pow(1.25, -Steps); // wheel up -> zoom in -> factor < 1
	ZoomAroundX(int(Ev->position().x()), Factor);
	emit ViewportChanged(m_LeftCycle, XToCycle(width()));
	Ev->accept();
	update();
}
