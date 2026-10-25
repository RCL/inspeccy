#include <inspeccy/version.h>
#include "MainWindow.h"

#include "IcicleWidget.h"
#include "InstructionView.h"
#include "InterruptView.h"
#include "MemoryView.h"
#include "OverviewWidget.h"
#include "RegisterView.h"
#include "TraceLaunchDialog.h"
#include "TraceLoadProgressDialog.h"
#include "TraceModel.h"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

MainWindow::MainWindow(QWidget* Parent) : QMainWindow(Parent)
{
	setWindowTitle(tr("inspeccy-viewer %1").arg(QStringLiteral(INSPECCY_VERSION)));
	resize(1280, 800);

	m_Model = new TraceModel(this);

	// === menu ============================================================
	auto* FileMenu = menuBar()->addMenu(tr("&File"));
	auto* OpenAct = FileMenu->addAction(tr("&Open..."));
	OpenAct->setShortcut(QKeySequence::Open);
	connect(OpenAct, &QAction::triggered, this, &MainWindow::OnFileOpen);

	auto* TraceAct = FileMenu->addAction(tr("&Trace..."));
	TraceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
	connect(TraceAct, &QAction::triggered, this, &MainWindow::OnFileTrace);

	m_RecentMenu = FileMenu->addMenu(tr("Open &Recent"));
	{
		QSettings Settings;
		m_Recent = Settings.value(QStringLiteral("recentTraces")).toStringList();
	}
	RebuildRecentMenu();

	// Recent-trace parameter sets (up to 10): re-open the Trace dialog pre-filled.
	m_HistoryMenu = FileMenu->addMenu(tr("Trace &History"));
	RebuildHistoryMenu();

	FileMenu->addSeparator();
	auto* SldAct = FileMenu->addAction(tr("&Locate symbols..."));
	SldAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
	connect(SldAct, &QAction::triggered, this, &MainWindow::OnLocateSymbols);
	auto* SaveShot = FileMenu->addAction(tr("Save &screenshot..."));
	SaveShot->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
	connect(SaveShot, &QAction::triggered, this,
		[this]
		{
			const QString Path = QFileDialog::getSaveFileName(this, tr("Save viewer screenshot"),
				QFileInfo(m_Model->BtrPath()).absolutePath() + QStringLiteral("/viewer.png"), tr("PNG image (*.png)"));
			if (Path.isEmpty())
			{
				return;
			}
			// Let the file dialog close and the viewer repaint before grabbing it.
			QTimer::singleShot(100, this,
				[this, Path]
				{
					if (!grab().save(Path, "PNG"))
					{
						QMessageBox::warning(this, tr("Screenshot failed"), tr("Could not save %1").arg(Path));
					}
					else
					{
						statusBar()->showMessage(tr("Screenshot saved: %1").arg(Path));
					}
				});
		});

	FileMenu->addSeparator();
	auto* QuitAct = FileMenu->addAction(tr("E&xit"));
	QuitAct->setShortcut(QKeySequence::Quit);
	connect(QuitAct, &QAction::triggered, this, &QWidget::close);

	// === layout ==========================================================
	// Vertical splitter: overview (thin) / icicle (small) / main area.
	// Callstacks in this workload are shallow, so the icicle only needs a
	// sliver - the bulk of the height goes to the assembly + Displays below.
	auto* VSplit = new QSplitter(Qt::Vertical, this);
	m_VSplit = VSplit;

	m_Overview = new OverviewWidget;
	m_Icicle = new IcicleWidget;
	m_Icicle->setMinimumHeight(96); // room for the ruler + a few call levels
	VSplit->addWidget(m_Overview);
	VSplit->addWidget(m_Icicle);

	// Main row: the assembly view (the star - gets most width) on the left;
	// on the right a vertical stack of two Displays. Each Display is a
	// MemoryView, which already switches between the hex grid and the ULA
	// screen renders, so EITHER panel can show whatever via its own Display
	// dropdown. We just pick the defaults: top = memory hex, bottom = the
	// ULA visible screen. The CPU-register state lives in the icicle ruler.
	auto* HSplit = new QSplitter(Qt::Horizontal);
	m_Instr = new InstructionView;

	auto* DisplaySplit = new QSplitter(Qt::Vertical);
	m_Memory = new MemoryView;
	m_Memory2 = new MemoryView;
	DisplaySplit->addWidget(m_Memory);
	DisplaySplit->addWidget(m_Memory2);
	DisplaySplit->setStretchFactor(0, 1);
	DisplaySplit->setStretchFactor(1, 1);

	m_AnalysisTabs = new QTabWidget;
	m_AnalysisTabs->addTab(m_Instr, tr("Instructions"));
	m_Interrupts = new InterruptView(m_Model);
	m_AnalysisTabs->addTab(m_Interrupts, tr("Frame ISRs"));
	HSplit->addWidget(m_AnalysisTabs);
	HSplit->addWidget(DisplaySplit);
	HSplit->setStretchFactor(0, 3); // assembly view: widest pane
	HSplit->setStretchFactor(1, 2);

	VSplit->addWidget(HSplit);
	VSplit->setStretchFactor(0, 0); // overview: fixed-ish band
	VSplit->setStretchFactor(1, 1); // icicle: small - shallow callstacks
	VSplit->setStretchFactor(2, 6); // main area: assembly + Displays

	// Explicit initial sizes so the first show honours the intent even before
	// the user drags anything (stretch factors only govern extra space).
	// Overview gets a real band, not a sliver: it now stacks CPU time by
	// source (main flow / each interrupt) under a legend, which needs height
	// to stay readable.
	VSplit->setSizes({132, 130, 620});
	HSplit->setSizes({760, 500});
	DisplaySplit->setSizes({320, 320});

	setCentralWidget(VSplit);
	auto* ViewMenu = menuBar()->addMenu(tr("&View"));
	auto* Isrs = ViewMenu->addAction(tr("&Frame ISR executions"));
	Isrs->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
	connect(Isrs, &QAction::triggered, this,
		[this]
		{
			m_AnalysisTabs->setCurrentWidget(m_Interrupts);
			m_Interrupts->FocusResults();
		});
	auto* Fit = ViewMenu->addAction(tr("&Fit selected ISR"));
	Fit->setShortcut(QKeySequence(QStringLiteral("F")));
	connect(Fit, &QAction::triggered, m_Interrupts, &InterruptView::FitSelected);
	auto* All = ViewMenu->addAction(tr("Fit &whole trace"));
	All->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
	connect(All, &QAction::triggered, m_Icicle, &IcicleWidget::FitWholeTrace);
	auto* Displays = ViewMenu->addAction(tr("Show &memory displays"));
	Displays->setCheckable(true);
	Displays->setChecked(true);
	Displays->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
	connect(Displays, &QAction::toggled, DisplaySplit, &QWidget::setVisible);
	connect(m_Interrupts, &InterruptView::ExecutionRequested, this, &MainWindow::FitExecution);
	connect(m_AnalysisTabs, &QTabWidget::currentChanged, this,
		[this](int Index)
		{
			if (Index == 1)
			{
				m_Interrupts->FocusResults();
			}
		});

	// === wiring ==========================================================
	m_Overview->SetModel(m_Model);
	m_Icicle->SetModel(m_Model);
	m_Instr->SetModel(m_Model);
	m_Memory->SetModel(m_Model);
	m_Memory2->SetModel(m_Model);
	m_Memory2->SetDisplayMode(MemoryView::kDisplayModeUlaVisible); // bottom Display: ULA screen

	connect(m_Overview, &OverviewWidget::CursorRequested, this, &MainWindow::OnCursorRequestedByFrame);
	connect(m_Icicle, &IcicleWidget::CursorRequested, this, &MainWindow::OnCursorRequestedByCycle);
	connect(m_Icicle, &IcicleWidget::ViewportChanged, m_Instr, &InstructionView::SetViewportRange);
	connect(m_Icicle, &IcicleWidget::ViewportChanged, m_Overview, &OverviewWidget::SetIcicleViewport);
	// SLD changes (after manual Locate or after trace open with embedded
	// paths) should trigger a re-paint of widgets that show symbols.
	connect(m_Model, &TraceModel::SldChanged, this,
		[this]
		{
			SetCursor(m_Cursor);
		});

	statusBar()->showMessage(tr("Ready. Open a .btr file (File -> Open) "
								"or trace one (File -> Trace)."));
}

