#pragma once

#include "core/Session.h"

#include <QDialog>
#include <QSqlDatabase>

class QLabel;
class QLineEdit;
class QPushButton;

class LoginDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QSqlDatabase database,
                         const QString &databaseFilePath,
                         QWidget *parent = nullptr);

    Session session() const;

private slots:
    void authenticate();

private:
    QSqlDatabase m_database;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLabel *m_errorLabel = nullptr;
    QPushButton *m_loginButton = nullptr;
    Session m_session;
};

