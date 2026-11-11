#include "mainwindow.h"
#include "lang.h"
#include "mapitem.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QImageReader>
#include <QLocale>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>

int runSelfTest(const QStringList &args);

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const bool selfTest = app.arguments().contains("--selftest");
    // The self-test gets its own cache, settings and recovery copy: it never touches the user's.
    QApplication::setApplicationName(selfTest ? "Vax Maps selftest" : "Vax Maps");
    QApplication::setOrganizationName("VaxMaps");
    QApplication::setWindowIcon(QIcon(":/vaxmaps.png"));
    if (!selfTest) dataDir = portableDataDir(QCoreApplication::applicationDirPath());
    if (!dataDir.isEmpty()) {   // settings in an .ini there instead of the registry
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dataDir);
    }
    const QString language = QSettings().value("language").toString();   // first run: the system's language
    setPortuguese(!selfTest && (language.isEmpty() ? QLocale::system().language() == QLocale::Portuguese : language == "pt"));
    QImageReader::setAllocationLimit(0);   // battle maps often exceed Qt's 256 MB decode limit

    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(45, 46, 51));
    p.setColor(QPalette::WindowText, QColor(225, 225, 228));
    p.setColor(QPalette::Base, QColor(32, 33, 37));
    p.setColor(QPalette::AlternateBase, QColor(45, 46, 51));
    p.setColor(QPalette::Text, QColor(225, 225, 228));
    p.setColor(QPalette::Button, QColor(55, 56, 62));
    p.setColor(QPalette::ButtonText, QColor(225, 225, 228));
    p.setColor(QPalette::Highlight, QColor(70, 120, 200));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(32, 33, 37));
    p.setColor(QPalette::ToolTipText, QColor(225, 225, 228));
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 145));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 125));
    app.setPalette(p);

    int rc;
    if (selfTest) {
        rc = runSelfTest(app.arguments());
    } else {
        MainWindow w;
        w.restoreSession();   // shows it
        const QStringList args = app.arguments();
        if (!w.offerRecovery() && args.size() > 1 && args[1].endsWith(".vaxmap", Qt::CaseInsensitive)) w.openFromUi(args[1]);
        rc = app.exec();
    }
    MapData::shutdown();
    if (selfTest) {   // leaves nothing behind
        for (auto where : {QStandardPaths::CacheLocation, QStandardPaths::AppLocalDataLocation}) {
            const QString dir = QStandardPaths::writableLocation(where);
            if (dir.endsWith("Vax Maps selftest") || dir.endsWith("Vax Maps selftest/cache")) QDir(dir).removeRecursively();   // never anything else
        }
        QSettings().clear();
    }
    return rc;
}