void MainWindow::OpenTraceFile(const QString& Path)
{
	m_Interrupts->Clear();
	// Bump generation BEFORE invalidating the model - any in-flight metrics
	// job from a prior open() will see a stale value when it tries to
	// complete and bail without touching widgets.
	const quint64 Gen = ++m_OpenGen;

	const QString Err = m_Model->Open(Path);
	if (!Err.isEmpty())
	{
		// Mirror to stderr so headless / CI / scripted runs can SEE the
		// failure. QMessageBox renders nowhere on offscreen platforms,
		// making errors otherwise invisible.
		qCritical("inspeccy-viewer: open failed: %s", qPrintable(Err));
		QMessageBox::critical(this, tr("Trace open failed"), Err);
		statusBar()->showMessage(tr("Open failed: %1").arg(Err));
		return;
	}
	qInfo("inspeccy-viewer: opened %s (%lld frames)", qPrintable(QFileInfo(Path).fileName()),
		(long long)m_Model->Index().size());
	if (!m_Model->UnresolvedSldPaths().isEmpty())
	{
		qInfo("inspeccy-viewer: %lld SLD path(s) in header could not be loaded - "
			  "use File -> Locate symbols... to point at them.",
			(long long)m_Model->UnresolvedSldPaths().size());
	}
	setWindowTitle(tr("inspeccy-viewer %1 - %2").arg(QStringLiteral(INSPECCY_VERSION), QFileInfo(Path).fileName()));
	statusBar()->showMessage(
		tr("Loaded %1 (%2 frames). Computing metrics...").arg(QFileInfo(Path).fileName()).arg(m_Model->Index().size()));

	// Clear any stale cursor / register state in widgets before the
	// metrics job comes back. SetCursor with a zeroed cursor is the
	// canonical "I have nothing meaningful to show" signal.
	SetCursor(Cursor{});

	// Modal progress dialog. Three outcomes:
	//   Completed  - exec() returns after worker emits metricsReady, widgets
	//                pick up full data via the .then() callback below.
	//   Cancelled  - worker bailed early; partial data left in model,
	//                widgets render whatever's there.
	//   Background - dialog dismisses, exec() returns immediately, worker
	//                keeps running, widgets update when it eventually
	//                completes. UI becomes interactive right away.
	auto* Dlg = new TraceLoadProgressDialog(this);
	Dlg->setAttribute(Qt::WA_DeleteOnClose);
	// Queued connections - TraceModel emits these from a worker thread.
	connect(m_Model, &TraceModel::ProgressUpdate, Dlg, &TraceLoadProgressDialog::SetProgress, Qt::QueuedConnection);
	connect(m_Model, &TraceModel::MetricsReady, Dlg, &TraceLoadProgressDialog::OnMetricsReady, Qt::QueuedConnection);
	connect(Dlg, &TraceLoadProgressDialog::CancelRequested, m_Model, &TraceModel::RequestCancelMetrics);

	// Compute per-frame metrics on a worker; refresh widgets on completion
	// ONLY IF this is still the current open generation.
	QtConcurrent::run(
		[this]
		{
			m_Model->ComputeMetrics();
		})
		.then(this,
			[this, Gen]
			{
				if (Gen != m_OpenGen.load())
				{
					// A newer open() superseded us - drop this result on the floor.
					return;
				}
				if (!m_Model->Index().isEmpty())
				{
					SetCursor(m_Model->CursorAtFrame(m_Model->Index().first().m_FrameNumber));
				}
				m_Overview->update();
				m_Icicle->FitWholeTrace();
				m_Interrupts->Refresh();
				statusBar()->showMessage(tr("Loaded %1 (%2 frames)")
						.arg(QFileInfo(m_Model->BtrPath()).fileName())
						.arg(m_Model->Index().size()));
			});

	// Block UI thread on dialog until it closes (Completed / Cancelled /
	// Backgrounded). The worker keeps running on its own thread.
	Dlg->exec();
}

