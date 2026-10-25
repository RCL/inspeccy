// Manual integration test: params.json output-directory [--reuse]
// Run with QT_QPA_PLATFORM=offscreen. Captures the real viewer, without desktop
// input injection, a second ranking implementation, or numeric viewport overrides.
#include "MainWindow.h"
#include "OffscreenFonts.h"
#include "IcicleWidget.h"
#include "InterruptView.h"
#include "TraceLaunchDialog.h"
#include "TraceModel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <cstdlib>
#include <cstdio>

static void Require(bool Ok, const char* Message)
{
	if (!Ok)
	{
		qCritical("FAIL: %s", Message);
		std::exit(1);
	}
}
static void Settle()
{
	QEventLoop Loop;
	QTimer::singleShot(100, &Loop, &QEventLoop::quit);
	Loop.exec();
}
static void Save(const QString& Path, const QByteArray& Data)
{
	QFile File(Path);
	Require(File.open(QIODevice::WriteOnly), "open output");
	Require(File.write(Data) == Data.size(), "write output");
}
static QAction* Action(MainWindow& Window, const QString& Shortcut)
{
	for (auto* A : Window.findChildren<QAction*>())
	{
		if (A->shortcut() == QKeySequence(Shortcut))
		{
			return A;
		}
	}
	return nullptr;
}
int main(int Argc, char** Argv)
{
	// Set before QApplication so this executable cannot activate desktop windows.
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication App(Argc, Argv);
	ConfigureOffscreenFonts();
	setvbuf(stderr, nullptr, _IONBF, 0);
	Require(App.arguments().size() >= 3, "usage: params.json output-directory [--reuse]");
	QDir Output(App.arguments()[2]);
	Require(Output.mkpath("."), "create output directory");
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, Output.absolutePath());
	App.setOrganizationName("inspeccy-ui-test");
	App.setApplicationName("viewer-usability");
	QFile ParamsFile(App.arguments()[1]);
	Require(ParamsFile.open(QIODevice::ReadOnly), "open capture parameters");
	QJsonParseError Error;
	const auto ParamsDoc = QJsonDocument::fromJson(ParamsFile.readAll(), &Error);
	Require(Error.error == QJsonParseError::NoError && ParamsDoc.isObject(), "valid capture parameters");
	const auto Params = ParamsDoc.object().toVariantMap();
	MainWindow Window;
	Window.resize(1900, 1100); // a normal maximized desktop-sized viewport
	Window.show();
	Settle();
	{
		TraceLaunchDialog Settings(&Window);
		Settings.ApplyParams(Params);
		Settings.show();
		Settle();
		Require(Settings.grab().save(Output.filePath("capture-settings.png")), "save capture dialog");
	}
	if (!App.arguments().contains("--reuse"))
	{
		TraceLaunchDialog Dialog(&Window);
		Dialog.ApplyParams(Params); // same widgets as selecting files / entering values
		Dialog.show();
		Settle();
		Require(Dialog.grab().save(Output.filePath("capture-settings.png")), "save capture dialog");
		QPushButton* Start = nullptr;
		for (auto* Button : Dialog.findChildren<QPushButton*>())
		{
			if (Button->text() == "Trace and Open")
			{
				Start = Button;
			}
		}
		Require(Start != nullptr, "Trace and Open button available");
		QEventLoop Capture;
		QObject::connect(&Dialog, &QDialog::finished, &Capture, &QEventLoop::quit);
		QTimer Watch;
		Watch.setInterval(1000);
		QObject::connect(&Watch, &QTimer::timeout,
			[&]
			{
				if (Start->isEnabled())
				{
					Capture.quit(); // finished, validation or CLI error
				}
			});
		qInfo("UI: click Trace and Open");
		Start->click();
		Watch.start();
		Capture.exec();
		Watch.stop();
		const auto* Log = Dialog.findChild<QPlainTextEdit*>();
		if (Log)
		{
			Save(Output.filePath("capture.log"), Log->toPlainText().toUtf8());
		}
		Require(Dialog.GetOutcome() == TraceLaunchDialog::kOutcomeTraceAndOpen, "capture succeeded (see capture.log)");
	}
	qInfo("UI: open recorded trace");
	QObject::connect(Window.Model(), &TraceModel::ProgressUpdate,
		[](int Percent, const QString& Status)
		{
			qInfo("load %d%% %s", Percent, qPrintable(Status));
		});
	Window.OpenTraceFile(Params.value("output").toString());
	Window.activateWindow(); // return from the load dialog to the offscreen viewer
	Settle();
	auto* Isrs = Action(Window, "Ctrl+I");
	auto* Displays = Action(Window, "Ctrl+M");
	Require(Isrs && Displays, "profiling menu actions available");
	Displays->trigger(); // View > Show memory displays (off)
	Isrs->trigger(); // View > Frame ISR executions, default descending duration
	Settle();
	auto* Pane = Window.findChild<InterruptView*>();
	Require(Pane != nullptr, "frame ISR pane available");
	auto* Table = Pane->findChild<QTreeWidget*>();
	Require(Table && Table->topLevelItemCount() >= 5, "at least five completed frame ISRs");
	QJsonArray Rows;
	for (int Rank = 1; Rank <= 5; ++Rank)
	{
		if (Rank > 1)
		{
			Require(App.focusWidget() == Table, "keyboard focus remains in ranked results");
			QKeyEvent Down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
			QApplication::sendEvent(Table, &Down);
			QKeyEvent Up(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
			QApplication::sendEvent(Table, &Up);
			Settle();
		}
		const auto* Item = Table->currentItem();
		Require(Item && Item->text(0).toInt() == Rank, "keyboard selected expected duration rank");
		Require(Item->isSelected(), "fitted ISR has a visible selected row");
		QStringList Labels;
		for (auto* Label : Pane->findChildren<QLabel*>())
		{
			Labels << Label->text();
		}
		const QString Name = QStringLiteral("isr-rank-%1-frame-%2.png").arg(Rank).arg(Item->text(1));
		Require(Window.grab().save(Output.filePath(Name)), "save viewer screenshot");
		QJsonObject Row;
		Row["rank"] = Rank;
		Row["frame"] = Item->text(1);
		Row["cycles"] = Item->text(2);
		Row["start_cycle"] = Item->text(3);
		Row["symbol"] = Item->text(4);
		Row["screenshot"] = Name;
		Row["details"] = Labels.join("\n");
		Rows.append(Row);
		qInfo("UI rank %d: frame %s, %s cycles, %s", Rank, qPrintable(Item->text(1)), qPrintable(Item->text(2)),
			qPrintable(Item->text(4)));
	}
	Save(Output.filePath("ranked-results.json"), QJsonDocument(Rows).toJson());
	qInfo("PASS: capture dialog, ranked table, Down-key navigation, automatic fit, five viewer screenshots");
	return 0;
}
