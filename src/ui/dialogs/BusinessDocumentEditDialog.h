#pragma once

#include "core/Session.h"
#include "services/InventoryService.h"

#include <QDialog>
#include <QSqlDatabase>

class QComboBox;
class QDateEdit;
class QLineEdit;
class QTableWidget;
class QTextEdit;

class BusinessDocumentEditDialog final : public QDialog
{
    Q_OBJECT

public:
    BusinessDocumentEditDialog(QSqlDatabase database,
                               Session session,
                               qlonglong documentId,
                               QWidget *parent = nullptr);

    bool saved() const { return m_saved; }

private slots:
    void addLine();
    void saveDocument();
    void toggleFullScreen();

private:
    bool loadDocument();
    void addLine(const PostedDocumentEditLine &line);
    PostedDocumentEdit editedDocument(QString *errorMessage) const;
    QComboBox *materialCombo(qlonglong selectedMaterialId) const;
    QComboBox *locationCombo(qlonglong selectedWarehouseId,
                             qlonglong selectedLocationId) const;
    qlonglong idText(const QLineEdit *edit) const;

    QSqlDatabase m_database;
    Session m_session;
    qlonglong m_documentId = 0;
    bool m_saved = false;
    bool m_fullScreen = false;
    PostedDocumentEdit m_original;
    QLineEdit *m_numberEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QComboBox *m_directionCombo = nullptr;
    QComboBox *m_statusCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QDateEdit *m_deliveryDateEdit = nullptr;
    QLineEdit *m_sourceDocumentEdit = nullptr;
    QLineEdit *m_productionRunEdit = nullptr;
    QLineEdit *m_inspectionNoticeEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QLineEdit *m_purposeEdit = nullptr;
    QLineEdit *m_supplierEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLineEdit *m_customerCompanyEdit = nullptr;
    QLineEdit *m_destinationEdit = nullptr;
    QLineEdit *m_contactEdit = nullptr;
    QLineEdit *m_phoneEdit = nullptr;
    QLineEdit *m_orderEdit = nullptr;
    QLineEdit *m_logisticsEdit = nullptr;
    QLineEdit *m_trackingEdit = nullptr;
    QTableWidget *m_lines = nullptr;
};
