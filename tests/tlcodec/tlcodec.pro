# Offline TL codec tests.
#
# A standalone project, deliberately NOT a SUBDIRS entry under symbogram.pro.
# tools\build-symbian.cmd runs `qmake symbogram.pro -r -spec symbian-abld` and
# then the ABLD.BAT that qmake drops in the project root; under a subdirs
# project the generated bld.inf, the .mmp, the .pkg and .rss all move down a
# directory, and every one of those is matched by root-anchored rules in
# .gitignore and by the pkg header logic in symbogram.pro's symbian{} block.
# Restructuring the one file whose failures are hardest to diagnose, to gain a
# `make check` on a platform that cannot run it, is a bad trade.
#
# Also deliberately NOT include(libkg/libkg.pri): that pulls in tgtransport.cpp
# and tgc_auth.cpp, which #include "apisecrets.h" and #error without it. These
# tests need no credentials and must build on a clone that has none -- which is
# also what makes this the one target that could run on a hosted CI runner.
#
# Build (Qt 4.8.7 + MinGW 4.8.2, the same toolchain build-desktop.cmd needs):
#   pwsh -File tools\run-tlcodec.ps1

QT       = core
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = tlcodec

LIBKG = $$PWD/../../libkg
INCLUDEPATH += $$LIBKG $$PWD

SOURCES += \
    $$PWD/main.cpp \
    $$PWD/tlcase.cpp \
    $$LIBKG/tgstream.cpp \
    $$LIBKG/tlschema.cpp \
    $$LIBKG/mtschema.cpp

HEADERS += \
    $$PWD/tlcase.h \
    $$LIBKG/tgstream.h \
    $$LIBKG/tlschema.h \
    $$LIBKG/mtschema.h
