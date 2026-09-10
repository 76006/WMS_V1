#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

struct OfficeTemplateDocument;
struct ProductionReturnRequest;

class ProductionReturnPage final : public QWidget
{
    Q_OBJECT

public:
    ProductionReturnPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void loadDocuments();
    void loadSourceLines();
    void submit();

private:
    int rowForWidget(const QWidget *widget, int column) const;
    void loadReturnLocations(int row);
    void chooseSerials(int row);
    void refreshRecentDocuments();
    void resetSubmissionToken();
    bool buildReturnForm(const ProductionReturnRequest &request,
                         OfficeTemplateDocument *form,
                         QString *errorMessage) const;

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_runCombo = nullptr;
    QComboBox *m_documentCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QTableWidget *m_linesTable = nullptr;
    QTableWidget *m_recentTable = nullptr;
    QPushButton *m_submitButton = nullptr;
    QString m_submissionToken;
};
