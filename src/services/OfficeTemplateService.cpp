#include "services/OfficeTemplateService.h"

#include "services/AttachmentService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <utility>

namespace {
void setError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString formCode(OfficeFormKind kind)
{
    switch (kind) {
    case OfficeFormKind::Inspection: return QStringLiteral("inspection");
    case OfficeFormKind::RawMaterialInbound: return QStringLiteral("rawInbound");
    case OfficeFormKind::ProductionIssue: return QStringLiteral("productionIssue");
    case OfficeFormKind::FinishedGoodsInbound: return QStringLiteral("finishedInbound");
    case OfficeFormKind::StockOutbound: return QStringLiteral("stockOutbound");
    case OfficeFormKind::DeliveryConfirmation: return QStringLiteral("deliveryConfirmation");
    }
    return {};
}

bool formKindFromCode(const QString &code, OfficeFormKind *kind)
{
    static const QMap<QString, OfficeFormKind> kinds = {
        {QStringLiteral("inspection"), OfficeFormKind::Inspection},
        {QStringLiteral("rawInbound"), OfficeFormKind::RawMaterialInbound},
        {QStringLiteral("productionIssue"), OfficeFormKind::ProductionIssue},
        {QStringLiteral("finishedInbound"), OfficeFormKind::FinishedGoodsInbound},
        {QStringLiteral("stockOutbound"), OfficeFormKind::StockOutbound},
        {QStringLiteral("deliveryConfirmation"), OfficeFormKind::DeliveryConfirmation}
    };
    const auto it = kinds.constFind(code);
    if (it == kinds.cend()) return false;
    if (kind) *kind = it.value();
    return true;
}

QString sheetName(OfficeFormKind kind)
{
    switch (kind) {
    case OfficeFormKind::ProductionIssue:
        return QStringLiteral("领料单");
    case OfficeFormKind::DeliveryConfirmation:
        return QStringLiteral("耗材-秘令0907（南京）");
    default:
        return QStringLiteral("Sheet1");
    }
}

QString safeFilePart(QString value)
{
    value = value.trimmed();
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    return value.isEmpty() ? QStringLiteral("未编号") : value;
}

bool isHandwrittenSignatureField(const QString &key)
{
    static const QSet<QString> fields = {
        QStringLiteral("warehouseKeeper"),
        QStringLiteral("departmentManager"),
        QStringLiteral("firstIssuer"),
        QStringLiteral("firstApprover"),
        QStringLiteral("secondIssuer"),
        QStringLiteral("secondApprover"),
        QStringLiteral("returner"),
        QStringLiteral("returnApprover"),
        QStringLiteral("reviewer"),
        QStringLiteral("qualityManager"),
        QStringLiteral("generalManager"),
        QStringLiteral("receiver"),
        QStringLiteral("inspector")
    };
    return fields.contains(key);
}

}

QString OfficeTemplateService::formTitle(OfficeFormKind kind)
{
    switch (kind) {
    case OfficeFormKind::Inspection: return QStringLiteral("材料检验通知单");
    case OfficeFormKind::RawMaterialInbound: return QStringLiteral("原材料入库单");
    case OfficeFormKind::ProductionIssue: return QStringLiteral("领料单");
    case OfficeFormKind::FinishedGoodsInbound: return QStringLiteral("成品入库单");
    case OfficeFormKind::StockOutbound: return QStringLiteral("出库单");
    case OfficeFormKind::DeliveryConfirmation: return QStringLiteral("送货确认单");
    }
    return QStringLiteral("业务单据");
}

QString OfficeTemplateService::templateFileName(OfficeFormKind kind)
{
    switch (kind) {
    case OfficeFormKind::Inspection:
        return QStringLiteral("BMJ-SMSP-16 材料检验通知单---A0 .xlsx");
    case OfficeFormKind::RawMaterialInbound:
        return QStringLiteral("BMJ-SMSP-18 原材料入库单---A0.xls");
    case OfficeFormKind::ProductionIssue:
        return QStringLiteral("BMJ-SMSP-17 领料单---A0.xls");
    case OfficeFormKind::FinishedGoodsInbound:
        return QStringLiteral("BMJ-SMSP-18 成品入库单---A0.xls");
    case OfficeFormKind::StockOutbound:
        return QStringLiteral("BMJ-SMSP-19 出库单---A0.xls");
    case OfficeFormKind::DeliveryConfirmation:
        return QStringLiteral("送货确认单 .xls");
    }
    return {};
}

QString OfficeTemplateService::outputFileName(const OfficeTemplateDocument &document)
{
    const QString suffix = QFileInfo(templateFileName(document.kind)).suffix();
    return QStringLiteral("%1_%2.%3")
        .arg(formTitle(document.kind), safeFilePart(document.documentNumber), suffix);
}

QString OfficeTemplateService::archiveRootPath()
{
    const QString documentsPath =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return documentsPath.isEmpty()
        ? QString()
        : QDir(documentsPath).filePath(QStringLiteral("冰美肌仓库系统表单"));
}

QList<OfficeTemplateLine> OfficeTemplateService::materialLines(
    QSqlDatabase database,
    const QList<StockMovementRequest> &lines,
    QString *errorMessage)
{
    QList<OfficeTemplateLine> result;
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT code,name,specification,unit,unit_usage FROM materials WHERE id=?"));
    for (const StockMovementRequest &source : lines) {
        query.bindValue(0, source.materialId);
        if (!query.exec() || !query.next()) {
            setError(errorMessage,
                     QStringLiteral("读取物料 %1 的模板资料失败：%2")
                         .arg(source.materialId)
                         .arg(query.lastError().text()));
            return {};
        }
        OfficeTemplateLine line;
        line.materialCode = query.value(0).toString();
        line.materialName = query.value(1).toString();
        line.specification = query.value(2).toString();
        line.unit = query.value(3).toString();
        line.unitUsage = query.value(4).toDouble();
        line.quantity = source.quantity;
        line.batchNo = source.batchNo;
        line.serialNumbers = source.serialNumbers.join(QStringLiteral("、"));
        line.notes = source.notes;
        result.append(line);
    }
    return result;
}

