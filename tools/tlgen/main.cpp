// Command-line front end for the vendored Kutegram TL generator.
//
// Why this file exists rather than an edit to tools/tl-generator/main.cpp:
// that tree is vendored and pinned (docs/VENDORED.md), and its own main.cpp
// hardcodes both the layer and the schema:
//
//     generate(":/mtproto.json", "MT", 0,   "tgstream.h");
//     generate(":/api.tl",       "TL", 166, "tgstream.h");
//
// Editing it would break the "no remaining file has been edited" property that
// makes the pin verifiable. So the vendored sources are compiled against this
// main instead, and the schema arrives as a filesystem path rather than a Qt
// resource -- which is also what lets us point the generator at a schema we
// pin ourselves (schema/api.tl) instead of the one baked into the vendored
// .qrc.
//
// The layer and the schema are taken from the SAME invocation on purpose.
// generator.cpp emits `#define API_LAYER <n>` into tlschema.h from its `layer`
// argument, so passing them together is what makes it structurally impossible
// to regenerate at one layer while announcing another. That pairing is not a
// nicety: readers are a switch on the constructor id with no default: case and
// TL has no length prefixes, so a reader built for one layer fed another
// layer's bytes does not fail -- it silently misparses the rest of the packet.

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>

#include "generator.h"

static int usage(const QString &argv0)
{
    QTextStream err(stderr);
    err << "usage: " << argv0 << " --api <api.tl> --layer <n> [--mtproto <mtproto.json>]" << endl;
    err << endl;
    err << "Writes tlschema.{h,cpp} and mtschema.{h,cpp} into the current directory," << endl;
    err << "with LF line endings. tools/gen-schema.ps1 wraps this and does the rest." << endl;
    return 2;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    QString apiPath;
    QString mtprotoPath;
    QString layerText;

    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == "--api" && i + 1 < args.size())          apiPath     = args.at(++i);
        else if (a == "--mtproto" && i + 1 < args.size()) mtprotoPath = args.at(++i);
        else if (a == "--layer" && i + 1 < args.size())   layerText   = args.at(++i);
        else return usage(args.value(0));
    }

    if (apiPath.isEmpty() || layerText.isEmpty())
        return usage(args.value(0));

    bool layerOk = false;
    const int layer = layerText.toInt(&layerOk);
    if (!layerOk || layer <= 0) {
        QTextStream(stderr) << "FAILED: --layer must be a positive integer, got '"
                            << layerText << "'" << endl;
        return 2;
    }

    // generate() opens the schema with QFile and returns silently on failure,
    // leaving a half-written or empty output that looks like a successful run.
    // Check the inputs here so a typo is a message rather than a mystery.
    if (!QFile::exists(apiPath)) {
        QTextStream(stderr) << "FAILED: no such schema: " << apiPath << endl;
        return 1;
    }
    if (!mtprotoPath.isEmpty() && !QFile::exists(mtprotoPath)) {
        QTextStream(stderr) << "FAILED: no such MTProto schema: " << mtprotoPath << endl;
        return 1;
    }

    QTextStream out(stdout);

    // Layer 0 suppresses the #define in generator.cpp, which is what upstream
    // passes for the MTProto schema: it is versionless and API_LAYER belongs to
    // the API schema alone.
    if (!mtprotoPath.isEmpty()) {
        out << "MT  <- " << mtprotoPath << endl;
        generate(mtprotoPath, "MT", 0, "tgstream.h");
    }

    out << "TL  <- " << apiPath << " (layer " << layer << ")" << endl;
    generate(apiPath, "TL", layer, "tgstream.h");

    return 0;
}
