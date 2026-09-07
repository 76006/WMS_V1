#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QLabel;
class QFrame;
class QTableWidget;

class DashboardPage final : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private:
    QVariant scalar(const QString &sql) const;
    QFrame *createMetricCard(const QString &label, QLabel **valueLabel);

    QSqlDatabase m_database;
    QLabel *m_materialCount = nullptr;
    QLabel *m_totalQuantity = nullptr;
    QLabel *m_todayInbound = nullptr;
    QLabel *m_todayOutbound = nullptr;
    QLabel *m_pendingCounts = nullptr;
    QLabel *m_lowStock = nullptr;
    QTableWidget *m_recentTable = nullptr;
};