QString OfficeTemplateService::findTemplateFile(OfficeFormKind kind)
{
    const QString fileName = templateFileName(kind);
    QStringList roots;
    roots << QDir::currentPath() << QCoreApplication::applicationDirPath();
    QDir parent(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 5 && parent.cdUp(); ++level) roots << parent.absolutePath();
    for (const QString &root : std::as_const(roots)) {
        const QString candidate = QDir(root).filePath(
            QStringLiteral("仓库系统表单模板/%1").arg(fileName));
        if (QFileInfo::exists(candidate)) return QDir::toNativeSeparators(candidate);
    }
    return {};
}

QJsonObject OfficeTemplateService::lineToJson(const OfficeTemplateLine &line)
{
    QJsonObject value;
    value.insert(QStringLiteral("code"), line.materialCode);
    value.insert(QStringLiteral("name"), line.materialName);
    value.insert(QStringLiteral("specification"), line.specification);
    value.insert(QStringLiteral("unit"), line.unit);
    value.insert(QStringLiteral("quantity"), line.quantity);
    value.insert(QStringLiteral("batchNo"), line.batchNo);
    value.insert(QStringLiteral("serialNumbers"), line.serialNumbers);
    value.insert(QStringLiteral("notes"), line.notes);
    value.insert(QStringLiteral("orderNumber"), line.orderNumber);
    value.insert(QStringLiteral("supplier"), line.supplier);
    value.insert(QStringLiteral("unitUsage"), line.unitUsage);
    value.insert(QStringLiteral("externalQuantity"), line.externalQuantity);
    value.insert(QStringLiteral("reworkQuantity"), line.reworkQuantity);
    value.insert(QStringLiteral("lossQuantity"), line.lossQuantity);
    value.insert(QStringLiteral("returnQuantity"), line.returnQuantity);
    return value;
}

OfficeTemplateLine OfficeTemplateService::lineFromJson(const QJsonObject &value)
{
    OfficeTemplateLine line;
    line.materialCode = value.value(QStringLiteral("code")).toString();
    line.materialName = value.value(QStringLiteral("name")).toString();
    line.specification = value.value(QStringLiteral("specification")).toString();
    line.unit = value.value(QStringLiteral("unit")).toString();
    line.quantity = value.value(QStringLiteral("quantity")).toDouble();
    line.batchNo = value.value(QStringLiteral("batchNo")).toString();
    line.serialNumbers = value.value(QStringLiteral("serialNumbers")).toString();
    line.notes = value.value(QStringLiteral("notes")).toString();
    line.orderNumber = value.value(QStringLiteral("orderNumber")).toString();
    line.supplier = value.value(QStringLiteral("supplier")).toString();
    line.unitUsage = value.value(QStringLiteral("unitUsage")).toDouble();
    line.externalQuantity = value.value(QStringLiteral("externalQuantity")).toDouble();
    line.reworkQuantity = value.value(QStringLiteral("reworkQuantity")).toDouble();
    line.lossQuantity = value.value(QStringLiteral("lossQuantity")).toDouble();
    line.returnQuantity = value.value(QStringLiteral("returnQuantity")).toDouble();
    return line;
}

QJsonObject OfficeTemplateService::documentToJson(const OfficeTemplateDocument &document)
{
    QJsonObject root;
    root.insert(QStringLiteral("kind"), formCode(document.kind));
    root.insert(QStringLiteral("documentNumber"), document.documentNumber);
    root.insert(QStringLiteral("documentDate"),
                document.documentDate.toString(QStringLiteral("yyyy-MM-dd")));
    QJsonObject fields;
    for (auto it = document.fields.cbegin(); it != document.fields.cend(); ++it)
        fields.insert(it.key(), it.value());
    root.insert(QStringLiteral("fields"), fields);
    QJsonArray lines;
    for (const OfficeTemplateLine &line : document.lines) lines.append(lineToJson(line));
    root.insert(QStringLiteral("lines"), lines);
    return root;
}

bool OfficeTemplateService::documentFromJson(const QJsonObject &value,
                                             OfficeTemplateDocument *document,
                                             QString *errorMessage)
{
    if (!document) {
        setError(errorMessage, QStringLiteral("表单资料无效。"));
        return false;
    }
    const QString code = value.value(QStringLiteral("kind")).toString();
    OfficeFormKind kind = OfficeFormKind::StockOutbound;
    if (!formKindFromCode(code, &kind)) {
        setError(errorMessage, QStringLiteral("无法识别的表单类型：%1").arg(code));
        return false;
    }
    OfficeTemplateDocument result;
    result.kind = kind;
    result.documentNumber = value.value(QStringLiteral("documentNumber")).toString();
    result.documentDate = QDate::fromString(
        value.value(QStringLiteral("documentDate")).toString(), QStringLiteral("yyyy-MM-dd"));
    const QJsonObject fields = value.value(QStringLiteral("fields")).toObject();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        result.fields.insert(it.key(), it.value().toString());
    const QJsonArray lines = value.value(QStringLiteral("lines")).toArray();
    for (const QJsonValue &line : lines) {
        if (line.isObject()) result.lines.append(lineFromJson(line.toObject()));
    }
    *document = result;
    return true;
}

bool OfficeTemplateService::documentFromPayload(const QString &payload,
                                                OfficeTemplateDocument *document,
                                                QString *errorMessage)
{
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(errorMessage, QStringLiteral("表单资料无法解析：%1").arg(parseError.errorString()));
        return false;
    }
    return documentFromJson(json.object(), document, errorMessage);
}

