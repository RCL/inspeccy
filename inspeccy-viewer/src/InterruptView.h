#pragma once
#include <QWidget>

class QLabel;
class QTreeWidget;
struct TraceModel;

// Individual completed ULA/frame ISR invocations across the entire recording.
struct InterruptView : public QWidget
{
	Q_OBJECT
  public:
	explicit InterruptView(TraceModel* Model, QWidget* Parent = nullptr);
	void Refresh();
	void Clear();
	void FocusResults();
	void FitSelected();

  signals:
	void ExecutionRequested(int CallFrameIndex);

  private:
	TraceModel* m_Model;
	QLabel* m_Summary;
	QLabel* m_Detail;
	QTreeWidget* m_Table;
};
