#include <inspeccy/version.h>
#include "CallGraphBuilder.h"
#include "IcicleWidget.h"
#include "MainWindow.h"
#include "OffscreenFonts.h"
#include "TraceLaunchDialog.h"
#include "TraceLoadProgressDialog.h"
#include "TraceModel.h"

#include <trace/trace_binary.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>

#include <algorithm>

int main(int Argc, char** Argv)
{
	QApplication App(Argc, Argv);
	ConfigureOffscreenFonts();
	// Organization + application identify QSettings' on-disk file. Used by
	// MainWindow's Recent Traces list.
	// QSettings file moves from ~/.config/inspeccy/inspeccy.conf
	// to ~/.config/inspeccy/inspeccy-viewer.conf with the rename (the CLI took
	// over the plain "inspeccy" name). Old recent-files list is lost; cheaper
	// than writing a migrator. Organization stays "inspeccy" (the family).
	App.setOrganizationName(QStringLiteral("inspeccy"));
	App.setApplicationName(QStringLiteral("inspeccy-viewer"));
	App.setApplicationVersion(QStringLiteral(INSPECCY_VERSION));
	App.setApplicationDisplayName(QStringLiteral("inspeccy-viewer"));

	QCommandLineParser Parser;
	Parser.setApplicationDescription(QStringLiteral("Qt-based inspector for .btr binary traces produced by "
													"inspeccy. Drop a path on the command line to open it directly."));
	Parser.addHelpOption();
	Parser.addVersionOption();
	Parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Optional .btr file to open at startup."));
	// --screenshot <path>: open the trace, wait for metricsReady, grab the
	// MainWindow, save as PNG, and exit. Headless capture for build-time
	// visual verification - runs cleanly under QT_QPA_PLATFORM=offscreen.
	QCommandLineOption ShotOpt(QStringLiteral("screenshot"),
		QStringLiteral("Render once, write PNG to <path>, then exit."), QStringLiteral("path"));
	Parser.addOption(ShotOpt);
	// Optional pre-toggles, only effective alongside --screenshot.
	QCommandLineOption TopFnsOpt(
		QStringLiteral("top-functions"), QStringLiteral("Capture with the instruction pane in TopFunctions mode."));
	Parser.addOption(TopFnsOpt);
	QCommandLineOption VramOpt(
		QStringLiteral("vram"), QStringLiteral("Capture with the memory pane in VRAM (ULA) mode."));
	Parser.addOption(VramOpt);
	QCommandLineOption InstrTabOpt(QStringLiteral("instr-tab"),
		QStringLiteral("Instruction pane tab 0..3 (0=Disassembly 1=Top exclusive "
					   "2=Top inclusive 3=Call structure)."),
		QStringLiteral("idx"));
	Parser.addOption(InstrTabOpt);
	QCommandLineOption MemModeOpt(QStringLiteral("mem-mode"),
		QStringLiteral("Direct memory-mode index 0..7 (0=Hex 1=Screen0 2=Screen1 "
					   "3=Visible 4=Invisible 5=TimexScreen2 6=TimexHicolor "
					   "7=TimexHires)."),
		QStringLiteral("idx"));
	Parser.addOption(MemModeOpt);
	QCommandLineOption DlgOpt(
		QStringLiteral("show-trace-dialog"), QStringLiteral("Open the Trace dialog before grabbing. Captures the "
															"DIALOG instead of the main window."));
	Parser.addOption(DlgOpt);
	QCommandLineOption PrefillOpt(QStringLiteral("prefill-binary"),
		QStringLiteral("Pre-fill the Trace dialog's binary field with <path>."), QStringLiteral("path"));
	Parser.addOption(PrefillOpt);
	QCommandLineOption PrefillMachineOpt(QStringLiteral("prefill-machine"),
		QStringLiteral("Pre-set machine index 0..4 (48K=0, 128K=1, Pentagon=2, Next50=3, Next60=4)."),
		QStringLiteral("idx"));
	Parser.addOption(PrefillMachineOpt);
	QCommandLineOption ZoomOpt(QStringLiteral("icicle-zoom"),
		QStringLiteral("Set icicle viewport for screenshot - \"<leftCycle>:<cyc/px>\""), QStringLiteral("range"));
	Parser.addOption(ZoomOpt);
	QCommandLineOption OvZoomOpt(QStringLiteral("overview-zoom"),
		QStringLiteral("Set overview zoom for screenshot - \"<firstFrame>:<frameCount>\""), QStringLiteral("range"));
	Parser.addOption(OvZoomOpt);
	QCommandLineOption CursorOpt(QStringLiteral("cursor-cycle"),
		QStringLiteral("Place the cursor at this absolute cycle for screenshot."), QStringLiteral("cycle"));
	Parser.addOption(CursorOpt);
	QCommandLineOption ProgOpt(QStringLiteral("show-progress-dialog"),
		QStringLiteral("Open the parse-progress dialog with mocked progress "
					   "values, then screenshot it instead of the main window."));
	Parser.addOption(ProgOpt);
	QCommandLineOption HlOpt(QStringLiteral("icicle-highlight"),
		QStringLiteral("Icicle highlight bitmask for screenshot - int (1=INT, 2=ROM, 4=DMA)."), QStringLiteral("mask"));
	Parser.addOption(HlOpt);
	// --bench: headless responsiveness benchmark. Opens the trace (blocking
	// until the call graph is built), then forces timed synchronous repaints
	// of the icicle at several viewport positions and zoom levels and prints
	// a table of milliseconds per paint. "Is scroll/zoom snappy?" becomes a
	// number that can be diffed before and after a fix, with no window at
	// all. Run under QT_QPA_PLATFORM=offscreen; add INSPECCY_PERF=1 for the
	// per-paint scanned/drawn frame counts logged by IcicleWidget.
	QCommandLineOption BenchOpt(
		QStringLiteral("bench"), QStringLiteral("Headless icicle repaint benchmark: print ms per paint, then exit."));
	Parser.addOption(BenchOpt);
	Parser.process(App);

	const QStringList Args = Parser.positionalArguments();
	if (Args.size() > 1)
	{
		// We don't have a "multiple-document interface" yet. Earlier the
		// viewer silently opened each arg in sequence, leaving only the
		// last one visible - confusing and undocumented. Refuse outright.
		qCritical("inspeccy-viewer: at most one .btr path may be "
				  "given on the command line (got %lld)",
			(long long)Args.size());
		return 4;
	}

	MainWindow Window;
	Window.show();
	if (!Args.isEmpty())
	{
		Window.OpenTraceFile(Args.first());
	}

	if (Parser.isSet(BenchOpt))
	{
		if (Args.isEmpty())
		{
			qCritical("inspeccy-viewer: --bench needs a .btr path");
			return 4;
		}
		// OpenTraceFile blocked until the metrics worker finished. Give the
		// window a realistic size, then let the queued .then() widget refresh
		// and the layout settle before measuring.
		Window.resize(1900, 1100);
		QCoreApplication::processEvents();
		TraceModel* Model = Window.Model();
		const auto& Frames = Model->CallGraph().Frames();
		const int N = Frames.size();
		if (N == 0)
		{
			qCritical("inspeccy-viewer: --bench: call graph is empty (metrics not built?)");
			return 5;
		}
		const uint64_t Extent =
			(Model->GetHeader() != nullptr && Model->GetHeader()->m_EndCycle != BinaryTrace::kSentinel)
				? Model->GetHeader()->m_EndCycle
				: Frames.last().m_EndCycle;
		IcicleWidget* Ic = Window.Icicle();
		const int W = std::max(1, Ic->width());
		qInfo("[bench] %d call frames, extent %llu cycles, icicle %d px wide", N, (unsigned long long)Extent, W);
		// Positions across the trace x zoom levels: the whole trace in view,
		// and zoomed to 1000 cyc/px (a "looking at a few frames" view). Each
		// cell is one forced synchronous repaint. The draw loop scans every
		// call frame from index 0 up to the viewport, so before the O(log N)
		// fix the right-hand positions are the slow ones.
		const double Fracs[] = {0.0, 0.25, 0.5, 0.75, 1.0};
		const double Cpps[] = {double(Extent) / double(W), 1000.0};
		qInfo("[bench] %6s  %12s  %9s", "pos", "cyc/px", "paint ms");
		for (double Cpp : Cpps)
		{
			for (double Fr : Fracs)
			{
				// Keep the viewport inside the trace: left = fr * (extent - visible).
				const double Visible = Cpp * double(W);
				const double MaxLeft = std::max(0.0, double(Extent) - Visible);
				const uint64_t Left = uint64_t(Fr * MaxLeft);
				Window.SetIcicleZoom(Left, Cpp);
				QElapsedTimer T;
				T.start();
				(void)Ic->grab(); // synchronous paintEvent
				qInfo("[bench] %5.0f%%  %12.1f  %9lld", Fr * 100.0, Cpp, (long long)T.elapsed());
			}
		}
		return 0;
	}

	const QString ShotPath = Parser.value(ShotOpt);
	if (!ShotPath.isEmpty())
	{
		const bool bTopFns = Parser.isSet(TopFnsOpt);
		const bool bVram = Parser.isSet(VramOpt);
		const bool bDialog = Parser.isSet(DlgOpt);
		// Drive a single event-loop iteration so the metrics worker job has
		// a chance to start; then grab. The dialog branch builds the modal
		// non-blocking (show() not exec()) so the timer thread can grab it.
		QTimer::singleShot(2500, &App,
			[&, bTopFns, bVram, bDialog]
			{
				if (Parser.isSet(ProgOpt))
				{
					auto* Pd = new TraceLoadProgressDialog(&Window);
					Pd->setAttribute(Qt::WA_DeleteOnClose);
					Pd->SetProgress(45, QStringLiteral("Reconstructing call graph..."));
					Pd->setModal(false);
					Pd->show();
					QTimer::singleShot(200, &App,
						[&, Pd]
						{
							const QPixmap Pm = Pd->grab();
							if (!Pm.save(ShotPath))
							{
								qCritical("inspeccy-viewer: failed to write screenshot to %s", qPrintable(ShotPath));
							}
							App.quit();
						});
					return;
				}
				if (bDialog)
				{
					auto* Dlg = new TraceLaunchDialog(&Window);
					Dlg->setAttribute(Qt::WA_DeleteOnClose);
					Dlg->setModal(false);
					const QString Pre = Parser.value(PrefillOpt);
					if (!Pre.isEmpty())
					{
						Dlg->PresetBinary(Pre);
					}
					const QString PreM = Parser.value(PrefillMachineOpt);
					if (!PreM.isEmpty())
					{
						Dlg->PresetMachine(PreM.toInt());
					}
					Dlg->show();
					QTimer::singleShot(200, &App,
						[&, Dlg]
						{
							const QPixmap Pm = Dlg->grab();
							if (!Pm.save(ShotPath))
							{
								qCritical("inspeccy-viewer: failed to write screenshot to %s", qPrintable(ShotPath));
							}
							App.quit();
						});
					return;
				}
				if (bTopFns)
				{
					Window.SetInstructionsTopFunctionsMode(true);
				}
				if (bVram)
				{
					Window.SetMemoryVramMode(true);
				}
				const QString Oz = Parser.value(OvZoomOpt);
				if (Oz.contains(QLatin1Char(':')))
				{
					const QStringList Parts = Oz.split(QLatin1Char(':'));
					Window.SetOverviewZoom(Parts.at(0).toInt(), Parts.at(1).toInt());
				}
				const QString It = Parser.value(InstrTabOpt);
				if (!It.isEmpty())
				{
					Window.SetInstructionsTabIndex(It.toInt());
				}
				const QString Mm = Parser.value(MemModeOpt);
				if (!Mm.isEmpty())
				{
					Window.SetMemoryModeIndex(Mm.toInt());
				}
				const QString Zoom = Parser.value(ZoomOpt);
				if (!Zoom.isEmpty())
				{
					const QStringList Parts = Zoom.split(QChar(':'));
					if (Parts.size() == 2)
					{
						Window.SetIcicleZoom(Parts[0].toULongLong(), Parts[1].toDouble());
					}
				}
				const QString CurC = Parser.value(CursorOpt);
				if (!CurC.isEmpty())
				{
					Window.SetCursorAtCycle(CurC.toULongLong());
				}
				const QString HlV = Parser.value(HlOpt);
				if (!HlV.isEmpty())
				{
					Window.SetIcicleHighlightMask(HlV.toUInt());
				}
				QTimer::singleShot(150, &App,
					[&]
					{
						const QPixmap Pm = Window.grab();
						if (!Pm.save(ShotPath))
						{
							qCritical("inspeccy-viewer: failed to write screenshot to %s", qPrintable(ShotPath));
						}
						App.quit();
					});
			});
	}
	return App.exec();
}