void MainWindow::FitExecution(int Index)
{
	const auto& Frames = m_Model->CallGraph().Frames();
	if (Index < 0 || Index >= Frames.size())
	{
		return;
	}
	const auto& F = Frames[Index];
	uint32_t MaxDepth = F.m_Depth;
	for (int I = Index + 1; I < Frames.size() && Frames[I].m_StartCycle < F.m_EndCycle; ++I)
	{
		MaxDepth = std::max(MaxDepth, Frames[I].m_Depth);
	}
	// Reserve every call level plus the ruler and HUD, without dragging a splitter.
	const int GraphHeight = 110 + int(MaxDepth + 1) * 20 + 24;
	const int Available = m_VSplit->height();
	m_VSplit->setSizes({100, GraphHeight, std::max(200, Available - 100 - GraphHeight)});
	SetCursor(m_Model->CursorAtCycle(F.m_StartCycle));
	m_Icicle->FitExecution(Index);
	statusBar()->showMessage(
		tr("Frame %1: complete ISR, %2 cycles. Up/Down selects another; F fits again; Ctrl+0 shows the whole trace.")
			.arg(m_Cursor.m_Frame)
			.arg(F.m_EndCycle - F.m_StartCycle));
}

void MainWindow::OnFileOpen()
{
	// Start directory priority: last-used (persisted in QSettings) -> the
	// folder the viewer executable lives in when there's no history yet.
	// We do NOT default to $HOME (Qt's built-in fallback) - that lands the
	// picker deep in the user's home, far from where .btr files are; and we
	// prefer the exe dir over the process CWD so a double-clicked launch
	// still starts somewhere predictable next to the tool.
	QSettings Settings;
	QString StartDir = Settings.value(QStringLiteral("lastOpenDir")).toString();
	if (StartDir.isEmpty() || !QFileInfo(StartDir).isDir())
	{
		StartDir = QCoreApplication::applicationDirPath();
	}
	const QString Path = QFileDialog::getOpenFileName(
		this, tr("Open binary trace"), StartDir, tr("Binary trace files (*.btr);;All files (*)"));
	if (Path.isEmpty())
	{
		return;
	}
	Settings.setValue(QStringLiteral("lastOpenDir"), QFileInfo(Path).absolutePath());
	AddToRecent(Path);
	OpenTraceFile(Path);
}

