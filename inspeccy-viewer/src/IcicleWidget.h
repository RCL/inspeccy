#pragma once
#include "Cursor.h"

#include <QPoint>
#include <QWidget>

#include <stdint.h>

struct TraceModel;

// Icicle / flame-graph paint over the reconstructed call frames in
// TraceModel::callGraph(). X axis = cycle position (zoomable + pannable),
// Y axis = call depth (root at top, deeper below - "icicle" orientation,
// matching Superluminal). The ruler strip (Phase 7c) is rendered as a
// reserved band at the top of this same widget.
struct IcicleWidget : public QWidget
{
	Q_OBJECT
  public:
	explicit IcicleWidget(QWidget* Parent = nullptr);
	void SetModel(TraceModel* M);

	// Programmatic viewport setter - used by the headless --icicle-zoom
	// capture path. Real interactive panning/zooming still flows through
	// the wheel / drag handlers.
	void SetViewportCycles(uint64_t LeftCycle, double CyclesPerPixel);
	// Programmatic highlight setter - same use case (headless capture).
	void SetHighlightMask(uint32_t Mask)
	{
		m_HighlightMask = Mask;
		update();
	}

  public slots:
	void SetCursor(const Cursor& C);
	void FitWholeTrace();
	void FitExecution(int Index);

  signals:
	// Emitted when the user clicks/scrubs the icicle to move the cursor.
	void CursorRequested(uint64_t Cycle);
	// Emitted on any viewport change (zoom or pan). Includes the post-change
	// visible window as [leftCycle, rightCycle). Down-stream panes (e.g. the
	// top-functions view) re-aggregate when this fires.
	void ViewportChanged(uint64_t LeftCycle, uint64_t RightCycle);

  public:
	// Highlight bitmask. When any bit is set, frames NOT matching the
	// selected categories are drawn dimmed so the matching ones stand out.
	enum HighlightBit
	{
		kHighlightBitHlNone = 0,
		kHighlightBitHlInterrupt = 1 << 0,
		kHighlightBitHlRom = 1 << 1,
		kHighlightBitHlDma = 1 << 2, // wired through in Batch 4
	};

  protected:
	void paintEvent(QPaintEvent* Ev) override;
	void mousePressEvent(QMouseEvent* Ev) override;
	void mouseMoveEvent(QMouseEvent* Ev) override;
	void mouseReleaseEvent(QMouseEvent* Ev) override;
	void wheelEvent(QWheelEvent* Ev) override;
	void resizeEvent(QResizeEvent* Ev) override;
	void keyPressEvent(QKeyEvent* Ev) override;
	void contextMenuEvent(QContextMenuEvent* Ev) override;
	bool event(QEvent* Ev) override; // for QEvent::ToolTip dispatch

  private:
	TraceModel* m_Model = nullptr;
	Cursor m_Cursor;

	// Viewport. Cycles-per-pixel needs to be a double because the dynamic
	// range is huge: at "whole trace" zoom a 7-minute demo trace is ~10^9
	// cycles wide; at "single instruction" zoom it's ~1 cycle. uint64 would
	// truncate fractional cpp at zoom-in.
	double m_CyclesPerPixel = 1.0;
	uint64_t m_LeftCycle = 0;
	int m_FittedExecution = -1;

	// Drag state.
	//   m_bPanning  -> middle-button / shift+LMB: drag pans the viewport.
	//   m_bScrubbing -> plain LMB held: drag keeps moving the cursor.
	bool m_bPanning = false;
	bool m_bScrubbing = false;
	QPoint m_DragLastPos;
	uint64_t m_DragLeftStart = 0;

	// The ruler band carries: frame#/time/cycle labels, frame ticks, the
	// cursor readout line (golden), AND the compact CPU state moved here
	// from the now-deleted RegisterView pane (two rows of registers).
	// 110 px gives ~8 lines of small monospace text without crowding the
	// icicle body.
	int RulerHeight() const
	{
		return 110;
	}
	int BarHeight() const
	{
		return 20;
	}

	uint64_t XToCycle(int X) const;
	int CycleToX(uint64_t C) const;

	void FitWholeTraceIfUnset();
	void ZoomAroundX(int X, double Factor);
	void PaintRuler(class QPainter& InOutP, int W, int RH, uint64_t LeftC, uint64_t RightC);
	// Step cursor to the call-frame whose StartCycle is the next (+1) or
	// previous (-1) one relative to the current cursor cycle. Emits
	// cursorRequested so MainWindow does the same broadcast as a click.
	void NavigateToCall(int Direction);

	uint32_t m_HighlightMask = kHighlightBitHlNone;
};
