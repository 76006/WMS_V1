#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLineEdit;
class QPushButton;
class QSqlQueryModel;
class QTableView;

class LedgerPage final : public QWidget
{
    Q_OBJECT

public:
    LedgerPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refresh();

signals:
    void stockChanged();

private slots:
    void clearFilters();
    void reverseSelectedItem();
    void updateActionState();

private:
    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QDateEdit *m_fromDate = nullptr;
    QDateEdit *m_toDate = nullptr;
    QTableView *m_table = nullptr;
    QSqlQueryModel *m_model = nullptr;
    QPushButton *m_reverseButton = nullptr;
};

