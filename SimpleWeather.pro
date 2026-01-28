QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = SimpleWeather
TEMPLATE = app

DEFINES += QT_DEPRECATED_WARNINGS


CONFIG += c++11

SOURCES += \
        main.cpp \
        mainwindow.cpp \
        translator.cpp

HEADERS += \
        mainwindow.h \
        translator.h

FORMS += \
        mainwindow.ui

RESOURCES += \
    res.qrc

RC_FILE += res/file.rc
OTHER_FILES += res/file.rc
