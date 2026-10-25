#pragma once
#include "Cursor.h"

#include <QLabel>
#include <QWidget>

struct TraceModel;

// Cursor-anchored CPU register dump. For v1 the values come from the
// nearest frame index entry's snapshot (= state at the START of that
// frame). Walking forward through delta records to the exact cursor
// cycle is Phase 2 work.

struct RegisterView : public QWidget
{
	Q_OBJECT
  public:
	explicit RegisterView(QWidget* Parent = nullptr);

	void SetModel(TraceModel* Model);

  public slots:
	void SetCursor(const Cursor& C);

  private:
	void Refresh();

	TraceModel* m_Model = nullptr;
	Cursor m_Cursor;

	QLabel* m_CycleLabel = nullptr;
	QLabel* m_FrameLabel = nullptr;
	QLabel* m_TimeLabel = nullptr;
	QLabel* m_PcLabel = nullptr;
	QLabel* m_SpLabel = nullptr;
	QLabel* m_AfLabel = nullptr;
	QLabel* m_BcLabel = nullptr;
	QLabel* m_DeLabel = nullptr;
	QLabel* m_HlLabel = nullptr;
	QLabel* m_IxLabel = nullptr;
	QLabel* m_IyLabel = nullptr;
	QLabel* m_NotesLabel = nullptr;
};