void MainWindow::OnFileTrace()
{
	TraceLaunchDialog Dlg(this);
	const int Rc = Dlg.exec();
	RebuildHistoryMenu(); // the dialog records params when a trace launches,
						  // even if the dialog is later cancelled
	if (Rc != QDialog::Accepted)
	{
		return;
	}
	const QString Out = Dlg.OutputPath();
	if (Out.isEmpty())
	{
		return;
	}
	AddToRecent(Out); // even Trace-only adds to recents - user might
					  // open it later from File -> Recent.
	if (Dlg.GetOutcome() == TraceLaunchDialog::kOutcomeTraceAndOpen)
	{
		OpenTraceFile(Out);
	}
}

void MainWindow::OnLocateSymbols()
{
	if (m_Model == nullptr || !m_Model->IsLoaded())
	{
		QMessageBox::information(
			this, tr("Locate symbols"), tr("Open a trace first; symbols attach to a loaded .btr."));
		return;
	}
	// Begin in the folder the currently-open binary (.btr) was loaded from -
	// the .sld.txt usually sits next to it. Fall back to the last-open dir,
	// then the exe dir, so we never dump the user into $HOME.
	QString StartDir = QFileInfo(m_Model->BtrPath()).absolutePath();
	if (StartDir.isEmpty() || !QFileInfo(StartDir).isDir())
	{
		StartDir = QSettings().value(QStringLiteral("lastOpenDir")).toString();
		if (StartDir.isEmpty() || !QFileInfo(StartDir).isDir())
		{
			StartDir = QCoreApplication::applicationDirPath();
		}
	}
	const QString Path = QFileDialog::getOpenFileName(
		this, tr("Locate sjasmplus .sld.txt"), StartDir, tr("SLD files (*.sld.txt *.sld);;All files (*)"));
	if (Path.isEmpty())
	{
		return;
	}
	const QString Err = m_Model->LoadSldFile(Path);
	if (!Err.isEmpty())
	{
		QMessageBox::warning(this, tr("Locate symbols"), tr("Failed to load:\n%1").arg(Err));
		return;
	}
	statusBar()->showMessage(tr("Loaded symbols from %1 (%2 total)")
								 .arg(QFileInfo(Path).fileName())
								 .arg(qsizetype(m_Model->Sld().SymbolCount())),
		5000);
	// Force the InstructionView / IcicleWidget to repaint with the new symbols.
	SetCursor(m_Cursor);
}

