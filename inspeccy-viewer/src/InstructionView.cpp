#include "InstructionView.h"

#include "CallGraphBuilder.h"
#include "RecordWalker.h"
#include "TraceModel.h"

#include <trace/trace_binary.h>

#include <QAction>
#include <QActionGroup>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QHeaderView>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <trace/disasm.h>
#include <trace/sld.h>
#include <trace/trace_binary.h>

#include <algorithm>
#include <stdio.h>

namespace
{

constexpr int kWindowSize = 50; // total rows around the cursor
constexpr int kRowsBeforeCsr = 25; // rows BEFORE the cursor row
constexpr int kTopFunctionsCap = 200; // rows shown in the Top-routines modes
constexpr int kCallTreeNodeCap = 4000; // call-path nodes built before we stop
									   // (a deeply recursive trace can produce
									   // arbitrarily many; the tree says how
									   // many it dropped rather than lying)

// Model role carrying a row's call-nesting depth (see InstrRow::Depth).
constexpr int kDepthRole = Qt::UserRole + 1;

QString Hex4(uint16_t V)
{
	return QStringLiteral("%1").arg(V, 4, 16, QChar('0')).toUpper();
}

// --- profiling helpers, shared by the two Top-routines tabs and the tree -----

// Identity of "the same routine": kind + entry page + entry PC. The page has to
// be in the key - the same address in two banks is two different routines, and
// on a paged machine they would otherwise be summed together.
inline uint32_t FrameKey(const CallFrame& F)
{
	return (uint32_t(F.m_KindV) << 24) | (uint32_t(F.m_TargetPageTag) << 16) | uint32_t(F.m_TargetPC);
}

const char* KindStr(uint8_t Kind)
{
	switch (Kind)
	{
	case CallFrame::kKindZ80Rst:
		return "RST";
	case CallFrame::kKindInterrupt:
		return "INT";
	case CallFrame::kKindTrappedCall:
		return "TRAP";
	default:
		return "CALL";
	}
}

// "CALL Renderer.Clear" / "INT $0038" - symbol when the SLD has one for the
// entry point, raw address otherwise. ROM pages are skipped for the same reason
// FormatRow skips them: sjasmplus rarely emits ROM labels, so a hit there would
// be a coincidence from another page's map.
QString KeyLabel(uint32_t Key, const SldSymbolMap* Sld)
{
	const uint16_t Pc = uint16_t(Key & 0xFFFF);
	const uint8_t Page = uint8_t((Key >> 16) & 0xFF);
	const uint8_t Kind = uint8_t((Key >> 24) & 0xFF);
	QString Name;
	if (Sld != nullptr && !BinaryTrace::PageTagIsRom(Page))
	{
		std::string Lbl;
		if (Sld->LookupNearest(Page, Pc, 256, Lbl))
		{
			Name = QString::fromStdString(Lbl);
		}
	}
	if (Name.isEmpty())
	{
		Name = QStringLiteral("$%1").arg(Hex4(Pc));
	}
	return QStringLiteral("%1 %2").arg(QString::fromLatin1(KindStr(Kind)), -4).arg(Name);
}

// The cycle range the profile tabs aggregate over: the icicle's viewport when
// it has published one, else the whole trace.
void ProfileWindow(const TraceModel& Model, uint64_t ViewL, uint64_t ViewR, uint64_t& OutL, uint64_t& OutR)
{
	if (ViewR > ViewL)
	{
		OutL = ViewL;
		OutR = ViewR;
		return;
	}
	const auto& Index = Model.Index();
	OutL = Index.isEmpty() ? 0 : Index.first().m_StartCycle;
	const BinaryTrace::Header* H = Model.GetHeader();
	OutR =
		(H != nullptr && H->m_EndCycle != BinaryTrace::kSentinel && H->m_EndCycle > OutL) ? H->m_EndCycle : UINT64_MAX;
}

// Per-frame inclusive cycles inside [L,R), and the exclusive ("self") figure:
// inclusive minus the inclusive time of its DIRECT children. Children are known
// exactly from CallFrame::Parent, recorded when the frame was pushed, so no
// nesting has to be re-derived here. Note an interrupt frame is a child of
// whatever it preempted, so a routine's self time correctly excludes an ISR
// that fired inside it.
void ComputeOverlaps(const QVector<CallFrame>& Frames, uint64_t L, uint64_t R, QVector<uint64_t>& InOutIncl,
	QVector<uint64_t>& InOutSelf)
{
	const int N = Frames.size();
	InOutIncl.fill(0, N);
	for (int IdxI = 0; IdxI < N; ++IdxI)
	{
		const uint64_t A = std::max(Frames[IdxI].m_StartCycle, L);
		const uint64_t B = std::min(Frames[IdxI].m_EndCycle, R);
		InOutIncl[IdxI] = (B > A) ? (B - A) : 0;
	}
	InOutSelf = InOutIncl;
	for (int IdxI = 0; IdxI < N; ++IdxI)
	{
		const int P = Frames[IdxI].m_Parent;
		if (P >= 0 && P < N)
		{
			// Saturating: a child should always fit inside its parent, but the
			// call-graph reconstruction is heuristic and we would rather report
			// zero self time than underflow to a huge number.
			InOutSelf[P] = (InOutSelf[P] > InOutIncl[IdxI]) ? (InOutSelf[P] - InOutIncl[IdxI]) : 0;
		}
	}
}

// Draws the normal item, then a horizontal rule at the top edge of any row
// whose call depth differs from the row above it - so every CALL (into a
// deeper level) and every RET (back out) shows as a clear line between the
// two callstack levels. Blue = going deeper, amber = coming back.
struct CallDepthDelegate final : public QStyledItemDelegate
{
  public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
	{
		QStyledItemDelegate::paint(Painter, Option, Index);
		if (!Index.isValid() || Index.row() <= 0)
		{
			return;
		}

		const int Depth = Index.data(kDepthRole).toInt();
		const int Prev = Index.model()->index(Index.row() - 1, Index.column()).data(kDepthRole).toInt();
		if (Depth == Prev)
		{
			return;
		}

		Painter->save();
		// Saturated so they read on either a light or dark row background.
		const QColor C = (Depth > Prev) ? QColor(0x1f, 0x6f, 0xd8) // CALL, deeper  -> blue
										: QColor(0xd0, 0x50, 0x18); // RET, shallower -> orange
		Painter->setPen(QPen(C, 2));
		const int Y = Option.rect.top() + 1;
		Painter->drawLine(Option.rect.left(), Y, Option.rect.right(), Y);
		Painter->restore();
	}
};

// Format the page:offset column the same way the text trace decoder does.
// 16K view collapses 8K-page index pairs (used for SNA-loaded 128K traces);
// 8K view shows the raw page index (NEX / dot-cmd defaults).
QString FormatPageOffset(uint8_t PageTag, uint16_t Pc, bool bB16KView)
{
	const bool bIsRom = BinaryTrace::PageTagIsRom(PageTag);
	const uint8_t Idx8 = bIsRom ? BinaryTrace::PageTagRomIndex(PageTag) : PageTag;
	if (bB16KView)
	{
		const uint8_t Idx16 = uint8_t(Idx8 >> 1);
		const uint16_t Off14 = uint16_t(Pc & 0x3FFF);
		return bIsRom ? QStringLiteral("R%1:%2").arg(Idx16).arg(Off14, 4, 16, QChar('0'))
					  : QStringLiteral("%1:%2").arg(Idx16, 2).arg(Off14, 4, 16, QChar('0'));
	}
	const uint16_t Off13 = uint16_t(Pc & 0x1FFF);
	return bIsRom ? QStringLiteral("R%1:%2").arg(Idx8).arg(Off13, 4, 16, QChar('0'))
				  : QStringLiteral("%1:%2").arg(Idx8, 2).arg(Off13, 4, 16, QChar('0'));
}

// Addresses that are RST / interrupt entry points or known OS-trap landings.
// When the CPU runs through these - and especially in inspeccy traces, where
// some are intercepted by the emulator's trap layer - the on-disk opcodes
// at these PCs disassemble as whatever the ROM image (or zeroed RAM, on a
// .nex run without ROM) happens to contain. Surfacing a trap label is more
// useful than showing nonsense bytes.
//
// When kRecord_TrappedCall records start being emitted by the writer, the
// formatter will read the trap-kind / api_id / result fields off that
// record directly; this static table is the v1 fallback.
struct TrapAddr
{
	uint16_t m_Pc;
	const char* m_Name;
};
constexpr TrapAddr kTrapAddrs[] = {
	{0x0000, "Cold start / reset"},
	{0x0008, "RST $08 - esxDOS / TR-DOS file API trap"},
	{0x0010, "RST $10 - BASIC PRINT_A_1"},
	{0x0018, "RST $18 - BASIC GET_CHAR"},
	{0x0020, "RST $20 - BASIC NEXT_CHAR (inspeccy trap-on-reset)"},
	{0x0028, "RST $28 - BASIC calculator"},
	{0x0030, "RST $30 - extra"},
	{0x0038, "RST $38 - IM 1 interrupt vector"},
	{0x5B00, "NextZXOS IM 1 stub"},
};

const char* TrapNameForPC(uint16_t Pc)
{
	for (const auto& T : kTrapAddrs)
	{
		if (Pc == T.m_Pc)
		{
			return T.m_Name;
		}
	}
	return nullptr;
}

QString FormatRow(const ViewerWalk::DecodedRecord& Rec, uint64_t FrameStartCycle, InstructionView::CycleMode CycleMode,
	bool bB16KView, const SldSymbolMap* Sld)
{
	using namespace BinaryTrace;

	char Mnem[64] = {0};
	QString Prefix;
	switch (Rec.m_Tag)
	{
	case kRecord_Instr:
		break;
	case kRecord_LdirIter:
		Prefix = QStringLiteral("[LDIR] ");
		break;
	case kRecord_Interrupt:
		Prefix = QStringLiteral("[INT] ");
		break;
	case kRecord_DmaDrip:
		Prefix = QStringLiteral("[DMA] ");
		break;
	case kRecord_InvalidOpcode:
		Prefix = QStringLiteral("[INVALID] ");
		break;
	case kRecord_RestartTrap:
		Prefix = QStringLiteral("[RST-trap] ");
		break;
	default:
		Prefix = QStringLiteral("[?] ");
		break;
	}

	// Trap-entry override: when PC lands on a known trap address, replace
	// the disassembled mnemonic with a clear label. Bytes at $0008 et al.
	// in a NEX run without ROM are zeroes/garbage and disassemble to
	// misleading nops/nops-and-friends; the label tells the user what's
	// actually happening instead of inviting them to puzzle over the bytes.
	QString MnemStr;
	if (Rec.m_Tag == kRecord_Instr)
	{
		if (const char* TrapName = TrapNameForPC(Rec.PC))
		{
			MnemStr = QStringLiteral("trapped OS call $%1: %2 - not emulated "
									 "(trace lacks TrappedCall records)")
						  .arg(Rec.PC, 4, 16, QChar('0'))
						  .toUpper()
						  .arg(QString::fromUtf8(TrapName));
		}
	}
	if (MnemStr.isEmpty())
	{
		Disassemble(Rec.m_Opcodes, Rec.PC, Mnem, sizeof(Mnem), nullptr);
		MnemStr = QString::fromUtf8(Mnem);
	}

	// Symbol lookup. RAM-only because sjasmplus rarely emits ROM-page labels
	// - matches the text formatter's convention. The symbol goes in its own
	// comment column far to the RIGHT of the mnemonic; long dotted labels
	// butting against the opcode made the opcode unreadable.
	QString SymComment;
	if (Sld != nullptr && Rec.m_Tag == kRecord_Instr && !BinaryTrace::PageTagIsRom(Rec.m_PageTag))
	{
		std::string Lbl;
		const uint8_t Page8K = Rec.m_PageTag;
		if (Sld->LookupNearest(Page8K, Rec.PC, 256, Lbl))
		{
			SymComment = QString::fromStdString(Lbl);
		}
	}

	// SinceVBlank displays the cycle offset from the most-recent
	// FrameBoundary. Prefixed with "+" so it's visually distinct from
	// absolute counts. Width 8 covers a 70k-cycle frame comfortably.
	QString CycCol;
	if (CycleMode == InstructionView::kCycleModeSinceVBlankCycle && Rec.m_Cycle >= FrameStartCycle)
	{
		CycCol = QStringLiteral("+%1").arg(Rec.m_Cycle - FrameStartCycle, 8, 10, QChar(' '));
	}
	else
	{
		CycCol = QStringLiteral("%1").arg(Rec.m_Cycle, 14, 10, QChar(' '));
	}

	// Address column shows: $PC + 24-bit (page:offset) bracket. Width fits
	// the wider of "(R3:1FFF)" / "($XXXX 16:1FFF)" so columns align across
	// ROM / RAM rows.
	const QString PageStr = FormatPageOffset(Rec.m_PageTag, Rec.PC, bB16KView);
	const QString AddrCol = QStringLiteral("$%1 (%2)").arg(Hex4(Rec.PC)).arg(PageStr);

	QString Body = Prefix + MnemStr;
	if (!SymComment.isEmpty())
	{
		// Pad the mnemonic out to a fixed column; width clears any Z80
		// mnemonic plus a record prefix, so the labels form a clean column.
		Body = QStringLiteral("%1 ; %2").arg(Body, -28).arg(SymComment);
	}

	return QStringLiteral("%1  %2  %3")
		.arg(CycCol)
		.arg(AddrCol, -16) // left-pad so the mnemonic column lines up
		.arg(Body);
}

} // namespace

