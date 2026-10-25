#include "TraceLoadProgressDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

TraceLoadProgressDialog::TraceLoadProgressDialog(QWidget* Parent) : QDialog(Parent)
{
	setWindowTitle(tr("Parsing trace"));
	setModal(true);
	setMinimumWidth(380);

	auto* Outer = new QVBoxLayout(this);
	m_Status = new QLabel(tr("Opening trace..."), this);
	Outer->addWidget(m_Status);
	m_Bar = new QProgressBar(this);
	m_Bar->setRange(0, 100);
	m_Bar->setValue(0);
	Outer->addWidget(m_Bar);

	auto* Row = new QHBoxLayout;
	Row->addStretch();
	m_Cancel = new QPushButton(tr("Cancel"), this);
	m_Bg = new QPushButton(tr("Parse in background"), this);
	connect(m_Cancel, &QPushButton::clicked, this, &TraceLoadProgressDialog::OnCancel);
	connect(m_Bg, &QPushButton::clicked, this, &TraceLoadProgressDialog::OnBackground);
	Row->addWidget(m_Cancel);
	Row->addWidget(m_Bg);
	Outer->addLayout(Row);
}

void TraceLoadProgressDialog::SetProgress(int Percent, const QString& Status)
{
	m_Bar->setValue(Percent);
	if (!Status.isEmpty())
	{
		m_Status->setText(Status);
	}
}

void TraceLoadProgressDialog::OnMetricsReady(bool bCancelled)
{
	m_Outcome = bCancelled ? kOutcomeCancelled : kOutcomeCompleted;
	// If the user hadn't already dismissed the dialog (Background button),
	// close it now that the worker is done.
	if (isVisible())
	{
		accept();
	}
}

void TraceLoadProgressDialog::OnCancel()
{
	m_Outcome = kOutcomeCancelled;
	m_Cancel->setEnabled(false);
	m_Bg->setEnabled(false);
	m_Status->setText(tr("Cancelling..."));
	emit CancelRequested();
	// Dialog stays modal until the worker's metricsReady fires (sets m_Outcome
	// and accepts); without waiting, the widgets would see the model partway
	// through a write.
}

void TraceLoadProgressDialog::OnBackground()
{
	m_Outcome = kOutcomeBackgrounded;
	accept();
}
