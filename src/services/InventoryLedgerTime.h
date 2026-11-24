#pragma once

#include <QString>

inline QString ledgerTimeOrder(const QString &column)
{
    // SQLite 默认时间是本地时间。只有旧流水显式带有时区时才转换。
    return QStringLiteral(
        "strftime('%Y-%m-%d %H:%M:%f',%1,CASE WHEN substr(%1,-1)='Z' "
        "OR instr(substr(%1,20),'+')>0 OR instr(substr(%1,20),'-')>0 "
        "THEN 'localtime' ELSE '+0 seconds' END)").arg(column);
}
