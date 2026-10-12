#pragma once

#include "services/InventoryService.h"

#include <QDate>
#include <QHash>
#include <QPair>
#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDialog;
class QHBoxLayout;
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
    // 用已经保存的送检通知明细预填入库表格。仓库、库位仍按物料默认值加载，
    // 由入库人员最终确认；本接口不会提交库存，也不会绕过 lines() 的完整校验。
    bool setInboundMaterials(const QList<StockMovementRequest> &materials,
                             QString *errorMessage = nullptr);
    void addToolbarAction(QWidget *action);

public slots:
    void refreshReferenceData();
    void addLine();
    void clearLines();
    void setPurchaseMode(bool enabled);
    void setProductionUsageMode(bool enabled);
    void setIssueOptionsMode(bool enabled);
    // 选择送检单时使用送检明细中的物料—批号对应关系；关闭后恢复流水批号。
    void setInspectionBatchMode(bool enabled);
    // 适用于领料页面：删除最后一行后保持空表，由用户主动点击“添加物料”。
    void setKeepEmptyWhenNoRows(bool enabled);
    void setProductionQuantity(double quantity);
    void setMaterialCategoryFilter(const QString &categoryCode);
    void setDocumentDate(const QDate &date);

signals:
    void productionBomRequested();

private:
    int rowForWidget(const QWidget *widget, int column) const;
    QComboBox *comboAt(int row, int column) const;
    void loadMaterials(QComboBox *combo, const QVariant &selected = {});
    void loadWarehouses(int row);
    void loadLocations(int row);
    void loadBatches(int row);
    void assignAutomaticBatch(int row);
    QString nextAutomaticBatchNumber(int excludedRow) const;
    void setBatchText(QComboBox *batch, const QString &text, bool automatic);
    void toggleFullScreen();
    void updateAvailable(int row);
    void loadUnitUsage(int row);
    void updateProductionQuantity(int row);
    void chooseSerials(int row);
    void removeLine(int row);
    void refreshRowNumbers();

    QSqlDatabase m_database;
    Mode m_mode = Mode::Outbound;
    bool m_purchaseMode = false;
    bool m_productionUsageMode = false;
    bool m_issueOptionsMode = false;
    bool m_inspectionBatchMode = false;
    QHash<qlonglong, QString> m_inspectionBatchNumbers;
    bool m_keepEmptyWhenNoRows = false;
    bool m_forceKeepEmptyWhenNoRows = false;
    double m_productionQuantity = 1.0;
    QString m_materialCategoryFilter;
    QDate m_documentDate = QDate::currentDate();
    QTableWidget *m_table = nullptr;
    QHBoxLayout *m_toolbarLayout = nullptr;
    QPushButton *m_importProductionBomButton = nullptr;
    QPushButton *m_fullScreenButton = nullptr;
    QDialog *m_fullScreenDialog = nullptr;
};
