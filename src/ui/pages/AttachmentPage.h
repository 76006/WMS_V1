#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QTableWidget;

class AttachmentPage final : public QWidget
{
    Q_OBJECT
public:
    AttachmentPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void loadDocuments();
    void loadAttachments();
    void upload();
    void download();
    void openAttachment();
    void deleteOrRestore();
    void updateActions();

private:
    qlonglong selectedDocumentId() const;
    qlonglong selectedAttachmentId() const;

    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_keywordEdit = nullptr;
    QTableWidget *m_documentTable = nullptr;
    QTableWidget *m_attachmentTable = nullptr;
    QCheckBox *m_showDeleted = nullptr;
    QPushButton *m_uploadButton = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
};
