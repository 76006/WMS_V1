#include "ui/dialogs/MaterialDialog.h"

#include "services/MaterialCodeService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPixmap>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int CategoryCodeRole = Qt::UserRole + 1;
}

MaterialDialog::MaterialDialog(QSqlDatabase database,
                               qlonglong materialId,
                               qlonglong operatorId,
                               QWidget *parent)
    : QDialog(parent), m_database(std::move(database)), m_materialId(materialId),
      m_operatorId(operatorId)
{
    setWindowTitle(materialId > 0 ? QStringLiteral("编辑物料") : QStringLiteral("新增物料"));
    setMinimumWidth(560);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setSpacing(10);

    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(QStringLiteral("M - 原材料"), QStringLiteral("M"));
    m_typeCombo->addItem(QStringLiteral("P - 成品"), QStringLiteral("P"));
    m_typeCombo->addItem(QStringLiteral("O - 耗材/包装/附件"), QStringLiteral("O"));
    m_projectCombo = new QComboBox(this);
    m_disciplineCombo = new QComboBox(this);
    m_disciplineCombo->addItem(QStringLiteral("1 - 结构件"), QStringLiteral("1"));
    m_disciplineCombo->addItem(QStringLiteral("2 - 电子件"), QStringLiteral("2"));
    m_disciplineCombo->addItem(QStringLiteral("9 - 其他"), QStringLiteral("9"));
    m_codeEdit = new QLineEdit(this);
    m_codeEdit->setMaxLength(64);
    m_codeEdit->setReadOnly(true);
    m_codeHint = new QLabel(QStringLiteral("编码保存后不可修改"), this);
    m_codeHint->setObjectName(QStringLiteral("mutedText"));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(128);
    m_specificationEdit = new QLineEdit(this);
    m_categoryCombo = new QComboBox(this);
    m_brandEdit = new QLineEdit(this);
    m_unitEdit = new QLineEdit(this);
    m_unitEdit->setMaxLength(20);
    m_minimumStockSpin = new QDoubleSpinBox(this);
    m_minimumStockSpin->setDecimals(6);
    m_minimumStockSpin->setRange(0, 999999999999.0);
    m_warehouseCombo = new QComboBox(this);
    m_locationCombo = new QComboBox(this);
    m_batchCheck = new QCheckBox(QStringLiteral("启用批次管理"), this);
    m_serialCheck = new QCheckBox(QStringLiteral("启用SN序列号管理"), this);
    m_notesEdit = new QTextEdit(this);
    m_notesEdit->setMaximumHeight(85);
    m_imagePreview = new QLabel(QStringLiteral("暂无图片"), this);
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imagePreview->setMinimumSize(180, 120);
    m_imagePreview->setMaximumSize(260, 180);
    m_imagePreview->setFrameShape(QFrame::StyledPanel);
    auto *imageRow = new QHBoxLayout;
    auto *chooseImageButton = new QPushButton(QStringLiteral("选择图片"), this);
    auto *removeImageButton = new QPushButton(QStringLiteral("移除图片"), this);
    auto *imageButtons = new QVBoxLayout;
    imageButtons->addWidget(chooseImageButton);
    imageButtons->addWidget(removeImageButton);
    imageButtons->addStretch();
    imageRow->addWidget(m_imagePreview);
    imageRow->addLayout(imageButtons);
    imageRow->addStretch();

    auto *trackingLayout = new QHBoxLayout;
    trackingLayout->addWidget(m_batchCheck);
    trackingLayout->addWidget(m_serialCheck);
    trackingLayout->addStretch();

    form->addRow(QStringLiteral("物料类型 *"), m_typeCombo);
    form->addRow(QStringLiteral("项目代码 *"), m_projectCombo);
    form->addRow(QStringLiteral("专业类别 *"), m_disciplineCombo);
    form->addRow(QStringLiteral("物料编码"), m_codeEdit);
    form->addRow(QString(), m_codeHint);
    form->addRow(QStringLiteral("物料名称 *"), m_nameEdit);
    form->addRow(QStringLiteral("规格型号"), m_specificationEdit);
    form->addRow(QStringLiteral("物料分类 *"), m_categoryCombo);
    form->addRow(QStringLiteral("品牌"), m_brandEdit);
    form->addRow(QStringLiteral("单位 *"), m_unitEdit);
    form->addRow(QStringLiteral("最低库存"), m_minimumStockSpin);
    form->addRow(QStringLiteral("默认仓库"), m_warehouseCombo);
    form->addRow(QStringLiteral("默认库位"), m_locationCombo);
    form->addRow(QStringLiteral("追溯方式"), trackingLayout);
    form->addRow(QStringLiteral("物料图片"), imageRow);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    root->addLayout(form);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color:#b91c1c;"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    root->addWidget(m_errorLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MaterialDialog::loadLocations);
    connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        applyDefaultCategory();
        refreshGeneratedCode();
    });
    connect(m_projectCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MaterialDialog::refreshGeneratedCode);
    connect(m_disciplineCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MaterialDialog::refreshGeneratedCode);
    connect(chooseImageButton, &QPushButton::clicked, this, &MaterialDialog::chooseImage);
    connect(removeImageButton, &QPushButton::clicked, this, &MaterialDialog::removeImage);
    connect(buttons, &QDialogButtonBox::accepted, this, &MaterialDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadReferenceData();
    if (m_materialId > 0) {
        loadMaterial();
        m_typeCombo->setEnabled(false);
        m_projectCombo->setEnabled(false);
        m_disciplineCombo->setEnabled(false);
    } else {
        applyDefaultCategory();
        refreshGeneratedCode();
    }
}

