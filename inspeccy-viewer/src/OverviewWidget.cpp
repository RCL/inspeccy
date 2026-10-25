#include "OverviewWidget.h"
#include "CpuSources.h"
#include "TraceModel.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <trace/trace_binary.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

namespace
{

// Map an absolute FrameNumber -> array index in m_Index (== bar index in
// the overview). Returns -1 if the cursor isn't inside the trace yet
// (e.g., zero-initialised pre-load) or the index is empty.
int FrameNumberToBar(const QVector<BinaryTrace::IndexEntry>& Index, uint64_t FrameNumber)
{
	if (Index.isEmpty())
	{
		return -1;
	}
	// Binary search for largest entry with FrameNumber <= frameNumber.
	int Lo = 0;
	int Hi = Index.size() - 1;
	if (FrameNumber < Index.first().m_FrameNumber)
	{
		return -1;
	}
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi + 1) / 2;
		if (Index[Mid].m_FrameNumber <= FrameNumber)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid - 1;
		}
	}
	return Lo;
}

// Thousands-separated cycle count - six-digit T-state totals are much easier
// to compare at a glance when grouped.
QString FormatCycles(uint64_t V)
{
	QString S = QString::number(V);
	for (int IdxI = S.size() - 3; IdxI > 0; IdxI -= 3)
	{
		S.insert(IdxI, QChar(0x2009)); // thin space
	}
	return S;
}

} // namespace

OverviewWidget::OverviewWidget(QWidget* Parent) : QWidget(Parent)
{
	setMinimumHeight(96);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	// Needed for the hover breakdown: mouseMoveEvent must fire with no button
	// held. (This used to be off because the only mouse handling was drag
	// scrubbing, which made tracking pure overhead.)
	setMouseTracking(true);
}

void OverviewWidget::SetModel(TraceModel* Model)
{
	m_Model = Model;
	FitWholeRun(); // a newly opened trace always starts fully zoomed out
}

void OverviewWidget::SetCursor(const Cursor& C)
{
	m_Cursor = C;
	update();
}

void OverviewWidget::SetIcicleViewport(uint64_t LeftCycle, uint64_t RightCycle)
{
	m_IcicleLeft = LeftCycle;
	m_IcicleRight = RightCycle;
	update();
}

// --- view window ------------------------------------------------------------

int OverviewWidget::TotalBars() const
{
	return (m_Model != nullptr) ? m_Model->CpuUtilisationPerFrame().size() : 0;
}

int OverviewWidget::ViewCount() const
{
	const int N = TotalBars();
	if (N <= 0)
	{
		return 1;
	}
	if (m_ViewCount <= 0)
	{
		return N; // unset -> whole run
	}
	return std::clamp(m_ViewCount, 1, N);
}

int OverviewWidget::ViewFirst() const
{
	const int N = TotalBars();
	if (N <= 0)
	{
		return 0;
	}
	return std::clamp(m_ViewFirst, 0, std::max(0, N - ViewCount()));
}

void OverviewWidget::FitWholeRun()
{
	m_ViewFirst = 0;
	m_ViewCount = 0; // 0 == "whole run", and stays right if N changes
	update();
}

void OverviewWidget::SetViewRange(int First, int Count)
{
	const int N = TotalBars();
	if (N <= 0)
	{
		return;
	}
	// Never zoom past a handful of frames - below that the bars carry no more
	// information and the trace is better read in the icicle.
	Count = std::clamp(Count, std::min(4, N), N);
	First = std::clamp(First, 0, N - Count);
	m_ViewCount = (Count >= N) ? 0 : Count;
	m_ViewFirst = (Count >= N) ? 0 : First;
	update();
}

void OverviewWidget::ZoomAroundX(int X, double Factor)
{
	const int W = std::max(1, width());
	// Keep the frame under the pointer stationary as the window scales.
	const double PivotFrac = double(std::clamp(X, 0, W - 1)) / double(W);
	const double PivotBar = double(ViewFirst()) + PivotFrac * double(ViewCount());
	const int NewCount = int(std::lround(double(ViewCount()) * Factor));
	SetViewRange(int(std::lround(PivotBar - PivotFrac * double(NewCount))), NewCount);
}

