#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QSqlQueryModel;
class QTableView;

class InventoryPage final : public QWidget
{
    Q_OBJECT

public:
    explicit InventoryPage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void loadFilters();
    void clearFilters();

private:
    QSqlDatabase m_database;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QLineEdit *m_batchEdit = nullptr;
    QTableView *m_table = nullptr;
    QSqlQueryModel *m_model = nullptr;
};