void MainWindow::OnOpenRecent()
{
	auto* Act = qobject_cast<QAction*>(sender());
	if (Act == nullptr)
	{
		return;
	}
	const QString Path = Act->data().toString();
	if (Path.isEmpty())
	{
		return;
	}
	if (!QFile::exists(Path))
	{
		QMessageBox::warning(this, tr("Recent trace"), tr("File no longer exists: %1").arg(Path));
		m_Recent.removeAll(Path);
		QSettings().setValue(QStringLiteral("recentTraces"), m_Recent);
		RebuildRecentMenu();
		return;
	}
	AddToRecent(Path); // bumps to top
	OpenTraceFile(Path);
}

void MainWindow::AddToRecent(const QString& Path)
{
	m_Recent.removeAll(Path);
	m_Recent.prepend(Path);
	while (m_Recent.size() > 10)
	{
		m_Recent.removeLast();
	}
	QSettings().setValue(QStringLiteral("recentTraces"), m_Recent);
	RebuildRecentMenu();
}

void MainWindow::RebuildRecentMenu()
{
	if (m_RecentMenu == nullptr)
	{
		return;
	}
	m_RecentMenu->clear();
	if (m_Recent.isEmpty())
	{
		auto* Placeholder = m_RecentMenu->addAction(tr("(no recent traces)"));
		Placeholder->setEnabled(false);
		return;
	}
	for (const QString& RecentPath : m_Recent)
	{
		const QString Label = QFileInfo(RecentPath).fileName() + QStringLiteral("  -  ") + RecentPath;
		auto* Act = m_RecentMenu->addAction(Label);
		Act->setData(RecentPath);
		connect(Act, &QAction::triggered, this, &MainWindow::OnOpenRecent);
	}
	m_RecentMenu->addSeparator();
	auto* ClearAct = m_RecentMenu->addAction(tr("Clear list"));
	connect(ClearAct, &QAction::triggered, this,
		[this]
		{
			m_Recent.clear();
			QSettings().setValue(QStringLiteral("recentTraces"), m_Recent);
			RebuildRecentMenu();
		});
}

void MainWindow::RebuildHistoryMenu()
{
	if (m_HistoryMenu == nullptr)
	{
		return;
	}
	m_HistoryMenu->clear();
	const QList<QVariantMap> Hist = TraceLaunchDialog::LoadRecentParams();
	if (Hist.isEmpty())
	{
		auto* Placeholder = m_HistoryMenu->addAction(tr("(no trace history)"));
		Placeholder->setEnabled(false);
		return;
	}
	for (const QVariantMap& P : Hist)
	{
		auto* Act = m_HistoryMenu->addAction(TraceLaunchDialog::ParamsLabel(P));
		Act->setData(P);
		connect(Act, &QAction::triggered, this, &MainWindow::OnRunHistory);
	}
	m_HistoryMenu->addSeparator();
	auto* ClearAct = m_HistoryMenu->addAction(tr("Clear list"));
	connect(ClearAct, &QAction::triggered, this,
		[this]
		{
			QSettings().remove(QStringLiteral("recentTraceParams"));
			RebuildHistoryMenu();
		});
}

