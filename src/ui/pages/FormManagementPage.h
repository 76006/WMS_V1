#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class FormManagementPage final : public QWidget
{
    Q_OBJECT

public:
    FormManagementPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void resetFilters();
    void openSelectedForm();
    void downloadSelectedForm();
    void retrySelectedForm();
    void showFullScreen();
    void updateActions();

private:
    qlonglong selectedAttachmentId() const;
    qlonglong selectedDocumentId() const;
    QString selectedBusinessStatus() const;
    bool saveAttachmentToTemporaryFile(qlonglong attachmentId, QString *path,
                                       QString *errorMessage) const;

    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_keywordEdit = nullptr;
    QComboBox *m_statusCombo = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_retryButton = nullptr;
};
