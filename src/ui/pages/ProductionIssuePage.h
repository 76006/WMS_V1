#pragma once

#include "core/Session.h"
#include "services/ProductionTask.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class StockLineTable;

class ProductionIssuePage final : public QWidget
{
    Q_OBJECT

public:
    ProductionIssuePage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void importProductBom();
    void importProcessBom();
    void submit();
    void updateIssueType();

private:
    void refreshRecentDocuments();
    void resetSubmissionToken();
    void refreshProductionStages();
    void refreshStageProgress();
    void selectProductionTask();
    void importStageBom(bool useRemaining);
    double taskPlanQuantity() const;

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_issueTypeCombo = nullptr;
    QComboBox *m_productCombo = nullptr;
    QLineEdit *m_productModelEdit = nullptr;
    QLineEdit *m_batchEdit = nullptr;
    QComboBox *m_taskCombo = nullptr;
    QComboBox *m_stageCombo = nullptr;
    QTableWidget *m_demandTable = nullptr;
    QLabel *m_stageSummary = nullptr;
    ProductionTask m_task;
    qlonglong m_taskRunId = 0;
    double m_savedPlanQuantity = 0;
    QString m_bomFingerprint;
    QDoubleSpinBox *m_issueQuantity = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLabel *m_numberLabel = nullptr;
    QLabel *m_usageHint = nullptr;
    QLabel *m_heading = nullptr;
    QGroupBox *m_productionFieldsGroup = nullptr;
    StockLineTable *m_lines = nullptr;
    QPushButton *m_submitButton = nullptr;
    QPushButton *m_processBomButton = nullptr;
    QPushButton *m_remainingButton = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QString m_submissionToken;
};
