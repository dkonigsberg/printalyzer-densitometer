QT       += core gui serialport

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

QMAKE_CXXFLAGS += -Wno-deprecated-copy

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    connectdialog.cpp \
    denscommand.cpp \
    densinterface.cpp \
    floatitemdelegate.cpp \
    main.cpp \
    mainwindow.cpp \
    slopecalibrationdialog.cpp \
    util.cpp

HEADERS += \
    connectdialog.h \
    denscommand.h \
    densinterface.h \
    floatitemdelegate.h \
    mainwindow.h \
    slopecalibrationdialog.h \
    util.h

FORMS += \
    connectdialog.ui \
    mainwindow.ui \
    slopecalibrationdialog.ui

RESOURCES += \
    densitometer.qrc

TRANSLATIONS += \
    densitometer_en_US.ts
CONFIG += lrelease
CONFIG += embed_translations

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target