// =============================================================================
// InstrListModel
// =============================================================================

InstrListModel::InstrListModel(QObject* Parent) : QAbstractListModel(Parent)
{
}

int InstrListModel::rowCount(const QModelIndex& Parent) const
{
	return Parent.isValid() ? 0 : m_Rows.size();
}

QVariant InstrListModel::data(const QModelIndex& Index, int Role) const
{
	if (!Index.isValid() || Index.row() < 0 || Index.row() >= m_Rows.size())
	{
		return {};
	}
	const InstrRow& R = m_Rows[Index.row()];
	switch (Role)
	{
	case Qt::DisplayRole:
		return R.m_Display;
	case Qt::ForegroundRole:
		if (R.m_bIsCursor)
		{
			return QBrush(QColor(0x18, 0x18, 0x18));
		}
		return {};
	case Qt::BackgroundRole:
		if (R.m_bIsCursor)
		{
			return QBrush(QColor(0xff, 0xd0, 0x40));
		}
		return {};
	case kDepthRole:
		return R.m_Depth;
	default:
		return {};
	}
}

void InstrListModel::SetRows(QVector<InstrRow> Rows)
{
	beginResetModel();
	m_Rows = std::move(Rows);
	m_CursorRow = -1;
	for (int IdxI = 0; IdxI < m_Rows.size(); ++IdxI)
	{
		if (m_Rows[IdxI].m_bIsCursor)
		{
			m_CursorRow = IdxI;
			break;
		}
	}
	endResetModel();
}

