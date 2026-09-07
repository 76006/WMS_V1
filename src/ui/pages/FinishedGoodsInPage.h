#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

class FinishedGoodsInPage final : public QWidget
{
    Q_OBJECT

public:
    FinishedGoodsInPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void loadRunDetails();
    void loadLocations();
    void generateSerialNumbers();
    void submit();

private:
    QStringList enteredSerialNumbers() const;
    void refreshRecentDocuments();
    void resetSubmissionToken();

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_runCombo = nullptr;
    QLabel *m_productLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QDoubleSpinBox *m_quantitySpin = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_serialEdit = nullptr;
    QPushButton *m_generateButton = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QPushButton *m_submitButton = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QString m_submissionToken;
    qlonglong m_productMaterialId = 0;
    double m_plannedQuantity = 0.0;
    double m_receivedQuantity = 0.0;
    bool m_requireSerial = false;
    QString m_productCode;
};
