#include "RegisterView.h"
#include "TraceModel.h"

#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace
{

QLabel* MakeMono(const QString& Initial)
{
	auto* L = new QLabel(Initial);
	QFont F = L->font();
	F.setFamily(QStringLiteral("monospace"));
	F.setStyleHint(QFont::Monospace);
	L->setFont(F);
	return L;
}

QString Hex16(uint16_t V)
{
	return QStringLiteral("$%1").arg(V, 4, 16, QChar('0')).toUpper();
}

QString FmtTime(uint64_t Nanos)
{
	const double Sec = double(Nanos) / 1.0e9;
	const int Mins = int(Sec) / 60;
	const double Rem = Sec - 60.0 * Mins;
	return QStringLiteral("%1:%2").arg(Mins, 2, 10, QChar('0')).arg(Rem, 6, 'f', 3, QChar('0'));
}

} // namespace

RegisterView::RegisterView(QWidget* Parent) : QWidget(Parent)
{
	auto* Outer = new QVBoxLayout(this);

	auto* Pos = new QGroupBox(tr("Position"), this);
	auto* PosForm = new QFormLayout(Pos);
	m_CycleLabel = MakeMono(QStringLiteral("-"));
	m_FrameLabel = MakeMono(QStringLiteral("-"));
	m_TimeLabel = MakeMono(QStringLiteral("-"));
	PosForm->addRow(tr("Cycle:"), m_CycleLabel);
	PosForm->addRow(tr("Frame:"), m_FrameLabel);
	PosForm->addRow(tr("Time:"), m_TimeLabel);
	Outer->addWidget(Pos);

	auto* Regs = new QGroupBox(tr("Registers (at frame start)"), this);
	auto* RegsForm = new QFormLayout(Regs);
	m_PcLabel = MakeMono(QStringLiteral("$0000"));
	m_SpLabel = MakeMono(QStringLiteral("$0000"));
	m_AfLabel = MakeMono(QStringLiteral("$0000"));
	m_BcLabel = MakeMono(QStringLiteral("$0000"));
	m_DeLabel = MakeMono(QStringLiteral("$0000"));
	m_HlLabel = MakeMono(QStringLiteral("$0000"));
	m_IxLabel = MakeMono(QStringLiteral("$0000"));
	m_IyLabel = MakeMono(QStringLiteral("$0000"));
	RegsForm->addRow(QStringLiteral("PC:"), m_PcLabel);
	RegsForm->addRow(QStringLiteral("SP:"), m_SpLabel);
	RegsForm->addRow(QStringLiteral("AF:"), m_AfLabel);
	RegsForm->addRow(QStringLiteral("BC:"), m_BcLabel);
	RegsForm->addRow(QStringLiteral("DE:"), m_DeLabel);
	RegsForm->addRow(QStringLiteral("HL:"), m_HlLabel);
	RegsForm->addRow(QStringLiteral("IX:"), m_IxLabel);
	RegsForm->addRow(QStringLiteral("IY:"), m_IyLabel);
	Outer->addWidget(Regs);

	m_NotesLabel = new QLabel(tr("(DMA / interrupt detail - Phase 2)"));
	m_NotesLabel->setStyleSheet(QStringLiteral("color: #888;"));
	Outer->addWidget(m_NotesLabel);

	Outer->addStretch();
}

void RegisterView::SetModel(TraceModel* Model)
{
	m_Model = Model;
	Refresh();
}

void RegisterView::SetCursor(const Cursor& C)
{
	m_Cursor = C;
	Refresh();
}

void RegisterView::Refresh()
{
	if (m_Model == nullptr || !m_Model->IsLoaded() || m_Model->Index().isEmpty())
	{
		m_CycleLabel->setText(QStringLiteral("-"));
		m_FrameLabel->setText(QStringLiteral("-"));
		m_TimeLabel->setText(QStringLiteral("-"));
		// Also clear the register labels - otherwise they keep showing
		// values from the previously-loaded trace, which is misleading.
		const QString Dash = QStringLiteral("-");
		m_PcLabel->setText(Dash);
		m_SpLabel->setText(Dash);
		m_AfLabel->setText(Dash);
		m_BcLabel->setText(Dash);
		m_DeLabel->setText(Dash);
		m_HlLabel->setText(Dash);
		m_IxLabel->setText(Dash);
		m_IyLabel->setText(Dash);
		return;
	}
	m_CycleLabel->setText(QStringLiteral("%1").arg(m_Cursor.m_Cycle));
	m_FrameLabel->setText(QStringLiteral("%1").arg(m_Cursor.m_Frame));
	m_TimeLabel->setText(FmtTime(m_Cursor.m_TimeNanos));

	// Look up the index entry for this frame to fetch the snapshot.
	const auto& Idx = m_Model->Index();
	int Lo = 0, Hi = Idx.size() - 1;
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi) / 2;
		if (Idx[Mid].m_FrameNumber < m_Cursor.m_Frame)
		{
			Lo = Mid + 1;
		}
		else
		{
			Hi = Mid;
		}
	}
	const auto& E = Idx[Lo];
	m_PcLabel->setText(Hex16(E.PC));
	m_SpLabel->setText(Hex16(E.SP));
	m_AfLabel->setText(Hex16(E.AF));
	m_BcLabel->setText(Hex16(E.BC));
	m_DeLabel->setText(Hex16(E.DE));
	m_HlLabel->setText(Hex16(E.HL));
	m_IxLabel->setText(Hex16(E.IX));
	m_IyLabel->setText(Hex16(E.IY));
}
