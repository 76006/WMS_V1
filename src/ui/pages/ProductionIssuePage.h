#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
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
    void submit();

private:
    void refreshRecentDocuments();
    void resetSubmissionToken();

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_productCombo = nullptr;
    QLineEdit *m_batchEdit = nullptr;
    QDoubleSpinBox *m_plannedQuantity = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLabel *m_numberLabel = nullptr;
    StockLineTable *m_lines = nullptr;
    QPushButton *m_submitButton = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QString m_submissionToken;
};
