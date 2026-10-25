#pragma once
#include "Cursor.h"

#include <QAbstractTableModel>
#include <QByteArray>
#include <QImage>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTableView;
struct TraceModel;

// Backing model for the hex grid. One row = 16 bytes. Columns 0..15 are hex
// bytes; column 16 is the ASCII gutter; column -1 (header) is the address.
// The full 64KB is exposed as 4096 rows.
struct HexTableModel : public QAbstractTableModel
{
	Q_OBJECT
  public:
	explicit HexTableModel(QObject* Parent = nullptr);

	int rowCount(const QModelIndex& Parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& Parent = QModelIndex()) const override;

	QVariant data(const QModelIndex& Index, int Role) const override;
	QVariant headerData(int Section, Qt::Orientation Orient, int Role) const override;

	// Replace the displayed 64KB block. Beyond-trace-end content is shown
	// as zero. The `changedBitmap` marks bytes that were written within
	// the last K cycles before cursor (Phase 8.5; currently empty).
	void SetBytes(QByteArray Bytes, QByteArray ChangedBitmap = {});

	// Sets the current bank-at-$C000 + ROM-index so the row header gutter
	// can render the second "page:offset" address column on each row
	// (16K-slot view - used for 48K / 128K / Pentagon traces).
	void SetBankInfo(uint8_t RomIdx, uint8_t BankAtC000);

	// For Next traces: pass NextRegs $50-$57 (8 bytes); the row header
	// gutter switches to 8K-slot mapping. Pass empty array to revert to
	// 16K-slot view.
	void SetNextRegMapping(const QByteArray& NextRegs5057);

  private:
	QByteArray m_Bytes; // exactly 65536 bytes once set
	QByteArray m_ChangedMask; // optional 65536-bit-wise highlighting
	uint8_t m_RomIdx = 0;
	uint8_t m_BankAtC000 = 0;
	QByteArray m_NextRegs; // empty -> use m_RomIdx/m_BankAtC000 view
};

struct MemoryView : public QWidget
{
	Q_OBJECT
  public:
	enum DisplayMode
	{
		kDisplayModeHex = 0, // raw hex grid
		kDisplayModeUlaScreen0 = 1, // bank 5 @ $4000-$5AFF (the "normal" ULA screen)
		kDisplayModeUlaScreen1 = 2, // bank 7 @ alt screen - Batch 4 (bank tracking)
		kDisplayModeUlaVisible = 3, // whichever screen the ULA is currently scanning out
		kDisplayModeUlaInvisible = 4, // the screen that's NOT being scanned out
		// Timex SCLD screen layouts. These are LAYOUTS, not machine states:
		// they say how to read the bytes, so you can point one at a buffer
		// whatever the machine is actually displaying. The trace carries no
		// port-$FF value, so the live SCLD mode can't be inferred anyway.
		kDisplayModeTimexScreen2 = 5, // second display file: bitmap+attrs at $6000-$7AFF
		kDisplayModeTimexHicolor = 6, // 8x1 colour: bitmap $4000, one attr per pixel ROW at $6000
		kDisplayModeTimexHires = 7, // 512x192 mono: $4000 and $6000 interleaved by byte
	};

	explicit MemoryView(QWidget* Parent = nullptr);
	void SetModel(TraceModel* M);

	// Programmatically select what this Display shows (drives the same combo
	// the user would). Lets the host default one panel to memory, another to
	// the ULA screen, while each stays freely switchable.
	void SetDisplayMode(DisplayMode M);

  public slots:
	void SetCursor(const Cursor& C);

  protected:
	void contextMenuEvent(QContextMenuEvent* Ev) override;

  private slots:
	void OnJumpToAddress();
	void OnModeChanged(int Idx);

  private:
	void Refresh();
	QByteArray ReconstructMemoryAt(const Cursor& C) const;
	QImage RenderUla(const QByteArray& Mem) const;
	// Timex SCLD layouts. `base` is the CPU address the display file starts at
	// ($4000 normally, $6000 for the second file); hicolor/hires always read the
	// pair $4000 + $6000 because that is what the hardware interleaves.
	QImage RenderTimex(const QByteArray& Mem, DisplayMode Layout) const;

	TraceModel* m_Model = nullptr;
	Cursor m_Cursor;

	QComboBox* m_ModeCombo = nullptr;
	QStackedWidget* m_Stack = nullptr;

	QTableView* m_Table = nullptr;
	HexTableModel* m_Rows = nullptr;
	QLineEdit* m_AddrEdit = nullptr;

	QLabel* m_VramImage = nullptr;

	DisplayMode m_Mode = kDisplayModeHex;
	QByteArray m_CurrentBytes;
};
