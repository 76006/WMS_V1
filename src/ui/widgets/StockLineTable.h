#pragma once

#include "services/InventoryService.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
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

public slots:
    void refreshReferenceData();
    void addLine();
    void clearLines();

private:
    int rowForWidget(const QWidget *widget, int column) const;
    QComboBox *comboAt(int row, int column) const;
    void loadMaterials(QComboBox *combo, const QVariant &selected = {});
    void loadWarehouses(int row);
    void loadLocations(int row);
    void loadBatches(int row);
    void updateAvailable(int row);
    void chooseSerials(int row);
    void removeLine(int row);

    QSqlDatabase m_database;
    Mode m_mode = Mode::Outbound;
    QTableWidget *m_table = nullptr;
};