// =============================================================================
// InstructionView
// =============================================================================

InstructionView::InstructionView(QWidget* Parent) : QWidget(Parent)
{
	auto* Outer = new QVBoxLayout(this);
	Outer->setContentsMargins(0, 0, 0, 0);

	// Tab index == Mode.
	m_Tabs = new QTabBar(this);
	m_Tabs->addTab(tr("Disassembly")); // Linear
	m_Tabs->addTab(tr("Top exclusive")); // TopExclusive
	m_Tabs->addTab(tr("Top inclusive")); // TopInclusive
	m_Tabs->addTab(tr("Call structure")); // CallStructure
	m_Tabs->setTabToolTip(kModeTopExclusive, tr("Routines ranked by their OWN cycles - time in callees and in any "
												"interrupt that preempted them is charged elsewhere."));
	m_Tabs->setTabToolTip(kModeTopInclusive, tr("Routines ranked by total cycles from call to return, callees "
												"included."));
	m_Tabs->setTabToolTip(kModeCallStructure, tr("Every call path observed, from the main flow down, with cycles and "
												 "call counts per path."));
	m_Tabs->setExpanding(false);
	m_Tabs->setDrawBase(false);
	connect(m_Tabs, &QTabBar::currentChanged, this, &InstructionView::OnTabChanged);
	Outer->addWidget(m_Tabs);

	m_Rows = new InstrListModel(this);

	m_List = new QListView(this);
	m_List->setModel(m_Rows);
	m_List->setItemDelegate(new CallDepthDelegate(m_List));
	m_List->setUniformItemSizes(true);
	m_List->setSelectionMode(QAbstractItemView::NoSelection);
	QFont F = m_List->font();
	F.setFamily(QStringLiteral("monospace"));
	F.setStyleHint(QFont::Monospace);
	m_List->setFont(F);

	// The call tree needs real columns and expand/collapse, which a QListView of
	// pre-formatted strings can't give - so it lives on its own page of a stack
	// rather than being flattened into indented text.
	m_Tree = new QTreeWidget(this);
	m_Tree->setColumnCount(5);
	m_Tree->setHeaderLabels({tr("call path"), tr("total T"), tr("self T"), tr("calls"), tr("% of window")});
	m_Tree->setUniformRowHeights(true);
	m_Tree->setAlternatingRowColors(true);
	m_Tree->setFont(F);
	m_Tree->header()->setStretchLastSection(false);
	m_Tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);

	m_Stack = new QStackedWidget(this);
	m_Stack->addWidget(m_List); // page 0 - Linear / Top exclusive / Top inclusive
	m_Stack->addWidget(m_Tree); // page 1 - Call structure
	Outer->addWidget(m_Stack);

	Refresh();
}

