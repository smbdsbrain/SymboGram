# Offline MTProto crypto tests.
#
# A standalone project, deliberately NOT a SUBDIRS entry under symbogram.pro,
# for the reason spelled out at the top of tests/tlcodec/tlcodec.pro: a subdirs
# build moves the generated bld.inf, .mmp, .pkg and .rss down a directory,
# where root-anchored .gitignore rules and the pkg header logic in symbogram.pro
# no longer match them.
#
# Also NOT include(libkg/libkg.pri): that pulls in tgtransport.cpp, which
# #includes "apisecrets.h" and #errors without it. crypto.cpp needs no
# credentials, so this target builds on a clone that has none.
#
# Build:
#   pwsh -File tools\run-crypto.ps1

QT       = core
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = crypto

LIBKG   = $$PWD/../../libkg
TLCODEC = $$PWD/../tlcodec
INCLUDEPATH += $$LIBKG $$TLCODEC $$PWD

include($$LIBKG/mbedtls/mbedtls.pri)

SOURCES += \
    $$PWD/main.cpp \
    $$TLCODEC/tlcase.cpp \
    $$LIBKG/crypto.cpp \
    $$LIBKG/tgstream.cpp \
    $$LIBKG/mtschema.cpp

HEADERS += \
    $$TLCODEC/tlcase.h \
    $$LIBKG/crypto.h \
    $$LIBKG/tgstream.h \
    $$LIBKG/mtschema.h
