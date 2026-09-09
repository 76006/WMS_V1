#pragma once

#include "core/Session.h"

#include <QList>
#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSqlQueryModel;
class QTabWidget;
class QTableView;
class QTreeWidget;

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
    void deleteMaterials();
    void batchEditMaterials();
    void importMaterials();
    void importBom();
    void exportMaterials();
    void manageProjects();
    void loadCategories();
    void refreshBomProducts();
    void refreshBomTree();
    void addBomChild();
    void editBomQuantity();
    void removeBomItem();

private:
    qlonglong selectedMaterialId() const;
    QList<qlonglong> selectedMaterialIds() const;

    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QComboBox *m_statusCombo = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_batchEditButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_projectButton = nullptr;
    QTableView *m_table = nullptr;
    QSqlQueryModel *m_model = nullptr;
    QLabel *m_countLabel = nullptr;
    QTabWidget *m_viewTabs = nullptr;
    QComboBox *m_bomProductCombo = nullptr;
    QTreeWidget *m_bomTree = nullptr;
    QPushButton *m_importBomButton = nullptr;
    QPushButton *m_addBomChildButton = nullptr;
    QPushButton *m_editBomQuantityButton = nullptr;
    QPushButton *m_removeBomItemButton = nullptr;
};
