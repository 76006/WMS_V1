#pragma once

#include "services/ProcessBom.h"

#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <QList>

// A task keeps its own BOM snapshot. Node keys identify occurrences, not just materials.
struct ProductionStage {
    QString key;
    QString parent;
    qlonglong materialId = 0;
    QString code, name, model, category;
    double quantity = 1;
    double perMachine = 1;
    QList<ProcessBomUsage> processMaterials;
    QString issueError;
};

struct ProductionDemand {
    qlonglong materialId = 0;
    QString code, name;
    double unitUsage = 0;
    double required = 0;
    double issued = 0;
    double remaining() const { return qMax(0.0, required - issued); }
};

class ProductionTask {
public:
    static bool load(QSqlDatabase db, qlonglong runId, qlonglong productId,
                     ProductionTask &task, QString *error);
    QString payload() const;
    QString fingerprint() const;
    QList<ProductionStage> issueStages() const;
    QList<ProductionStage> receiptStages() const;
    bool isReceiptStage(const QString &key) const;
    bool usesProcessBom() const { return m_usesProcessBom; }
    const ProductionStage *stage(const QString &key) const;
    QString stagePath(const QString &key) const;
    bool demand(QSqlDatabase db, qlonglong runId, const QString &key, double count,
                QList<ProductionDemand> &result, QString *error) const;
    static double received(QSqlDatabase db, qlonglong runId, const QString &key, QString *error);
private:
    void expandMaterialDemand(const QString &parent, double multiplier,
                              QMap<qlonglong, ProductionDemand> &grouped) const;
    QList<ProductionStage> m_nodes;
    bool m_usesProcessBom = false;
    QList<ProductionStage> m_processStages;
};
