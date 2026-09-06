#include "tgsrpworker.h"

#include <string.h>

#include "debug.h"

TgSrpWorker::TgSrpWorker(const SrpParams &params, const QByteArray &passwordUtf8,
                         QObject *parent)
    : QThread(parent)
    , _params(params)
    , _password(passwordUtf8)
    , _cancel(false)
{
}

TgSrpWorker::~TgSrpWorker()
{
    if (!_password.isEmpty()) {
        memset(_password.data(), 0, _password.size());
    }
}

void TgSrpWorker::cancel()
{
    _cancel = true;
}

bool TgSrpWorker::step(qint32 done, qint32 total)
{
    if (_cancel) {
        return false;
    }

    if (total > 0) {
        emit progress(done * 100 / total);
    }

    return true;
}

void TgSrpWorker::run()
{
    SrpProof proof;
    const QString problem = srpCompute(_params, _password, proof, this);

    // Held no longer than the computation that needs it. This is best effort
    // and docs/security.md says so: the QString it came from is reference
    // counted by the QML engine and Symbian offers no way to pin a page.
    if (!_password.isEmpty()) {
        memset(_password.data(), 0, _password.size());
        _password.clear();
    }

    if (_cancel) {
        // The client stopped listening before asking for this. Reporting into
        // a torn-down flow would reopen a page the user has left.
        return;
    }

    if (!problem.isEmpty()) {
        kgWarning() << "SRP failed:" << problem;
        emit failed(problem);
        return;
    }

    if (proof.m1.isEmpty()) {
        emit failed("The password check was cancelled.");
        return;
    }

    emit proved(proof.srpId, proof.a, proof.m1);
}