int OverviewWidget::XToBar(int X) const
{
	const int W = std::max(1, width());
	const int B = ViewFirst() + int((qint64(std::clamp(X, 0, W - 1)) * qint64(ViewCount())) / qint64(W));
	return std::clamp(B, 0, std::max(0, TotalBars() - 1));
}

int OverviewWidget::BarToX(double Bar) const
{
	const int W = std::max(1, width());
	return int(((Bar - double(ViewFirst())) / double(ViewCount())) * double(W));
}

// Fractional index-entry position of a cycle. Interpolating inside the entry
// keeps the timeline box's edges where they belong when the track is zoomed in
// far enough that one frame is many pixels wide.
double OverviewWidget::BarAtCycle(uint64_t Cycle) const
{
	const auto& Index = m_Model->Index();
	const int N = Index.size();
	if (N <= 0)
	{
		return 0.0;
	}
	if (Cycle <= Index.first().m_StartCycle)
	{
		return 0.0;
	}

	int Lo = 0, Hi = N - 1;
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi + 1) / 2;
		if (Index[Mid].m_StartCycle <= Cycle)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid - 1;
		}
	}
	const uint64_t Span = FrameSpan(Lo);
	const double Frac = (Span > 0) ? std::min(1.0, double(Cycle - Index[Lo].m_StartCycle) / double(Span)) : 0.0;
	return double(Lo) + Frac;
}

int OverviewWidget::CycleToX(uint64_t Cycle) const
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return -1;
	}
	if (m_Model->Index().isEmpty())
	{
		return -1;
	}
	return BarToX(BarAtCycle(Cycle));
}

// The frame a downsampled column represents: the busiest one it covers.
int OverviewWidget::WorstFrameIn(int First, int Last) const
{
	const auto& Util = m_Model->CpuUtilisationPerFrame();
	int Best = First;
	float BestU = -1.0f;
	for (int F = First; F <= Last && F < Util.size(); ++F)
	{
		if (Util[F] > BestU)
		{
			BestU = Util[F];
			Best = F;
		}
	}
	return Best;
}

int OverviewWidget::LegendHeight() const
{
	if (m_Model == nullptr || m_Model->SourceIds().isEmpty())
	{
		return 0;
	}
	return QFontMetrics(font()).height() + 6;
}

uint64_t OverviewWidget::FrameSpan(int Entry) const
{
	using namespace BinaryTrace;
	const auto& Index = m_Model->Index();
	if (Entry < 0 || Entry >= Index.size())
	{
		return 0;
	}
	// Use the cycle span between index entries, divided by their frame delta.
	// The header's nominal period does not reflect later turbo changes;
	// sparse entries may also span multiple idle frames.
	if (Entry + 1 < Index.size())
	{
		const uint64_t CycDelta = Index[Entry + 1].m_StartCycle - Index[Entry].m_StartCycle;
		const uint32_t FrmDelta = Index[Entry + 1].m_FrameNumber - Index[Entry].m_FrameNumber;
		if (FrmDelta > 0 && CycDelta / FrmDelta > 0)
		{
			return CycDelta / FrmDelta;
		}
	}
	// Last frame (or a degenerate index): prefer the real tail span, then the
	// header's nominal period as a last resort.
	const Header* H = m_Model->GetHeader();
	if (H != nullptr && H->m_EndCycle != kSentinel && H->m_EndCycle > Index[Entry].m_StartCycle)
	{
		return H->m_EndCycle - Index[Entry].m_StartCycle;
	}
	const uint32_t NominalPeriod = m_Model->FrameIntPeriod();
	if (NominalPeriod != 0)
	{
		return NominalPeriod;
	}
	return 0;
}