bool OfficeTemplateService::renderToFile(const OfficeTemplateDocument &document,
                                         const QString &outputPath,
                                         QString *errorMessage)
{
    if (document.lines.isEmpty()) {
        setError(errorMessage, QStringLiteral("%1没有可填写的明细。")
                                   .arg(formTitle(document.kind)));
        return false;
    }
    const QString templatePath = findTemplateFile(document.kind);
    if (templatePath.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("找不到模板“%1”。请确认“仓库系统表单模板”文件夹位于程序目录或项目目录。")
                     .arg(templateFileName(document.kind)));
        return false;
    }
    const QFileInfo outputInfo(outputPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        setError(errorMessage, QStringLiteral("无法创建表单保存目录：%1")
                                   .arg(outputInfo.absolutePath()));
        return false;
    }
    if (QFileInfo::exists(outputPath) && !QFile::remove(outputPath)) {
        setError(errorMessage, QStringLiteral("无法覆盖表单文件，文件可能正在使用：%1")
                                   .arg(outputPath));
        return false;
    }
    if (!QFile::copy(templatePath, outputPath)) {
        setError(errorMessage, QStringLiteral("复制表单模板失败：%1").arg(templatePath));
        return false;
    }

    QJsonObject root = documentToJson(document);
    root.insert(QStringLiteral("sheetName"), sheetName(document.kind));
    root.insert(QStringLiteral("outputPath"), QDir::toNativeSeparators(outputPath));
    // 脚本按引擎逐个完整重试，每次都要从未修改的模板重新复制，避免失败的表单污染下一次尝试。
    root.insert(QStringLiteral("templatePath"), QDir::toNativeSeparators(templatePath));
    // 手写签字字段只在填写模板时过滤，存档载荷仍保留完整资料。
    QJsonObject fields;
    for (auto it = document.fields.cbegin(); it != document.fields.cend(); ++it) {
        if (!isHandwrittenSignatureField(it.key())) fields.insert(it.key(), it.value());
    }
    root.insert(QStringLiteral("fields"), fields);

    QTemporaryDir workingDir;
    if (!workingDir.isValid()) {
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("无法创建表单处理临时目录。"));
        return false;
    }
    const QString jsonPath = workingDir.filePath(QStringLiteral("form.json"));
    const QString scriptPath = workingDir.filePath(QStringLiteral("fill_office_template.ps1"));
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::WriteOnly)
        || jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("无法准备模板填写数据。"));
        return false;
    }
    jsonFile.close();
    QFile scriptResource(QStringLiteral(":/resources/fill_office_template.ps1"));
    if (!scriptResource.open(QIODevice::ReadOnly)) {
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("程序内缺少模板填写组件。"));
        return false;
    }
    QFile scriptFile(scriptPath);
    QByteArray scriptData = scriptResource.readAll();
    if (!scriptData.startsWith("\xEF\xBB\xBF")) scriptData.prepend("\xEF\xBB\xBF");
    if (!scriptFile.open(QIODevice::WriteOnly)
        || scriptFile.write(scriptData) < 0) {
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("无法准备模板填写组件。"));
        return false;
    }
    scriptFile.close();

    QProcess process;
    process.setProgram(QStringLiteral("powershell.exe"));
    process.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
                          QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
                          QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath, jsonPath});
    process.start();
    if (!process.waitForStarted(10000)) {
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("无法启动 Windows 表单填写组件：%1")
                                   .arg(process.errorString()));
        return false;
    }
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished();
        QFile::remove(outputPath);
        setError(errorMessage, QStringLiteral("填写模板超时。请关闭正在阻塞的 Excel 或 WPS 表格对话框后重试。"));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        QFile::remove(outputPath);
        setError(errorMessage,
                 QStringLiteral("无法填写%1。请确认本机已安装 Microsoft Excel 或 WPS 表格，相关自动化组件可用，且模板没有被其他程序占用。%2")
                     .arg(formTitle(document.kind),
                          details.isEmpty() ? QString() : QStringLiteral("\n\n%1").arg(details)));
        return false;
    }
    if (!QFileInfo::exists(outputPath) || QFileInfo(outputPath).size() == 0) {
        setError(errorMessage, QStringLiteral("%1生成后为空。")
                                   .arg(formTitle(document.kind)));
        return false;
    }
    return true;
}

bool OfficeTemplateService::saveToDocumentsArchive(const OfficeTemplateDocument &document,
                                                   QString *savedPath,
                                                   QString *errorMessage)
{
    if (savedPath) savedPath->clear();
    const QString number = document.documentNumber.trimmed();
    if (number.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("%1缺少单号，无法保存到归档文件夹。").arg(formTitle(document.kind)));
        return false;
    }
    // 归档文件名与正式附件保持一致：只用单号，不带表单名称或随机预览名。
    const QString suffix = QFileInfo(templateFileName(document.kind)).suffix();
    if (suffix.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("无法确定%1的输出文件格式。").arg(formTitle(document.kind)));
        return false;
    }
    const QString rootPath = archiveRootPath();
    if (rootPath.isEmpty()) {
        setError(errorMessage, QStringLiteral("无法获取“我的文档”目录。"));
        return false;
    }
    const QString formDirectory = QDir(rootPath).filePath(formTitle(document.kind));
    if (!QDir().mkpath(formDirectory) || !QDir(formDirectory).exists()) {
        setError(errorMessage, QStringLiteral("无法创建表单归档目录：%1")
                                   .arg(QDir::toNativeSeparators(formDirectory)));
        return false;
    }
    const QString archiveFileName = QStringLiteral("%1.%2").arg(safeFilePart(number), suffix);
    const QString archivePath = QDir(formDirectory).filePath(archiveFileName);
    const auto setArchiveError = [errorMessage, &archivePath](const QString &reason) {
        setError(errorMessage, QStringLiteral("无法保存表单到：%1（%2）")
                                   .arg(QDir::toNativeSeparators(archivePath), reason));
    };

    // 先在临时目录完整生成表单，成功后才原子替换归档，绝不提前破坏已有的有效正本。
    QTemporaryDir workDirectory;
    if (!workDirectory.isValid()) {
        setError(errorMessage, QStringLiteral("无法创建表单临时目录。"));
        return false;
    }
    const QString renderedPath = workDirectory.filePath(archiveFileName);
    QString renderError;
    if (!renderToFile(document, renderedPath, &renderError)) {
        setError(errorMessage, renderError);
        return false;
    }
    QFile renderedFile(renderedPath);
    if (!renderedFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法读取已生成的%1：%2")
                                   .arg(formTitle(document.kind), renderedFile.errorString()));
        return false;
    }
    const QByteArray data = renderedFile.readAll();
    renderedFile.close();
    if (data.isEmpty()) {
        setError(errorMessage, QStringLiteral("%1生成后为空。").arg(formTitle(document.kind)));
        return false;
    }

    QSaveFile archiveFile(archivePath);
    if (!archiveFile.open(QIODevice::WriteOnly)) {
        setArchiveError(archiveFile.errorString());
        return false;
    }
    if (archiveFile.write(data) != data.size()) {
        archiveFile.cancelWriting();
        setArchiveError(archiveFile.errorString());
        return false;
    }
    if (!archiveFile.commit()) {
        setArchiveError(QStringLiteral("%1，请确认文件没有被 Excel 或 WPS 表格占用")
                            .arg(archiveFile.errorString()));
        return false;
    }
    if (savedPath) *savedPath = archivePath;
    return true;
}

