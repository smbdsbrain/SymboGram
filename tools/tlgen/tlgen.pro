# Builds the vendored Kutegram TL generator against our own argv-driven main.
#
# Deliberately a separate project rather than tools/tl-generator/generator.pro:
# that .pro is part of the pinned vendored tree and lists upstream's main.cpp
# and resources.qrc. Compiling the same sources from here leaves the pin
# byte-verifiable (see docs/VENDORED.md) while still letting us choose the
# schema and the layer at run time.
#
# resources.qrc is deliberately NOT included. Upstream compiles api.tl and
# mtproto.json into the binary; we pass paths instead, so the generator can be
# aimed at the schema we pin in schema/ rather than the one frozen at the
# vendored commit.
#
# Qt 4.8.7 with MinGW 4.8.2 -- the same toolchain tools\build-desktop.cmd
# already requires, so this adds no new dependency. Qt 5.15 also works; Qt 6
# does not, because the generator uses QTextStream endl and QRegExp, both
# removed there.

QT       = core
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = tlgen

GEN = $$PWD/../tl-generator

INCLUDEPATH += $$GEN $$GEN/qt-json

SOURCES += \
    $$PWD/main.cpp \
    $$GEN/shared.cpp \
    $$GEN/method.cpp \
    $$GEN/constructor.cpp \
    $$GEN/schema.cpp \
    $$GEN/generator.cpp \
    $$GEN/crc32.cpp \
    $$GEN/qt-json/json.cpp

HEADERS += \
    $$GEN/shared.h \
    $$GEN/method.h \
    $$GEN/constructor.h \
    $$GEN/schema.h \
    $$GEN/generator.h \
    $$GEN/crc32.h \
    $$GEN/qt-json/json.h