OverviewWidget::Breakdown OverviewWidget::BreakdownAtX(int X) const
{
	Breakdown B;
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return B;
	}
	const int N = m_Model->CpuUtilisationPerFrame().size();
	const int W = width();
	if (N <= 0 || W <= 0)
	{
		return B;
	}

	// Frames covered by this pixel column, inside the current zoom window.
	const int VFirst = ViewFirst();
	const int VCount = ViewCount();
	int FStart = 0;
	int FEnd = 0; // exclusive
	if (VCount <= W)
	{
		// One frame spans several pixels: invert the bar's x range.
		FStart = XToBar(X);
		FEnd = FStart + 1;
	}
	else
	{
		FStart = VFirst + int((qint64(X) * qint64(VCount)) / qint64(W));
		FEnd = VFirst + int((qint64(X + 1) * qint64(VCount)) / qint64(W));
		if (FEnd <= FStart)
		{
			FEnd = FStart + 1;
		}
		FStart = std::clamp(FStart, 0, N - 1);
		FEnd = std::clamp(FEnd, FStart + 1, N);
	}

	const int Cols = m_Model->SourceIds().size();
	B.m_FirstFrame = FStart;
	B.m_LastFrame = FEnd - 1;
	B.m_FrameCount = FEnd - FStart;
	B.m_FrameBudget = FrameSpan(FStart);

	// Mean busy cycles across the group, for context under the peak.
	uint64_t ActiveSum = 0;
	for (int F = FStart; F < FEnd; ++F)
	{
		const uint64_t Span = FrameSpan(F);
		ActiveSum += uint64_t(double(m_Model->CpuUtilisationPerFrame()[F]) * double(Span) + 0.5);
	}
	B.m_MeanActive = ActiveSum / uint64_t(B.m_FrameCount);

	// ...and the frame the column actually draws.
	B.m_WorstFrame = WorstFrameIn(FStart, FEnd - 1);
	const uint64_t WSpan = FrameSpan(B.m_WorstFrame);
	B.m_WorstSource.fill(0, Cols);
	if (Cols == 0)
	{
		// No per-source split available (metrics still running, or a trace
		// whose walk produced nothing). Still report the busy total so the
		// hover says something true rather than 0%.
		B.m_WorstActive = uint64_t(double(m_Model->CpuUtilisationPerFrame()[B.m_WorstFrame]) * double(WSpan) + 0.5);
	}
	else
	{
		for (int C = 0; C < Cols; ++C)
		{
			const uint64_t Cy = uint64_t(double(m_Model->SourceUtilisationAt(B.m_WorstFrame, C)) * double(WSpan) + 0.5);
			B.m_WorstSource[C] = Cy;
			B.m_WorstActive += Cy;
		}
	}
	return B;
}