void MaterialDialog::loadReferenceData()
{
    m_categoryCombo->clear();
    m_categoryCombo->addItem(QStringLiteral("请选择"), QVariant());
    QSqlQuery categories(m_database);
    categories.exec(QStringLiteral(
        "SELECT id, code, name FROM material_categories WHERE is_active=1 ORDER BY sort_order, name"));
    while (categories.next()) {
        const int index = m_categoryCombo->count();
        m_categoryCombo->addItem(categories.value(2).toString(), categories.value(0));
        m_categoryCombo->setItemData(index, categories.value(1), CategoryCodeRole);
    }

    m_projectCombo->blockSignals(true);
    m_projectCombo->clear();
    QSqlQuery projects(m_database);
    projects.exec(m_materialId > 0
        ? QStringLiteral("SELECT code,name FROM material_projects ORDER BY code")
        : QStringLiteral("SELECT code,name FROM material_projects WHERE is_active=1 ORDER BY code"));
    while (projects.next()) {
        m_projectCombo->addItem(QStringLiteral("%1 - %2")
                                    .arg(projects.value(0).toString(), projects.value(1).toString()),
                                projects.value(0));
    }
    const int defaultProject = m_projectCombo->findData(QStringLiteral("SM01"));
    if (defaultProject >= 0) m_projectCombo->setCurrentIndex(defaultProject);
    m_projectCombo->blockSignals(false);

    m_warehouseCombo->clear();
    m_warehouseCombo->addItem(QStringLiteral("未设置"), QVariant());
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id, code, name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(
            QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(), warehouses.value(2).toString()),
            warehouses.value(0));
    }
    loadLocations();
}

void MaterialDialog::applyDefaultCategory()
{
    if (m_materialId > 0) return;
    const QString type = m_typeCombo->currentData().toString();
    const QString categoryCode = type == QStringLiteral("P") ? QStringLiteral("FINISHED")
        : type == QStringLiteral("O") ? QStringLiteral("CONSUMABLE")
                                      : QStringLiteral("RAW");
    const int index = m_categoryCombo->findData(categoryCode, CategoryCodeRole);
    if (index >= 0) m_categoryCombo->setCurrentIndex(index);
}

void MaterialDialog::refreshGeneratedCode()
{
    if (m_materialId > 0) return;
    QString error;
    const QString code = MaterialCodeService::nextCode(
        m_database, m_typeCombo->currentData().toString(),
        m_projectCombo->currentData().toString(),
        m_disciplineCombo->currentData().toString(), &error);
    m_codeEdit->setText(code);
    if (code.isEmpty()) {
        m_codeHint->setText(error);
        m_codeHint->setStyleSheet(QStringLiteral("color:#b91c1c;"));
    } else {
        m_codeHint->setText(QStringLiteral("自动生成；保存时再次校验，保存后不可修改"));
        m_codeHint->setStyleSheet(QString());
    }
}

void MaterialDialog::loadLocations()
{
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    m_locationCombo->clear();
    m_locationCombo->addItem(QStringLiteral("未设置"), QVariant());
    if (warehouseId <= 0) {
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, code, name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(warehouseId);
    query.exec();
    while (query.next()) {
        QString label = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) {
            label += QStringLiteral(" - ") + query.value(2).toString();
        }
        m_locationCombo->addItem(label, query.value(0));
    }
    if (m_pendingLocationId > 0) {
        const int index = m_locationCombo->findData(m_pendingLocationId);
        if (index >= 0) {
            m_locationCombo->setCurrentIndex(index);
        }
        m_pendingLocationId = 0;
    }
}

