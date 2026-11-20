#include "server/LegacyImport.h"
#include "core/SystemProfile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <cmath>
#include <limits>

namespace {
struct Problem { QString message; };
void require(bool condition, const QString &message)
{
    if (!condition) throw Problem{message};
}

QString quoteName(QString name)
{
    return '"' + name.replace('"', "\"\"") + '"';
}

QSqlQuery read(QSqlDatabase db, const QString &sql, const QVariantList &args = {})
{
    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.setNumericalPrecisionPolicy(QSql::HighPrecision);
    if (args.isEmpty()) {
        if (!q.exec(sql)) throw Problem{q.lastError().text()};
    } else {
        if (!q.prepare(sql)) throw Problem{q.lastError().text()};
        for (const auto &arg : args) q.addBindValue(arg);
        if (!q.exec()) throw Problem{q.lastError().text()};
    }
    return q;
}

QVariant scalar(QSqlDatabase db, const QString &sql, const QVariantList &args = {})
{
    auto q = read(db, sql, args);
    require(q.next(), QStringLiteral("无法读取校验结果。"));
    return q.value(0);
}

void checkReadEnd(const QSqlQuery &q)
{
    if (q.lastError().isValid()) throw Problem{q.lastError().text()};
}

struct Baseline {
    QStringList tables;
    QMap<QString, QList<QJsonObject>> seeds;
};

Baseline loadBaseline(const QString &product)
{
    QFile file(QStringLiteral(":/database/postgresql/business-schema.json"));
    require(file.open(QIODevice::ReadOnly), QStringLiteral("缺少业务数据库结构。"));
    const auto schema = QJsonDocument::fromJson(file.readAll()).object();
    const QRegularExpression create(QStringLiteral("^CREATE TABLE (\\w+) \\("));
    Baseline result;
    for (const auto &statement : schema.value("statements").toArray()) {
        const auto match = create.match(statement.toString());
        if (match.hasMatch()) result.tables << match.captured(1);
    }
    // Match the product-specific seed values installed by BusinessDatabase::initialize.
    for (const auto &row : schema.value("seeds").toArray()) {
        const auto seed = row.toObject();
        const auto table = seed.value("table").toString();
        auto values = seed.value("values").toObject();
        if (table == "system_identity") {
            values["profile_id"] = product;
            values["display_name"] = SystemProfile::displayName();
        } else if (table == "material_projects") {
            values["code"] = SystemProfile::defaultProjectCode();
            values["name"] = SystemProfile::defaultProjectName();
        } else if (table == "number_rules") {
            values["prefix"] = SystemProfile::defaultNumberPrefixes().value(
                values.value("document_type").toString(), values.value("prefix").toString());
        } else if (table == "users") {
            // The installer generates the password hash. A first login may update last_login_at.
            values.remove("password_hash");
            values.remove("last_login_at");
        }
        result.seeds[table].append(values);
    }
    require(!result.seeds.value("schema_migrations").isEmpty() && result.tables.contains("users"),
            QStringLiteral("无法识别业务数据库结构。"));
    return result;
}

void requireEmptyTarget(QSqlDatabase db, const Baseline &baseline)
{
    for (const auto &table : baseline.tables) {
        const auto &seeds = baseline.seeds.value(table);
        const auto from = " FROM " + quoteName(table);
        const auto occupied = QStringLiteral("目标库已有数据或自定义配置（%1），请使用空业务库。").arg(table);
        if (table == "audit_logs") {
            // Opening the v2 desktop once writes this exact bootstrap login record.
            require(!scalar(db, "SELECT EXISTS(SELECT 1 FROM audit_logs WHERE "
                        "NOT ((user_id=1 AND action='LOGIN' AND entity_type='user' AND entity_id=1 AND detail='') IS TRUE))").toBool(),
                    occupied);
            continue;
        }
        if (seeds.isEmpty()) {
            require(!scalar(db, "SELECT EXISTS(SELECT 1" + from + ")").toBool(), occupied);
            continue;
        }
        require(scalar(db, "SELECT count(*)" + from).toLongLong() == seeds.size(), occupied);
        for (const auto &seed : seeds) {
            QStringList predicates;
            QVariantList arguments;
            for (auto column = seed.begin(); column != seed.end(); ++column) {
                // The cast also makes comparison of citext columns case-sensitive.
                predicates << (quoteName(column.key()) + "::text IS NOT DISTINCT FROM CAST(? AS text)");
                arguments << column.value().toVariant().toString();
            }
            require(scalar(db, "SELECT count(*)" + from + " WHERE " + predicates.join(" AND "),
                           arguments).toInt() == 1, occupied);
        }
    }
    require(!scalar(db, "SELECT EXISTS(SELECT 1 FROM network_requests)").toBool(),
            QStringLiteral("目标库已有联网操作记录，请使用空业务库。"));
}

struct Column {
    QString name;
    QString type;
    bool identity;
};

QList<Column> columnsFor(QSqlDatabase target, QSqlDatabase source, const QString &table)
{
    auto query = read(target, "SELECT column_name,udt_name,is_identity FROM information_schema.columns "
                      "WHERE table_schema='wms_business' AND table_name=? ORDER BY ordinal_position", {table});
    QList<Column> columns;
    QSet<QString> names;
    while (query.next()) {
        Column column{query.value(0).toString(),query.value(1).toString(),query.value(2).toString()=="YES"};
        names.insert(column.name);
        columns.append(column);
    }
    checkReadEnd(query);
    auto sourceColumns = read(source, "PRAGMA table_xinfo(" + quoteName(table) + ")");
    QSet<QString> sourceNames;
    while (sourceColumns.next()) sourceNames.insert(sourceColumns.value(1).toString());
    checkReadEnd(sourceColumns);
    require(!names.isEmpty() && names == sourceNames, QStringLiteral("单机与联网表结构不一致：%1。").arg(table));
    return columns;
}

QVariant normalized(const QVariant &value, const Column &column, bool fromSqlite)
{
    if (!value.isValid() || value.isNull()) return QVariant();
    if (column.type == "bytea") {
        require(!fromSqlite || value.metaType().id()==QMetaType::QByteArray,
                QStringLiteral("附件内容类型异常：%1").arg(column.name));
        return value.toByteArray();
    }
    if (column.type == "int4" || column.type == "int8") {
        bool ok = false;
        const auto integer = value.toString().toLongLong(&ok);
        require(ok, QStringLiteral("无法无损转换整数字段：%1").arg(column.name));
        return integer;
    }
    if (column.type == "numeric") {
        if (fromSqlite && value.metaType().id() == QMetaType::Double) {
            require(std::isfinite(value.toDouble()), QStringLiteral("数量中含有非有限数值。"));
            // Shortest round-tripping text keeps ordinary quantities such as 0.1 exact
            // in PostgreSQL NUMERIC instead of expanding the binary floating-point tail.
            return QString::number(value.toDouble(),'g',QLocale::FloatingPointShortest);
        }
        return value.toString();
    }
    require(column.type == "text" || column.type == "citext", QStringLiteral("未知列类型：%1").arg(column.type));
    require(!fromSqlite || value.metaType().id() == QMetaType::QString,
            QStringLiteral("无法无损转换文本字段：%1").arg(column.name));
    const QString text = value.toString();
    require(!text.contains(QChar::Null), QStringLiteral("文本字段中含有零字符：%1").arg(column.name));
    return text;
}

struct TableResult {
    qint64 rows = 0;
    qint64 largestId = 0;
    bool identity = false;
};

TableResult copyTable(QSqlDatabase target, QSqlDatabase source, const QString &table)
{
    const auto columns = columnsFor(target, source, table);
    QStringList fields, marks;
    for (const auto &column : columns) {
        fields << quoteName(column.name);
        marks << "?";
    }
    const auto names = fields.join(',');
    auto rows = read(source, "SELECT " + names + " FROM " + quoteName(table));
    QSqlQuery insert(target);
    insert.setNumericalPrecisionPolicy(QSql::HighPrecision);
    if (!insert.prepare("INSERT INTO " + quoteName(table) + " (" + names + ") VALUES (" +
                        marks.join(',') + ") RETURNING " + names)) throw Problem{insert.lastError().text()};
    TableResult result;
    for (const auto &column : columns) result.identity |= column.identity;
    while (rows.next()) {
        QVariantList expected;
        for (qsizetype i=0; i<columns.size(); ++i) {
            const auto value = normalized(rows.value(i), columns[i], true);
            insert.bindValue(i, value);
            expected << value;
            if (columns[i].identity) result.largestId = qMax(result.largestId, value.toLongLong());
        }
        if (!insert.exec() || !insert.next())
            throw Problem{QStringLiteral("导入 %1 第 %2 行失败：%3").arg(table).arg(result.rows+1).arg(insert.lastError().databaseText().section('\n',0,0))};
        for (qsizetype i=0; i<columns.size(); ++i) {
            const auto actual = normalized(insert.value(i), columns[i], false);
            bool same = actual.isNull() == expected[i].isNull() && actual == expected[i];
            // PostgreSQL can expand exponential notation to decimal for NUMERIC.
            if (!same && !actual.isNull() && columns[i].type == "numeric" &&
                rows.value(i).metaType().id() == QMetaType::Double) {
                bool ok = false;
                const double number = actual.toDouble(&ok);
                same = ok && number == rows.value(i).toDouble();
            }
            require(same, QStringLiteral("写入后校验不匹配：%1.%2，第 %3 行。")
                          .arg(table,columns[i].name).arg(result.rows+1));
        }
        ++result.rows;
        insert.finish();
    }
    checkReadEnd(rows);
    require(result.rows == scalar(target,"SELECT count(*) FROM " + quoteName(table)).toLongLong(),
            QStringLiteral("行数校验失败：%1。").arg(table));
    return result;
}
} // namespace

