#include <QApplication>
#include "qmlapplicationviewer.h"

#include <QFontDatabase>
#include <QTextCodec>
#include <QFont>
#include <QFontMetrics>
#include "tgclient.h"
#include "dialogsmodel.h"
#include "messagesmodel.h"
#include "systemname.h"
#include "avatardownloader.h"
#include "foldersmodel.h"
#include "currentuserinfo.h"
#include "platformutils.h"
#include <QSystemSemaphore>
#include <QSharedMemory>

// Two levels so the argument is macro-expanded before being stringified.
#define KG_STRINGIFY_(x) #x
#define KG_STRINGIFY(x) KG_STRINGIFY_(x)

#if QT_VERSION >= 0x050000
#include <QQmlContext>
#include <QQmlEngine>
#else
#include <QtDeclarative>
#endif

#if QT_VERSION >= 0x040702
#include <QNetworkConfigurationManager>
#include <QNetworkSession>
#endif

int main(int argc, char *argv[])
{
    //TODO OpenGL acceleration
    //Causes some crashes on Windows, Symbian 9.2-9.3?, research it
    //Requires custom fonts, but they can't be install on iOS
#if QT_VERSION < 0x050000
    //QApplication::setGraphicsSystem("opengl");
#endif

    QApplication app(argc, argv);

    QSystemSemaphore sema("SymboGram_semaphore", 1);
    bool isRunning;
    sema.acquire();

    {
        QSharedMemory shmem("SymboGram_shared");
        shmem.attach();
    }

    QSharedMemory shmem("SymboGram_shared");
    if (shmem.attach())
    {
        isRunning = true;
    }
    else
    {
        shmem.create(1);
        isRunning = false;
    }

    sema.release();
    if (isRunning) {
        //TODO raise SymboGram window
        return 1;
    }

    //TODO: keypad UI navigation
#ifdef Q_OS_SYMBIAN
    QApplication::setAttribute(Qt::AA_S60DisablePartialScreenInputMode, false);
//    QApplication::setNavigationMode(Qt::NavigationModeCursorAuto);
#endif

#if QT_VERSION >= 0x050300
    QApplication::setAttribute(Qt::AA_UseOpenGLES, true);
#endif

    // VERSION arrives unquoted from the .pro (see the comment there: the
    // escaped-quote form breaks the symbian-abld build), so stringify it here.
    QApplication::setApplicationVersion(QString(KG_STRINGIFY(VERSION)));
    QApplication::setApplicationName("SymboGram");
    QApplication::setOrganizationName("SymboGram");
    QApplication::setOrganizationDomain("github.com/smbdsbrain/SymboGram");

    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
#if QT_VERSION < 0x050000
    QTextCodec::setCodecForTr(codec);
    QTextCodec::setCodecForCStrings(codec);
#endif
    QTextCodec::setCodecForLocale(codec);

    TgClient::registerQML();
    qmlRegisterType<DialogsModel>("SymboGram", 1, 0, "DialogsModel");
    qmlRegisterType<MessagesModel>("SymboGram", 1, 0, "MessagesModel");
    qmlRegisterType<AvatarDownloader>("SymboGram", 1, 0, "AvatarDownloader");
    qmlRegisterType<FoldersModel>("SymboGram", 1, 0, "FoldersModel");
    qmlRegisterType<CurrentUserInfo>("SymboGram", 1, 0, "CurrentUserInfo");
    qmlRegisterUncreatableType<PlatformUtils>("SymboGram", 1, 0, "PlatformUtils", "PlatformUtils is uncreatable. Use platformUtils root property.");

    //TODO show status pane without button group on Symbian
    QmlApplicationViewer viewer;
    viewer.setOrientation(QmlApplicationViewer::ScreenOrientationAuto);
    viewer.rootContext()->setContextProperty("symbogramVersion", QApplication::applicationVersion());
    viewer.rootContext()->setContextProperty("symbogramPlatform", systemName());
    viewer.rootContext()->setContextProperty("platformUtils", new PlatformUtils(&viewer));
    viewer.rootContext()->setContextProperty("kgScaling", QFontMetrics(app.font()).height() / 14.0f);
    viewer.setMainQmlFile(QLatin1String("qrc:///qml/Main.qml"));
#if QT_VERSION >= 0x050000
    viewer.setTitle("SymboGram");
#else
    viewer.setWindowTitle("SymboGram");
#endif
    viewer.showExpanded();

#if QT_VERSION >= 0x040702
    QNetworkConfigurationManager manager;
    if (manager.capabilities() & QNetworkConfigurationManager::NetworkSessionRequired) {
        //TODO save network selection
        QNetworkConfiguration config = manager.defaultConfiguration();
        QNetworkSession* networkSession = new QNetworkSession(config);
        networkSession->open(); //TODO reset network selection
    }
#endif

    return app.exec();
}
