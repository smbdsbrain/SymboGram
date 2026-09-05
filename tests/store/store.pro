# Offline local-cache tests.
#
# A standalone project, deliberately NOT a SUBDIRS entry under symbogram.pro,
# for the reason spelled out at the top of tests/tlcodec/tlcodec.pro: a subdirs
# build moves the generated bld.inf, .mmp, .pkg and .rss down a directory,
# where the root-anchored .gitignore rules and the pkg header logic in
# symbogram.pro no longer match them.
#
# Also NOT include(libkg/libkg.pri): that pulls in tgtransport.cpp, which
# #includes "apisecrets.h" and #errors without it. TgStore talks to QtSql and
# to nothing else in the client, so it links on a clone with no credentials.
#
# Build:
#   pwsh -File tools\run-store.ps1

QT       = core sql
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = store

LIBKG   = $$PWD/../../libkg
TLCODEC = $$PWD/../tlcodec
INCLUDEPATH += $$LIBKG $$TLCODEC $$PWD

SOURCES += \
    $$PWD/main.cpp \
    $$TLCODEC/tlcase.cpp \
    $$LIBKG/tgstore.cpp \
    $$LIBKG/tgstream.cpp \
    $$LIBKG/tlschema.cpp

HEADERS += \
    $$TLCODEC/tlcase.h \
    $$LIBKG/tgstore.h \
    $$LIBKG/tgstream.h \
    $$LIBKG/tlschema.h
