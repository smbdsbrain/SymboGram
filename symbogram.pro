TARGET = SymboGram
APPNAME = SymboGram
# Fork version numbering starts fresh. Claiming upstream's 1.2.0 while shipping
# different code would make bug reports ambiguous; the lineage is recorded
# separately below and shown in the About screen.
VERSION = 0.1.0
UPSTREAM_VERSION = 1.2.0
DEFINES += UPSTREAM_VERSION=$$UPSTREAM_VERSION
# NB: qmake's $$replace() takes a REGEX, so the original
#     $$replace(VERSION, ".", ",")
# matched every character and expanded 1.2.0 to ",,,,,", producing a .pkg
# header with an empty version that makesis rejects ("Expected numeric value").
# $$split takes a literal separator, so this needs no escaping.
VERSION_PARTS = $$split(VERSION, ".")
PKG_VERSION = $$join(VERSION_PARTS, ",")
# Passed unquoted and stringified in main.cpp via KG_STRINGIFY. Emitting the
# quotes here instead produces `MACRO VERSION=\"1.2.0\"` in the generated .mmp,
# which Symbian's cpp.exe rejects with "unterminated string or character
# constant" -- the backslash escaping that works for the desktop mkspecs does
# not survive the symbian-abld one.
DEFINES += VERSION=$$VERSION
#DATE = $$system(date /t)
#DEFINES += BUILDDATE=\"\\\"$$DATE\\\"\"
#COMMIT_SHA = $$system(git log --pretty=format:%h -n 1);
#DEFINES += COMMIT_SHA=\"\\\"$$COMMIT_SHA\\\"\"

greaterThan(QT_MAJOR_VERSION, 4) {
    QT += core widgets qml quick network xml
    win32:!winrt:QT += winextras
}
!greaterThan(QT_MAJOR_VERSION, 4) {
    QT += core declarative gui network xml
}

winrt {
    WINRT_MANIFEST.background = lightSkyBlue
    WINRT_MANIFEST.description = "An unofficial Telegram client, written in Qt Quick and C++."
    WINRT_MANIFEST.logo_large = wpassets/logo_150x150.png
    WINRT_MANIFEST.logo_medium = wpassets/logo_71x71.png
    WINRT_MANIFEST.logo_small = wpassets/logo_44x44.png
    WINRT_MANIFEST.logo_splash = wpassets/logo_480x800.png
    WINRT_MANIFEST.logo_store = wpassets/logo_store.png
    WINRT_MANIFEST.logo_wide = wpassets/logo_310x150.png
    WINRT_MANIFEST.publisherid = "CN=smbdsbrain"
    WINRT_MANIFEST.identity = "7f3c1a94-5d62-4b08-9e71-2ac6d4f80b13"
    WINRT_MANIFEST.publisher = "smbdsbrain"
    WINRT_MANIFEST.version = $$VERSION".0"
}

DEFINES += QT_USE_FAST_CONCATENATION QT_USE_FAST_OPERATOR_PLUS
# `qmake CONFIG+=devlog` keeps qDebug/qWarning compiled in and routes them to a
# file via the handler in src/main.cpp. Needed because Qt sends qDebug to
# OutputDebugString for GUI-subsystem apps on Windows, and Symbian has no
# console at all -- so an ordinary release build is completely silent on both.
# Attaching a console subsystem instead is not an option: it collides with Qt's
# qtmain and fails to link on WinMain@16.
devlog {
    DEFINES += SYMBOGRAM_DEVLOG=1
} else {
    CONFIG(release, debug|release):DEFINES += QT_NO_DEBUG_OUTPUT KG_NO_DEBUG KG_NO_INFO
}

QML_IMPORT_PATH =

win32:RC_FILE = symbogram.rc
macx:ICON = symbogram.icns

