#include "core/SystemProfile.h"
#include "core/Session.h"
#include "network/RemoteBackend.h"
#include "services/OfficeTemplateService.h"
#include "ui/pages/StockInPage.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/StockLineTable.h"
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>

static void require(bool ok, const QString &error) {
    if (!ok) throw std::runtime_error(error.toStdString());
}
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    SystemProfile::setServerProfile(qEnvironmentVariable("WMS_SUPPLIER_PRODUCT"));
    SystemProfile::setNetworkClientMode(true);
    QStringList archives;
    int result=0;
    try {
        auto db=RemoteBackend::connect(QUrl(qEnvironmentVariable("WMS_SUPPLIER_URL")),qgetenv("WMS_SUPPLIER_TOKEN"));
        Session session;session.userId=1;session.username="admin";session.displayName="Supplier test";session.roleCode="ADMIN";
        const auto ids=qEnvironmentVariable("WMS_SUPPLIER_MATERIALS").split(',');
        StockMovementRequest existing;existing.materialId=ids[0].toLongLong();existing.batchNo="SUPPLIER-0";
        QString error;
        auto material=OfficeTemplateService::materialLines(db,{existing},&error);
        require(material.size()==1 && material[0].supplier==QStringLiteral("原批次供应商0"),"Batch supplier: "+error);
        existing.supplier=QStringLiteral("送检供应商");
        material=OfficeTemplateService::materialLines(db,{existing},&error);
        require(material.size()==1 && material[0].supplier==existing.supplier,"Inspection supplier precedence");
        StockInPage page(db,session);
        QComboBox *type=nullptr;
        for(auto *combo:page.findChildren<QComboBox *>())if(combo->findData("TLRK")>=0)type=combo;
        require(type,"Missing document type");type->setCurrentIndex(type->findData("TLRK"));
        QLineEdit *supplier=nullptr;
        for(auto *edit:page.findChildren<QLineEdit *>())if(edit->placeholderText().contains(QStringLiteral("供应商默认值")))supplier=edit;
        require(supplier && supplier->isEnabled(),"Return inbound supplier disabled");supplier->setText(QStringLiteral("整单默认供应商"));
        auto *source=page.findChild<StockLineTable *>();
        auto *table=source->findChild<QTableWidget *>();
        while(table->rowCount()<2)source->addLine();
        for(int row=0;row<2;++row) {
            auto *combo=qobject_cast<QComboBox *>(table->cellWidget(row,0));
            combo->setCurrentIndex(combo->findData(ids[row].toLongLong()));
            qobject_cast<QComboBox *>(table->cellWidget(row,3))->setCurrentText(QString("SUPPLIER-%1").arg(row));
            qobject_cast<QDoubleSpinBox *>(table->cellWidget(row,7))->setValue(row+2);
        }
        bool posted=false,edited=false;
        QStringList failures;
        QObject::connect(&page,&StockInPage::stockChanged,[&]{posted=true;});
        QTimer timer;
        QObject::connect(&timer,&QTimer::timeout,[&]{
            for(auto *widget:QApplication::topLevelWidgets()) {
                if(!widget->isVisible())continue;
                if(auto *dialog=qobject_cast<DocumentTemplateDialog *>(widget)) {
                    auto *lines=dialog->findChild<QTableWidget *>();
                    if(lines->columnCount()!=10 || lines->item(0,8)->text()!=QStringLiteral("整单默认供应商"))failures<<"Supplier defaults/column";
                    if(!(lines->item(0,8)->flags() & Qt::ItemIsEditable) || (lines->item(0,7)->flags() & Qt::ItemIsEditable))failures<<"Editable flags";
                    lines->item(0,8)->setText(QStringLiteral("逐行供应商甲"));
                    lines->item(1,8)->setText(QStringLiteral("逐行供应商乙"));
                    edited=true;QMetaObject::invokeMethod(dialog,"acceptForm",Qt::DirectConnection);
                } else if(auto *box=qobject_cast<QMessageBox *>(widget)) {
                    if(box->icon()==QMessageBox::Question)box->button(QMessageBox::Yes)->click();
                    else {if(box->icon()!=QMessageBox::Information)failures<<box->text();box->button(QMessageBox::Ok)->click();}
                }
            }
        });timer.start(20);
        QMetaObject::invokeMethod(&page,"submit",Qt::DirectConnection);timer.stop();
        QSqlQuery query(db);
        require(query.exec("SELECT id,document_no,supplier FROM business_documents WHERE document_type='TLRK' ORDER BY id DESC LIMIT 1") && query.next(),"Missing posted document");
        auto id=query.value(0).toLongLong();
        OfficeTemplateDocument form;form.kind=OfficeFormKind::RawMaterialInbound;form.documentNumber=query.value(1).toString();
        require(form.documentNumber.startsWith("SUPTEST"),"Unsafe test document prefix");
        archives<<OfficeTemplateService::archiveFilePath(form);
        require(posted && edited && failures.isEmpty(),failures.join('\n'));
        require(query.value(2).toString()==QStringLiteral("整单默认供应商"),"Return header supplier not saved");
        const auto checkPayload=[&](const QString &first,const QString &second) {
            QSqlQuery q(db);q.prepare("SELECT payload,status FROM document_forms WHERE document_id=? AND form_kind='rawInbound'");q.addBindValue(id);
            require(q.exec() && q.next(),"Missing form payload");
            require(q.value(1).toString()=="COMPLETED","Form attachment not completed");
            const auto data=QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object().value("lines").toArray();
            require(data.size()==2 && data[0].toObject().value("supplier").toString()==first && data[1].toObject().value("supplier").toString()==second,"Saved suppliers mismatch");
        };
        checkPayload(QStringLiteral("逐行供应商甲"),QStringLiteral("逐行供应商乙"));
        require(query.exec("SELECT supplier FROM batches ORDER BY material_id") && query.next() && query.value(0).toString()==QStringLiteral("逐行供应商甲") && query.next() && query.value(0).toString()==QStringLiteral("逐行供应商乙"),"Batch suppliers not saved");
        QStringList errors;
        InventoryService service(db,1);
        PostedDocumentEdit edit;
        require(service.loadPostedDocument(id,&edit,&error),error);
        edit.notes="Revised test note";
        require(service.revisePostedDocument(edit,&error),error);
        require(query.exec("SELECT supplier FROM batches ORDER BY material_id") && query.next() && query.value(0).toString()==QStringLiteral("逐行供应商甲") && query.next() && query.value(0).toString()==QStringLiteral("逐行供应商乙"),"Unrelated revision replaced per-line suppliers");
        require(OfficeTemplateService::synchronizeDocumentForms(db,1,id,&errors,false),errors.join('\n'));
        checkPayload(QStringLiteral("逐行供应商甲"),QStringLiteral("逐行供应商乙"));
        require(service.loadPostedDocument(id,&edit,&error),error);
        edit.supplier=QStringLiteral("修订后供应商");
        require(service.revisePostedDocument(edit,&error),error);
        require(OfficeTemplateService::synchronizeDocumentForms(db,1,id,&errors,false),errors.join('\n'));
        checkPayload(edit.supplier,edit.supplier);
        QTextStream(stdout)<<"PASS: actual inbound UI, supplier defaults, editable per-line values, database, form attachment and regeneration\n";
    } catch(const std::exception &e) {QTextStream(stderr)<<e.what()<<'\n';result=1;}
    for(const auto &path:archives)QFile::remove(path);
    return result;
}