QString OverviewWidget::TooltipFor(const Breakdown& B) const
{
	if (B.m_FirstFrame < 0 || m_Model == nullptr)
	{
		return {};
	}
	const auto& Index = m_Model->Index();
	const auto& Ids = m_Model->SourceIds();

	QString Head;
	if (B.m_FirstFrame == B.m_LastFrame)
	{
		Head = tr("Frame %1").arg(Index[B.m_FirstFrame].m_FrameNumber);
	}
	else
	{
		Head = tr("Frames %1-%2 (%3 frames)")
				   .arg(Index[B.m_FirstFrame].m_FrameNumber)
				   .arg(Index[B.m_LastFrame].m_FrameNumber)
				   .arg(B.m_LastFrame - B.m_FirstFrame + 1);
	}

	// The table describes ONE real frame - the busiest in the column - because
	// that is what the bar is drawn from. Quoting a multi-frame sum here used to
	// read as a wrong frame length (a 6-frame TS2068 column looked like a
	// 352k-cycle frame instead of 58 688).
	const double Budget = double(B.m_FrameBudget ? B.m_FrameBudget : 1);
	const double Pct = 100.0 * double(B.m_WorstActive) / Budget;

	// Bold the identity line only - the qualifier below it reads as a note.
	Head = QStringLiteral("<b>%1</b>").arg(Head);
	if (B.m_FrameCount > 1)
	{
		Head += QStringLiteral("<br><i>%1</i>")
					.arg(tr("column drawn from its busiest frame, %1").arg(Index[B.m_WorstFrame].m_FrameNumber));
	}

	QString S = QStringLiteral("%1<br>%2<br><table cellspacing='2'>")
					.arg(Head)
					.arg(tr("CPU %1% busy of %2 T").arg(Pct, 0, 'f', 1).arg(FormatCycles(B.m_FrameBudget)));

	for (int C = 0; C < Ids.size(); ++C)
	{
		const uint64_t Cy = B.m_WorstSource.value(C, 0);
		if (Cy == 0 && Ids[C] != CpuSource::kMain)
		{
			continue; // don't list idle sources
		}
		const double P = 100.0 * double(Cy) / Budget;
		S += QStringLiteral("<tr>"
							"<td bgcolor='%1' width='10'>&nbsp;</td>"
							"<td>&nbsp;%2&nbsp;&nbsp;</td>"
							"<td align='right'>%3%&nbsp;&nbsp;</td>"
							"<td align='right'>%4 T</td>"
							"</tr>")
				 .arg(CpuSource::Colour(Ids[C]).name())
				 .arg(CpuSource::Name(Ids[C]).toHtmlEscaped())
				 .arg(P, 0, 'f', 1)
				 .arg(FormatCycles(Cy));
	}
	// Remainder of the frame not accounted for by executed instructions - HALT
	// idle and/or cycles lost to ULA contention. The budget is the real per-frame
	// cycle span (see frameSpan; turbo-aware), so this closes the 100%.
	if (B.m_FrameBudget > B.m_WorstActive)
	{
		const uint64_t Idle = B.m_FrameBudget - B.m_WorstActive;
		const double P = 100.0 * double(Idle) / Budget;
		S += QStringLiteral("<tr>"
							"<td bgcolor='#303030' width='10'>&nbsp;</td>"
							"<td>&nbsp;%1&nbsp;&nbsp;</td>"
							"<td align='right'>%2%&nbsp;&nbsp;</td>"
							"<td align='right'>%3 T</td>"
							"</tr>")
				 .arg(tr("idle / contention"))
				 .arg(P, 0, 'f', 1)
				 .arg(FormatCycles(Idle));
	}
	S += QStringLiteral("</table>");

	// Context for the peak: what the rest of the column looked like.
	if (B.m_FrameCount > 1)
	{
		S += QStringLiteral("<i>%1</i>")
				 .arg(tr("mean over these %1 frames: %2% busy")
						 .arg(B.m_FrameCount)
						 .arg(100.0 * double(B.m_MeanActive) / Budget, 0, 'f', 1));
	}
	return S;
}

