#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QCheckBox;
class QDateEdit;
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
    void loadLocations();
    void clearFilters();
    void showSelectedHistory();

private:
    QSqlDatabase m_database;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QLineEdit *m_batchEdit = nullptr;
    QCheckBox *m_dateFilterCheck = nullptr;
    QDateEdit *m_fromDate = nullptr;
    QDateEdit *m_toDate = nullptr;
    QTableView *m_table = nullptr;
    QSqlQueryModel *m_model = nullptr;
};
