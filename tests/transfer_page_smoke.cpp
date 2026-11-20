#include "core/SystemProfile.h"
#include "core/Session.h"
#include "network/RemoteBackend.h"
#include "ui/pages/TransferPage.h"
#include "ui/widgets/StockLineTable.h"
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc,argv);
    SystemProfile::setServerProfile(qEnvironmentVariable("WMS_TRANSFER_TEST_PRODUCT"));
    auto db=RemoteBackend::connect(QUrl(qEnvironmentVariable("WMS_TRANSFER_TEST_URL")),
                                   qgetenv("WMS_TRANSFER_TEST_TOKEN"));
    Session session;
    session.userId=1;
    session.username="admin";
    session.displayName="Transfer UI test";
    session.roleCode="ADMIN";
    TransferPage page(db,session);
    auto *source=page.findChild<StockLineTable *>();
    auto *table=source ? source->findChild<QTableWidget *>() : nullptr;
    auto *target=page.findChild<QComboBox *>("transferTargetWarehouse");
    const auto materials=qEnvironmentVariable("WMS_TRANSFER_TEST_MATERIALS").split(',');
    if (!table || !target || materials.size()!=2) return 1;
    const auto destination=qEnvironmentVariable("WMS_TRANSFER_TEST_TARGET").toLongLong();
    target->setCurrentIndex(target->findData(destination));
    if (target->currentIndex()<0) return 2;
    source->clearLines();
    while(table->rowCount()<2)source->addLine();
    for(int row=0;row<2;++row) {
        auto *material=qobject_cast<QComboBox *>(table->cellWidget(row,0));
        auto *quantity=qobject_cast<QDoubleSpinBox *>(table->cellWidget(row,7));
        if (!material || !quantity) return 3;
        material->setCurrentIndex(material->findData(materials[row].toLongLong()));
        if(material->currentIndex()<=0)return 4;
        quantity->setValue(row==0?5:7);
    }
    QString error;
    if(source->lines(&error).size()!=2) {QTextStream(stderr)<<error;return 5;}
    int confirmations=0,successes=0;
    QStringList failures;
    bool posted=false;
    QObject::connect(&page,&TransferPage::stockChanged,[&]{posted=true;});
    QTimer dialogs;
    QObject::connect(&dialogs,&QTimer::timeout,[&]{
        for(auto *widget:QApplication::topLevelWidgets()) {
            auto *box=qobject_cast<QMessageBox *>(widget);
            if(!box || !box->isVisible())continue;
            if(box->icon()==QMessageBox::Question) {++confirmations;box->button(QMessageBox::Yes)->click();}
            else {if(box->icon()==QMessageBox::Information)++successes;else failures<<box->text();box->button(QMessageBox::Ok)->click();}
        }
    });
    dialogs.start(10);
    QMetaObject::invokeMethod(&page,"submit",Qt::DirectConnection);
    dialogs.stop();
    if(!posted || confirmations!=1 || successes!=1 || !failures.isEmpty()) {
        QTextStream(stderr)<<"posted="<<posted<<" confirmations="<<confirmations<<" successes="<<successes<<'\n'<<failures.join('\n');return 6;
    }
    QSqlQuery q(db);
    if(!q.exec("SELECT id FROM business_documents WHERE document_type='DB' ORDER BY id DESC LIMIT 1") || !q.next())return 7;
    const auto id=q.value(0).toLongLong();
    q.prepare("SELECT count(*),sum(quantity) FROM business_document_items WHERE document_id=?");
    q.addBindValue(id);
    if(!q.exec() || !q.next() || q.value(0).toInt()!=2 || q.value(1).toDouble()!=12)return 8;
    QTableWidget *recent=nullptr;
    for(auto *candidate:page.findChildren<QTableWidget *>())
        if(candidate->property("businessDocumentTable").toBool())recent=candidate;
    if(!recent || recent->rowCount()<2 || recent->item(0,0)->text()!=recent->item(1,0)->text())return 9;
    QTextStream(stdout)<<"PASS: actual multi-line TransferPage confirmation, submission and recent detail rows\n";
    return 0;
}