void InstructionView::SetModel(TraceModel* M)
{
	m_Model = M;
	m_Cursor = Cursor{};
	Refresh();
}

void InstructionView::SetCursor(const Cursor& C)
{
	m_Cursor = C;
	Refresh();
}

void InstructionView::SetViewportRange(uint64_t LeftCycle, uint64_t RightCycle)
{
	m_ViewLeft = LeftCycle;
	m_ViewRight = RightCycle;
	// Every aggregating tab is scoped to the icicle viewport, so all three
	// re-aggregate when it moves. Linear is cursor-driven and ignores this.
	if (m_Mode != kModeLinear)
	{
		Refresh();
	}
}

void InstructionView::OnTabChanged(int Index)
{
	m_Mode = (Index >= kModeLinear && Index <= kModeCallStructure) ? Mode(Index) : kModeLinear;
	Refresh();
}

void InstructionView::SetMode(Mode M)
{
	if (m_Tabs != nullptr && m_Tabs->currentIndex() != int(M))
	{
		m_Tabs->setCurrentIndex(int(M)); // fires onTabChanged -> refresh
		return;
	}
	m_Mode = M;
	Refresh();
}

void InstructionView::Refresh()
{
	if (m_Stack != nullptr)
	{
		m_Stack->setCurrentIndex(m_Mode == kModeCallStructure ? 1 : 0);
	}
	switch (m_Mode)
	{
	case kModeTopExclusive:
		RefreshTopFunctions(true);
		break;
	case kModeTopInclusive:
		RefreshTopFunctions(false);
		break;
	case kModeCallStructure:
		RefreshCallStructure();
		break;
	case kModeLinear:
	default:
		RefreshLinear();
		break;
	}
}