bool LegacyImport::run(QSqlDatabase target, QSqlDatabase source, const QString &product,
                       QString *report, QString *error)
{
    if (report) report->clear();
    if (error) error->clear();
    bool transaction = false;
    try {
        require(target.isOpen() && source.isOpen() && target.driverName()=="QPSQL" && source.driverName()=="QSQLITE",
                QStringLiteral("导入需要单机快照和联网 PostgreSQL 连接。"));
        require(product==SystemProfile::profileId(), QStringLiteral("产品配置不匹配。"));
        const auto baseline = loadBaseline(product);
        QSet<QString> sourceTables;
        auto schema = read(source, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT GLOB 'sqlite_*'");
        while (schema.next()) sourceTables.insert(schema.value(0).toString());
        checkReadEnd(schema);
        require(sourceTables == QSet<QString>(baseline.tables.cbegin(),baseline.tables.cend()),
                QStringLiteral("单机表与受支持的版本不一致，未执行导入。"));
        require(scalar(source, "SELECT profile_id FROM system_identity WHERE singleton_id=1").toString()==product &&
                scalar(source, "PRAGMA application_id").toInt()==SystemProfile::databaseApplicationId(),
                QStringLiteral("单机数据库产品身份不匹配。"));
        require(scalar(source, "SELECT count(*) FROM users u JOIN roles r ON u.role_id=r.id WHERE "
                      "r.code='ADMIN' AND u.is_active=1").toInt()>0, QStringLiteral("单机数据库没有可用的管理员。"));

        require(target.transaction(), QStringLiteral("无法开始导入事务。"));
        transaction = true;
        read(target, "SET LOCAL search_path=wms_business,public,pg_catalog");
        read(target, "SET LOCAL lock_timeout='5s'");
        read(target, "SET LOCAL statement_timeout='120s'");
        require(scalar(target, "SELECT pg_try_advisory_xact_lock(1977011602)").toBool(),
                QStringLiteral("目标库正在写入，请停止该系统后台并重试。"));
        QStringList tables;
        for (const auto &table : baseline.tables) tables << quoteName(table);
        read(target, "LOCK TABLE " + tables.join(',') + ",network_identity,network_requests IN ACCESS EXCLUSIVE MODE");
        require(scalar(target, "SELECT count(*)=1 AND bool_and(product=? AND version=2) FROM network_identity",
                       {product}).toBool(),
                QStringLiteral("联网目标库产品或版本不匹配。"));
        require(scalar(target, "SELECT to_regclass('wms_business.legacy_import_history') IS NULL").toBool(),
                QStringLiteral("此目标库已经完成过单机导入，重复执行已拒绝。"));
        auto targetSchema = read(target, "SELECT table_name FROM information_schema.tables "
                                 "WHERE table_schema='wms_business' AND table_type='BASE TABLE'");
        QSet<QString> targetTables;
        while (targetSchema.next()) targetTables.insert(targetSchema.value(0).toString());
        checkReadEnd(targetSchema);
        auto expectedTables = sourceTables;
        expectedTables.insert("network_identity");
        expectedTables.insert("network_requests");
        require(targetTables == expectedTables, QStringLiteral("联网业务库的表与受支持的版本不一致。"));
        requireEmptyTarget(target,baseline);
        const QStringList actorTables{"audit_logs","attachments","inspection_notices","material_images"};
        // Only turn off WMS's actor-stamping triggers, inside this locked transaction.
        // Keep all FK and check constraints, preserving original authors and timestamps.
        for (const auto &table : actorTables)
            read(target, "ALTER TABLE " + quoteName(table) + " DISABLE TRIGGER authenticated_actor");
        read(target, "SET CONSTRAINTS ALL DEFERRED");
        read(target, "TRUNCATE TABLE " + tables.join(','));
        QJsonObject counts;
        QMap<QString,qint64> nextId;
        qint64 total = 0;
        for (const auto &table : baseline.tables) {
            const auto copied = copyTable(target,source,table);
            counts.insert(table,copied.rows);
            total += copied.rows;
            if (copied.identity) {
                const qint64 sqliteHigh = scalar(source,
                    "SELECT coalesce(max(seq),0) FROM sqlite_sequence WHERE name=?", {table}).toLongLong();
                const qint64 high = qMax(sqliteHigh,copied.largestId);
                require(high<std::numeric_limits<qint64>::max(), QStringLiteral("编号空间已耗尽：%1").arg(table));
                nextId.insert(table,high+1);
            }
        }
        read(target, "SET CONSTRAINTS ALL IMMEDIATE");
        for (const auto &table : actorTables)
            read(target, "ALTER TABLE " + quoteName(table) + " ENABLE TRIGGER authenticated_actor");
        for (auto i=nextId.cbegin(); i!=nextId.cend(); ++i) {
            // ALTER ... RESTART is transactional. setval() would survive a rollback.
            read(target, "ALTER TABLE " + quoteName(i.key()) + " ALTER COLUMN id RESTART WITH " + QString::number(i.value()));
        }
        read(target, "CREATE TABLE legacy_import_history (singleton integer PRIMARY KEY CHECK(singleton=1), "
                     "product text NOT NULL, rows_imported bigint NOT NULL, imported_at timestamptz NOT NULL DEFAULT now())");
        read(target, "INSERT INTO legacy_import_history(singleton,product,rows_imported) VALUES(1,?,?)",{product,total});
        require(target.commit(), QStringLiteral("未能确认导入提交，请核对目标库后再试。"));
        transaction = false;
        if (report) *report = QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"product",product},{"tables",counts},{"totalRows",total}}).toJson(QJsonDocument::Compact));
        return true;
    } catch (const Problem &problem) {
        if (transaction) target.rollback();
        if (error) *error=problem.message;
        return false;
    }
}
