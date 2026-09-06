#include "tlcase.h"

#include <QDataStream>
#include <QStringList>
#include <QTextStream>
#include <QVariantList>
#include <QVariantMap>

TlReport::TlReport()
    : planned(0), _n(0), _failed(0), _body()
{
}

void TlReport::ok(const QString &name, const QString &note)
{
    ++_n;
    _body += QString("ok %1 - %2").arg(_n).arg(name);
    if (!note.isEmpty())
        _body += " # " + note;
    _body += "\n";
}

void TlReport::fail(const QString &name, const QString &why)
{
    ++_n;
    ++_failed;
    _body += QString("not ok %1 - %2 # %3\n").arg(_n).arg(name).arg(why);
}

int TlReport::finish()
{
    QTextStream out(stdout);
    out << "TAP version 13\n";
    out << "1.." << _n << "\n";
    out << _body;
    out.flush();
    // Cap at 125: shells reserve 126 and above, and a run with more than 125
    // failures is not a number anyone acts on anyway.
    return _failed > 125 ? 125 : _failed;
}

QVariant tlStripFlags(const QVariant &v)
{
    if (v.type() == QVariant::Map) {
        QVariantMap in = v.toMap();
        QVariantMap out;
        for (QVariantMap::const_iterator it = in.begin(); it != in.end(); ++it) {
            if (it.key() == "flags" || it.key() == "flags2")
                continue;
            out.insert(it.key(), tlStripFlags(it.value()));
        }
        return out;
    }
    if (v.type() == QVariant::List) {
        QVariantList in = v.toList();
        QVariantList out;
        for (int i = 0; i < in.size(); ++i)
            out.append(tlStripFlags(in.at(i)));
        return out;
    }
    return v;
}

bool tlDecodeStrict(const QByteArray &in, READ_METHOD read, void *cb,
                    QVariant &out, QString &why)
{
    // Deliberately not tlDeserialize<R>(): that template takes the reader as a
    // non-type template parameter, so it cannot be called with a runtime
    // READ_METHOD out of a table -- and it builds the packet internally and
    // discards it, which is precisely the object we need to interrogate.
    TgPacket packet(in);
    QVariant obj;
    (*read)(packet, obj, cb);

    if (packet.stream.status() != QDataStream::Ok) {
        why = "reader ran past the end of the buffer (ReadPastEnd)";
        return false;
    }
    if (!packet.stream.atEnd()) {
        // Almost always an unrecognised constructor id somewhere inside: the
        // switch matched nothing, 4 bytes were consumed, and the reader carried
        // on decoding the wrong offsets.
        const qint64 left = qint64(in.size()) - packet.stream.device()->pos();
        why = QString("%1 trailing byte(s) -- unknown constructor swallowed").arg(left);
        return false;
    }
    // Only meaningful for constructors. readVector yields a QVariantList, which
    // has no "_" key and would fail this unconditionally.
    if (obj.type() == QVariant::Map && obj.toMap().value("_").toInt() == 0) {
        why = "empty object -- top-level constructor id not in this layer";
        return false;
    }

    out = obj;
    return true;
}

QString tlFirstDifference(const QByteArray &a, const QByteArray &b)
{
    const int n = qMin(a.size(), b.size());
    int i = 0;
    while (i < n && a.at(i) == b.at(i))
        ++i;

    if (i == n && a.size() != b.size())
        return QString("identical for %1 bytes then length differs: %2 vs %3")
                .arg(n).arg(a.size()).arg(b.size());

    const int from = qMax(0, i - 4);
    return QString("offset 0x%1: expected %2, got %3")
            .arg(i, 0, 16)
            .arg(QString(a.mid(from, 12).toHex()))
            .arg(QString(b.mid(from, 12).toHex()));
}

void tlRunCase(TlReport &r, const TlCase &c)
{
    const QByteArray in = QByteArray::fromHex(QByteArray(c.hex));
    if (in.isEmpty()) {
        r.fail(c.name, "empty or unparseable hex vector");
        return;
    }

    QVariant obj;
    QString why;
    if (!tlDecodeStrict(in, c.read, c.rcb, obj, why)) {
        r.fail(c.name, why);
        return;
    }

    TgPacket w;
    (*c.write)(w, obj, c.wcb);
    const QByteArray out = w.toByteArray();

    if (c.mode & TlExact) {
        if (out != in) {
            r.fail(c.name, "re-encode differs: " + tlFirstDifference(in, out));
            return;
        }
        r.ok(c.name, "exact");
        return;
    }

    QVariant obj2;
    if (!tlDecodeStrict(out, c.read, c.rcb, obj2, why)) {
        r.fail(c.name, "re-encoded bytes do not decode: " + why);
        return;
    }
    if (tlStripFlags(obj) != tlStripFlags(obj2)) {
        r.fail(c.name, "structural mismatch between decode and re-decode");
        return;
    }
    r.ok(c.name, out == in ? "structural (bytes matched too)" : "structural");
}

void tlExpectBytes(TlReport &r, const char *name, WRITE_METHOD write, void *cb,
                   const QVariant &obj, const char *hex)
{
    const QByteArray want = QByteArray::fromHex(QByteArray(hex));

    TgPacket packet;
    (*write)(packet, obj, cb);
    const QByteArray got = packet.toByteArray();

    if (got == want) {
        r.ok(QString::fromLatin1(name));
        return;
    }

    r.fail(QString::fromLatin1(name),
           QString("%1 (expected %2 bytes, got %3)")
               .arg(tlFirstDifference(want, got))
               .arg(want.size())
               .arg(got.size()));
}
