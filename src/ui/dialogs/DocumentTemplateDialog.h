#pragma once

#include "services/OfficeTemplateService.h"

#include <QDialog>
#include <QMap>

class QLabel;
class QLineEdit;
class QTableWidget;

class DocumentTemplateDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit DocumentTemplateDialog(OfficeTemplateDocument document,
                                    QWidget *parent = nullptr);

    OfficeTemplateDocument document() const;

private slots:
    void previewTemplate();
    void acceptForm();

private:
    void addEditableFields();
    void populateLines();

    OfficeTemplateDocument m_document;
    QMap<QString, QLineEdit *> m_fieldEdits;
    QTableWidget *m_lineTable = nullptr;
};
