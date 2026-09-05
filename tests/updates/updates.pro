# Offline update-sequence tests.
#
# A standalone project, deliberately NOT a SUBDIRS entry under symbogram.pro,
# for the reason spelled out at the top of tests/tlcodec/tlcodec.pro: a subdirs
# build moves the generated bld.inf, .mmp, .pkg and .rss down a directory,
# where the root-anchored .gitignore rules and the pkg header logic in
# symbogram.pro no longer match them.
#
# Also NOT include(libkg/libkg.pri): that pulls in tgtransport.cpp, which
# #includes "apisecrets.h" and #errors without it. TgUpdatesState is separated
# from TgUpdatesManager precisely so the rules can be linked without any of
# that, and tested with no network and no credentials.
#
# Build:
#   pwsh -File tools\run-updates.ps1

QT       = core
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = updates

LIBKG   = $$PWD/../../libkg
TLCODEC = $$PWD/../tlcodec
INCLUDEPATH += $$LIBKG $$TLCODEC $$PWD

SOURCES += \
    $$PWD/main.cpp \
    $$TLCODEC/tlcase.cpp \
    $$LIBKG/tgupdatesstate.cpp \
    $$LIBKG/tgstream.cpp \
    $$LIBKG/tlschema.cpp

HEADERS += \
    $$TLCODEC/tlcase.h \
    $$LIBKG/tgupdatesstate.h \
    $$LIBKG/tgstream.h \
    $$LIBKG/tlschema.h