void InstructionView::RefreshLinear()
{
	using namespace BinaryTrace;

	if (m_Model == nullptr || !m_Model->IsLoaded() || m_Model->BtrData() == nullptr)
	{
		m_Rows->SetRows({});
		return;
	}
	const auto& Index = m_Model->Index();
	if (Index.isEmpty())
	{
		m_Rows->SetRows({});
		return;
	}

	int Entry = 0;
	{
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
		Entry = Lo;
	}

	const uint8_t* Base = m_Model->BtrData();
	const uint8_t* Cur = Base + Index[Entry].m_FirstByteOffset;
	const uint8_t* Eof = Base + m_Model->BtrSize();

	uint64_t RunCycle = Index[Entry].m_StartCycle;
	uint64_t RunFrame = Index[Entry].m_FrameNumber;
	uint64_t FrameStartCycle = Index[Entry].m_StartCycle;

	// Collect (record, frame-start-cycle-at-the-time). Frame-start updates
	// every time we see a FrameBoundary; subsequent rows tag the new value
	// until the next boundary.
	QVector<QPair<ViewerWalk::DecodedRecord, uint64_t>> All;
	All.reserve(kWindowSize * 4);
	int CursorIdx = -1;
	int CollectedAfter = 0;
	while (Cur < Eof)
	{
		ViewerWalk::DecodedRecord Rec;
		if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
		{
			break;
		}
		if (ViewerWalk::IsStateRecord(Rec.m_Tag))
		{
			continue; // v4 video event, not an instruction
		}
		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			// The FrameBoundary's Cycle is the cycle AT the boundary, which
			// is the start of the new frame.
			FrameStartCycle = Rec.m_Cycle;
			continue;
		}
		All.append({Rec, FrameStartCycle});
		if (CursorIdx < 0 && Rec.m_Cycle >= m_Cursor.m_Cycle)
		{
			CursorIdx = All.size() - 1;
		}
		if (CursorIdx >= 0)
		{
			++CollectedAfter;
			if (CollectedAfter >= kWindowSize - kRowsBeforeCsr)
			{
				break;
			}
		}
	}

	if (All.isEmpty())
	{
		m_Rows->SetRows({});
		return;
	}
	if (CursorIdx < 0)
	{
		CursorIdx = All.size() - 1;
	}

	const int FirstIdx = std::max(0, CursorIdx - kRowsBeforeCsr);
	const int LastIdx = std::min<int>(All.size() - 1, CursorIdx + (kWindowSize - kRowsBeforeCsr));

	QVector<InstrRow> Rows;
	Rows.reserve(LastIdx - FirstIdx + 1);
	const bool bB16K =
		m_Model->GetHeader() != nullptr && (m_Model->GetHeader()->m_Flags & BinaryTrace::kFlag_16KPageView) != 0;
	const SldSymbolMap* SldPtr = (m_Model->Sld().SymbolCount() > 0) ? &m_Model->Sld() : nullptr;
	// Call-nesting depth per row from the reconstructed call graph. frameAtCycle
	// returns the deepest open frame (or -1 at top level); +1 so main = 0,
	// first CALL = 1. Empty when metrics haven't run yet -> all 0 -> no lines.
	const CallGraphBuilder& Cg = m_Model->CallGraph();
	for (int IdxI = FirstIdx; IdxI <= LastIdx; ++IdxI)
	{
		const auto& [rec, fsc] = All[IdxI];
		InstrRow Row;
		Row.m_Cycle = rec.m_Cycle;
		Row.m_FrameStartCycle = fsc;
		Row.PC = rec.PC;
		Row.m_Tag = rec.m_Tag;
		Row.m_bIsCursor = (IdxI == CursorIdx);
		const int Fidx = Cg.FrameAtCycle(rec.m_Cycle);
		Row.m_Depth = (Fidx < 0) ? 0 : int(Cg.Frames()[Fidx].m_Depth) + 1;
		Row.m_Display = FormatRow(rec, fsc, m_CycleMode, bB16K, SldPtr);
		Rows.append(Row);
	}
	m_Rows->SetRows(std::move(Rows));

	const int NewCursorRow = m_Rows->CursorRow();
	if (NewCursorRow >= 0)
	{
		const QModelIndex Idx = m_Rows->index(NewCursorRow, 0);
		m_List->scrollTo(Idx, QAbstractItemView::PositionAtCenter);
	}
}