void OverviewWidget::paintEvent(QPaintEvent*)
{
	QPainter P(this);
	P.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));

	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		P.setPen(QColor(0x90, 0x90, 0x90));
		P.drawText(rect(), Qt::AlignCenter, tr("Open a .btr file (File -> Open)"));
		return;
	}

	const auto& Util = m_Model->CpuUtilisationPerFrame();
	const int N = Util.size();
	if (N <= 0)
	{
		// Trace loaded but index empty - explicit hint to the user.
		P.setPen(QColor(0xcc, 0x80, 0x40));
		P.drawText(rect(), Qt::AlignCenter,
			tr("Trace loaded but no .fidx sidecar - "
			   "overview unavailable. Rebuild with --trace-index-out."));
		return;
	}

	const int W = width();
	const int H = height();
	const auto& Ids = m_Model->SourceIds();
	const int Cols = Ids.size();

	// --- legend -------------------------------------------------------------
	const int LegendH = LegendHeight();
	if (LegendH > 0)
	{
		const QFontMetrics Fm(font());
		int Lx = 4;
		const int Ly = 3;
		const int Sw = Fm.height() - 4; // swatch side
		P.setPen(QColor(0xc8, 0xc8, 0xc8));
		for (int C = 0; C < Cols; ++C)
		{
			const QString Name = CpuSource::Name(Ids[C]);
			const int Tw = Fm.horizontalAdvance(Name);
			if (Lx + Sw + 4 + Tw > W - 4)
			{
				break; // ran out of room; drop the rest
			}
			P.fillRect(Lx, Ly + 2, Sw, Sw, CpuSource::Colour(Ids[C]));
			P.drawText(Lx + Sw + 4, Ly, Tw, Fm.height(), Qt::AlignLeft | Qt::AlignVCenter, Name);
			Lx += Sw + 4 + Tw + 12;
		}
	}

	// --- stacked bars -------------------------------------------------------
	// Bars grow up from the bottom edge. Segments are laid out by accumulating
	// fractions and rounding once per boundary, so adjacent bands share an edge
	// exactly - rounding each band's height independently leaves 1px seams.
	const int ChartH = std::max(1, H - LegendH);

	auto PaintColumn = [&](int XStart, int BarW, const float* Fracs, float TotalU)
	{
		if (Cols == 0)
		{
			const int BarH = int(ChartH * TotalU);
			const QColor C = QColor::fromHsv(int(120.0f * (1.0f - TotalU)), 200, 220);
			P.fillRect(XStart, H - BarH, BarW, BarH, C);
			return;
		}
		float Acc = 0.0f;
		int PrevY = H;
		for (int C = 0; C < Cols; ++C)
		{
			Acc += Fracs[C];
			const int Y = H - int(float(ChartH) * std::min(Acc, 1.0f) + 0.5f);
			if (Y < PrevY)
			{
				P.fillRect(XStart, Y, BarW, PrevY - Y, CpuSource::Colour(Ids[C]));
				PrevY = Y;
			}
		}
	};

	// Draw one frame's stack into a column.
	auto PaintFrame = [&](int XStart, int BarW, int F)
	{
		QVector<float> Acc(Cols);
		for (int C = 0; C < Cols; ++C)
		{
			Acc[C] = m_Model->SourceUtilisationAt(F, C);
		}
		PaintColumn(XStart, BarW, Acc.constData(), Util[F]);
	};

	const int VFirst = ViewFirst();
	const int VCount = ViewCount();
	if (VCount <= W)
	{
		// Zoomed in far enough that every frame gets its own column: exact.
		for (int IdxI = 0; IdxI < VCount; ++IdxI)
		{
			const int F = VFirst + IdxI;
			const int XStart = (IdxI * W) / VCount;
			const int XEnd = ((IdxI + 1) * W) / VCount;
			PaintFrame(XStart, std::max(1, XEnd - XStart), F);
		}
	}
	else
	{
		// More frames than pixels. Downsample by PEAK, not by mean: a column
		// shows its busiest frame, whole, so a single frame that saturated
		// still reads at full height instead of being averaged away by its
		// quieter neighbours. Averaging is what a "how typical" view would
		// want; this track exists to answer "did anything overrun".
		for (int X = 0; X < W; ++X)
		{
			const int FStart = VFirst + int((qint64(X) * qint64(VCount)) / qint64(W));
			const int FEnd = VFirst + int((qint64(X + 1) * qint64(VCount)) / qint64(W));
			if (FEnd <= FStart)
			{
				continue;
			}
			PaintFrame(X, 1, WorstFrameIn(FStart, std::min(FEnd, N) - 1));
		}
	}

	// Icicle viewport rectangle: translucent fill spanning the cycle range
	// currently visible in the icicle. Painted BEFORE the cursor line so
	// the cursor reads on top. Skipped if the viewport spans the entire
	// trace (no zoom in) - would just dim the whole track for no info.
	if (m_IcicleRight > m_IcicleLeft)
	{
		const int kXL = CycleToX(m_IcicleLeft);
		const int kXR = CycleToX(m_IcicleRight);
		if (kXL >= 0 && kXR > kXL && (kXR - kXL) < W - 2)
		{
			P.fillRect(kXL, LegendH, kXR - kXL, H - LegendH, QColor(0xff, 0xff, 0xff, 0x30));
			// Edge brackets to make the box legible against the bars.
			P.setPen(QPen(QColor(0xff, 0xff, 0xff, 0xa0), 1));
			P.drawLine(kXL, LegendH, kXL, H - 1);
			P.drawLine(kXR, LegendH, kXR, H - 1);
		}
	}

	// Hover breakdowns can't be exercised headlessly, so the same debug switch
	// that dumps the whole-trace split (INSPECCY_OVERVIEW_DEBUG, see
	// TraceModel::computeMetrics) also writes the tooltip text for the middle
	// column to <path>.tooltip, which is enough to check the cycle arithmetic
	// and the markup without a mouse.
	if (const char* DbgPath = getenv("INSPECCY_OVERVIEW_DEBUG"))
	{
		static bool bDumped = false;
		// Only once metrics have actually produced buckets - the first paint
		// happens while the worker is still walking the trace.
		if (!bDumped && Cols > 0)
		{
			bDumped = true;
			if (FILE* F = fopen((std::string(DbgPath) + ".tooltip").c_str(), "w"))
			{
				// Fixed sample points usually all land in one uniformly-busy
				// stretch, which proves nothing about peak-preserving
				// downsampling. So also seek out the column where the busiest
				// frame differs MOST from the group mean - that is the column
				// a mean-based renderer would have flattened.
				int GapX = -1;
				double GapPct = 0.0;
				for (int X = 0; X < W; ++X)
				{
					const Breakdown C = BreakdownAtX(X);
					if (C.m_FirstFrame < 0 || C.m_FrameCount <= 1 || C.m_FrameBudget == 0)
					{
						continue;
					}
					const double D = 100.0 * double(C.m_WorstActive - std::min(C.m_WorstActive, C.m_MeanActive)) /
									 double(C.m_FrameBudget);
					if (D > GapPct)
					{
						GapPct = D;
						GapX = X;
					}
				}
				fprintf(F, "widest peak-vs-mean gap: %.1f pp at x=%d\n\n", GapPct, GapX);
				const int Xs[] = {0, GapX >= 0 ? GapX : W / 2, W / 2};
				for (int X : Xs)
				{
					if (X < 0)
					{
						continue;
					}
					fprintf(F, "x=%d: %s\n\n", X, qPrintable(TooltipFor(BreakdownAtX(X))));
				}
				fclose(F);
			}
		}
	}

	// Cursor. Convert the cursor's *absolute* FrameNumber back into an
	// array index (= bar position) via the model's index. Use a thicker
	// pen (2px) + a small triangular marker on top so it stays visible
	// against the bars during interactive drag scrubs.
	const int Bar = FrameNumberToBar(m_Model->Index(), m_Cursor.m_Frame);
	if (Bar >= 0 && Bar >= ViewFirst() && Bar < ViewFirst() + ViewCount())
	{
		const int XCursor = BarToX(double(Bar));
		P.setPen(QPen(QColor(0xff, 0xd0, 0x40), 2));
		P.drawLine(XCursor, LegendH, XCursor, H);
		// Triangle marker - easier to spot than a bare line during drag.
		QPolygon Tri;
		Tri << QPoint(XCursor - 4, LegendH) << QPoint(XCursor + 4, LegendH) << QPoint(XCursor, LegendH + 6);
		P.setBrush(QColor(0xff, 0xd0, 0x40));
		P.setPen(Qt::NoPen);
		P.drawPolygon(Tri);
	}
}

