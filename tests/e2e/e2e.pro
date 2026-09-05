# Headless end-to-end harness (test tiers 1 and 2).
#
# Standalone, like tests/tlcodec -- see the long note there for why
# symbogram.pro must stay a single non-SUBDIRS app project.
#
# Unlike tlcodec this one DOES include libkg.pri: it needs the transport, and
# therefore needs apisecrets.h. tools/run-e2e.ps1 generates it first, exactly as
# the two build scripts do.
#
# QT -= gui declarative after the include: libkg's registerQML() is guarded by
# #if defined(QT_QML_LIB) || defined(QT_DECLARATIVE_LIB), so the library links
# cleanly into a console app with neither present.

QT       = core network xml sql
CONFIG  += console
CONFIG  -= app_bundle
TEMPLATE = app
TARGET   = e2e

ROOT = $$PWD/../..
include($$ROOT/libkg/libkg.pri)

INCLUDEPATH += $$ROOT/libkg $$PWD

SOURCES += \
    $$PWD/main.cpp \
    $$PWD/runner.cpp \
    $$PWD/steps.cpp \
    $$PWD/scenarios.cpp

HEADERS += \
    $$PWD/event.h \
    $$PWD/scenario.h \
    $$PWD/steps.h \
    $$PWD/runner.h \
    $$PWD/scenarios.h
