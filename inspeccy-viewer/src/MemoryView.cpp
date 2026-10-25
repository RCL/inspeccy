#include "MemoryView.h"

#include "RecordWalker.h"
#include "TraceModel.h"

#include <QAction>
#include <QActionGroup>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

#include <trace/trace_binary.h>

namespace
{

constexpr int kBytesPerRow = 16;
constexpr int kRowCount = 65536 / kBytesPerRow; // 4096
constexpr int kHexCols = kBytesPerRow;
constexpr int kAsciiCol = kBytesPerRow; // last column
constexpr int kColCount = kBytesPerRow + 1;

QString HexByte(uint8_t V)
{
	return QStringLiteral("%1").arg(V, 2, 16, QChar('0')).toUpper();
}

// ZX Spectrum classic palette (palette 0). Bright doubles the saturation of
// the underlying ink/paper colour. Indices follow GRB bit convention (bit 0
// = Green, bit 1 = Red, bit 2 = Blue) - matches the ULA attribute layout.
// bTimex: the SCLD generates brightness independently of colour, so on
// TS/TC 2068/2048 "bright black" is a real visible dark grey (the Sinclair
// ULA keeps it black).
QRgb UlaColour(uint8_t Idx, bool bBright, bool bTimex)
{
	// 0=BLACK 1=BLUE 2=RED 3=MAGENTA 4=GREEN 5=CYAN 6=YELLOW 7=WHITE
	if (Idx == 0 && bBright && bTimex)
	{
		return qRgb(0x40, 0x40, 0x40);
	}
	const uint8_t Lo = bBright ? 0xFF : 0xCD;
	const uint8_t R = (Idx & 0x2) ? Lo : 0;
	const uint8_t G = (Idx & 0x4) ? Lo : 0;
	const uint8_t B = (Idx & 0x1) ? Lo : 0;
	return qRgb(R, G, B);
}

} // namespace

// =============================================================================
// HexTableModel
// =============================================================================

HexTableModel::HexTableModel(QObject* Parent) : QAbstractTableModel(Parent)
{
	m_Bytes = QByteArray(65536, '\0');
}

int HexTableModel::rowCount(const QModelIndex& Parent) const
{
	return Parent.isValid() ? 0 : kRowCount;
}

int HexTableModel::columnCount(const QModelIndex& Parent) const
{
	return Parent.isValid() ? 0 : kColCount;
}