void OverviewWidget::EmitCursorAtX(int X)
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return;
	}
	const auto& Index = m_Model->Index();
	const int N = Index.size();
	if (N <= 0)
	{
		return;
	}
	// Convert pixel column -> bar index -> index-entry's absolute frame
	// number. Emitting a raw bar count was wrong: when entries are sparse
	// (one per ULA frame) the absolute FrameNumber for bar K is much
	// larger than K, and the receiver (MainWindow) needs the absolute
	// number to look up via TraceModel::cursorAtFrame. xToBar() also folds in
	// the zoom window, so scrubbing stays under the pointer when zoomed.
	const int Clamped = std::clamp(XToBar(X), 0, N - 1);
	emit CursorRequested(Index[Clamped].m_FrameNumber);
}

void OverviewWidget::mousePressEvent(QMouseEvent* Ev)
{
	if (Ev->button() == Qt::LeftButton)
	{
		EmitCursorAtX(Ev->pos().x());
	}
	else if (Ev->button() == Qt::MiddleButton)
	{
		// Pan, same binding the icicle uses.
		m_bPanning = true;
		m_PanLastPos = Ev->pos();
		m_PanFirstAtPress = ViewFirst();
		// Qualified: this class has its own setCursor(const Cursor&) slot,
		// which hides QWidget::setCursor.
		QWidget::setCursor(Qt::ClosedHandCursor);
	}
}

