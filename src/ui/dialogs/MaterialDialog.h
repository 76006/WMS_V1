#pragma once

#include <QDialog>
#include <QSqlDatabase>
#include <QByteArray>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QTextEdit;

class MaterialDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit MaterialDialog(QSqlDatabase database,
                            qlonglong materialId = 0,
                            qlonglong operatorId = 0,
                            QWidget *parent = nullptr);

private slots:
    void loadLocations();
    void refreshGeneratedCode();
    void generateAvailableCode();
    void applyDefaultCategory();
    void chooseImage();
    void removeImage();
    void save();

private:
    void loadReferenceData();
    void loadMaterial();
    void updateImagePreview();
    void showError(const QString &message);

    QSqlDatabase m_database;
    qlonglong m_materialId = 0;
    qlonglong m_operatorId = 0;
    QComboBox *m_typeCombo = nullptr;
    QComboBox *m_projectCombo = nullptr;
    QComboBox *m_disciplineCombo = nullptr;
    QLineEdit *m_codeEdit = nullptr;
    QLabel *m_codeHint = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_specificationEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QLineEdit *m_brandEdit = nullptr;
    QLineEdit *m_unitEdit = nullptr;
    QDoubleSpinBox *m_unitUsageSpin = nullptr;
    QComboBox *m_processingMethodCombo = nullptr;
    QDoubleSpinBox *m_minimumStockSpin = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QCheckBox *m_batchCheck = nullptr;
    QCheckBox *m_serialCheck = nullptr;
    QComboBox *m_statusCombo = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLabel *m_imagePreview = nullptr;
    QByteArray m_imageData;
    QString m_imageFileName;
    QString m_imageMimeType;
    bool m_imageChanged = false;
    bool m_codeManuallyEdited = false;
    QLabel *m_errorLabel = nullptr;
    qlonglong m_pendingLocationId = 0;
};
