#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QDateEdit;
class QLineEdit;
class QTableWidget;

class AuditLogPage final : public QWidget
{
    Q_OBJECT
public:
    explicit AuditLogPage(QSqlDatabase database, QWidget *parent = nullptr, bool loadInitially = true);

public slots:
    void refresh();

private slots:
    void exportLogs();

private:
    QSqlDatabase m_database;
    QDateEdit *m_fromDate = nullptr;
    QDateEdit *m_toDate = nullptr;
    QLineEdit *m_keywordEdit = nullptr;
    QTableWidget *m_table = nullptr;
};
