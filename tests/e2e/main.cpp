// Headless end-to-end harness.
//
// Drives libkg against a real Telegram data centre with no QML and no phone.
// Two tiers share this binary:
//
//   --tier=test   Telegram's test environment. Accounts are disposable
//                 (99966XYYYY, code = the DC id five times) and can be created
//                 and logged into programmatically, so this tier runs from
//                 nothing.
//   --tier=prod   production. Login is NOT automatable -- SMS has been
//                 unavailable to third-party api_ids since 2023-02-18, leaving
//                 sentCodeTypeApp, which needs another logged-in session. So
//                 this tier requires a session that already exists and skips
//                 cleanly when there is none.
//
// The tier is on argv rather than in the environment on purpose: it decides
// which account gets touched, and that should be visible in the command that
// touched it.

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include "tgclient.h"

#include "runner.h"
#include "scenarios.h"

static const char* kTestDcHost = "149.154.167.40";   // test DC 2, public

struct Options {
    Options() : tier("test"), dc(2), deadlineSec(600), listOnly(false) {}
    QString tier;
    QString sessionDir;
    QString sessionName;
    // Ties the halves of the gap scenario together: one run sends it,
    // a later run has to receive it.
    QString text;
    // Where the client half records who it is, so the sending half can
    // address it. A file rather than an argument: an account id is not a
    // secret, but it is nobody's business but this machine's.
    QString peerFile;
    QString phone;
    QString code;
    QString only;
    int     dc;
    int     deadlineSec;
    bool    listOnly;
};

static int usage()
{
    QTextStream e(stderr);
    e << "usage: e2e --tier=test|prod [options]\n"
      << "\n"
      << "  --tier=test|prod      which environment (default: test)\n"
      << "  --session-dir=PATH    where the session ini lives (default: a fresh temp dir)\n"
      << "  --session-name=NAME   session file suffix (default: e2e)\n"
      << "  --phone=99966XYYYY    test-DC number; the code is derived from X\n"
      << "  --dc=N                test DC to bootstrap on (default: 2)\n"
      << "  --only=NAME           run one scenario\n"
      << "  --text=STRING         message text, for the halves of the gap scenario\n"
      << "  --peer-file=PATH      where the gap halves exchange the target account\n"
      << "  --deadline=SECONDS    hard stop (default: 600)\n"
      << "  --list                print scenario names and exit\n"
      << "\n"
      << "exit: 0 pass, 1 failure, 2 infrastructure, 77 skipped\n";
    return 2;
}