QVariant HexTableModel::data(const QModelIndex& Index, int Role) const
{
	if (!Index.isValid())
	{
		return {};
	}
	const int Row = Index.row();
	const int Col = Index.column();
	if (Row < 0 || Row >= kRowCount || Col < 0 || Col >= kColCount)
	{
		return {};
	}

	const int BaseAddr = Row * kBytesPerRow;

	if (Role == Qt::DisplayRole)
	{
		if (Col < kHexCols)
		{
			const int Addr = BaseAddr + Col;
			return HexByte(uint8_t(m_Bytes[Addr]));
		}
		QString S;
		S.reserve(kBytesPerRow);
		for (int IdxI = 0; IdxI < kBytesPerRow; ++IdxI)
		{
			const uint8_t B = uint8_t(m_Bytes[BaseAddr + IdxI]);
			S += (B >= 0x20 && B < 0x7F) ? QChar(B) : QChar('.');
		}
		return S;
	}
	if (Role == Qt::TextAlignmentRole)
	{
		return Col < kHexCols ? int(Qt::AlignCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
	}
	if (Role == Qt::BackgroundRole && Col < kHexCols && !m_ChangedMask.isEmpty())
	{
		const int Addr = BaseAddr + Col;
		if (uint8_t(m_ChangedMask[Addr]) != 0)
		{
			return QBrush(QColor(0xff, 0xb0, 0x40, 0x70));
		}
	}
	return {};
}

QVariant HexTableModel::headerData(int Section, Qt::Orientation Orient, int Role) const
{
	if (Role != Qt::DisplayRole)
	{
		return {};
	}
	if (Orient == Qt::Vertical)
	{
		// Two modes:
		//   8K-slot view (Next): m_NextRegs[0..7] gives the 8K page index
		//                        for each 8K slot. $FF means ROM.
		//   16K-slot view (48K/128K/Pentagon): m_RomIdx + m_BankAtC000
		//                        drive the four 16K slots.
		const int Addr = Section * kBytesPerRow;
		const QString CpuAddr = QStringLiteral("$%1").arg(Addr, 4, 16, QChar('0')).toUpper();
		QString Page;
		int Off = 0;
		if (m_NextRegs.size() == 8)
		{
			const int Slot8 = (Addr >> 13) & 0x07; // 8K slot index
			const uint8_t V = uint8_t(m_NextRegs[Slot8]);
			if (V == 0xFF)
			{
				Page = QStringLiteral("ROM");
			}
			else
			{
				Page = QString::number(V);
			}
			Off = Addr & 0x1FFF; // 13-bit offset
		}
		else if (Addr < 0x4000)
		{
			Page = QStringLiteral("R%1").arg(m_RomIdx);
			Off = Addr;
		}
		else if (Addr < 0x8000)
		{
			Page = QStringLiteral("5");
			Off = Addr - 0x4000;
		}
		else if (Addr < 0xC000)
		{
			Page = QStringLiteral("2");
			Off = Addr - 0x8000;
		}
		else
		{
			Page = QString::number(m_BankAtC000);
			Off = Addr - 0xC000;
		}
		return QStringLiteral("%1  %2:%3").arg(CpuAddr).arg(Page).arg(Off, 4, 16, QChar('0')).toUpper();
	}
	if (Section < kHexCols)
	{
		return HexByte(uint8_t(Section));
	}
	return QStringLiteral("ASCII");
}

void HexTableModel::SetBankInfo(uint8_t RomIdx, uint8_t BankAtC000)
{
	if (m_RomIdx == RomIdx && m_BankAtC000 == BankAtC000)
	{
		return;
	}
	m_RomIdx = RomIdx;
	m_BankAtC000 = BankAtC000;
	// Headers changed for every row - emit headerDataChanged for the
	// whole vertical header.
	emit headerDataChanged(Qt::Vertical, 0, kRowCount - 1);
}

void HexTableModel::SetNextRegMapping(const QByteArray& NextRegs5057)
{
	if (m_NextRegs == NextRegs5057)
	{
		return;
	}
	m_NextRegs = NextRegs5057;
	emit headerDataChanged(Qt::Vertical, 0, kRowCount - 1);
}

void HexTableModel::SetBytes(QByteArray Bytes, QByteArray ChangedBitmap)
{
	beginResetModel();
	if (Bytes.size() != 65536)
	{
		Bytes.resize(65536);
	}
	m_Bytes = std::move(Bytes);
	if (!ChangedBitmap.isEmpty() && ChangedBitmap.size() != 65536)
	{
		ChangedBitmap.resize(65536);
	}
	m_ChangedMask = std::move(ChangedBitmap);
	endResetModel();
}

// =============================================================================
// MemoryView
// =============================================================================

MemoryView::MemoryView(QWidget* Parent) : QWidget(Parent)
{
	auto* Outer = new QVBoxLayout(this);
	Outer->setContentsMargins(0, 0, 0, 0);

	// Mode / address row.
	auto* TopRow = new QHBoxLayout;
	m_ModeCombo = new QComboBox(this);
	m_ModeCombo->addItem(tr("Memory view"), int(kDisplayModeHex));
	m_ModeCombo->addItem(tr("ULA Screen 0"), int(kDisplayModeUlaScreen0));
	m_ModeCombo->addItem(tr("ULA Screen 1"), int(kDisplayModeUlaScreen1));
	m_ModeCombo->addItem(tr("ULA Visible"), int(kDisplayModeUlaVisible));
	m_ModeCombo->addItem(tr("ULA Invisible"), int(kDisplayModeUlaInvisible));
	m_ModeCombo->addItem(tr("Timex Screen 2"), int(kDisplayModeTimexScreen2));
	m_ModeCombo->addItem(tr("Timex Hicolor 8x1"), int(kDisplayModeTimexHicolor));
	m_ModeCombo->addItem(tr("Timex Hires 512x192"), int(kDisplayModeTimexHires));
	m_ModeCombo->setItemData(
		kDisplayModeTimexScreen2, tr("Second display file: bitmap and attributes at $6000-$7AFF."), Qt::ToolTipRole);
	m_ModeCombo->setItemData(kDisplayModeTimexHicolor,
		tr("8x1 colour: bitmap at $4000, one attribute per PIXEL row at $6000."), Qt::ToolTipRole);
	m_ModeCombo->setItemData(kDisplayModeTimexHires,
		tr("512x192 mono: $4000 and $6000 interleaved byte by byte. Drawn white "
		   "on black - the ink/paper pair lives in port $FF, which the trace "
		   "does not record."),
		Qt::ToolTipRole);
	// All ULA modes are enabled now. Bank-7 contents are tracked from the
	// first write that happens while bank 7 is mapped at $C000; until then
	// Screen 1 just renders zeros (which displays as solid black). That's
	// accurate for traces that never touch bank 7 - no placeholder needed.
	connect(m_ModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MemoryView::OnModeChanged);
	TopRow->addWidget(new QLabel(tr("Display:"), this));
	TopRow->addWidget(m_ModeCombo);

	TopRow->addSpacing(8);
	TopRow->addWidget(new QLabel(tr("Goto:"), this));
	m_AddrEdit = new QLineEdit(this);
	m_AddrEdit->setPlaceholderText(tr("e.g. 4000 or $4000"));
	m_AddrEdit->setMaximumWidth(120);
	connect(m_AddrEdit, &QLineEdit::returnPressed, this, &MemoryView::OnJumpToAddress);
	TopRow->addWidget(m_AddrEdit);
	TopRow->addStretch();
	Outer->addLayout(TopRow);

	// Stacked content: hex grid OR vram image.
	m_Stack = new QStackedWidget(this);

	m_Rows = new HexTableModel(this);
	m_Table = new QTableView;
	m_Table->setModel(m_Rows);
	m_Table->setShowGrid(false);
	m_Table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_Table->horizontalHeader()->setDefaultSectionSize(22);
	m_Table->verticalHeader()->setDefaultSectionSize(16);
	m_Table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	// Row-header gutter wide enough for "$XXXX  P:XXXX" (CPU addr + page:offset).
	m_Table->verticalHeader()->setFixedWidth(110);
	QFont F = m_Table->font();
	F.setFamily(QStringLiteral("monospace"));
	F.setStyleHint(QFont::Monospace);
	m_Table->setFont(F);
	m_Stack->addWidget(m_Table);

	m_VramImage = new QLabel;
	m_VramImage->setAlignment(Qt::AlignCenter);
	m_VramImage->setStyleSheet(QStringLiteral("background-color: #181818;"));
	m_Stack->addWidget(m_VramImage);

	Outer->addWidget(m_Stack);
	Refresh();
}

void MemoryView::SetModel(TraceModel* M)
{
	m_Model = M;
	m_Cursor = Cursor{};
	Refresh();
}

void MemoryView::SetCursor(const Cursor& C)
{
	m_Cursor = C;
	Refresh();
}

void MemoryView::SetDisplayMode(DisplayMode M)
{
	const int Idx = m_ModeCombo->findData(int(M));
	if (Idx >= 0)
	{
		m_ModeCombo->setCurrentIndex(Idx); // fires onModeChanged -> refresh
	}
}

void MemoryView::OnModeChanged(int Idx)
{
	// Hex -> page 0 of the stacked widget; everything else -> the VRAM page.
	// For Hex, the QTableView shows; for any ULA mode the QLabel shows.
	// Bank-aware modes (UlaScreen1 / Visible / Invisible) clamp to the
	// current behaviour until Batch 4 wires them.
	m_Mode = static_cast<DisplayMode>(Idx);
	if (m_Mode == kDisplayModeHex)
	{
		m_Stack->setCurrentIndex(0);
	}
	else
	{
		m_Stack->setCurrentIndex(1);
	}
	Refresh();
}

void MemoryView::contextMenuEvent(QContextMenuEvent* Ev)
{
	QMenu Menu(this);
	auto* Group = new QActionGroup(&Menu);
	Group->setExclusive(true);

	struct Entry
	{
		DisplayMode m_M;
		const char* m_Label;
		bool m_bEnabled;
	};
	const Entry Entries[] = {
		{kDisplayModeHex, "Memory view", true},
		{kDisplayModeUlaScreen0, "ULA Screen 0", true},
		{kDisplayModeUlaScreen1, "ULA Screen 1", true},
		{kDisplayModeUlaVisible, "ULA Visible", true},
		{kDisplayModeUlaInvisible, "ULA Invisible", true},
		{kDisplayModeTimexScreen2, "Timex Screen 2", true},
		{kDisplayModeTimexHicolor, "Timex Hicolor 8x1", true},
		{kDisplayModeTimexHires, "Timex Hires 512x192", true},
	};
	for (const auto& E : Entries)
	{
		auto* A = Menu.addAction(tr(E.m_Label));
		A->setCheckable(true);
		A->setChecked(m_Mode == E.m_M);
		A->setEnabled(E.m_bEnabled);
		A->setData(int(E.m_M));
		Group->addAction(A);
	}
	QAction* Picked = Menu.exec(Ev->globalPos());
	if (Picked != nullptr && Picked->isEnabled())
	{
		m_ModeCombo->setCurrentIndex(Picked->data().toInt());
	}
	Ev->accept();
}

QByteArray MemoryView::ReconstructMemoryAt(const Cursor& C) const
{
	using namespace BinaryTrace;

	QByteArray Shadow(65536, '\0');
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		return Shadow;
	}
	if (m_Model->MemSnapshotCount() == 0)
	{
		return Shadow;
	}

	const auto& Index = m_Model->Index();
	if (Index.isEmpty())
	{
		return Shadow;
	}

	int Chosen = 0;
	for (int IdxI = 0; IdxI < m_Model->MemSnapshotCount(); ++IdxI)
	{
		const int E = m_Model->MemSnapshotIndexEntry(IdxI);
		if (E < 0 || E >= Index.size())
		{
			break;
		}
		if (Index[E].m_StartCycle <= C.m_Cycle)
		{
			Chosen = IdxI;
		}
		else
		{
			break;
		}
	}
	Shadow = m_Model->MemSnapshotAt(Chosen);

	const int StartEntry = m_Model->MemSnapshotIndexEntry(Chosen);
	if (StartEntry < 0 || StartEntry >= Index.size())
	{
		return Shadow;
	}

	const uint8_t* Base = m_Model->BtrData();
	if (Base == nullptr)
	{
		return Shadow;
	}
	const uint8_t* Cur = Base + Index[StartEntry].m_FirstByteOffset;
	const uint8_t* Eof = Base + m_Model->BtrSize();

	uint64_t RunCycle = Index[StartEntry].m_StartCycle;
	uint64_t RunFrame = Index[StartEntry].m_FrameNumber;

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
		if (Rec.m_Cycle > C.m_Cycle)
		{
			break;
		}
		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			continue;
		}
		for (uint8_t IdxI = 0; IdxI < Rec.m_NumMemWrites; ++IdxI)
		{
			Shadow[Rec.m_MemAddr[IdxI]] = char(Rec.m_MemVal[IdxI]);
		}
	}
	return Shadow;
}