void MaterialDialog::loadMaterial()
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT code, name, specification, category_id, brand, unit, minimum_stock, "
        "default_warehouse_id, default_location_id, require_batch, require_serial, notes "
        "FROM materials WHERE id=?"));
    query.addBindValue(m_materialId);
    if (!query.exec() || !query.next()) {
        showError(QStringLiteral("无法读取物料资料：%1").arg(query.lastError().text()));
        return;
    }
    m_codeEdit->setText(query.value(0).toString());
    const QString materialCode = query.value(0).toString().toUpper();
    const int typeIndex = m_typeCombo->findData(materialCode.left(1));
    if (typeIndex >= 0) m_typeCombo->setCurrentIndex(typeIndex);
    for (int index = 0; index < m_projectCombo->count(); ++index) {
        const QString project = m_projectCombo->itemData(index).toString();
        const QString suffix = materialCode.mid(1 + project.size());
        if (materialCode.mid(1).startsWith(project) && suffix.size() == 4) {
            m_projectCombo->setCurrentIndex(index);
            const int disciplineIndex = m_disciplineCombo->findData(suffix.left(1));
            if (disciplineIndex >= 0) m_disciplineCombo->setCurrentIndex(disciplineIndex);
            break;
        }
    }
    m_nameEdit->setText(query.value(1).toString());
    m_specificationEdit->setText(query.value(2).toString());
    m_categoryCombo->setCurrentIndex(m_categoryCombo->findData(query.value(3)));
    m_brandEdit->setText(query.value(4).toString());
    m_unitEdit->setText(query.value(5).toString());
    m_minimumStockSpin->setValue(query.value(6).toDouble());
    m_pendingLocationId = query.value(8).toLongLong();
    const int warehouseIndex = m_warehouseCombo->findData(query.value(7));
    m_warehouseCombo->setCurrentIndex(warehouseIndex >= 0 ? warehouseIndex : 0);
    loadLocations();
    m_batchCheck->setChecked(query.value(9).toBool());
    m_serialCheck->setChecked(query.value(10).toBool());
    m_notesEdit->setPlainText(query.value(11).toString());

    QSqlQuery image(m_database);
    image.prepare(QStringLiteral(
        "SELECT original_file_name,mime_type,image_data FROM material_images WHERE material_id=?"));
    image.addBindValue(m_materialId);
    if (image.exec() && image.next()) {
        m_imageFileName = image.value(0).toString();
        m_imageMimeType = image.value(1).toString();
        m_imageData = image.value(2).toByteArray();
        updateImagePreview();
    }
}

void MaterialDialog::chooseImage()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择物料图片"), {},
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.size() > 5 * 1024 * 1024) {
        showError(QStringLiteral("物料图片不能超过 5 MB。"));
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        showError(QStringLiteral("读取图片失败：%1").arg(file.errorString()));
        return;
    }
    const QByteArray data = file.readAll();
    QPixmap pixmap;
    if (!pixmap.loadFromData(data)) {
        showError(QStringLiteral("所选文件不是受支持的图片。"));
        return;
    }
    m_imageData = data;
    m_imageFileName = QFileInfo(path).fileName();
    m_imageMimeType = QMimeDatabase().mimeTypeForFile(path).name();
    m_imageChanged = true;
    m_errorLabel->hide();
    updateImagePreview();
}

void MaterialDialog::removeImage()
{
    m_imageData.clear();
    m_imageFileName.clear();
    m_imageMimeType.clear();
    m_imageChanged = true;
    updateImagePreview();
}

void MaterialDialog::updateImagePreview()
{
    if (m_imageData.isEmpty()) {
        m_imagePreview->setPixmap(QPixmap());
        m_imagePreview->setText(QStringLiteral("暂无图片"));
        m_imagePreview->setToolTip({});
        return;
    }
    QPixmap pixmap;
    pixmap.loadFromData(m_imageData);
    m_imagePreview->setText({});
    m_imagePreview->setPixmap(pixmap.scaled(m_imagePreview->size(), Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation));
    m_imagePreview->setToolTip(m_imageFileName);
}

