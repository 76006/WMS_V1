#include "services/OfficeTemplateService.h"

#include "services/AttachmentService.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
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

QJsonObject lineToJson(const OfficeTemplateLine &line)
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
    case OfficeFormKind::Inspection: return QStringLiteral("送检单");
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

    QJsonObject root;
    root.insert(QStringLiteral("kind"), formCode(document.kind));
    root.insert(QStringLiteral("sheetName"), sheetName(document.kind));
    root.insert(QStringLiteral("outputPath"), QDir::toNativeSeparators(outputPath));
    root.insert(QStringLiteral("documentNumber"), document.documentNumber);
    root.insert(QStringLiteral("documentDate"),
                document.documentDate.toString(QStringLiteral("yyyy-MM-dd")));
    QJsonObject fields;
    for (auto it = document.fields.cbegin(); it != document.fields.cend(); ++it) {
        if (!isHandwrittenSignatureField(it.key())) fields.insert(it.key(), it.value());
    }
    root.insert(QStringLiteral("fields"), fields);
    QJsonArray lines;
    for (const OfficeTemplateLine &line : document.lines) lines.append(lineToJson(line));
    root.insert(QStringLiteral("lines"), lines);

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

bool OfficeTemplateService::attachToDocument(const OfficeTemplateDocument &document,
                                             QSqlDatabase database,
                                             qlonglong operatorId,
                                             qlonglong businessDocumentId,
                                             QString *errorMessage)
{
    QTemporaryDir directory;
    if (!directory.isValid()) {
        setError(errorMessage, QStringLiteral("无法创建表单临时目录。"));
        return false;
    }
    const QString fileName = outputFileName(document);
    const QString path = directory.filePath(fileName);
    if (!renderToFile(document, path, errorMessage)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法读取已生成的表单：%1").arg(file.errorString()));
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();
    const QString mime = QFileInfo(path).suffix().compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
        : QStringLiteral("application/vnd.ms-excel");

    QStringList saveErrors;
    AttachmentService attachments(database, operatorId);
    QString databaseError;
    if (!attachments.uploadDocumentAttachment(businessDocumentId, fileName, mime, data,
                                              nullptr, &databaseError)) {
        saveErrors.append(QStringLiteral("数据库附件：%1").arg(databaseError));
    }

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
        if (!formDirectory.isEmpty() && QDir(formDirectory).exists()) {
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

    if (!archivedPath.isEmpty()
        && !QDesktopServices::openUrl(QUrl::fromLocalFile(archivedPath))) {
        saveErrors.append(QStringLiteral("表单已保存，但 Windows 无法自动打开：%1")
                              .arg(QDir::toNativeSeparators(archivedPath)));
    }

    if (!saveErrors.isEmpty()) {
        setError(errorMessage, saveErrors.join(QStringLiteral("\n")));
        return false;
    }
    return true;
}