bool OfficeTemplateService::openPreview(const OfficeTemplateDocument &document, QWidget *parent)
{
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("IceBeautyWms/form-previews"));
    QDir().mkpath(root);
    const QString suffix = QFileInfo(templateFileName(document.kind)).suffix();
    const QString path = QDir(root).filePath(
        QStringLiteral("%1_%2.%3")
            .arg(formTitle(document.kind),
                 QUuid::createUuid().toString(QUuid::WithoutBraces), suffix));
    QString error;
    if (!renderToFile(document, path, &error)) {
        if (parent) QMessageBox::warning(parent, QStringLiteral("模板预览失败"), error);
        return false;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        if (parent) {
            QMessageBox::warning(parent, QStringLiteral("无法打开模板"),
                                 QStringLiteral("模板已生成，但 Windows 无法打开：%1").arg(path));
        }
        return false;
    }
    return true;
}

bool OfficeTemplateService::hasIncompleteDocumentForms(QSqlDatabase database, qlonglong documentId)
{
    if (documentId <= 0) return false;
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM document_forms WHERE document_id=? AND status IN ('PENDING','FAILED') LIMIT 1"));
    query.addBindValue(documentId);
    if (!query.exec()) return false;
    return query.next();
}

bool OfficeTemplateService::upsertDocumentForm(QSqlDatabase database,
                                               qlonglong documentId,
                                               const OfficeTemplateDocument &document,
                                               qlonglong *formRecordId,
                                               QString *errorMessage)
{
    const QString payload = QString::fromUtf8(
        QJsonDocument(documentToJson(document)).toJson(QJsonDocument::Compact));
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO document_forms(document_id,form_kind,payload,status,last_error) "
        "VALUES(?,?,?,'PENDING','') "
        "ON CONFLICT(document_id,form_kind) DO UPDATE SET "
        "payload=excluded.payload,status='PENDING',last_error='',"
        "updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime')"));
    insert.addBindValue(documentId);
    insert.addBindValue(formCode(document.kind));
    insert.addBindValue(payload);
    if (!insert.exec()) {
        setError(errorMessage, QStringLiteral("无法保存表单恢复记录：%1")
                                   .arg(insert.lastError().text()));
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id FROM document_forms WHERE document_id=? AND form_kind=?"));
    query.addBindValue(documentId);
    query.addBindValue(formCode(document.kind));
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("无法读取表单恢复记录：%1")
                                   .arg(query.lastError().text()));
        return false;
    }
    if (formRecordId) *formRecordId = query.value(0).toLongLong();
    return true;
}

void OfficeTemplateService::markDocumentFormFailed(QSqlDatabase database,
                                                   qlonglong formRecordId,
                                                   const QString &message,
                                                   QString *errorMessage)
{
    // 只更新状态和错误原因，始终保留已存入的载荷，便于之后重试。
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE document_forms SET status='FAILED',last_error=?,"
        "updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime') WHERE id=?"));
    query.addBindValue(message);
    query.addBindValue(formRecordId);
    QString text = message;
    if (!query.exec()) {
        text += QStringLiteral("\n（同时无法更新表单记录状态：%1）")
                    .arg(query.lastError().text());
    }
    setError(errorMessage, text);
}

bool OfficeTemplateService::markDocumentFormCompleted(QSqlDatabase database,
                                                      qlonglong formRecordId,
                                                      qlonglong attachmentId,
                                                      QString *errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE document_forms SET status='COMPLETED',last_error='',attachment_id=?,"
        "updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime') WHERE id=?"));
    query.addBindValue(attachmentId > 0 ? QVariant(attachmentId) : QVariant());
    query.addBindValue(formRecordId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("表单已生成，但无法更新表单记录状态：%1")
                                   .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool OfficeTemplateService::storeDocumentFormAttachment(QSqlDatabase database,
                                                        qlonglong formRecordId,
                                                        qlonglong attachmentId,
                                                        QString *errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE document_forms SET attachment_id=?,"
        "updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime') WHERE id=?"));
    query.addBindValue(attachmentId);
    query.addBindValue(formRecordId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("附件已上传，但无法记录表单附件：%1")
                                   .arg(query.lastError().text()));
        return false;
    }
    return true;
}

qlonglong OfficeTemplateService::documentFormAttachmentId(QSqlDatabase database,
                                                          qlonglong formRecordId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT attachment_id FROM document_forms WHERE id=?"));
    query.addBindValue(formRecordId);
    if (!query.exec() || !query.next()) return 0;
    return query.value(0).toLongLong();
}

bool OfficeTemplateService::isReusableAttachment(QSqlDatabase database,
                                                 qlonglong documentId,
                                                 qlonglong attachmentId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM attachments WHERE id=? AND business_type='business_document' "
        "AND business_id=? AND is_deleted=0"));
    query.addBindValue(attachmentId);
    query.addBindValue(documentId);
    return query.exec() && query.next();
}

QString OfficeTemplateService::incompleteFormHint(bool formCompleted)
{
    return formCompleted
        ? QStringLiteral("业务单据已生效，库存没有变化；表单已保存到数据库附件和“我的文档\\冰美肌仓库系统表单”归档文件夹并标记为已完成，仅未能自动打开，请在“附件管理”中手动打开。")
        : QStringLiteral("业务单据已生效，库存没有变化；表单记录已保留，可在“附件管理”中选择该单据并点击“重新生成未完成表单”重试。");
}

bool OfficeTemplateService::attachToDocument(const OfficeTemplateDocument &document,
                                             QSqlDatabase database,
                                             qlonglong operatorId,
                                             qlonglong businessDocumentId,
                                             QString *errorMessage,
                                             bool openArchivedFile)
{
    qlonglong formRecordId = 0;
    QString error;
    if (!upsertDocumentForm(database, businessDocumentId, document, &formRecordId, &error)) {
        setError(errorMessage, error);
        return false;
    }
    bool formCompleted = false;
    error.clear();
    if (persistDocumentForm(database, operatorId, businessDocumentId, formRecordId, document,
                            &formCompleted, &error, openArchivedFile)) {
        return true;
    }
    setError(errorMessage,
             QStringLiteral("%1\n\n%2").arg(error, incompleteFormHint(formCompleted)));
    return false;
}