symbian {
    LIBS += -lavkon -lapgrfx -lcone -leikcore -lapmime

    contains(SYMBIAN_VERSION, Symbian3) {
        DEFINES += SYMBIAN3_READY=1
        include(pigler/qt-library/pigler.pri)
    }

    ICON = symbogram.svg
    # Own UID, so SymboGram installs alongside upstream Kutegram rather than
    # replacing it. 0xE0000000-0xEFFFFFFF is the unprotected range that needs no
    # Symbian Signed involvement. Derived as
    #     0xE0000000 | (crc32("SymboGram") & 0x0FFFFFFF)
    # purely so the number is reproducible rather than plucked from the air.
    TARGET.UID3 = 0xE4A51BF7
    DEFINES += SYMBIAN_UID=$$TARGET.UID3

    # These five are the complete set of user-grantable capabilities, so the
    # package self-signs and installs on a stock, unmodified device. SwEvent --
    # which upstream requests -- is a SYSTEM capability and is not self-signable;
    # a SIS carrying it is rejected on any phone without a developer certificate
    # or an installserver patch. It is only needed by the
    # BringToForeground/SendMessage path in src/platformutils.cpp, which is
    # #ifdef'd out to a StartDocument fallback unless you opt in with
    #     qmake CONFIG+=swevent
    TARGET.CAPABILITY += ReadUserData WriteUserData UserEnvironment NetworkServices LocalServices
    CONFIG(swevent) {
        TARGET.CAPABILITY += SwEvent
        DEFINES += SYMBOGRAM_HAVE_SWEVENT=1
    }

    # Upstream leaves these commented, inheriting qmlapplicationviewer's
    # 128 KB/32 MB default. The E6 has 256 MB; a 32 MB ceiling will not survive
    # long chats once history and image caches are live.
    TARGET.EPOCHEAPSIZE = 0x400000 0x4000000
    TARGET.EPOCSTACKSIZE = 0x14000

    supported_platforms = \
            "[0x1028315F],0,0,0,{\"S60ProductID\"}" \ # Symbian^1
            "[0x20022E6D],0,0,0,{\"S60ProductID\"}" \ # Symbian^3
            "[0x102032BE],0,0,0,{\"S60ProductID\"}" \ # Symbian 9.2
            "[0x102752AE],0,0,0,{\"S60ProductID\"}" \ # Symbian 9.3
            "[0x2003A678],0,0,0,{\"S60ProductID\"}"   # Symbian Belle

    default_deployment.pkg_prerules -= pkg_platform_dependencies
    supported_platforms_deployment.pkg_prerules += supported_platforms
    DEPLOYMENT += supported_platforms_deployment

    vendor_info = \
        " " \
        "; Localised Vendor name" \
        "%{\"smbdsbrain\"}" \
        " " \
        "; Unique Vendor name" \
        ":\"smbdsbrain\"" \
        " "
    package.pkg_prerules += vendor_info

    header = "$${LITERAL_HASH}{\"SymboGram\"},(0xE4A51BF7),$$PKG_VERSION,TYPE=SA,RU"
    package.pkg_prerules += header

    DEPLOYMENT += package
    DEPLOYMENT.installer_header = "$${LITERAL_HASH}{\"SymboGram Installer\"},(0xEEC88BF5),$$PKG_VERSION"
}

INCLUDEPATH += src

SOURCES +=  \
    src/main.cpp \
    src/dialogsmodel.cpp \
    src/messagesmodel.cpp \
    src/avatardownloader.cpp \
    src/foldersmodel.cpp \
    src/currentuserinfo.cpp \
    src/messageutil.cpp \
    src/platformutils.cpp

HEADERS += \
    src/dialogsmodel.h \
    src/messagesmodel.h \
    src/avatardownloader.h \
    src/foldersmodel.h \
    src/currentuserinfo.h \
    src/messageutil.h \
    src/platformutils.h

OTHER_FILES += \
    qtc_packaging/debian_harmattan/rules \
    qtc_packaging/debian_harmattan/README \
    qtc_packaging/debian_harmattan/copyright \
    qtc_packaging/debian_harmattan/control \
    qtc_packaging/debian_harmattan/compat \
    qtc_packaging/debian_harmattan/changelog

RESOURCES += \
    resources.qrc

include(libkg/libkg.pri)

include(qmlapplicationviewer/qmlapplicationviewer.pri)
qtcAddDeployment()

DISTFILES += \
    android/AndroidManifest.xml \
    android/gradle/wrapper/gradle-wrapper.jar \
    android/gradlew \
    android/res/values/libs.xml \
    android/build.gradle \
    android/gradle/wrapper/gradle-wrapper.properties \
    android/gradlew.bat

ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