QImage MemoryView::RenderUla(const QByteArray& Mem) const
{
	// ULA classic mode: 6144 bytes of bitmap at $4000-$57FF, 768 bytes of
	// attributes at $5800-$5AFF. Addressing is the famous "scrambled" form:
	//   addr = $4000 | ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | (x >> 3)
	// The MSB of the byte is the leftmost pixel.
	QImage Img(256, 192, QImage::Format_RGB32);
	if (Mem.size() < 0x5B00)
	{
		Img.fill(QColor(0x18, 0x18, 0x18));
		return Img;
	}
	const uint8_t* M = reinterpret_cast<const uint8_t*>(Mem.constData());
	const bool bTimex = (m_Model != nullptr) && m_Model->IsTimexMachine();

	for (int Y = 0; Y < 192; ++Y)
	{
		QRgb* Line = reinterpret_cast<QRgb*>(Img.scanLine(Y));
		const int kYC = Y & 0xC0;
		const int kY7 = Y & 7;
		const int kY38 = Y & 0x38;
		for (int X = 0; X < 256; ++X)
		{
			const int BitmapAddr = 0x4000 | (kYC << 5) | (kY7 << 8) | (kY38 << 2) | (X >> 3);
			const uint8_t Pix = M[BitmapAddr];
			const bool bInk = (Pix >> (7 - (X & 7))) & 1;

			const int AttrAddr = 0x5800 + (Y >> 3) * 32 + (X >> 3);
			const uint8_t Attr = M[AttrAddr];
			const uint8_t Ink = Attr & 0x07;
			const uint8_t Paper = (Attr >> 3) & 0x07;
			const bool bBright = (Attr & 0x40) != 0;

			Line[X] = bInk ? UlaColour(Ink, bBright, bTimex) : UlaColour(Paper, bBright, bTimex);
		}
	}
	return Img;
}

