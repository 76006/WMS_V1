#pragma once

#include "services/InventoryReportService.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class InventoryReportPage final : public QWidget
{
    Q_OBJECT

public:
    explicit InventoryReportPage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void updatePeriodMode();
    void resetPeriod();
    void exportMonthly();

private:
    void showAnnual(const QList<AnnualInventoryReportRow> &rows);
    void showMonthly(const QList<MonthlyMaterialReportRow> &summaryRows,
                     const QList<InventoryMovementReportRow> &detailRows);

    QSqlDatabase m_database;
    QComboBox *m_periodMode = nullptr;
    QSpinBox *m_yearSpin = nullptr;
    QComboBox *m_monthCombo = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QPushButton *m_exportButton = nullptr;
    QLabel *m_inboundTotal = nullptr;
    QLabel *m_outboundTotal = nullptr;
    QLabel *m_giftTotal = nullptr;
    QLabel *m_billableTotal = nullptr;
    QLabel *m_scopeLabel = nullptr;
    QStackedWidget *m_views = nullptr;
    QTableWidget *m_annualTable = nullptr;
    QTableWidget *m_monthlySummaryTable = nullptr;
    QTableWidget *m_monthlyDetailTable = nullptr;
    QList<MonthlyMaterialReportRow> m_summaryRows;
    QList<InventoryMovementReportRow> m_detailRows;
};
