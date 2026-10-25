#pragma once
#include "Cursor.h"

#include <QMainWindow>
#include <QStringList>
#include <atomic>

class QMenu;
struct TraceModel;
struct OverviewWidget;
struct IcicleWidget;
struct InstructionView;
struct MemoryView;
struct InterruptView;
class QTabWidget;
class QSplitter;

struct MainWindow : public QMainWindow
{
	Q_OBJECT
  public:
	explicit MainWindow(QWidget* Parent = nullptr);

	// Convenience for command-line argument: open a file at startup.
	void OpenTraceFile(const QString& Path);

	// Pane selection for the command-line screenshot helper.
	void SetInstructionsTopFunctionsMode(bool bOn);
	// Direct tab index for the instruction pane: 0 Disassembly, 1 Top
	// exclusive, 2 Top inclusive, 3 Call structure. Used by --instr-tab.
	void SetInstructionsTabIndex(int Idx);
	// Overview horizontal zoom, in index-entry (frame) units. Used by
	// --overview-zoom for headless capture.
	void SetOverviewZoom(int FirstFrame, int FrameCount);
	void SetMemoryVramMode(bool bOn);
	void SetMemoryModeIndex(int Idx); // direct combo-index override
	void SetIcicleZoom(uint64_t LeftCycle, double CyclesPerPixel);
	void SetCursorAtCycle(uint64_t Cycle);
	void SetIcicleHighlightMask(uint32_t Mask);

	// Read access for main.cpp's headless --bench: read the cycle extent and
	// call-frame count from the model, and force timed synchronous repaints
	// of the icicle via icicle()->grab().
	TraceModel* Model() const
	{
		return m_Model;
	}
	IcicleWidget* Icicle() const
	{
		return m_Icicle;
	}

  private slots:
	// Qt slots - connected via function pointer, so the names are ours to
	// pick (Qt doesn't mandate the casing the way it does for overridden
	// virtuals like paintEvent). House style: PascalCase.
	void OnFileOpen();
	void OnFileTrace();
	void OnLocateSymbols();
	void OnOpenRecent();
	void OnRunHistory(); // re-open the Trace dialog pre-filled from a history entry
	void OnCursorRequestedByFrame(uint64_t Frame);
	void OnCursorRequestedByCycle(uint64_t Cycle);

  private:
	void SetCursor(const Cursor& NewCursor);
	void AddToRecent(const QString& Path);
	void RebuildRecentMenu();
	void RebuildHistoryMenu();
	void FitExecution(int Index);

	QMenu* m_RecentMenu = nullptr;
	QMenu* m_HistoryMenu = nullptr;
	QStringList m_Recent;

	TraceModel* m_Model = nullptr;
	OverviewWidget* m_Overview = nullptr;
	IcicleWidget* m_Icicle = nullptr;
	InstructionView* m_Instr = nullptr;
	MemoryView* m_Memory = nullptr; // top-right Display (hex by default)
	MemoryView* m_Memory2 = nullptr; // bottom-right Display (ULA by default)
	InterruptView* m_Interrupts = nullptr;
	QTabWidget* m_AnalysisTabs = nullptr;
	QSplitter* m_VSplit = nullptr;

	Cursor m_Cursor;

	// Generation counter: incremented on every OpenTraceFile(). The
	// QtConcurrent metrics job captures the value at submit time and
	// checks it on completion - if a newer open() ran in between, the
	// stale job's completion is dropped instead of clobbering current
	// state. Cheap, race-free.
	std::atomic<quint64> m_OpenGen{0};
};
