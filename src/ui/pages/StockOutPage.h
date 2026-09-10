#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class StockLineTable;

class StockOutPage final : public QWidget
{
    Q_OBJECT

public:
    StockOutPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void submit();
    void updateSalesFieldsVisibility();
    void showSalesDetails(int row, int column);

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
    QGroupBox *m_salesDetailsGroup = nullptr;
    QLineEdit *m_customerCompanyEdit = nullptr;
    QLineEdit *m_destinationEdit = nullptr;
    QLineEdit *m_customerContactEdit = nullptr;
    QLineEdit *m_customerPhoneEdit = nullptr;
    QLineEdit *m_salesOrderEdit = nullptr;
    QLineEdit *m_logisticsCompanyEdit = nullptr;
    QLineEdit *m_trackingNumberEdit = nullptr;
    QDateEdit *m_deliveryDateEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    StockLineTable *m_lines = nullptr;
    QPushButton *m_submitButton = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QString m_submissionToken;
};
