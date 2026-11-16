#include "core/SystemProfile.h"

#include <QDir>
#include <QStandardPaths>

bool SystemProfile::isMeiJingLing()
{
#if defined(WMS_PROFILE_MEI_JING_LING)
    return true;
#else
    return false;
#endif
}

QString SystemProfile::profileId()
{
    return isMeiJingLing() ? QStringLiteral("MEI_JING_LING")
                           : QStringLiteral("ICE_BEAUTY");
}

QString SystemProfile::displayName()
{
    return isMeiJingLing() ? QStringLiteral("美鲸灵") : QStringLiteral("冰美肌");
}

QString SystemProfile::windowTitle()
{
    return displayName() + QStringLiteral("库存管理");
}

QString SystemProfile::organizationName()
{
    return isMeiJingLing() ? QStringLiteral("MeiJingLing") : QStringLiteral("IceBeauty");
}

QString SystemProfile::applicationName()
{
    return isMeiJingLing() ? QStringLiteral("MeiJingLingWms")
                           : QStringLiteral("IceBeautyWms");
}

QString SystemProfile::databaseFileName()
{
    return isMeiJingLing() ? QStringLiteral("mei_jing_ling_wms.db")
                           : QStringLiteral("ice_beauty_wms.db");
}

int SystemProfile::databaseApplicationId()
{
    // SQLite 文件头 application_id："MJWM" / "IBWM"。
    // 即使身份表被误删，另一套程序也会在迁移和业务写入前拒绝打开。
    return isMeiJingLing() ? 0x4D4A574D : 0x4942574D;
}

QString SystemProfile::templateDirectoryName()
{
    // 冰美肌继续沿用现有目录，避免升级后找不到已经投入使用的模板。
    return isMeiJingLing() ? QStringLiteral("美鲸灵仓库系统表单模板")
                           : QStringLiteral("仓库系统表单模板");
}

QString SystemProfile::archiveDirectoryName()
{
    return displayName() + QStringLiteral("仓库系统表单");
}

QString SystemProfile::inspectionNumberStem()
{
    return isMeiJingLing() ? QStringLiteral("MJL-JY") : QStringLiteral("BMJ-JY");
}

QString SystemProfile::batchNumberPrefix()
{
    return isMeiJingLing() ? QStringLiteral("MJL") : QStringLiteral("SM");
}

QString SystemProfile::defaultProjectCode()
{
    return isMeiJingLing() ? QStringLiteral("MG01") : QStringLiteral("SM01");
}

QString SystemProfile::defaultProjectName()
{
    return defaultProjectCode() + QStringLiteral("项目");
}

QString SystemProfile::temporaryPath(const QString &childDirectory)
{
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(applicationName());
    return childDirectory.trimmed().isEmpty()
        ? root : QDir(root).filePath(childDirectory);
}

QMap<QString, QString> SystemProfile::defaultNumberPrefixes()
{
    QMap<QString, QString> values = {
        {QStringLiteral("CGRK"), QStringLiteral("CGRK")},
        {QStringLiteral("SCWG"), QStringLiteral("SCWG")},
        {QStringLiteral("TLRK"), QStringLiteral("TLRK")},
        {QStringLiteral("QTRK"), QStringLiteral("QTRK")},
        {QStringLiteral("QC"), QStringLiteral("QC")},
        {QStringLiteral("SCLL"), QStringLiteral("SMLL")},
        {QStringLiteral("SCTL"), QStringLiteral("SMTL")},
        {QStringLiteral("CPRK"), QStringLiteral("CPRK")},
        {QStringLiteral("XSCK"), QStringLiteral("XSCK")},
        {QStringLiteral("SHCK"), QStringLiteral("SHCK")},
        {QStringLiteral("YFCK"), QStringLiteral("YFCK")},
        {QStringLiteral("WXLY"), QStringLiteral("WXLY")},
        {QStringLiteral("YPLY"), QStringLiteral("YPLY")},
        {QStringLiteral("QTCK"), QStringLiteral("QTCK")},
        {QStringLiteral("DB"), QStringLiteral("DB")},
        {QStringLiteral("PD"), QStringLiteral("PD")},
        {QStringLiteral("CX"), QStringLiteral("CX")}
    };
    if (isMeiJingLing()) {
        for (auto it = values.begin(); it != values.end(); ++it)
            it.value().prepend(QStringLiteral("MJL"));
        // 《编码规则.xls》明确规定美鲸灵领料单号为
        // LL + 年月日 + “-” + 两位流水号。横线和流水号位数由编号服务处理。
        values[QStringLiteral("SCLL")] = QStringLiteral("LL");
    }
    return values;
}
