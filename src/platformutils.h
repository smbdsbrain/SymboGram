#ifndef PLATFORMUTILS_H
#define PLATFORMUTILS_H

#include <QObject>
#include <QColor>
#if !defined(Q_OS_SYMBIAN) && !defined(Q_OS_WINPHONE)
#include <QSystemTrayIcon>
#include <QMenu>
#endif
#include <QUrl>
#include <QHash>
#include <QWidget>
// QVariantMap is used below. On desktop it arrives transitively via
// QSystemTrayIcon/QMenu, but those are excluded on Symbian by the guard above,
// so moc_platformutils.cpp fails there without this.
#include <QVariant>

#ifdef SYMBIAN3_READY
#include "QPiglerAPI.h"
#endif

// QNetworkSession arrived in 4.7.2. Bearer management is platform integration,
// which is what this class is for, and keeping it here means libkg needs no
// opinion about how a phone gets online.
#if QT_VERSION >= 0x040702
#include <QNetworkConfigurationManager>
#include <QNetworkSession>
#endif

class PlatformUtils : public QObject
{
    Q_OBJECT
private:
    QWidget* window;
#if !defined(Q_OS_SYMBIAN) && !defined(Q_OS_WINPHONE)
    QSystemTrayIcon trayIcon;
    QMenu trayMenu;
#endif
    QHash<qint64, QVariantMap> unread;
#ifdef SYMBIAN3_READY
    QPiglerAPI pigler;
    qint32 piglerId;
#endif
#if QT_VERSION >= 0x040702
    QNetworkConfigurationManager *networkManager;
    QNetworkSession *networkSession;
#endif

public:
    explicit PlatformUtils(QObject *parent = 0);

signals:
    // The device has a usable bearer again. Nothing in the transport can
    // observe this, so it waits out a backoff chosen while there was no
    // network; this lets the UI cut that short.
    void networkOnline();


public slots:
    // Opens a network session when the platform requires one, restoring the
    // configuration chosen last time. Called once at startup.
    void openNetworkSession();

#if QT_VERSION >= 0x040702
    void networkOnlineStateChanged(bool online);
#endif

    void showAndRaise();
    void quit();

#if !defined(Q_OS_SYMBIAN) && !defined(Q_OS_WINPHONE)
    void trayActivated(QSystemTrayIcon::ActivationReason reason);
    void messageClicked();
    void menuTriggered(QAction* action);
#endif

#ifdef SYMBIAN3_READY
    void piglerHandleTap(qint32 notificationId);
#endif

    void windowsExtendFrameIntoClientArea(int left, int top, int right, int bottom);
    bool windowsIsCompositionEnabled();
    QColor windowsRealColorizationColor();
    bool isWindows();

    void gotNewMessage(qint64 peerId, QString peerName, QString senderName, QString text, bool silent);
};

void openUrl(QUrl url);

#endif // PLATFORMUTILS_H
