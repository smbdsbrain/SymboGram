#ifndef TGTRANSPORT_H
#define TGTRANSPORT_H

#include <QObject>
#include <QTcpSocket>
#include <QBasicTimer>
#include <QList>
#include "debug.h"
#include "tgstream.h"

class TgClient;

class TgTransport : public QObject
{
    Q_OBJECT
private:
    TgClient *_client;
    QTcpSocket *_socket;
    QBasicTimer _timer;

    bool testMode;
    bool mediaOnly;
    qint32 currentDc;
    quint16 currentPort;
    QString currentHost;
    bool isMain;

    TgObject tgConfig;

    QByteArray nonce;
    QByteArray serverNonce;
    QByteArray newNonce;

    QByteArray authKey;
    qint64 serverSalt;
    qint64 authKeyId;
    qint32 timeOffset;
    qint32 sequence;
    qint64 lastMessageId;
    qint64 sessionId;
    qint64 pingId;
    qint64 userId;

    QHash<qint64, QByteArray> pendingMessages;
    QHash<qint64, QByteArray> migrationMessages;
    QHash<qint64, QByteArray> floodMessages;

    QString _sessionName;

    TgVector msgsToAck;

    qint64 authCheckMsgId;

    bool initialized;

    // True from the first req_pq_multi until the key exchange completes.
    // Plaintext messages are legal only in that window, and an already-loaded
    // authKey does not mark the end of it: a session with a key but no user id
    // re-runs the exchange with the old key still in place.
    bool _handshaking;

    // Message ids already accepted from the server, newest last. Bounded: see
    // acceptMessageId().
    QList<qint64> _seenMessageIds;

    // Consecutive rejected packets. Reset by the first packet that verifies.
    qint32 _badPackets;

public:
    explicit TgTransport(TgClient *parent = 0, QString sessionName = "", qint32 dcId = 0,
                         bool useTestDc = false);
    ~TgTransport();

    template <WRITE_METHOD W> qint64 sendPlainObject(QVariant i);
    template <WRITE_METHOD W> qint64 sendMTObject(QVariant i);
    template <WRITE_METHOD W> qint64 sendMTServiceObject(QVariant i);

signals:
    
public slots:
    void timerEvent(QTimerEvent *event);

    void resetSession();
    void saveSession(bool reset = false);
    void loadSession();

    bool hasSession();
    bool hasUserId();
    TgLong getUserId();

    void checkAuthorization();

    TgLong sendMsgsAck();

    TgObject config();
    void setConfig(TgObject config);
    void migrateTo(qint32 dcId);
    void resetDc();
    void setDc(QString host, quint16 port, qint32 dcId);
    qint32 dcId();

    void start();
    void stop(bool sendMsgsAckBool = true);

    qint64 sendPlainMessage(QByteArray data, qint64 oldMid);
    qint64 sendMTMessage(QByteArray data, qint64 oldMid, bool isService);
    void authorize();
    void sendIntermediate(QByteArray data);
    QByteArray readIntermediate();
    void processMessage(QByteArray message);
    bool acceptMessageId(qint64 messageId);
    void countBadPacket();
    void initConnection();
    QByteArray gzipPacket(QByteArray data);
    qint64 getNewMessageId();
    qint32 generateSequence(bool isContent);

    void broadcastMessageChange(qint64 oldMsg, qint64 newMsg);

    void _connected();
    void _disconnected();
    void _readyRead();
    void _bytesSent(qint64 count);
    void _error(QAbstractSocket::SocketError socketError);

    void handleObject(QByteArray data, qint64 messageId);
    void handleResPQ(QByteArray data, qint64 messageId);
    void handleServerDHParamsOk(QByteArray data, qint64 messageId);
    void handleDhGenOk(QByteArray data, qint64 messageId);
    void handleMsgContainer(QByteArray data, qint64 messageId);
    void handleRpcResult(QByteArray data, qint64 messageId);
    void handleGzipPacked(QByteArray data, qint64 messageId);
    void handleRpcError(QByteArray data, qint64 messageId);
    void handlePingMethod(QByteArray data, qint64 messageId);
    void handleMsgCopy(QByteArray data, qint64 messageId);
    void handleBadMsgNotification(QByteArray data, qint64 messageId);
    void handleBadServerSalt(QByteArray data, qint64 messageId);
    void handleConfig(QByteArray data, qint64 messageId);
    void handleAuthorization(QByteArray data, qint64 messageId);
    void handleVector(QByteArray data, qint64 messageId);
    void handleMsgDetailedInfo(QByteArray data, qint64 messageId);
    void handleMsgNewDetailedInfo(QByteArray data, qint64 messageId);
    void handleBool(QByteArray data, qint64 messageId);
};

template <WRITE_METHOD W> qint64 TgTransport::sendPlainObject(QVariant i)
{
    kgDebug() << "Sending plain object:" << GETID(i.toMap());
    return sendPlainMessage(tlSerialize<W>(i), 0);
}

template <WRITE_METHOD W> qint64 TgTransport::sendMTObject(QVariant i)
{
    kgDebug() << "Sending MT object:" << GETID(i.toMap());
    return sendMTMessage(tlSerialize<W>(i), 0, false);
}

// For the service messages of the protocol itself -- msgs_ack, ping, pong.
//
// They take an even sequence number, do not advance the content counter and
// are not held for retry: nothing re-sends a ping, and a message the server
// answers out of band never clears itself from the pending map.
template <WRITE_METHOD W> qint64 TgTransport::sendMTServiceObject(QVariant i)
{
    kgDebug() << "Sending MT service object:" << GETID(i.toMap());
    return sendMTMessage(tlSerialize<W>(i), 0, true);
}

#endif // TGTRANSPORT_H