void MainWindow::OnRunHistory()
{
	auto* Act = qobject_cast<QAction*>(sender());
	if (Act == nullptr)
	{
		return;
	}
	const QVariantMap P = Act->data().toMap();
	if (P.isEmpty())
	{
		return;
	}

	// Re-open the Trace dialog pre-filled so the user can review/tweak before
	// re-running (per the chosen behaviour - not a blind re-run).
	TraceLaunchDialog Dlg(this);
	Dlg.ApplyParams(P);
	const int Rc = Dlg.exec();
	RebuildHistoryMenu(); // a re-run may have bumped/added this entry
	if (Rc != QDialog::Accepted)
	{
		return;
	}
	const QString Out = Dlg.OutputPath();
	if (Out.isEmpty())
	{
		return;
	}
	AddToRecent(Out);
	if (Dlg.GetOutcome() == TraceLaunchDialog::kOutcomeTraceAndOpen)
	{
		OpenTraceFile(Out);
	}
}

void MainWindow::OnCursorRequestedByFrame(uint64_t Frame)
{
	if (m_Model->Index().isEmpty())
	{
		return;
	}
	SetCursor(m_Model->CursorAtFrame(Frame));
}

void MainWindow::OnCursorRequestedByCycle(uint64_t Cycle)
{
	if (m_Model->Index().isEmpty())
	{
		return;
	}
	SetCursor(m_Model->CursorAtCycle(Cycle));
}

void MainWindow::SetInstructionsTopFunctionsMode(bool bOn)
{
	m_Instr->SetMode(bOn ? InstructionView::kModeTopInclusive : InstructionView::kModeLinear);
}

void MainWindow::SetMemoryVramMode(bool bOn)
{
	auto* Combo = m_Memory->findChild<QComboBox*>();
	if (Combo == nullptr)
	{
		return;
	}
	Combo->setCurrentIndex(bOn ? 1 : 0);
}

void MainWindow::SetInstructionsTabIndex(int Idx)
{
	if (Idx < 0 || Idx > int(InstructionView::kModeCallStructure))
	{
		return;
	}
	m_Instr->SetMode(InstructionView::Mode(Idx));
}

void MainWindow::SetOverviewZoom(int FirstFrame, int FrameCount)
{
	m_Overview->SetViewWindow(FirstFrame, FrameCount);
}

void MainWindow::SetMemoryModeIndex(int Idx)
{
	auto* Combo = m_Memory->findChild<QComboBox*>();
	if (Combo == nullptr)
	{
		return;
	}
	Combo->setCurrentIndex(Idx);
}

void MainWindow::SetIcicleZoom(uint64_t LeftCycle, double CyclesPerPixel)
{
	m_Icicle->SetViewportCycles(LeftCycle, CyclesPerPixel);
}

void MainWindow::SetCursorAtCycle(uint64_t Cycle)
{
	if (m_Model == nullptr || m_Model->Index().isEmpty())
	{
		return;
	}
	SetCursor(m_Model->CursorAtCycle(Cycle));
}

void MainWindow::SetIcicleHighlightMask(uint32_t Mask)
{
	m_Icicle->SetHighlightMask(Mask);
}

void MainWindow::SetCursor(const Cursor& NewCursor)
{
	m_Cursor = NewCursor;
	m_Overview->SetCursor(NewCursor);
	m_Icicle->SetCursor(NewCursor);
	m_Instr->SetCursor(NewCursor);
	m_Memory->SetCursor(NewCursor);
	m_Memory2->SetCursor(NewCursor);
}
