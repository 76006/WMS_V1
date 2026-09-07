#pragma once

#include <QDialog>
#include <QSqlDatabase>

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
                            QWidget *parent = nullptr);

private slots:
    void loadLocations();
    void save();

private:
    void loadReferenceData();
    void loadMaterial();
    void showError(const QString &message);

    QSqlDatabase m_database;
    qlonglong m_materialId = 0;
    QLineEdit *m_codeEdit = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_specificationEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QLineEdit *m_brandEdit = nullptr;
    QLineEdit *m_unitEdit = nullptr;
    QDoubleSpinBox *m_minimumStockSpin = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QCheckBox *m_batchCheck = nullptr;
    QCheckBox *m_serialCheck = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLabel *m_errorLabel = nullptr;
    qlonglong m_pendingLocationId = 0;
};