// The test environment's login code is the DC id repeated five times. That is
// the whole reason this tier can run unattended.
static QString testCodeForPhone(const QString &phone)
{
    // 99966XYYYY -- X at index 5 is the DC id.
    if (phone.length() < 6 || !phone.startsWith("99966")) return QString();
    const QChar dc = phone.at(5);
    return QString(5, dc);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Must be set before any TgClient is constructed: TgClient derives its
    // session and cache directories from QSettings' path, which is keyed on
    // these two names. Get them wrong and the session lands somewhere
    // surprising rather than failing.
    QCoreApplication::setOrganizationName("SymboGram");
    QCoreApplication::setApplicationName("SymboGram");
    QCoreApplication::setApplicationVersion("0.1.0-e2e");

    Options o;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if      (a.startsWith("--tier="))         o.tier = a.mid(7);
        else if (a.startsWith("--session-dir="))  o.sessionDir = a.mid(14);
        else if (a.startsWith("--session-name=")) o.sessionName = a.mid(15);
        else if (a.startsWith("--phone="))        o.phone = a.mid(8);
        else if (a.startsWith("--code="))         o.code = a.mid(7);
        else if (a.startsWith("--only="))         o.only = a.mid(7);
        else if (a.startsWith("--dc="))           o.dc = a.mid(5).toInt();
        else if (a.startsWith("--deadline="))     o.deadlineSec = a.mid(11).toInt();
        else if (a.startsWith("--text="))         o.text = a.mid(7);
        else if (a.startsWith("--peer-file="))    o.peerFile = a.mid(12);
        else if (a == "--list")                   o.listOnly = true;
        else return usage();
    }

    QTextStream out(stdout);

    if (o.listOnly) {
        out << "connect\nlogin\nread\nsend\nnegative\nupdates\ngap-arm\ngap-send\ngap-check\n";
        return 0;
    }

    const bool testDc = (o.tier == "test");
    if (!testDc && o.tier != "prod") return usage();

    if (o.sessionName.isEmpty()) o.sessionName = "e2e";

    if (o.sessionDir.isEmpty()) {
        out << "1..0 # SKIP no --session-dir given\n";
        out << "# The harness will not guess where to put an auth key.\n";
        return 77;
    }
    QDir().mkpath(o.sessionDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, o.sessionDir);

    // Production cannot log in from cold, so a missing session is "skipped",
    // not "failed". A missing credential is not a defect in the code under
    // test, and reporting it as one trains people to ignore red runs.
    if (!testDc) {
        const QString ini = QDir(o.sessionDir).filePath(
                    "SymboGram/SymboGram_" + o.sessionName + ".ini");
        if (!QFile::exists(ini)) {
            out << "1..0 # SKIP prod tier: no session at " << ini << "\n";
            out << "# Copy a session in first. NEVER point --session-dir at\n";
            out << "# secrets/session directly: TgTransport::handleRpcError calls\n";
            out << "# resetSession() on any 401, which erases the auth key.\n";
            return 77;
        }
    }

    QString phone = o.phone;
    QString code;
    if (testDc) {
        if (phone.isEmpty()) phone = QString("99966%1%2").arg(o.dc).arg(1234);
        code = testCodeForPhone(phone);
        if (code.isEmpty()) {
            out << "1..0 # SKIP --phone must be a 99966XYYYY test number\n";
            return 77;
        }
    }

    Scenario *scenario = 0;
    const QString which = o.only.isEmpty() ? (testDc ? QString("login") : QString("read")) : o.only;
    if      (which == "connect")  scenario = makeConnectScenario();
    else if (which == "login")    scenario = makeLoginScenario();
    else if (which == "read")     scenario = makeReadScenario();
    else if (which == "send")     scenario = makeSendScenario();
    else if (which == "negative") scenario = makeNegativeScenario();
    else if (which == "updates")  scenario = makeUpdatesScenario();
    else if (which == "gap-arm")   scenario = makeGapArmScenario();
    else if (which == "gap-send")  scenario = makeGapSendScenario();
    else if (which == "gap-check") scenario = makeGapCheckScenario();
    else {
        out << "1..0 # SKIP unknown scenario '" << which << "'\n";
        return 77;
    }

    if (!testDc && (which == "login" || which == "negative")) {
        out << "1..0 # SKIP '" << which << "' is test-tier only\n";
        out << "# Production login needs a code delivered to another session.\n";
        delete scenario;
        return 77;
    }

    TgClient client(0, 0, o.sessionName, testDc);
    ScenarioRunner runner(&client, scenario);

    runner.seed("phone", phone);
    runner.seed("code", code);
    // An explicit --code wins over anything derived from the sentCode response.
    if (!o.code.isEmpty()) runner.seed("code_override", o.code);
    runner.seed("peer_file", o.peerFile);
    runner.seed("text", o.text.isEmpty()
                ? QString("SymboGram e2e %1")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                : o.text);

    QObject::connect(&runner, SIGNAL(done()), &app, SLOT(quit()));

    // Hard stop, independent of any step timeout, so a wedged transport cannot
    // hang a CI job forever.
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, SIGNAL(timeout()), &app, SLOT(quit()));
    deadline.start(o.deadlineSec * 1000);

    out << "# tier=" << o.tier
        << " scenario=" << scenario->name()
        << " dc=" << (testDc ? kTestDcHost : "production")
        << " session=" << o.sessionDir << "\n";
    out.flush();

    runner.start();
    app.exec();

    out << "TAP version 13\n";
    out << "1.." << scenario->count() << "\n";
    const QStringList lines = runner.tap();
    for (int i = 0; i < lines.size(); ++i) out << lines.at(i) << "\n";

    int rc;
    if (!runner.passed() && runner.stepsRun() == 0) {
        out << "Bail out! nothing ran (deadline or no connection)\n";
        rc = 2;
    } else if (!runner.passed()) {
        out << "# first failure: " << runner.failure() << "\n";
        rc = 1;
    } else {
        rc = 0;
    }

    out.flush();
    delete scenario;
    return rc;
}
