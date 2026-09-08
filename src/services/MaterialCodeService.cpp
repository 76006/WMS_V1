#include "services/MaterialCodeService.h"

#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

namespace {
void setCodeError(QString *target, const QString &message)
{
    if (target) *target = message;
}
}

QString MaterialCodeService::normalizeProjectCode(const QString &projectCode)
{
    return projectCode.trimmed().toUpper();
}

bool MaterialCodeService::isValidProjectCode(const QString &projectCode)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Z][A-Z0-9]{1,7}$"));
    return expression.match(normalizeProjectCode(projectCode)).hasMatch();
}

bool MaterialCodeService::isValidMaterialType(const QString &materialType)
{
    const QString value = materialType.trimmed().toUpper();
    return value == QStringLiteral("M") || value == QStringLiteral("P")
        || value == QStringLiteral("O");
}

bool MaterialCodeService::isValidDisciplineCode(const QString &disciplineCode)
{
    const QString value = disciplineCode.trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("2")
        || value == QStringLiteral("9");
}

QString MaterialCodeService::nextCode(QSqlDatabase database,
                                      const QString &materialType,
                                      const QString &projectCode,
                                      const QString &disciplineCode,
                                      QString *errorMessage)
{
    const QString type = materialType.trimmed().toUpper();
    const QString project = normalizeProjectCode(projectCode);
    const QString discipline = disciplineCode.trimmed();
    if (!database.isOpen()) {
        setCodeError(errorMessage, QStringLiteral("数据库尚未连接。"));
        return {};
    }
    if (!isValidMaterialType(type) || !isValidProjectCode(project)
        || !isValidDisciplineCode(discipline)) {
        setCodeError(errorMessage, QStringLiteral("物料类型、项目代码或专业类别无效。"));
        return {};
    }

    const QString prefix = type + project + discipline;
    const QRegularExpression exact(QStringLiteral("^%1(\\d{3})$")
                                       .arg(QRegularExpression::escape(prefix)),
                                   QRegularExpression::CaseInsensitiveOption);
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT code FROM materials WHERE code LIKE ? COLLATE NOCASE"));
    query.addBindValue(prefix + QStringLiteral("%"));
    if (!query.exec()) {
        setCodeError(errorMessage, QStringLiteral("读取物料流水号失败：%1")
                                       .arg(query.lastError().text()));
        return {};
    }
    int maximum = 0;
    while (query.next()) {
        const QRegularExpressionMatch match = exact.match(query.value(0).toString());
        if (match.hasMatch()) maximum = qMax(maximum, match.captured(1).toInt());
    }
    if (maximum >= 999) {
        setCodeError(errorMessage, QStringLiteral("编号前缀 %1 的三位流水号已经用完。")
                                       .arg(prefix));
        return {};
    }
    return prefix + QStringLiteral("%1").arg(maximum + 1, 3, 10, QLatin1Char('0'));
}
