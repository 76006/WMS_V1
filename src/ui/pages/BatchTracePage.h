#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QTableWidget;

class BatchTracePage final : public QWidget
{
    Q_OBJECT
public:
    explicit BatchTracePage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void loadBatches();
    void loadHistory();

private:
    QSqlDatabase m_database;
    QLineEdit *m_keywordEdit = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QTableWidget *m_batchTable = nullptr;
    QTableWidget *m_historyTable = nullptr;
};
