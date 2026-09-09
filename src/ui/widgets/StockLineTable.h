#pragma once

#include "services/InventoryService.h"

#include <QPair>
#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QPushButton;
class QTableWidget;

class StockLineTable final : public QWidget
{
    Q_OBJECT

public:
    enum class Mode { Inbound, Outbound };

    explicit StockLineTable(QSqlDatabase database,
                            Mode mode = Mode::Outbound,
                            QWidget *parent = nullptr);

    QList<StockMovementRequest> lines(QString *errorMessage = nullptr) const;
    QStringList purchaseWarnings() const;
    bool setProductionMaterials(const QList<QPair<qlonglong, double>> &materials,
                                QString *errorMessage = nullptr);

public slots:
    void refreshReferenceData();
    void addLine();
    void clearLines();
    void setPurchaseMode(bool enabled);
    void setProductionUsageMode(bool enabled);
    void setProductionQuantity(double quantity);
    void setMaterialCategoryFilter(const QString &categoryCode);

signals:
    void productionBomRequested();

private:
    int rowForWidget(const QWidget *widget, int column) const;
    QComboBox *comboAt(int row, int column) const;
    void loadMaterials(QComboBox *combo, const QVariant &selected = {});
    void loadWarehouses(int row);
    void loadLocations(int row);
    void loadBatches(int row);
    void updateAvailable(int row);
    void loadUnitUsage(int row);
    void updateProductionQuantity(int row);
    void chooseSerials(int row);
    void removeLine(int row);

    QSqlDatabase m_database;
    Mode m_mode = Mode::Outbound;
    bool m_purchaseMode = false;
    bool m_productionUsageMode = false;
    bool m_keepEmptyWhenNoRows = false;
    bool m_requireExplicitMaterialSelection = false;
    double m_productionQuantity = 1.0;
    QString m_materialCategoryFilter;
    QTableWidget *m_table = nullptr;
    QPushButton *m_importProductionBomButton = nullptr;
};
