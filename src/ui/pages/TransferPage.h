#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class StockLineTable;

class TransferPage final : public QWidget
{
    Q_OBJECT
public:
    TransferPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void loadTargetLocations();
    void refreshTransfers();
    void updateReversalState();
    void submit();
    void reverseSelectedTransfer();

private:
    QSqlDatabase m_database;
    Session m_session;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    StockLineTable *m_sourceLine = nullptr;
    QComboBox *m_targetWarehouse = nullptr;
    QComboBox *m_targetLocation = nullptr;
    QPushButton *m_submitButton = nullptr;
    QTableWidget *m_transferTable = nullptr;
    QPushButton *m_reverseButton = nullptr;
};