void MaterialDialog::save()
{
    QString code = m_codeEdit->text().trimmed().toUpper();
    const QString name = m_nameEdit->text().trimmed();
    const QString unit = m_unitEdit->text().trimmed();
    if (name.isEmpty() || unit.isEmpty() || m_categoryCombo->currentData().isNull()) {
        showError(QStringLiteral("请完整填写物料名称、分类和单位。"));
        return;
    }
    if (m_materialId <= 0 && (m_typeCombo->currentIndex() < 0
        || m_projectCombo->currentIndex() < 0 || m_disciplineCombo->currentIndex() < 0)) {
        showError(QStringLiteral("请选择物料类型、项目代码和专业类别。"));
        return;
    }
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    const qlonglong locationId = m_locationCombo->currentData().toLongLong();
    if ((warehouseId <= 0) != (locationId <= 0)) {
        showError(QStringLiteral("默认仓库和默认库位必须同时设置，或同时留空。"));
        return;
    }

    QSqlQuery begin(m_database);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        showError(QStringLiteral("无法开始保存事务：%1").arg(begin.lastError().text()));
        return;
    }
    if (m_materialId <= 0) {
        QString error;
        code = MaterialCodeService::nextCode(
            m_database, m_typeCombo->currentData().toString(),
            m_projectCombo->currentData().toString(),
            m_disciplineCombo->currentData().toString(), &error);
        if (code.isEmpty()) {
            m_database.rollback();
            showError(error);
            return;
        }
        m_codeEdit->setText(code);
    }
    QSqlQuery query(m_database);
    if (m_materialId > 0) {
        query.prepare(QStringLiteral(
            "UPDATE materials SET code=?, name=?, specification=?, category_id=?, brand=?, unit=?, "
            "minimum_stock=?, default_warehouse_id=?, default_location_id=?, require_batch=?, "
            "require_serial=?, notes=?, updated_at=? WHERE id=?"));
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO materials(code, name, specification, category_id, brand, unit, minimum_stock, "
            "default_warehouse_id, default_location_id, require_batch, require_serial, notes) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    }
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(m_specificationEdit->text().trimmed());
    query.addBindValue(m_categoryCombo->currentData());
    query.addBindValue(m_brandEdit->text().trimmed());
    query.addBindValue(unit);
    query.addBindValue(m_minimumStockSpin->value());
    query.addBindValue(warehouseId > 0 ? QVariant(warehouseId) : QVariant());
    query.addBindValue(locationId > 0 ? QVariant(locationId) : QVariant());
    query.addBindValue(m_batchCheck->isChecked());
    query.addBindValue(m_serialCheck->isChecked());
    query.addBindValue(m_notesEdit->toPlainText().trimmed());
    if (m_materialId > 0) {
        query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        query.addBindValue(m_materialId);
    }
    if (!query.exec()) {
        m_database.rollback();
        showError(QStringLiteral("保存失败。请检查物料编码是否重复。\n%1").arg(query.lastError().text()));
        return;
    }
    if (m_materialId <= 0) m_materialId = query.lastInsertId().toLongLong();

    if (m_imageChanged) {
        QSqlQuery image(m_database);
        if (m_imageData.isEmpty()) {
            image.prepare(QStringLiteral("DELETE FROM material_images WHERE material_id=?"));
            image.addBindValue(m_materialId);
        } else {
            image.prepare(QStringLiteral(
                "INSERT INTO material_images(material_id,original_file_name,mime_type,sha256,image_data,updated_by,updated_at) "
                "VALUES(?,?,?,?,?,?,?) ON CONFLICT(material_id) DO UPDATE SET "
                "original_file_name=excluded.original_file_name,mime_type=excluded.mime_type,"
                "sha256=excluded.sha256,image_data=excluded.image_data,updated_by=excluded.updated_by,"
                "updated_at=excluded.updated_at"));
            image.addBindValue(m_materialId);
            image.addBindValue(m_imageFileName);
            image.addBindValue(m_imageMimeType);
            image.addBindValue(QString::fromLatin1(QCryptographicHash::hash(
                m_imageData, QCryptographicHash::Sha256).toHex()));
            image.addBindValue(m_imageData);
            image.addBindValue(m_operatorId > 0 ? QVariant(m_operatorId) : QVariant());
            image.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        }
        if (!image.exec()) {
            m_database.rollback();
            showError(QStringLiteral("保存物料图片失败：%1").arg(image.lastError().text()));
            return;
        }
    }
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)"));
    audit.addBindValue(m_operatorId > 0 ? QVariant(m_operatorId) : QVariant());
    audit.addBindValue(QStringLiteral("MATERIAL_SAVE"));
    audit.addBindValue(QStringLiteral("material"));
    audit.addBindValue(m_materialId);
    audit.addBindValue(code + QStringLiteral(" - ") + name);
    if (!audit.exec() || !m_database.commit()) {
        m_database.rollback();
        showError(QStringLiteral("提交保存失败：%1").arg(m_database.lastError().text()));
        return;
    }
    accept();
}

void MaterialDialog::showError(const QString &message)
{
    m_errorLabel->setText(message);
    m_errorLabel->show();
}
