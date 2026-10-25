#include "InterruptView.h"
#include "CpuSources.h"
#include "TraceModel.h"

#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
struct NumericItem : QTreeWidgetItem
{
	bool operator<(const QTreeWidgetItem& Other) const override
	{
		const int Col = treeWidget()->sortColumn();
		if (Col < 4)
		{
			return data(Col, Qt::UserRole).toULongLong() < Other.data(Col, Qt::UserRole).toULongLong();
		}
		return QTreeWidgetItem::operator<(Other);
	}
};

QString EntryLabel(const TraceModel& Model, const CallFrame& F)
{
	std::string Symbol;
	if (!BinaryTrace::PageTagIsRom(F.m_TargetPageTag) &&
		Model.Sld().LookupNearest(F.m_TargetPageTag, F.m_TargetPC, 256, Symbol))
	{
		return QString::fromStdString(Symbol);
	}
	return QStringLiteral("$%1 (no SLD symbol)")
		.arg(QString::number(F.m_TargetPC, 16).rightJustified(4, QChar('0')).toUpper());
}
} // namespace

InterruptView::InterruptView(TraceModel* Model, QWidget* Parent) : QWidget(Parent), m_Model(Model)
{
	auto* Layout = new QVBoxLayout(this);
	m_Summary = new QLabel(this);
	m_Summary->setWordWrap(true);
	Layout->addWidget(m_Summary);
	auto* Fit = new QPushButton(tr("Fit selected ISR (F)"), this);
	Fit->setToolTip(
		tr("Fit the full execution and all its call levels. Up/Down in the table selects and fits another ISR."));
	Layout->addWidget(Fit, 0, Qt::AlignLeft);
	connect(Fit, &QPushButton::clicked, this, &InterruptView::FitSelected);
	m_Table = new QTreeWidget(this);
	m_Table->setAccessibleName(tr("Frame ISR executions"));
	m_Table->setHeaderLabels(
		{tr("Rank"), tr("Frame"), tr("Inclusive cycles"), tr("Start cycle"), tr("SLD entry symbol")});
	m_Table->setRootIsDecorated(false);
	m_Table->setAlternatingRowColors(true);
	m_Table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_Table->setSortingEnabled(true);
	m_Table->sortByColumn(2, Qt::DescendingOrder);
	m_Table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_Table->header()->setStretchLastSection(true);
	Layout->addWidget(m_Table, 1);
	m_Detail = new QLabel(this);
	m_Detail->setWordWrap(true);
	m_Detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
	Layout->addWidget(m_Detail);
	connect(m_Table, &QTreeWidget::currentItemChanged, this,
		[this]
		{
			FitSelected();
		});
	connect(m_Table, &QTreeWidget::itemActivated, this,
		[this]
		{
			FitSelected();
		});
	connect(Model, &TraceModel::SldChanged, this, &InterruptView::Refresh);
	Clear();
}

void InterruptView::Clear()
{
	m_Table->clear();
	m_Detail->clear();
	m_Summary->setText(tr("Open a trace to rank completed frame ISR executions."));
}

void InterruptView::Refresh()
{
	QSignalBlocker Block(m_Table);
	const int Previous = m_Table->currentItem() ? m_Table->currentItem()->data(4, Qt::UserRole).toInt() : -1;
	m_Table->clear();
	m_Detail->clear();
	QVector<int> Ranked;
	int Partial = 0, Unknown = 0;
	const auto& Frames = m_Model->CallGraph().Frames();
	for (int Idx = 0; Idx < Frames.size(); ++Idx)
	{
		const auto& F = Frames[Idx];
		if (F.m_KindV != CallFrame::kKindInterrupt)
		{
			continue;
		}
		if (F.m_InterruptSource == CpuSource::kIntUnknown)
		{
			++Unknown;
		}
		if (F.m_InterruptSource != CpuSource::kIntBase + BinaryTrace::kIntSrc_Ula)
		{
			continue;
		}
		if (!F.m_bComplete)
		{
			++Partial;
			continue;
		}
		if (F.m_EndCycle > F.m_StartCycle)
		{
			Ranked.append(Idx);
		}
	}
	std::stable_sort(Ranked.begin(), Ranked.end(),
		[&Frames](int A, int B)
		{
			return Frames[A].m_EndCycle - Frames[A].m_StartCycle > Frames[B].m_EndCycle - Frames[B].m_StartCycle;
		});
	m_Table->setSortingEnabled(false);
	QTreeWidgetItem* Keep = nullptr;
	for (int Rank = 0; Rank < Ranked.size(); ++Rank)
	{
		const int Idx = Ranked[Rank];
		const auto& F = Frames[Idx];
		auto* Item = new NumericItem;
		const quint64 Values[] = {quint64(Rank + 1), m_Model->CursorAtCycle(F.m_StartCycle).m_Frame,
			F.m_EndCycle - F.m_StartCycle, F.m_StartCycle};
		for (int Col = 0; Col < 4; ++Col)
		{
			Item->setText(Col, QString::number(Values[Col]));
			Item->setData(Col, Qt::UserRole, Values[Col]);
			Item->setTextAlignment(Col, Qt::AlignRight | Qt::AlignVCenter);
		}
		Item->setText(4, EntryLabel(*m_Model, F));
		Item->setData(4, Qt::UserRole, Idx);
		m_Table->addTopLevelItem(Item);
		if (Idx == Previous)
		{
			Keep = Item;
		}
	}
	m_Table->setSortingEnabled(true);
	if (Keep)
	{
		m_Table->setCurrentItem(Keep);
	}
	m_Summary->setText(
		tr("Entire capture: %1 completed frame ISRs. Ranked by inclusive cycles (includes nested calls). "
		   "%2 partial and %3 unknown-source ISRs excluded. Select a row to fit; Up/Down to compare.")
			.arg(Ranked.size())
			.arg(Partial)
			.arg(Unknown));
	Block.unblock();
	if (Keep)
	{
		FitSelected();
	}
}

void InterruptView::FocusResults()
{
	m_Table->setFocus();
	if (!m_Table->currentItem() && m_Table->topLevelItemCount())
	{
		m_Table->setCurrentItem(m_Table->topLevelItem(0));
	}
	else
	{
		// Gaining focus can establish the first current cell without selecting
		// its row. Keep the visible highlight consistent with the fitted ISR.
		if (auto* Item = m_Table->currentItem())
		{
			Item->setSelected(true);
		}
		FitSelected();
	}
}

void InterruptView::FitSelected()
{
	const auto* Item = m_Table->currentItem();
	if (!Item)
	{
		return;
	}
	const int Idx = Item->data(4, Qt::UserRole).toInt();
	const auto& Frames = m_Model->CallGraph().Frames();
	if (Idx < 0 || Idx >= Frames.size())
	{
		return;
	}
	const auto& F = Frames[Idx];
	m_Detail->setText(tr("Rank %1 | Frame %2 | %3 | page %4:$%5\n"
						 "%6 inclusive cycles | start %7, end %8 (exclusive)")
			.arg(Item->text(0), Item->text(1), EntryLabel(*m_Model, F))
			.arg(F.m_TargetPageTag)
			.arg(F.m_TargetPC, 4, 16, QChar('0'))
			.arg(F.m_EndCycle - F.m_StartCycle)
			.arg(F.m_StartCycle)
			.arg(F.m_EndCycle));
	emit ExecutionRequested(Idx);
}
