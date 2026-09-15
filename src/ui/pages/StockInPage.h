#pragma once

#include "core/Session.h"
#include "services/InventoryService.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class StockLineTable;

class StockInPage final : public QWidget
{
    Q_OBJECT

public:
    StockInPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();
    void refreshInspectionNotices();

signals:
    void stockChanged();

private slots:
    void submit();
    void updateInspectionRequirement();
    void openInspectionForm();

private:
    void resetSubmissionToken();
    void refreshRecentDocuments();

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_typeCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLabel *m_numberLabel = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QLineEdit *m_purposeEdit = nullptr;
    QLineEdit *m_supplierEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QComboBox *m_inspectionCombo = nullptr;
    QPushButton *m_inspectionButton = nullptr;
    QLabel *m_inspectionStatusLabel = nullptr;
    StockLineTable *m_lines = nullptr;
    QPushButton *m_submitButton = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QString m_submissionToken;
    qlonglong m_loadedInspectionNoticeId = 0;
};