bool OfficeTemplateService::attachToInspectionNotice(const OfficeTemplateDocument &document,
                                                     QSqlDatabase database,
                                                     qlonglong operatorId,
                                                     qlonglong inspectionNoticeId,
                                                     QString *savedPath,
                                                     QString *errorMessage)
{
    if (savedPath) savedPath->clear();
    if (!database.isOpen() || operatorId <= 0 || inspectionNoticeId <= 0
        || document.kind != OfficeFormKind::Inspection) {
        setError(errorMessage, QStringLiteral("材料检验通知单、当前用户或数据库无效。"));
        return false;
    }

    QSqlQuery notice(database);
    notice.prepare(QStringLiteral(
        "SELECT inspection_no,template_file_attachment_id FROM inspection_notices WHERE id=?"));
    notice.addBindValue(inspectionNoticeId);
    if (!notice.exec() || !notice.next()) {
        setError(errorMessage, QStringLiteral("找不到需要保存的材料检验通知单。"));
        return false;
    }
    const QString inspectionNumber = notice.value(0).toString();
    if (inspectionNumber.compare(document.documentNumber.trimmed(), Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QStringLiteral("通知单号与待保存表单不一致，已停止保存。"));
        return false;
    }

    QString archivePath;
    QString archiveError;
    if (!saveToDocumentsArchive(document, &archivePath, &archiveError)) {
        setError(errorMessage, archiveError);
        return false;
    }
    QFile archiveFile(archivePath);
    if (!archiveFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法读取已归档的材料检验通知单：%1")
                                   .arg(archiveFile.errorString()));
        return false;
    }
    const QByteArray data = archiveFile.readAll();
    archiveFile.close();
    if (data.isEmpty() || data.size() > AttachmentService::MaximumAttachmentBytes) {
        setError(errorMessage, data.isEmpty()
                                   ? QStringLiteral("材料检验通知单生成后为空。")
                                   : QStringLiteral("材料检验通知单附件不能超过50 MB。"));
        return false;
    }
    const QString fileName = outputFileName(document);
    const QString mime = QFileInfo(fileName).suffix().compare(
                             QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
        : QStringLiteral("application/vnd.ms-excel");
    const QString sha256 = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    qlonglong attachmentId = notice.value(1).toLongLong();
    if (attachmentId > 0) {
        QSqlQuery valid(database);
        valid.prepare(QStringLiteral(
            "SELECT 1 FROM attachments WHERE id=? AND business_type='inspection_notice' "
            "AND business_id=?"));
        valid.addBindValue(attachmentId);
        valid.addBindValue(inspectionNoticeId);
        if (!valid.exec() || !valid.next()) attachmentId = 0;
    }

    if (!database.transaction()) {
        setError(errorMessage, QStringLiteral("无法开始保存通知单附件：%1")
                                   .arg(database.lastError().text()));
        return false;
    }
    QSqlQuery attachment(database);
    if (attachmentId > 0) {
        attachment.prepare(QStringLiteral(
            "UPDATE attachments SET original_file_name=?,mime_type=?,file_size=?,sha256=?,"
            "file_data=?,uploaded_by=?,uploaded_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime'),"
            "is_deleted=0,deleted_by=NULL,deleted_at=NULL WHERE id=?"));
        attachment.addBindValue(fileName);
        attachment.addBindValue(mime);
        attachment.addBindValue(data.size());
        attachment.addBindValue(sha256);
        attachment.addBindValue(data);
        attachment.addBindValue(operatorId);
        attachment.addBindValue(attachmentId);
    } else {
        attachment.prepare(QStringLiteral(
            "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,"
            "file_size,sha256,file_data,uploaded_by) VALUES('inspection_notice',?,?,?,?,?,?,?)"));
        attachment.addBindValue(inspectionNoticeId);
        attachment.addBindValue(fileName);
        attachment.addBindValue(mime);
        attachment.addBindValue(data.size());
        attachment.addBindValue(sha256);
        attachment.addBindValue(data);
        attachment.addBindValue(operatorId);
    }
    if (!attachment.exec()) {
        const QString databaseError = attachment.lastError().text();
        database.rollback();
        setError(errorMessage, QStringLiteral("保存材料检验通知单附件失败：%1")
                                   .arg(databaseError));
        return false;
    }
    if (attachmentId <= 0) attachmentId = attachment.lastInsertId().toLongLong();

    const QString payload = QString::fromUtf8(
        QJsonDocument(documentToJson(document)).toJson(QJsonDocument::Compact));
    QSqlQuery updateNotice(database);
    updateNotice.prepare(QStringLiteral(
        "UPDATE inspection_notices SET template_file_attachment_id=?,template_payload=?,"
        "archive_path=?,updated_by=?,"
        "updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime') WHERE id=?"));
    updateNotice.addBindValue(attachmentId);
    updateNotice.addBindValue(payload);
    updateNotice.addBindValue(QDir::toNativeSeparators(archivePath));
    updateNotice.addBindValue(operatorId);
    updateNotice.addBindValue(inspectionNoticeId);
    QSqlQuery audit(database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'INSPECTION_NOTICE_FORM_SAVE','inspection_notice',?,?)"));
    audit.addBindValue(operatorId);
    audit.addBindValue(inspectionNoticeId);
    audit.addBindValue(QStringLiteral("%1 / %2")
                           .arg(inspectionNumber, QDir::toNativeSeparators(archivePath)));
    if (!updateNotice.exec() || updateNotice.numRowsAffected() != 1 || !audit.exec()
        || !database.commit()) {
        const QString databaseError = !updateNotice.lastError().text().isEmpty()
            ? updateNotice.lastError().text()
            : (!audit.lastError().text().isEmpty() ? audit.lastError().text()
                                                   : database.lastError().text());
        database.rollback();
        setError(errorMessage, QStringLiteral("记录材料检验通知单附件失败：%1")
                                   .arg(databaseError));
        return false;
    }
    if (savedPath) *savedPath = archivePath;
    return true;
}

