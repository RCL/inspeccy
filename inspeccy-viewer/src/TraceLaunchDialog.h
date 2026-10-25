#pragma once
#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;

// Modal dialog that captures all the inputs needed to invoke the emulator
// in trace-recording mode. On accept(), m_OutputPath holds the .btr path
// the user should open next (if outcome() == TraceAndOpen). Cancellation,
// process failure, and process success share the same close path - the
// dialog only accept()s once the emulator exits cleanly.
struct TraceLaunchDialog : public QDialog
{
	Q_OBJECT
  public:
	enum Outcome
	{
		kOutcomeCancelled = 0,
		kOutcomeTraced = 1, // .btr written, do not open
		kOutcomeTraceAndOpen = 2, // .btr written, MainWindow should open it
	};

	explicit TraceLaunchDialog(QWidget* Parent = nullptr);
	~TraceLaunchDialog() override;

	// Pre-fill the binary path AS IF the user picked it via the Browse
	// dialog. Used by the headless --show-trace-dialog screenshot path so
	// we can verify the file-constrained machine combo in CI.
	void PresetBinary(const QString& Path);
	// Force a specific machine for the screenshot path.
	void PresetMachine(int Idx);

	// The full parameter set as a QVariantMap (binary, machine tag, ram, run
	// unit/value, ROM dir, TR-DOS, TR-DOS run-file, output). Used by the recent-
	// trace History: currentParams() snapshots the widgets; applyParams() restores
	// them (binary first, so detectAndConstrainMachine's cascade doesn't clobber
	// the restored fields).
	QVariantMap CurrentParams() const;
	void ApplyParams(const QVariantMap& P);

	// Recent-trace history persistence (QSettings "recentTraceParams", cap 10).
	static QList<QVariantMap> LoadRecentParams();
	static void RememberParams(const QVariantMap& P);
	static QString ParamsLabel(const QVariantMap& P);

	QString OutputPath() const
	{
		return m_OutputPath;
	}
	Outcome GetOutcome() const
	{
		return m_Outcome;
	}

  private slots:
	void OnBrowseBinary();
	void OnBrowseRomDir();
	void OnBrowseOutput();
	void OnBinaryEdited();
	void OnTrace();
	void OnTraceAndOpen();
	void OnProcessFinished(int ExitCode, int ExitStatus);
	void OnProcessReadyRead();

  private:
	void RunTrace(bool bOpenAfter);
	void SetRunning(bool bOn);
	void DetectAndConstrainMachine(const QString& BinaryPath);
	void RefreshTrdFileList(const QString& Path); // repopulate the B-file combo (TRD/SCL)
	QString DefaultOutputFor(const QString& BinaryPath) const;
	QString FindEmulatorBinary() const;
	QString FindRomDir() const;
	static QString DetectBinaryKind(const QString& Path); // "nex"/"sna48"/"sna128"/"tap"/"trd"/"unknown"
	// BASIC (".B") file names in a TR-DOS .trd/.scl image, in catalogue order.
	static QStringList ListTrdBasicFiles(const QString& Path);

	QLineEdit* m_Binary = nullptr;
	QPushButton* m_BinBrowse = nullptr;

	QComboBox* m_Machine = nullptr;
	QLabel* m_MachineNote = nullptr;

	QComboBox* m_RamMb = nullptr;
	QLabel* m_RamNote = nullptr;

	QComboBox* m_RunUnit = nullptr;
	QSpinBox* m_RunVal = nullptr;
	QSpinBox* m_TraceFromSeconds = nullptr;
	QLineEdit* m_Sld = nullptr;
	QPushButton* m_SldBrowse = nullptr;

	QLineEdit* m_RomDir = nullptr;
	QPushButton* m_RomBrowse = nullptr;
	QLabel* m_RomNote = nullptr;

	QCheckBox* m_TrdosCheck = nullptr;
	QLabel* m_TrdosNote = nullptr;

	// TR-DOS run-file selector - shown only when a .trd/.scl image is chosen.
	// Populated with the image's BASIC (".B") files; the choice becomes
	// --trd-run <name>. m_TrdFileLabel is the form-row label (hidden with it).
	QLabel* m_TrdFileLabel = nullptr;
	QComboBox* m_TrdFile = nullptr;
	QLabel* m_TrdFileNote = nullptr;

	QLineEdit* m_Output = nullptr;
	QPushButton* m_OutBrowse = nullptr;

	QPlainTextEdit* m_Log = nullptr;

	QPushButton* m_BtnCancel = nullptr;
	QPushButton* m_BtnTrace = nullptr;
	QPushButton* m_BtnTraceOpen = nullptr;

	QProcess* m_Proc = nullptr;

	Outcome m_Outcome = kOutcomeCancelled;
	QString m_OutputPath;
	bool m_bOpenAfter = false;
};
