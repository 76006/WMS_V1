#include "server/LanDatabase.h"
#include "server/LanServer.h"
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
    parser.setApplicationDescription("WMS PostgreSQL LAN query pilot (no production stock posting)");
    parser.addHelpOption();
    parser.addOptions({{"product", "ICE_BEAUTY or MEI_JING_LING", "product", "ICE_BEAUTY"},
        {"listen", "Listen address", "address", "127.0.0.1"},
        {"port", "HTTP(S) port", "port", "18080"},
        {"init-demo", "Initialize a new test schema and exit"},
        {"allow-insecure-lan", "Allow HTTP over a trusted test LAN"},
        {"tls-cert", "PEM server certificate", "path"},
        {"tls-key", "PEM private key", "path"}});
    parser.process(app);
    const auto product = parser.value("product");
    if (product != "ICE_BEAUTY" && product != "MEI_JING_LING") {
        QTextStream(stderr) << "Unknown product\n"; return 1;
    }
    LanDatabase database(product);
    QString error;
    if (!database.open(&error)) { QTextStream(stderr) << error << '\n'; return 2; }
    if (parser.isSet("init-demo")) {
        if (!database.initializeDemo(qEnvironmentVariable("WMS_DEMO_ADMIN_PASSWORD"), &error)) {
            QTextStream(stderr) << error << '\n'; return 3;
        }
        QTextStream(stdout) << "Initialized isolated test schema for " << product << '\n'; return 0;
    }
    if (!database.verify(&error)) { QTextStream(stderr) << error << '\n'; return 4; }
    bool portOk = false;
    const int port = parser.value("port").toInt(&portOk);
    const QHostAddress address(parser.value("listen"));
    if (!portOk || port < 1 || port > 65535 || address.isNull()) { QTextStream(stderr) << "Invalid listen address/port\n"; return 5; }
    LanServer server(database);
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
        QTextStream(stderr) << "Cannot listen on requested address/port\n"; return 7;
    }
    QTextStream(stdout) << "WmsServer ready " << (tls ? "https" : "http") << "://"
                        << address.toString() << ':' << port << " product=" << product << Qt::endl;
    return app.exec();
}
