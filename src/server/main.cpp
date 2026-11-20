#include "server/LanDatabase.h"
#include "server/ServerStartup.h"
#include "server/LanServer.h"
#include "server/BusinessApi.h"
#include "server/LegacyImport.h"
#include "server/LegacySource.h"
#include "core/SystemProfile.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("WmsServer");
    QCommandLineParser parser;
    parser.setApplicationDescription("WMS PostgreSQL business server");
    parser.addHelpOption();
    parser.addOptions({{"product", "ICE_BEAUTY or MEI_JING_LING", "product", "ICE_BEAUTY"},
        {"listen", "Listen address", "address", "127.0.0.1"},
        {"port", "HTTP(S) port (IceBeauty 18080, MeiJingLing 18081)", "port"},
        {"init-demo", "Initialize a new test schema and exit"},
        {"init-lan-empty", "Initialize the legacy LAN schema without sample inventory and exit"},
        {"init-business", "Initialize the complete business schema and exit"},
        {"import-legacy", "Import a legacy SQLite database into an empty business database and exit", "path"},
        {"allow-insecure-lan", "Allow HTTP over a trusted test LAN"},
        {"tls-cert", "PEM server certificate", "path"},
        {"tls-key", "PEM private key", "path"}});
    parser.process(app);
    const bool demo = parser.isSet("init-demo");
    const bool empty = parser.isSet("init-lan-empty");
    const bool importLegacy = parser.isSet("import-legacy");
    if (int(demo) + int(empty) + int(parser.isSet("init-business")) + int(importLegacy) > 1) {
        QTextStream(stderr) << "Choose only one initialization option\n";
        return 1;
    }
    const auto product = parser.value("product");
    if (product != "ICE_BEAUTY" && product != "MEI_JING_LING") {
        QTextStream(stderr) << "Unknown product\n"; return 1;
    }
    LanDatabase database(product);
    SystemProfile::setServerProfile(product);
    QString error;
    if (!ServerStartup::prepare(product, &error)) { QTextStream(stderr) << error << '\n'; return 2; }
    if (!database.open(&error)) { QTextStream(stderr) << error << '\n'; return 2; }
    if (!demo && !empty && !parser.isSet("init-business") &&
        !BusinessDatabase::upgradeProcessBoms(database.connection(), product, &error)) {
        QTextStream(stderr) << error << '\n'; return 4;
    }
    if (importLegacy) {
        LegacySource source;
        QString report;
        if (!source.prepare(parser.value("import-legacy"), product, &error)
            || !LegacyImport::run(database.connection(), source.database(), product, &report, &error)) {
            QTextStream(stderr) << error << '\n'; return 8;
        }
        QTextStream(stdout) << report << Qt::endl;
        return 0;
    }
    if(parser.isSet("init-business")) {
        if(!BusinessDatabase::initialize(database.connection(),product,qEnvironmentVariable("WMS_DEMO_ADMIN_PASSWORD"),&error)) {
            QTextStream(stderr)<<error<<'\n';return 3;
        }
        QTextStream(stdout)<<"Initialized full business schema for "<<product<<'\n';return 0;
    }
    if (demo || empty) {
        if (!database.initialize(qEnvironmentVariable("WMS_DEMO_ADMIN_PASSWORD"), demo, &error)) {
            QTextStream(stderr) << error << '\n'; return 3;
        }
        QTextStream(stdout) << "Initialized LAN schema for " << product << '\n'; return 0;
    }
    if (!database.verify(&error)) { QTextStream(stderr) << error << '\n'; return 4; }
    bool portOk = false;
    const QString portValue = parser.isSet("port") ? parser.value("port")
        : product == QStringLiteral("MEI_JING_LING") ? QStringLiteral("18081") : QStringLiteral("18080");
    const int port = portValue.toInt(&portOk);
    const QHostAddress address(parser.value("listen"));
    if (!portOk || port < 1 || port > 65535 || address.isNull()) { QTextStream(stderr) << "Invalid listen address/port\n"; return 5; }
    LanServer server(database);
    BusinessApi business(server.http(),database.connection(),product);
    const bool tls = parser.isSet("tls-cert") || parser.isSet("tls-key");
    if (tls) {
        QFile certFile(parser.value("tls-cert")), keyFile(parser.value("tls-key"));
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) return 6;
        QSslCertificate cert(certFile.readAll());
        QSslKey key(keyFile.readAll(), QSsl::Rsa);
        if (cert.isNull() || key.isNull()) { QTextStream(stderr) << "Invalid PEM certificate/RSA key\n"; return 6; }
        server.http().sslSetup(cert, key);
    } else if (!address.isLoopback() && !parser.isSet("allow-insecure-lan")) {
        QTextStream(stderr) << "Non-loopback requires TLS or explicit --allow-insecure-lan for test data\n"; return 6;
    }
    if (!server.http().listen(address, static_cast<quint16>(port))) {
        QTextStream(stderr) << "Cannot listen on port " << port
                            << ". Close old WMS listeners with the one-click launcher or scripts/stop-wms-listeners.ps1.\n"; return 7;
    }
    QTextStream(stdout) << "WmsServer ready " << (tls ? "https" : "http") << "://"
                        << address.toString() << ':' << port << " product=" << product << Qt::endl;
    return app.exec();
}
