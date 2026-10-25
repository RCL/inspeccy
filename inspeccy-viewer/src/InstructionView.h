#pragma once
#include "Cursor.h"

#include <QAbstractListModel>
#include <QWidget>
#include <QVector>

#include <stdint.h>

class QListView;
class QStackedWidget;
class QTabBar;
class QTreeWidget;
struct TraceModel;

// One displayable row in the InstructionView. Each row is either an actual
// instruction record (Tag = kRecord_Instr / LdirIter / DmaDrip / etc.) or a
// marker for an interrupt / invalid-opcode exit. The Display string is
// pre-formatted at refresh time so paint stays cheap.
struct InstrRow
{
	uint64_t m_Cycle = 0;
	uint64_t m_FrameStartCycle = 0; // cycle of the most-recent FrameBoundary
									// <= Cycle; used by SinceVBlank mode
	uint16_t PC = 0;
	uint8_t m_Tag = 0; // BinaryTrace::kRecord_*
	bool m_bIsCursor = false; // highlight target?
	int m_Depth = 0; // call-nesting depth at this cycle
					 // (0 = main, 1 = first CALL, ...). Drives
					 // the horizontal separator the delegate
					 // draws where depth changes CALL/RET.
	QString m_Display;
};

// Backing model for the QListView. Cheap to reset - we rebuild on each cursor
// move with a ~50-row window, so we use beginResetModel / endResetModel rather
// than per-row dataChanged signals.
struct InstrListModel : public QAbstractListModel
{
	Q_OBJECT
  public:
	explicit InstrListModel(QObject* Parent = nullptr);

	int rowCount(const QModelIndex& Parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& Index, int Role) const override;

	void SetRows(QVector<InstrRow> Rows);
	int CursorRow() const
	{
		return m_CursorRow;
	}

  private:
	QVector<InstrRow> m_Rows;
	int m_CursorRow = -1;
};

struct InstructionView : public QWidget
{
	Q_OBJECT
  public:
	// Tab order is the enum order - the tab bar's index IS the Mode.
	enum Mode
	{
		kModeLinear = 0, // ~50 records around the cursor
		kModeTopExclusive = 1, // routines by SELF cycles (children excluded)
		kModeTopInclusive = 2, // routines by TOTAL cycles (children included)
		kModeCallStructure = 3, // calling-context tree from the main flow down
	};
	// Kept as the historical name for "the aggregated view" - maps to
	// TopInclusive, which is what the single old tab showed.
	static constexpr Mode kTopFunctions = kModeTopInclusive;

	// Cycle column display:
	//   Absolute     - raw cycle counter as in the .btr
	//   SinceVBlank  - offset from the most recent FrameBoundary (default)
	enum CycleMode
	{
		kCycleModeAbsoluteCycle = 0,
		kCycleModeSinceVBlankCycle = 1,
	};

	explicit InstructionView(QWidget* Parent = nullptr);
	void SetModel(TraceModel* M);
	// Select the Disassembly (Linear) or Top-functions view. Keeps the tab bar
	// in sync; used by MainWindow and the headless capture path.
	void SetMode(Mode M);

  public slots:
	void SetCursor(const Cursor& C);
	// Forwarded by MainWindow from IcicleWidget::viewportChanged. The
	// TopFunctions mode re-aggregates on each call. Linear mode ignores.
	void SetViewportRange(uint64_t LeftCycle, uint64_t RightCycle);

  protected:
	void contextMenuEvent(QContextMenuEvent* Ev) override;

  private slots:
	void OnTabChanged(int Index);

  private:
	void Refresh();
	void RefreshLinear();
	// Flat "top routines" list. bExclusive selects self cycles (a routine's own
	// time, children and any interrupt that preempted it excluded) over
	// inclusive cycles (everything between call and return).
	void RefreshTopFunctions(bool bExclusive);
	// Calling-context tree: one node per distinct call PATH, so a routine
	// reached from two places appears under each, with its own totals.
	void RefreshCallStructure();

	TraceModel* m_Model = nullptr;
	QListView* m_List = nullptr;
	InstrListModel* m_Rows = nullptr;
	QTabBar* m_Tabs = nullptr;
	QStackedWidget* m_Stack = nullptr; // 0 = m_List, 1 = m_Tree
	QTreeWidget* m_Tree = nullptr; // CallStructure only
	Cursor m_Cursor;
	Mode m_Mode = kModeLinear;
	CycleMode m_CycleMode = kCycleModeSinceVBlankCycle;

	uint64_t m_ViewLeft = 0;
	uint64_t m_ViewRight = 0;
};