void InstructionView::contextMenuEvent(QContextMenuEvent* Ev)
{
	QMenu Menu(this);
	auto* Group = new QActionGroup(&Menu);
	Group->setExclusive(true);

	auto* ActAbs = Menu.addAction(tr("Cycles: absolute"));
	ActAbs->setCheckable(true);
	ActAbs->setChecked(m_CycleMode == kCycleModeAbsoluteCycle);
	Group->addAction(ActAbs);

	auto* ActRel = Menu.addAction(tr("Cycles: since last VBlank"));
	ActRel->setCheckable(true);
	ActRel->setChecked(m_CycleMode == kCycleModeSinceVBlankCycle);
	Group->addAction(ActRel);

	QAction* Picked = Menu.exec(Ev->globalPos());
	if (Picked == ActAbs && m_CycleMode != kCycleModeAbsoluteCycle)
	{
		m_CycleMode = kCycleModeAbsoluteCycle;
		Refresh();
	}
	else if (Picked == ActRel && m_CycleMode != kCycleModeSinceVBlankCycle)
	{
		m_CycleMode = kCycleModeSinceVBlankCycle;
		Refresh();
	}
	Ev->accept();
}

void InstructionView::RefreshTopFunctions(bool bExclusive)
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		m_Rows->SetRows({});
		return;
	}

	uint64_t L = 0, R = 0;
	ProfileWindow(*m_Model, m_ViewLeft, m_ViewRight, L, R);

	const auto& Frames = m_Model->CallGraph().Frames();
	QVector<uint64_t> Incl, Self;
	ComputeOverlaps(Frames, L, R, Incl, Self);

	// One bucket per distinct routine - kind + entry page + entry PC. Every
	// invocation of it anywhere in the trace folds into the same row, which is
	// the difference between this and the Call structure tab.
	struct Agg
	{
		uint64_t m_Cycles = 0;
		int m_Count = 0;
	};
	QHash<uint32_t, Agg> Agg;
	for (int IdxI = 0; IdxI < Frames.size(); ++IdxI)
	{
		if (Incl[IdxI] == 0)
		{
			continue; // outside the window
		}
		auto& Acc = Agg[FrameKey(Frames[IdxI])];
		Acc.m_Cycles += bExclusive ? Self[IdxI] : Incl[IdxI];
		Acc.m_Count += 1;
	}

	QVector<std::tuple<uint64_t, int, uint32_t>> Bucket;
	Bucket.reserve(Agg.size());
	for (auto It = Agg.constBegin(); It != Agg.constEnd(); ++It)
	{
		Bucket.append({It.value().m_Cycles, It.value().m_Count, It.key()});
	}
	std::sort(Bucket.begin(), Bucket.end(),
		[](const auto& A, const auto& B)
		{
			return std::get<0>(A) > std::get<0>(B);
		});
	const int N = std::min<int>(Bucket.size(), kTopFunctionsCap);

	const SldSymbolMap* Sld = (m_Model->Sld().SymbolCount() > 0) ? &m_Model->Sld() : nullptr;
	const double Window = double(R > L ? R - L : 1);

	QVector<InstrRow> Rows;
	Rows.reserve(N + 3);

	InstrRow Head;
	Head.m_Display =
		bExclusive
			? QStringLiteral(
				  "      self T       %%    calls   routine  (self = own code, children and interrupts excluded)")
			: QStringLiteral("     total T       %%    calls   routine  (total = call to return, children included)");
	Rows.append(Head);

	InstrRow Sub;
	Sub.m_Display = QStringLiteral("  window %1 T over %2 routines%3")
						.arg(R - L)
						.arg(Agg.size())
						.arg(Bucket.size() > N ? QStringLiteral(", showing top %1").arg(N) : QString());
	Rows.append(Sub);

	for (int IdxI = 0; IdxI < N; ++IdxI)
	{
		const auto& [cyc, cnt, key] = Bucket[IdxI];
		InstrRow Row;
		Row.m_Display = QStringLiteral("%1  %2  %3   %4")
							.arg(cyc, 12, 10, QChar(' '))
							.arg(QStringLiteral("%1%").arg(100.0 * double(cyc) / Window, 5, 'f', 1), 6, QChar(' '))
							.arg(cnt, 6, 10, QChar(' '))
							.arg(KeyLabel(key, Sld));
		Rows.append(Row);
	}
	m_Rows->SetRows(std::move(Rows));
}

