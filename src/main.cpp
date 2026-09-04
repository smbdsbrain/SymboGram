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
#include <QSettings>

// Two levels so the argument is macro-expanded before being stringified.
#define KG_STRINGIFY_(x) #x
#define KG_STRINGIFY(x) KG_STRINGIFY_(x)

#ifdef SYMBOGRAM_DEVLOG
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMutex>

// Qt routes qDebug() to OutputDebugString for GUI-subsystem apps on Windows,
// and on Symbian there is no console at all, so in both cases a release build
// is simply silent. Rather than fight the linker for a console subsystem
// (which collides with Qt's qtmain and fails on WinMain@16), write the log to
// a file. Same mechanism serves the phone, where it is the only option.
//
// Path: $SYMBOGRAM_LOG_FILE, else C:\Data\SymboGram\log.txt on Symbian,
// else symbogram.log beside the executable.
static QFile g_logFile;
static QMutex g_logMutex;

static void symbogramMessageHandler(QtMsgType type, const char *msg)
{
    static const char *const levels[] = { "DEBUG", "WARN ", "CRIT ", "FATAL" };
    const char *level = (type >= 0 && type <= 3) ? levels[type] : "?????";

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        g_logFile.write(QDateTime::currentDateTime()
                            .toString("yyyy-MM-dd hh:mm:ss.zzz").toLatin1());
        g_logFile.write(" [");
        g_logFile.write(level);
        g_logFile.write("] ");
        g_logFile.write(msg);
        g_logFile.write("\n");
        g_logFile.flush();          // a crash must not cost us the last lines
    }
    if (type == QtFatalMsg) {
        abort();
    }
}

static void installDevLog()
{
    QString path = QString::fromLocal8Bit(qgetenv("SYMBOGRAM_LOG_FILE"));
    if (path.isEmpty()) {
#ifdef Q_OS_SYMBIAN
        path = "C:\\Data\\SymboGram\\log.txt";
#else
        path = QCoreApplication::applicationDirPath() + "/symbogram.log";
#endif
    }
    QDir().mkpath(QFileInfo(path).absolutePath());

    // Truncate per run: these are diagnostic logs for one session, and an
    // ever-growing file on a phone is its own problem.
    g_logFile.setFileName(path);
    if (g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qInstallMsgHandler(symbogramMessageHandler);
    }
}
#endif // SYMBOGRAM_DEVLOG

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

#ifdef SYMBOGRAM_DEVLOG
    // As early as possible, but after QApplication so applicationDirPath works.
    installDevLog();
    qDebug("SymboGram %s starting (Qt %s)", KG_STRINGIFY(VERSION), qVersion());
#endif

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

    // Optional redirect for where the session lives. TgTransport persists the
    // auth key through QSettings(IniFormat, UserScope, ...), and TgClient
    // derives its session and cache directories from that same path, so one
    // override moves all of it.
    //
    // The desktop test build uses this to keep the auth key inside the
    // gitignored secrets/ tree rather than %APPDATA%, so a real session can be
    // reused across runs without it sitting somewhere easy to commit by
    // accident. Unset everywhere else, so Symbian behaviour is unchanged.
    const QByteArray sessionDir = qgetenv("SYMBOGRAM_SESSION_DIR");
    if (!sessionDir.isEmpty()) {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QString::fromLocal8Bit(sessionDir));
    }

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
