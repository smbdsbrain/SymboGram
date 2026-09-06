#include "tgclient.h"

#include <string.h>

#include "tgsrpworker.h"
#include "tlschema.h"
#include "debug.h"

// The two-step-verification flow, driven from here rather than from QML.
//
// QML supplies the password and nothing else. That is what makes "every
// attempt uses freshly fetched parameters" a property of the code rather than
// a discipline the interface has to remember: srp_id expires server-side, and
// a stale one is rejected with SRP_ID_INVALID, which is indistinguishable from
// a wrong password to anyone reading the screen.

void TgClient::authCheckPasswordSRP(QString password)
{
    if (_srpWorker != 0) {
        kgDebug() << "Password check already running";
        return;
    }

    _pendingPassword = password.toUtf8();
    _srpRetried = false;
    _passwordRequestId = accountGetPassword().toLongLong();
}

void TgClient::cancelPasswordCheck()
{
    _passwordRequestId = 0;
    _checkPasswordRequestId = 0;
    _srpRetried = false;

    if (!_pendingPassword.isEmpty()) {
        memset(_pendingPassword.data(), 0, _pendingPassword.size());
        _pendingPassword.clear();
    }

    if (_srpWorker == 0) {
        return;
    }

    // Stop listening first, then let it wind down: the worker checks the flag
    // between PBKDF2 blocks, and waiting for it here would block the GUI
    // thread on exactly the computation this design exists to keep off it.
    _srpWorker->disconnect(this);
    _srpWorker->cancel();
    _srpWorker = 0;
}

void TgClient::handleAccountPassword(TgObject password, qint64 messageId)
{
    if (_passwordRequestId == 0 || messageId != _passwordRequestId) {
        // A fetch made to show the hint, not to answer with a proof.
        return;
    }

    _passwordRequestId = 0;

    if (_pendingPassword.isEmpty()) {
        return;
    }

    if (!password["has_password"].toBool()) {
        emit passwordCheckFailed("This account has no cloud password.");
        cancelPasswordCheck();
        return;
    }

    TgObject algo = password["current_algo"].toMap();

    // passwordKdfAlgoUnknown means the server declined to say how the password
    // is derived, and any other constructor is a scheme this client cannot
    // compute. Both have to stop here: guessing produces a well-formed proof
    // that is simply wrong, which reaches the user as "wrong password".
    if (GETID(algo) != TLType::PasswordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow) {
        emit passwordCheckFailed("This account uses a password scheme this client does not support.");
        cancelPasswordCheck();
        return;
    }

    SrpParams params;
    params.p = algo["p"].toByteArray();
    params.g = algo["g"].toInt();
    params.salt1 = algo["salt1"].toByteArray();
    params.salt2 = algo["salt2"].toByteArray();
    params.srpB = password["srp_B"].toByteArray();
    params.srpId = password["srp_id"].toLongLong();

    // Deliberately unparented, and it deletes itself when run() returns.
    // A QThread destroyed by its parent while still running aborts the
    // process, and TgClient can be torn down mid-derivation.
    _srpWorker = new TgSrpWorker(params, _pendingPassword);

    connect(_srpWorker, SIGNAL(progress(qint32)), this, SLOT(handleSrpProgress(qint32)));
    connect(_srpWorker, SIGNAL(proved(qint64, QByteArray, QByteArray)),
            this, SLOT(handleSrpProved(qint64, QByteArray, QByteArray)));
    connect(_srpWorker, SIGNAL(failed(QString)), this, SLOT(handleSrpFailed(QString)));
    connect(_srpWorker, SIGNAL(finished()), _srpWorker, SLOT(deleteLater()));

    _srpWorker->start(QThread::LowPriority);
}

void TgClient::handleSrpProgress(qint32 percent)
{
    emit passwordCheckProgress(percent);
}

void TgClient::handleSrpProved(qint64 srpId, QByteArray a, QByteArray m1)
{
    _srpWorker = 0;

    _checkPasswordRequestId = authCheckPassword(srpId, a, m1).toLongLong();

    // The proof is bound to this srp_id. If the server answers FLOOD_WAIT, the
    // transport must drop it rather than resend it on the next ping tick: the
    // replay would be rejected and would cost another attempt.
    _transport->doNotReplay(_checkPasswordRequestId);

    // The proof is sent; the password itself has no further use. Retrying
    // SRP_ID_INVALID needs it, so it is released only once the server has
    // answered -- see handlePasswordRpcError.
}

void TgClient::handleSrpFailed(QString reason)
{
    _srpWorker = 0;

    emit passwordCheckFailed(reason);
    cancelPasswordCheck();
}

bool TgClient::handlePasswordRpcError(QString errorMessage, qint64 messageId)
{
    if (_checkPasswordRequestId == 0 || messageId != _checkPasswordRequestId) {
        return false;
    }

    _checkPasswordRequestId = 0;

    // Not a wrong password: the parameters went stale between the fetch and
    // the proof. Retried once, with a fresh srp_id, because surfacing it would
    // tell the user their password is wrong when it is not.
    if (errorMessage == "SRP_ID_INVALID" && !_srpRetried && !_pendingPassword.isEmpty()) {
        kgInfo() << "SRP parameters went stale; refetching";
        _srpRetried = true;
        _passwordRequestId = accountGetPassword().toLongLong();
        return true;
    }

    cancelPasswordCheck();
    return false;
}
