#ifndef TGSRPWORKER_H
#define TGSRPWORKER_H

#include <QByteArray>
#include <QString>
#include <QThread>

#include "srp.h"

// Runs the SRP proof off the GUI thread.
//
// Nothing else in this codebase is threaded, and tgtransport.cpp carries
// //TODO: lock markers where message-id generation would need protection if
// anything moved, so the boundary here is deliberately the narrowest one that
// still helps: the worker touches no TgClient state, no socket, no store and
// no model, and it never generates a message id. It computes and hands back
// value types; the GUI thread is what sends the request.
//
// It has to exist at all because the derivation is 100000 PBKDF2-HMAC-SHA512
// iterations plus three 2048-bit modular exponentiations, which on the ARM11
// this targets is seconds. Chunking it across timer ticks was the alternative,
// and it does not work: the exponentiations are atomic inside mbedtls, so the
// interface would stay responsive for the first phase and then freeze for the
// part the user can least interpret.
//
// QThread rather than a QObject moved to one because there is no event loop to
// serve -- run() is a single long computation, not a queue of requests.
class TgSrpWorker : public QThread, public Pbkdf2Sink
{
    Q_OBJECT

public:
    TgSrpWorker(const SrpParams &params, const QByteArray &passwordUtf8,
                QObject *parent = 0);
    ~TgSrpWorker();

    // Safe from the GUI thread while run() is executing. The parameters expire
    // server-side, so a computation the user has walked away from is worth
    // nothing and should stop holding the password.
    void cancel();

    bool step(qint32 done, qint32 total);

signals:
    void progress(qint32 percent);
    void proved(qint64 srpId, QByteArray a, QByteArray m1);
    void failed(QString reason);

protected:
    void run();

private:
    // Written by the constructor on the calling thread and read only by run().
    // start() is the happens-before edge, which is why there is no mutex here:
    // a mutex would imply these may be written after the thread is running,
    // and they must not be.
    SrpParams  _params;
    QByteArray _password;

    volatile bool _cancel;
};

#endif // TGSRPWORKER_H
