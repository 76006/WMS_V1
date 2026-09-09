#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QSqlQueryModel;
class QTableView;

class ShipmentQueryPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ShipmentQueryPage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void resetFilters();
    void loadSelectedShipmentDetails();

private:
    void loadFilterOptions();
    void loadShipments();
    void loadShipmentDetails(qlonglong documentId);
    QString customerFilter() const;
    QString projectFilter() const;

    QSqlDatabase m_database;
    QLineEdit *m_orderEdit = nullptr;
    QComboBox *m_customerCombo = nullptr;
    QComboBox *m_projectCombo = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_detailTitle = nullptr;
    QTableView *m_shipmentTable = nullptr;
    QTableView *m_detailTable = nullptr;
    QSqlQueryModel *m_shipmentModel = nullptr;
    QSqlQueryModel *m_detailModel = nullptr;
};