void InstructionView::RefreshCallStructure()
{
	if (m_Tree == nullptr)
	{
		return;
	}
	m_Tree->clear();
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return;
	}

	uint64_t L = 0, R = 0;
	ProfileWindow(*m_Model, m_ViewLeft, m_ViewRight, L, R);

	const auto& Frames = m_Model->CallGraph().Frames();
	QVector<uint64_t> Incl, Self;
	ComputeOverlaps(Frames, L, R, Incl, Self);

	// Calling-context tree. A node is a distinct call PATH, not a routine: the
	// same routine called from two places gets a node under each parent, which
	// is what makes the tree answer "where did this time come from".
	struct CtNode
	{
		uint32_t m_Key = 0;
		uint64_t m_Incl = 0;
		uint64_t m_Self = 0;
		uint32_t m_Count = 0;
		QVector<int> m_Children;
	};
	QVector<CtNode> Nodes;
	QHash<quint64, int> Lookup; // (parentNode+1) << 32 | routine key
	QVector<int> FrameNode(Frames.size(), -1);
	QVector<int> Roots;

	for (int IdxI = 0; IdxI < Frames.size(); ++IdxI)
	{
		if (Incl[IdxI] == 0)
		{
			continue;
		}
		const int Pf = Frames[IdxI].m_Parent;
		const int Pn = (Pf >= 0 && Pf < Frames.size()) ? FrameNode[Pf] : -1;
		const uint32_t Key = FrameKey(Frames[IdxI]);
		const quint64 Lk = (quint64(uint32_t(Pn + 1)) << 32) | quint64(Key);
		int Idx = Lookup.value(Lk, -1);
		if (Idx < 0)
		{
			CtNode N;
			N.m_Key = Key;
			Nodes.append(N);
			Idx = Nodes.size() - 1;
			Lookup.insert(Lk, Idx);
			if (Pn >= 0)
			{
				Nodes[Pn].m_Children.append(Idx);
			}
			else
			{
				Roots.append(Idx);
			}
		}
		FrameNode[IdxI] = Idx;
		Nodes[Idx].m_Incl += Incl[IdxI];
		Nodes[Idx].m_Self += Self[IdxI];
		Nodes[Idx].m_Count += 1;
	}

	const SldSymbolMap* Sld = (m_Model->Sld().SymbolCount() > 0) ? &m_Model->Sld() : nullptr;
	const uint64_t WindowT = (R > L) ? (R - L) : 1;

	auto SortByIncl = [&Nodes](QVector<int>& InOutV)
	{
		std::sort(InOutV.begin(), InOutV.end(),
			[&Nodes](int A, int B)
			{
				return Nodes[A].m_Incl > Nodes[B].m_Incl;
			});
	};

	auto MakeItem = [&](const QString& Name, uint64_t InclT, uint64_t SelfT, quint64 Count, bool bCount)
	{
		auto* It = new QTreeWidgetItem;
		It->setText(0, Name);
		It->setText(1, QString::number(InclT));
		It->setText(2, QString::number(SelfT));
		It->setText(3, bCount ? QString::number(Count) : QString());
		It->setText(4, QStringLiteral("%1%").arg(100.0 * double(InclT) / double(WindowT), 0, 'f', 1));
		for (int C = 1; C <= 4; ++C)
		{
			It->setTextAlignment(C, Qt::AlignRight | Qt::AlignVCenter);
		}
		return It;
	};

	// Synthetic root: the whole window. Its children are every call that opened
	// at depth 0 - i.e. everything the main flow set off, interrupts included
	// (an interrupt taken outside any call really did happen in the main flow).
	uint64_t RootIncl = 0;
	for (int IdxRow : Roots)
	{
		RootIncl += Nodes[IdxRow].m_Incl;
	}
	QTreeWidgetItem* RootItem = MakeItem(
		tr("main flow - %1 T window").arg(WindowT), WindowT, (WindowT > RootIncl) ? (WindowT - RootIncl) : 0, 0, false);
	m_Tree->addTopLevelItem(RootItem);

	SortByIncl(Roots);

	int Emitted = 0;
	int Omitted = 0;
	struct Pending
	{
		int m_Node;
		QTreeWidgetItem* m_ParentItem;
	};
	QVector<Pending> Stack;
	for (int IdxI = Roots.size() - 1; IdxI >= 0; --IdxI)
	{
		Stack.append({Roots[IdxI], RootItem});
	}

	while (!Stack.isEmpty())
	{
		const Pending P = Stack.takeLast();
		const CtNode& N = Nodes[P.m_Node];
		if (Emitted >= kCallTreeNodeCap)
		{
			++Omitted;
			continue;
		}
		++Emitted;
		QTreeWidgetItem* Item = MakeItem(KeyLabel(N.m_Key, Sld), N.m_Incl, N.m_Self, N.m_Count, true);
		P.m_ParentItem->addChild(Item);

		QVector<int> Kids = N.m_Children;
		SortByIncl(Kids);
		for (int IdxI = Kids.size() - 1; IdxI >= 0; --IdxI)
		{
			Stack.append({Kids[IdxI], Item});
		}
	}

	if (Omitted > 0)
	{
		// Never silently truncate - say what was dropped.
		auto* Note = new QTreeWidgetItem;
		Note->setText(0, tr("... %1 more call-path nodes not shown (cap %2)").arg(Omitted).arg(kCallTreeNodeCap));
		RootItem->addChild(Note);
	}

	RootItem->setExpanded(true);
	for (int IdxI = 0; IdxI < RootItem->childCount(); ++IdxI)
	{
		RootItem->child(IdxI)->setExpanded(true);
	}
	for (int C = 0; C <= 4; ++C)
	{
		m_Tree->resizeColumnToContents(C);
	}
}
