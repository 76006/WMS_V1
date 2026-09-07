#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QPushButton;
class QSqlQueryModel;
class QTableView;

class MaterialPage final : public QWidget
{
    Q_OBJECT

public:
    MaterialPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refresh();

signals:
    void dataChanged();

private slots:
    void addMaterial();
    void editMaterial();
    void loadCategories();

private:
    qlonglong selectedMaterialId() const;

    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QTableView *m_table = nullptr;
    QSqlQueryModel *m_model = nullptr;
};