bool OfficeTemplateService::retryIncompleteDocumentForms(QSqlDatabase database,
                                                         qlonglong operatorId,
                                                         qlonglong documentId,
                                                         QStringList *completedTitles,
                                                         QStringList *errors,
                                                         bool openArchivedFiles)
{
    if (documentId <= 0) {
        if (errors) errors->append(QStringLiteral("没有选择业务单据。"));
        return false;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id,payload FROM document_forms WHERE document_id=? "
        "AND status IN ('PENDING','FAILED') ORDER BY id"));
    query.addBindValue(documentId);
    if (!query.exec()) {
        if (errors) {
            errors->append(QStringLiteral("读取未完成表单失败：%1").arg(query.lastError().text()));
        }
        return false;
    }
    QList<QPair<qlonglong, QString>> records;
    while (query.next()) {
        records.append(qMakePair(query.value(0).toLongLong(), query.value(1).toString()));
    }
    if (records.isEmpty()) return true;

    bool allCompleted = true;
    for (const QPair<qlonglong, QString> &record : std::as_const(records)) {
        OfficeTemplateDocument document;
        QString parseError;
        if (!documentFromPayload(record.second, &document, &parseError)) {
            const QString message = QStringLiteral("表单记录 %1 无法重试：%2")
                                        .arg(record.first)
                                        .arg(parseError);
            markDocumentFormFailed(database, record.first, message, nullptr);
            if (errors) errors->append(message);
            allCompleted = false;
            continue;
        }
        const QString title = formTitle(document.kind);
        bool formCompleted = false;
        QString persistError;
        if (!persistDocumentForm(database, operatorId, documentId, record.first, document,
                                 &formCompleted, &persistError, openArchivedFiles)) {
            // 表单本身已保存完成、仅自动打开失败时，仍算重新生成成功，只提示该问题。
            if (!formCompleted) allCompleted = false;
            if (errors) errors->append(QStringLiteral("%1：%2").arg(title, persistError));
            if (formCompleted && completedTitles) completedTitles->append(title);
            continue;
        }
        if (completedTitles) completedTitles->append(title);
    }
    return allCompleted;
}

bool OfficeTemplateService::persistDocumentForm(QSqlDatabase database,
                                                qlonglong operatorId,
                                                qlonglong documentId,
                                                qlonglong formRecordId,
                                                const OfficeTemplateDocument &document,
                                                bool *formCompleted,
                                                QString *errorMessage,
                                                bool openArchivedFile)
{
    if (formCompleted) *formCompleted = false;
    QTemporaryDir workDirectory;
    if (!workDirectory.isValid()) {
        markDocumentFormFailed(database, formRecordId,
                               QStringLiteral("无法创建表单临时目录。"), errorMessage);
        return false;
    }
    const QString fileName = outputFileName(document);
    const QString path = workDirectory.filePath(fileName);
    QString renderError;
    if (!renderToFile(document, path, &renderError)) {
        markDocumentFormFailed(database, formRecordId, renderError, errorMessage);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        markDocumentFormFailed(database, formRecordId,
                               QStringLiteral("无法读取已生成的表单：%1").arg(file.errorString()),
                               errorMessage);
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();
    const QString mime = QFileInfo(path).suffix().compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
        : QStringLiteral("application/vnd.ms-excel");

    // 重试或修改时复用附件编号，但必须把内容、摘要和上传信息一并替换。
    qlonglong attachmentId = documentFormAttachmentId(database, formRecordId);
    if (attachmentId > 0 && !isReusableAttachment(database, documentId, attachmentId)) {
        attachmentId = 0;
    }
    if (attachmentId > 0) {
        if (!database.transaction()) {
            markDocumentFormFailed(database, formRecordId,
                                   QStringLiteral("无法开始替换表单附件：%1")
                                       .arg(database.lastError().text()),
                                   errorMessage);
            return false;
        }
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE attachments SET original_file_name=?,mime_type=?,file_size=?,sha256=?,"
            "file_data=?,uploaded_by=?,uploaded_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime'),"
            "is_deleted=0,deleted_by=NULL,deleted_at=NULL WHERE id=? AND business_type='business_document' "
            "AND business_id=?"));
        update.addBindValue(fileName);
        update.addBindValue(mime);
        update.addBindValue(data.size());
        update.addBindValue(QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
        update.addBindValue(data);
        update.addBindValue(operatorId);
        update.addBindValue(attachmentId);
        update.addBindValue(documentId);
        QSqlQuery audit(database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
            "VALUES(?,'DOCUMENT_FORM_REPLACE','attachment',?,?)"));
        audit.addBindValue(operatorId);
        audit.addBindValue(attachmentId);
        audit.addBindValue(QStringLiteral("%1 / %2")
                               .arg(document.documentNumber, formTitle(document.kind)));
        if (!update.exec() || update.numRowsAffected() != 1 || !audit.exec()
            || !database.commit()) {
            const QString databaseError = !update.lastError().text().isEmpty()
                ? update.lastError().text()
                : (!audit.lastError().text().isEmpty() ? audit.lastError().text()
                                                       : database.lastError().text());
            database.rollback();
            markDocumentFormFailed(database, formRecordId,
                                   QStringLiteral("替换数据库附件失败：%1").arg(databaseError),
                                   errorMessage);
            return false;
        }
    } else {
        AttachmentService attachments(database, operatorId);
        QString databaseError;
        qlonglong uploadedId = 0;
        if (!attachments.uploadDocumentAttachment(documentId, fileName, mime, data,
                                                  &uploadedId, &databaseError)) {
            markDocumentFormFailed(database, formRecordId,
                                   QStringLiteral("数据库附件：%1").arg(databaseError),
                                   errorMessage);
            return false;
        }
        attachmentId = uploadedId;
        QString recordError;
        if (!storeDocumentFormAttachment(database, formRecordId, attachmentId, &recordError)) {
            markDocumentFormFailed(database, formRecordId, recordError, errorMessage);
            return false;
        }
    }

    QStringList saveErrors;
    QString archivedPath;
    const QString rootPath = archiveRootPath();
    if (rootPath.isEmpty()) {
        saveErrors.append(QStringLiteral("无法获取“我的文档”目录。"));
    } else {
        QString formDirectory;
        const QList<OfficeFormKind> kinds = {
            OfficeFormKind::Inspection,
            OfficeFormKind::RawMaterialInbound,
            OfficeFormKind::ProductionIssue,
            OfficeFormKind::FinishedGoodsInbound,
            OfficeFormKind::StockOutbound,
            OfficeFormKind::DeliveryConfirmation
        };
        for (const OfficeFormKind kind : kinds) {
            const QString directory = QDir(rootPath).filePath(formTitle(kind));
            if (kind == document.kind) formDirectory = directory;
            if (!QDir().mkpath(directory)) {
                saveErrors.append(QStringLiteral("无法创建表单归档目录：%1")
                                      .arg(QDir::toNativeSeparators(directory)));
            }
        }
        if (formDirectory.isEmpty() || !QDir(formDirectory).exists()) {
            saveErrors.append(QStringLiteral("无法创建表单归档目录：%1")
                                  .arg(QDir::toNativeSeparators(rootPath)));
        } else {
            const QString suffix = QFileInfo(templateFileName(document.kind)).suffix();
            const QString archiveFileName = QStringLiteral("%1.%2")
                                                .arg(safeFilePart(document.documentNumber), suffix);
            const QString archivePath = QDir(formDirectory).filePath(archiveFileName);
            QSaveFile archiveFile(archivePath);
            if (!archiveFile.open(QIODevice::WriteOnly)
                || archiveFile.write(data) != data.size()
                || !archiveFile.commit()) {
                saveErrors.append(QStringLiteral("无法保存表单到：%1（%2）")
                                      .arg(QDir::toNativeSeparators(archivePath),
                                           archiveFile.errorString()));
            } else {
                archivedPath = archivePath;
            }
        }
    }

    // 数据库附件和本地归档文件夹都成功，表单才算完成。
    if (archivedPath.isEmpty()) {
        if (saveErrors.isEmpty()) {
            saveErrors.append(QStringLiteral("无法保存表单到“我的文档\\冰美肌仓库系统表单”归档文件夹。"));
        }
        markDocumentFormFailed(database, formRecordId,
                               saveErrors.join(QStringLiteral("\n")), errorMessage);
        return false;
    }
    if (formCompleted) *formCompleted = true;
    QString recordError;
    if (!markDocumentFormCompleted(database, formRecordId, attachmentId, &recordError)) {
        if (formCompleted) *formCompleted = false;
        setError(errorMessage, recordError);
        return false;
    }

    // 自动打开失败只报告，不改变已经完成的表单状态。
    if (openArchivedFile && !QDesktopServices::openUrl(QUrl::fromLocalFile(archivedPath))) {
        setError(errorMessage, QStringLiteral("表单已保存，但 Windows 无法自动打开：%1")
                                   .arg(QDir::toNativeSeparators(archivedPath)));
        return false;
    }
    return true;
}

