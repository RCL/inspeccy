#pragma once
#include "Cursor.h"

#include <QPoint>
#include <QVector>
#include <QWidget>

#include <stdint.h>

struct TraceModel;

// "Where am I in the trace" track. One vertical bar per emulated frame; the
// bar's height is that frame's CPU utilisation, and the bar is STACKED by where
// the time went - main flow at the bottom, then one band per interrupt source
// (frame interrupt, CTC0, ...), each in its own colour. A legend names the
// colours; hovering a bar pops up the itemised split for that frame. Cursor
// position is drawn as a vertical line; click / drag scrubs the cursor.

struct OverviewWidget : public QWidget
{
	Q_OBJECT
  public:
	explicit OverviewWidget(QWidget* Parent = nullptr);

	void SetModel(TraceModel* Model);
	// Programmatic zoom, mirroring IcicleWidget::setViewportCycles - used by
	// the headless --overview-zoom capture path. Interactive zoom still flows
	// through the wheel / context menu.
	void SetViewWindow(int FirstFrame, int FrameCount)
	{
		SetViewRange(FirstFrame, FrameCount);
	}

	QSize sizeHint() const override
	{
		return {600, 132};
	}
	QSize minimumSizeHint() const override
	{
		return {200, 96};
	}

  public slots:
	void SetCursor(const Cursor& C);
	// Receives the icicle's current visible cycle window so we can paint a
	// translucent "viewport rectangle" showing the user where in the trace
	// the icicle is zoomed into.
	void SetIcicleViewport(uint64_t LeftCycle, uint64_t RightCycle);

  signals:
	void CursorRequested(uint64_t Frame); // user-driven scrub by frame index

  protected:
	void paintEvent(QPaintEvent* Ev) override;
	void mousePressEvent(QMouseEvent* Ev) override;
	void mouseMoveEvent(QMouseEvent* Ev) override;
	void mouseReleaseEvent(QMouseEvent* Ev) override;
	void wheelEvent(QWheelEvent* Ev) override;
	void contextMenuEvent(QContextMenuEvent* Ev) override;
	void leaveEvent(QEvent* Ev) override;

  private:
	// The frames a single pixel column covers, and how their cycles split
	// across sources. SourceCycles is parallel to TraceModel::sourceIds().
	struct Breakdown
	{
		int m_FirstFrame = -1; // index-entry indices, [First, Last]
		int m_LastFrame = -1;
		int m_FrameCount = 0; // frames this column covers
		uint64_t m_FrameBudget = 0; // nominal cycles in ONE frame

		// The frame the column actually DRAWS: the busiest one in the group.
		// A mean would hide the single frame that blew the budget, which is
		// the whole thing you scan this track for - so downsampling keeps the
		// peak, and the numbers below describe that real frame rather than a
		// synthetic average of several.
		int m_WorstFrame = -1;
		uint64_t m_WorstActive = 0;
		QVector<uint64_t> m_WorstSource;

		// Group mean, reported alongside so the peak has context.
		uint64_t m_MeanActive = 0;
	};

	void EmitCursorAtX(int X);
	int CycleToX(uint64_t Cycle) const;
	int LegendHeight() const;
	// Cycle span of one index entry (frame), 0 if not determinable.
	uint64_t FrameSpan(int Entry) const;

	// --- horizontal view window, in index-entry (frame) units ---------------
	// The track can be zoomed into a sub-range of the run. Everything that maps
	// between x and frames goes through these, so the bars, the cursor line, the
	// timeline box and click-to-scrub all stay consistent when zoomed.
	int TotalBars() const; // index entries in the trace
	int ViewFirst() const; // first entry shown
	int ViewCount() const; // entries shown (>= 1)
	void FitWholeRun(); // reset the zoom
	void SetViewRange(int First, int Count); // clamped
	void ZoomAroundX(int X, double Factor);
	int XToBar(int X) const; // pixel -> entry index
	double BarAtCycle(uint64_t Cycle) const; // cycle -> fractional entry
	int BarToX(double Bar) const; // fractional entry -> pixel
	// Busiest frame in [first, last] - the one a downsampled column draws.
	int WorstFrameIn(int First, int Last) const;
	// Index-entry range shown at pixel column x, or a Breakdown with
	// FirstFrame < 0 if x is outside the data.
	Breakdown BreakdownAtX(int X) const;
	QString TooltipFor(const Breakdown& B) const;

	TraceModel* m_Model = nullptr;
	Cursor m_Cursor;
	uint64_t m_IcicleLeft = 0;
	uint64_t m_IcicleRight = 0;

	// Zoom window. m_ViewCount == 0 means "not set yet" -> fit the whole run,
	// so a freshly opened trace always starts fully zoomed out.
	int m_ViewFirst = 0;
	int m_ViewCount = 0;
	bool m_bPanning = false;
	QPoint m_PanLastPos;
	int m_PanFirstAtPress = 0;
};