// Timex SCLD screen layouts, read straight out of the 64KB CPU view. Bank 5
// covers $4000-$7FFF there, so the second display file at $6000 is directly
// addressable - no bank shadow needed, unlike the 128K alt screen.
//
// Bright black renders as SCLD dark grey (UlaColour's bTimex path), matching
// the still encoder and the composite renderer.
QImage MemoryView::RenderTimex(const QByteArray& Mem, DisplayMode Layout) const
{
	const bool bHires = (Layout == kDisplayModeTimexHires);
	QImage Img(bHires ? 512 : 256, 192, QImage::Format_RGB32);
	if (Mem.size() < 0x7B00)
	{
		Img.fill(QColor(0x18, 0x18, 0x18));
		return Img;
	}
	const uint8_t* M = reinterpret_cast<const uint8_t*>(Mem.constData());
	// File 1 / file 2 bases. TimexScreen2 shifts the WHOLE classic layout up by
	// $2000; the other two read both files at once.
	const int Base1 = (Layout == kDisplayModeTimexScreen2) ? 0x6000 : 0x4000;
	const int Base2 = 0x6000;

	for (int Y = 0; Y < 192; ++Y)
	{
		QRgb* Line = reinterpret_cast<QRgb*>(Img.scanLine(Y));
		// The famous scrambled row address, shared by every mode.
		const int RowOff = ((Y & 0xC0) << 5) | ((Y & 7) << 8) | ((Y & 0x38) << 2);

		for (int Col = 0; Col < 32; ++Col)
		{
			if (bHires)
			{
				// 512x192 mono: each 8-pixel byte of file 1 is followed on
				// screen by the matching byte of file 2, giving 16 pixels per
				// column. Ink/paper come from port $FF bits 5-3, which the .btr
				// does not record - so this draws the canonical white on black.
				const uint8_t kB1 = M[Base1 + RowOff + Col];
				const uint8_t kB2 = M[Base2 + RowOff + Col];
				for (int Bit = 0; Bit < 8; ++Bit)
				{
					Line[Col * 16 + Bit] = ((kB1 >> (7 - Bit)) & 1) ? qRgb(0xFF, 0xFF, 0xFF) : qRgb(0, 0, 0);
					Line[Col * 16 + 8 + Bit] = ((kB2 >> (7 - Bit)) & 1) ? qRgb(0xFF, 0xFF, 0xFF) : qRgb(0, 0, 0);
				}
				continue;
			}

			const uint8_t Pix = M[Base1 + RowOff + Col];
			// Hicolor takes one attribute per PIXEL row from the second file at
			// the same scrambled offset - that is the whole point of 8x1. The
			// classic layouts take one per CHARACTER row from $1800.
			const uint8_t Attr = (Layout == kDisplayModeTimexHicolor) ? M[Base2 + RowOff + Col]
																	  : M[Base1 + 0x1800 + (Y >> 3) * 32 + Col];
			const uint8_t Ink = Attr & 0x07;
			const uint8_t Paper = (Attr >> 3) & 0x07;
			const bool bBright = (Attr & 0x40) != 0;
			for (int Bit = 0; Bit < 8; ++Bit)
			{
				Line[Col * 8 + Bit] =
					((Pix >> (7 - Bit)) & 1) ? UlaColour(Ink, bBright, true) : UlaColour(Paper, bBright, true);
			}
		}
	}
	return Img;
}

