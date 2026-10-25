#pragma once
#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

// Modal "we're parsing" dialog. Shown while TraceModel::computeMetrics
// runs on a QtConcurrent worker. Three exit paths:
//   * Completed  - worker emitted metricsReady normally; dialog auto-closes.
//   * Cancelled  - user clicked Cancel, sets the model's cancel flag, worker
//                  bails at the next checkpoint; widgets see partial state.
//   * Background - user clicked "Parse in background"; dialog dismisses,
//                  worker keeps running, UI becomes interactive immediately.
struct TraceLoadProgressDialog : public QDialog
{
	Q_OBJECT
  public:
	enum Outcome
	{
		kOutcomeCompleted = 0,
		kOutcomeCancelled = 1,
		kOutcomeBackgrounded = 2,
	};

	explicit TraceLoadProgressDialog(QWidget* Parent = nullptr);

	Outcome GetOutcome() const
	{
		return m_Outcome;
	}

  public slots:
	void SetProgress(int Percent, const QString& Status);
	void OnMetricsReady(bool bCancelled);

  signals:
	void CancelRequested();

  private slots:
	void OnCancel();
	void OnBackground();

  private:
	QLabel* m_Status = nullptr;
	QProgressBar* m_Bar = nullptr;
	QPushButton* m_Cancel = nullptr;
	QPushButton* m_Bg = nullptr;
	Outcome m_Outcome = kOutcomeCancelled; // default if user X's the dialog
};