void OverviewWidget::mouseReleaseEvent(QMouseEvent* Ev)
{
	if (Ev->button() == Qt::MiddleButton && m_bPanning)
	{
		m_bPanning = false;
		unsetCursor();
	}
}

void OverviewWidget::wheelEvent(QWheelEvent* Ev)
{
	// angleDelta is in 1/8th degrees; one notch = 120. Wheel up zooms in,
	// matching the icicle.
	const double Steps = double(Ev->angleDelta().y()) / 120.0;
	if (Steps == 0.0)
	{
		Ev->ignore();
		return;
	}
	ZoomAroundX(int(Ev->position().x()), pow(1.25, -Steps));
	Ev->accept();
}

void OverviewWidget::contextMenuEvent(QContextMenuEvent* Ev)
{
	QMenu Menu(this);
	auto* ActFit = Menu.addAction(tr("Fit whole run"));
	ActFit->setEnabled(m_ViewCount > 0);

	auto* ActToTimeline = Menu.addAction(tr("Zoom to timeline below"));
	// Only meaningful when the icicle is showing a proper sub-range.
	const bool bHaveViewport = (m_IcicleRight > m_IcicleLeft);
	ActToTimeline->setEnabled(bHaveViewport);

	Menu.addSeparator();
	auto* ActIn = Menu.addAction(tr("Zoom in"));
	auto* ActOut = Menu.addAction(tr("Zoom out"));

	QAction* Picked = Menu.exec(Ev->globalPos());
	if (Picked == ActFit)
	{
		FitWholeRun();
	}
	else if (Picked == ActToTimeline && bHaveViewport)
	{
		const int A = int(BarAtCycle(m_IcicleLeft));
		const int B = int(ceil(BarAtCycle(m_IcicleRight)));
		SetViewRange(A, std::max(1, B - A));
	}
	else if (Picked == ActIn)
	{
		ZoomAroundX(Ev->pos().x(), 1.0 / 1.5);
	}
	else if (Picked == ActOut)
	{
		ZoomAroundX(Ev->pos().x(), 1.5);
	}
	Ev->accept();
}

void OverviewWidget::mouseMoveEvent(QMouseEvent* Ev)
{
	if (m_bPanning)
	{
		const int Dx = Ev->pos().x() - m_PanLastPos.x();
		const int W = std::max(1, width());
		// Drag right -> move the window earlier, so the content follows the hand.
		const int Shift = int(std::lround(-double(Dx) * double(ViewCount()) / double(W)));
		if (Shift != 0)
		{
			m_PanLastPos = Ev->pos();
			SetViewRange(ViewFirst() + Shift, ViewCount());
		}
		return;
	}
	if (Ev->buttons() & Qt::LeftButton)
	{
		EmitCursorAtX(Ev->pos().x());
	}

	// Hover breakdown. Shown for any position over the track (including while
	// dragging, where it doubles as a readout of what you are scrubbing past).
	const Breakdown B = BreakdownAtX(Ev->pos().x());
	const QString Text = TooltipFor(B);
	if (Text.isEmpty())
	{
		QToolTip::hideText();
	}
	else
	{
		QToolTip::showText(Ev->globalPosition().toPoint(), Text, this);
	}
}

void OverviewWidget::leaveEvent(QEvent* Ev)
{
	QToolTip::hideText();
	QWidget::leaveEvent(Ev);
}