void MemoryView::Refresh()
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		m_Rows->SetBytes(QByteArray(65536, '\0'));
		m_VramImage->setPixmap(QPixmap());
		m_VramImage->setText(tr("(open a .btr to view memory)"));
		return;
	}
	m_CurrentBytes = ReconstructMemoryAt(m_Cursor);

	// Update the row-header page:offset column from the tracked mapping.
	// On Next traces we use the 8K NextReg slots; on 128K-class machines
	// we use the four 16K slots derived from $7FFD.
	const uint8_t B7ffd = m_Model->Bank7FFDAtCycle(m_Cursor.m_Cycle);
	const uint8_t BankAtC000 = B7ffd & 0x07;
	const uint8_t RomIdx = (B7ffd & 0x10) ? 1 : 0;
	m_Rows->SetBankInfo(RomIdx, BankAtC000);
	if (m_Model->IsNextMachine())
	{
		m_Rows->SetNextRegMapping(m_Model->NextRegs5057AtCycle(m_Cursor.m_Cycle));
	}
	else
	{
		m_Rows->SetNextRegMapping(QByteArray());
	}

	if (m_Mode == kDisplayModeTimexScreen2 || m_Mode == kDisplayModeTimexHicolor || m_Mode == kDisplayModeTimexHires)
	{
		// All three read bank 5 out of the CPU view, where it spans
		// $4000-$7FFF - both display files are in the snapshot already.
		const QImage Img = RenderTimex(m_CurrentBytes, m_Mode);
		m_VramImage->setPixmap(QPixmap::fromImage(Img.scaled(512, 384, Qt::KeepAspectRatio, Qt::FastTransformation)));
	}
	else if (m_Mode != kDisplayModeHex)
	{
		// Screen-bank selection: Screen 0 is bank 5 (always at $4000 in
		// the CPU view, so just use the current snapshot's $4000-$5AFF).
		// Screen 1 is bank 7 - bank-7 shadow captures writes that
		// happened while bank 7 was mapped at $C000.
		const bool bUseScreen1 = (m_Mode == kDisplayModeUlaScreen1) ||
								 (m_Mode == kDisplayModeUlaVisible && (B7ffd & 0x08) != 0) ||
								 (m_Mode == kDisplayModeUlaInvisible && (B7ffd & 0x08) == 0);

		QByteArray VramSource;
		if (bUseScreen1)
		{
			// Pad bank 7 into a 64KB buffer at $4000 so the existing
			// ULA-addressing renderer (which expects $4000-anchored bytes)
			// works unchanged.
			const QByteArray kB7 = m_Model->Bank7AtCycle(m_Cursor.m_Cycle);
			VramSource = QByteArray(65536, '\0');
			VramSource.replace(0x4000, qMin(0x1B00, int(kB7.size())), kB7.constData(), qMin(0x1B00, int(kB7.size())));
		}
		else
		{
			VramSource = m_CurrentBytes;
		}
		QImage Img = RenderUla(VramSource);
		QPixmap Pm = QPixmap::fromImage(Img.scaled(512, 384, Qt::KeepAspectRatio, Qt::FastTransformation));
		m_VramImage->setPixmap(Pm);
	}
	else
	{
		m_Rows->SetBytes(m_CurrentBytes);
	}
}

void MemoryView::OnJumpToAddress()
{
	QString S = m_AddrEdit->text().trimmed();
	if (S.startsWith(QChar('$')))
	{
		S.remove(0, 1);
	}
	if (S.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
	{
		S.remove(0, 2);
	}
	bool bOk = false;
	const uint Addr = S.toUInt(&bOk, 16);
	if (!bOk || Addr > 0xFFFF)
	{
		return;
	}
	const int Row = int(Addr / kBytesPerRow);
	const QModelIndex Idx = m_Rows->index(Row, int(Addr % kBytesPerRow));
	m_Table->scrollTo(Idx, QAbstractItemView::PositionAtTop);
	m_Table->setCurrentIndex(Idx);
}