bool OfficeTemplateService::synchronizeDocumentForms(QSqlDatabase database,
                                                     qlonglong operatorId,
                                                     qlonglong documentId,
                                                     QStringList *errors,
                                                     bool openArchivedFiles)
{
    if (errors) errors->clear();
    if (!database.isOpen() || operatorId <= 0 || documentId <= 0) {
        if (errors) errors->append(QStringLiteral("数据库、当前用户或业务单据无效。"));
        return false;
    }

    QSqlQuery header(database);
    header.prepare(QStringLiteral(
        "SELECT d.document_no,d.document_type,d.document_date,d.handler_name,d.purpose,d.supplier,"
        "d.notes,COALESCE(s.customer_company,''),COALESCE(s.destination,''),"
        "COALESCE(s.contact_name,''),COALESCE(s.contact_phone,''),COALESCE(s.sales_order_no,''),"
        "COALESCE(s.logistics_company,''),COALESCE(s.tracking_no,''),"
        "COALESCE(s.delivery_date,''),COALESCE(p.batch_no,''),COALESCE(p.product_name,''),"
        "COALESCE(p.product_model,''),COALESCE(p.planned_quantity,0) "
        "FROM business_documents d LEFT JOIN sales_outbound_details s ON s.document_id=d.id "
        "LEFT JOIN production_runs p ON p.id=d.production_run_id WHERE d.id=?"));
    header.addBindValue(documentId);
    if (!header.exec() || !header.next()) {
        if (errors) errors->append(QStringLiteral("读取修改后的业务单据失败：%1")
                                       .arg(header.lastError().text()));
        return false;
    }

    QList<OfficeTemplateLine> materialRows;
    QSqlQuery lines(database);
    lines.prepare(QStringLiteral(
        "SELECT m.code,m.name,m.specification,m.unit,m.unit_usage,i.quantity,i.batch_no,i.notes,"
        "GROUP_CONCAT(DISTINCT sn.serial_no) "
        "FROM business_document_items i JOIN materials m ON m.id=i.material_id "
        "LEFT JOIN inventory_ledger l ON l.document_item_id=i.id "
        "LEFT JOIN inventory_ledger_serials x ON x.ledger_id=l.id "
        "LEFT JOIN serial_numbers sn ON sn.id=x.serial_id WHERE i.document_id=? "
        "GROUP BY i.id ORDER BY i.line_number,i.id"));
    lines.addBindValue(documentId);
    if (!lines.exec()) {
        if (errors) errors->append(QStringLiteral("读取修改后的业务明细失败：%1")
                                       .arg(lines.lastError().text()));
        return false;
    }
    while (lines.next()) {
        OfficeTemplateLine line;
        line.materialCode = lines.value(0).toString();
        line.materialName = lines.value(1).toString();
        line.specification = lines.value(2).toString();
        line.unit = lines.value(3).toString();
        line.unitUsage = lines.value(4).toDouble();
        line.quantity = lines.value(5).toDouble();
        line.batchNo = lines.value(6).toString();
        line.notes = lines.value(7).toString();
        line.serialNumbers = lines.value(8).toString().replace(QLatin1Char(','), QStringLiteral("、"));
        materialRows.append(line);
    }
    if (materialRows.isEmpty()) {
        if (errors) errors->append(QStringLiteral("修改后的业务单据没有可同步的明细。"));
        return false;
    }

    QSqlQuery forms(database);
    forms.prepare(QStringLiteral(
        "SELECT form_kind,payload FROM document_forms WHERE document_id=? ORDER BY id"));
    forms.addBindValue(documentId);
    if (!forms.exec()) {
        if (errors) errors->append(QStringLiteral("读取表单记录失败：%1").arg(forms.lastError().text()));
        return false;
    }
    QList<QPair<QString, QString>> records;
    while (forms.next()) records.append({forms.value(0).toString(), forms.value(1).toString()});
    bool allOk = true;
    QStringList desiredKinds;
    const QString documentType = header.value(1).toString().trimmed().toUpper();
    bool recognizedDocumentType = true;
    if (documentType == QStringLiteral("SCLL") || documentType == QStringLiteral("SCTL")) {
        desiredKinds.append(formCode(OfficeFormKind::ProductionIssue));
    } else if (documentType == QStringLiteral("CPRK")
               || documentType == QStringLiteral("SCWG")) {
        desiredKinds.append(formCode(OfficeFormKind::FinishedGoodsInbound));
    } else if (QStringList{QStringLiteral("CGRK"), QStringLiteral("TLRK"),
                           QStringLiteral("QTRK"), QStringLiteral("QC")}.contains(documentType)) {
        desiredKinds.append(formCode(OfficeFormKind::RawMaterialInbound));
    } else if (QStringList{QStringLiteral("XSCK"), QStringLiteral("WXLY"),
                           QStringLiteral("YPLY"), QStringLiteral("QTCK")}.contains(documentType)) {
        desiredKinds.append(formCode(OfficeFormKind::StockOutbound));
        if (documentType == QStringLiteral("XSCK"))
            desiredKinds.append(formCode(OfficeFormKind::DeliveryConfirmation));
    } else if (documentType != QStringLiteral("DB") && documentType != QStringLiteral("PD")
               && documentType != QStringLiteral("CX")) {
        recognizedDocumentType = false;
    }
    if (recognizedDocumentType) {
        for (int index = records.size() - 1; index >= 0; --index) {
            if (desiredKinds.contains(records.at(index).first)) continue;
            QSqlQuery removeAttachment(database);
            removeAttachment.prepare(QStringLiteral(
                "UPDATE attachments SET is_deleted=1,deleted_by=?,deleted_at=? WHERE id IN "
                "(SELECT attachment_id FROM document_forms WHERE document_id=? AND form_kind=?)"));
            removeAttachment.addBindValue(operatorId);
            removeAttachment.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            removeAttachment.addBindValue(documentId);
            removeAttachment.addBindValue(records.at(index).first);
            QSqlQuery removeForm(database);
            removeForm.prepare(QStringLiteral(
                "DELETE FROM document_forms WHERE document_id=? AND form_kind=?"));
            removeForm.addBindValue(documentId);
            removeForm.addBindValue(records.at(index).first);
            if (!removeAttachment.exec() || !removeForm.exec()) {
                allOk = false;
                if (errors) errors->append(QStringLiteral("清理不再适用的表单失败：%1")
                                               .arg(records.at(index).first));
            }
            records.removeAt(index);
        }
    }
    for (const QString &kind : std::as_const(desiredKinds)) {
        bool exists = false;
        for (const auto &record : std::as_const(records)) {
            if (record.first == kind) { exists = true; break; }
        }
        if (!exists) records.append({kind, QString()});
    }
    if (records.isEmpty()) return allOk;
    for (const auto &record : std::as_const(records)) {
        OfficeFormKind kind;
        if (!formKindFromCode(record.first, &kind)) {
            allOk = false;
            if (errors) errors->append(QStringLiteral("无法识别表单类型：%1").arg(record.first));
            continue;
        }
        OfficeTemplateDocument document;
        QString parseError;
        if (!documentFromPayload(record.second, &document, &parseError)) {
            document.kind = kind;
        }
        document.kind = kind;
        document.documentNumber = header.value(0).toString();
        document.documentDate = QDate::fromString(header.value(2).toString(), Qt::ISODate);
        if (kind == OfficeFormKind::DeliveryConfirmation) {
            const QDate delivery = QDate::fromString(header.value(14).toString(), Qt::ISODate);
            if (delivery.isValid()) document.documentDate = delivery;
        }
        document.fields.insert(QStringLiteral("handler"), header.value(3).toString());
        document.fields.insert(QStringLiteral("purpose"), header.value(4).toString());
        document.fields.insert(QStringLiteral("supplier"), header.value(5).toString());
        document.fields.insert(QStringLiteral("notes"), header.value(6).toString());
        document.fields.insert(QStringLiteral("customerCompany"), header.value(7).toString());
        document.fields.insert(QStringLiteral("destination"), header.value(8).toString());
        document.fields.insert(QStringLiteral("customerContact"), header.value(9).toString());
        document.fields.insert(QStringLiteral("customerPhone"), header.value(10).toString());
        document.fields.insert(QStringLiteral("salesOrderNumber"), header.value(11).toString());
        document.fields.insert(QStringLiteral("logisticsCompany"), header.value(12).toString());
        document.fields.insert(QStringLiteral("trackingNumber"), header.value(13).toString());
        document.fields.insert(QStringLiteral("productionBatch"), header.value(15).toString());
        document.fields.insert(QStringLiteral("productName"), header.value(16).toString());
        document.fields.insert(QStringLiteral("productModel"), header.value(17).toString());
        document.fields.insert(QStringLiteral("plannedQuantity"),
                               QString::number(header.value(18).toDouble(), 'g', 12));
        document.lines = materialRows;
        for (OfficeTemplateLine &line : document.lines) {
            if (kind == OfficeFormKind::DeliveryConfirmation)
                line.orderNumber = header.value(11).toString();
            if (kind == OfficeFormKind::Inspection) {
                line.orderNumber = document.fields.value(QStringLiteral("purchaseOrderNumber"));
                line.supplier = document.fields.value(QStringLiteral("supplier"));
            }
        }
        QString syncError;
        if (!attachToDocument(document, database, operatorId, documentId, &syncError,
                              openArchivedFiles)) {
            allOk = false;
            if (errors) errors->append(QStringLiteral("%1：%2")
                                           .arg(formTitle(kind), syncError));
            continue;
        }
    }
    return allOk;
}
