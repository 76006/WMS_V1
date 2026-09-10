#pragma once

#include "services/InventoryService.h"

#include <QDialog>
#include <QList>
#include <QSqlDatabase>

#include "services/OfficeTemplateService.h"

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QTableWidget;
class QTextEdit;

class InspectionDialog final : public QDialog
{
    Q_OBJECT

public:
    InspectionDialog(QSqlDatabase database,
                     QList<StockMovementRequest> lines,
                     InboundInspectionRequest inspection,
                     QWidget *parent = nullptr);

    InboundInspectionRequest inspection() const;
    // “保存并返回入库单”时立即归档的送检单正本路径；未保存成功时为空。
    QString archivedPath() const { return m_archivedPath; }

private slots:
    void chooseAttachment();
    void printInspectionForm();
    void acceptInspection();

private:
    void populateLines();
    void populateSupplierCombo();
    QString resultText() const;
    QString supplierText() const;
    OfficeTemplateDocument templateDocument() const;

    QSqlDatabase m_database;
    QList<StockMovementRequest> m_lines;
    InboundInspectionRequest m_inspection;
    QLineEdit *m_numberEdit = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QDateEdit *m_arrivalDateEdit = nullptr;
    QLineEdit *m_inspectorEdit = nullptr;
    QLineEdit *m_entrustedEdit = nullptr;
    QLineEdit *m_notificationDepartmentEdit = nullptr;
    QLineEdit *m_purchaseOrderEdit = nullptr;
    QComboBox *m_supplierCombo = nullptr;
    QComboBox *m_resultCombo = nullptr;
    QTextEdit *m_conclusionEdit = nullptr;
    QLabel *m_attachmentLabel = nullptr;
    QTableWidget *m_lineTable = nullptr;
    QString m_archivedPath;
};
