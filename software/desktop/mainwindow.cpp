#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QtCore/QDebug>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QFileDialog>
#include <QtGui/QImage>
#include <QtGui/QValidator>

#include "connectdialog.h"
#include "densinterface.h"
#include "slopecalibrationdialog.h"
#include "util.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , statusLabel_(new QLabel)
    , serialPort_(new QSerialPort(this))
    , densInterface_(new DensInterface(this))
{
    // Setup initial state of menu items
    ui->setupUi(this);
    ui->actionConnect->setEnabled(true);
    ui->actionDisconnect->setEnabled(false);
    ui->actionConfigure->setEnabled(true);
    ui->actionExit->setEnabled(true);

    ui->refreshSensorsPushButton->setEnabled(false);
    ui->screenshotButton->setEnabled(false);

    ui->statusBar->addWidget(statusLabel_);

    // Calibration (gain) field validation
    ui->med0LineEdit->setValidator(util::createFloatValidator(22.0, 27.0, 6));
    ui->med1LineEdit->setValidator(util::createFloatValidator(22.0, 27.0, 6));
    ui->high0LineEdit->setValidator(util::createFloatValidator(360.0, 440.0, 6));
    ui->high1LineEdit->setValidator(util::createFloatValidator(360.0, 440.0, 6));
    ui->max0LineEdit->setValidator(util::createFloatValidator(8500.0, 9900.0, 6));
    ui->max1LineEdit->setValidator(util::createFloatValidator(9100.0, 10700.0, 6));
    connect(ui->med0LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);
    connect(ui->med1LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);
    connect(ui->high0LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);
    connect(ui->high1LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);
    connect(ui->max0LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);
    connect(ui->max1LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalGainTextChanged);

    // Calibration (slope) field validation
    ui->b0LineEdit->setValidator(util::createFloatValidator(-100.0, 100.0, 6));
    ui->b1LineEdit->setValidator(util::createFloatValidator(-100.0, 100.0, 6));
    ui->b2LineEdit->setValidator(util::createFloatValidator(-100.0, 100.0, 6));
    connect(ui->b0LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalSlopeTextChanged);
    connect(ui->b1LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalSlopeTextChanged);
    connect(ui->b2LineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalSlopeTextChanged);

    // Calibration (reflection density) field validation
    ui->reflLoDensityLineEdit->setValidator(util::createFloatValidator(0.0, 2.5, 2));
    ui->reflLoReadingLineEdit->setValidator(util::createFloatValidator(0.0, 500.0, 6));
    ui->reflHiDensityLineEdit->setValidator(util::createFloatValidator(0.0, 2.5, 2));
    ui->reflHiReadingLineEdit->setValidator(util::createFloatValidator(0.0, 500.0, 6));
    connect(ui->reflLoDensityLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalReflectionTextChanged);
    connect(ui->reflLoReadingLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalReflectionTextChanged);
    connect(ui->reflHiDensityLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalReflectionTextChanged);
    connect(ui->reflHiReadingLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalReflectionTextChanged);

    // Calibration (transmission density) field validation
    ui->tranLoReadingLineEdit->setValidator(util::createFloatValidator(0.0, 500.0, 6));
    ui->tranHiDensityLineEdit->setValidator(util::createFloatValidator(0.0, 5.0, 2));
    ui->tranHiReadingLineEdit->setValidator(util::createFloatValidator(0.0, 500.0, 6));
    connect(ui->tranLoReadingLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalTransmissionTextChanged);
    connect(ui->tranHiDensityLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalTransmissionTextChanged);
    connect(ui->tranHiReadingLineEdit, &QLineEdit::textChanged, this, &MainWindow::onCalTransmissionTextChanged);

    // Top-level UI signals
    connect(ui->actionConnect, &QAction::triggered, this, &MainWindow::openConnection);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::closeConnection);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);
    //connect(ui->actionConfigure, &QAction::triggered, settings_, &SettingsDialog::show);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::about);

    // Diagnostics UI signals
    connect(ui->refreshSensorsPushButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetSystemInternalSensors);
    connect(ui->screenshotButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetDiagDisplayScreenshot);

    // Calibration UI signals
    connect(ui->calGetAllPushButton, &QPushButton::clicked, this, &MainWindow::onCalGetAllValues);
    connect(ui->gainGetPushButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetCalGain);
    connect(ui->gainSetPushButton, &QPushButton::clicked, this, &MainWindow::onCalGainSetClicked);
    connect(ui->slopeGetPushButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetCalSlope);
    connect(ui->slopeSetPushButton, &QPushButton::clicked, this, &MainWindow::onCalSlopeSetClicked);
    connect(ui->reflGetPushButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetCalReflection);
    connect(ui->reflSetPushButton, &QPushButton::clicked, this, &MainWindow::onCalReflectionSetClicked);
    connect(ui->tranGetPushButton, &QPushButton::clicked, densInterface_, &DensInterface::sendGetCalTransmission);
    connect(ui->tranSetPushButton, &QPushButton::clicked, this, &MainWindow::onCalTransmissionSetClicked);
    connect(ui->slopeCalPushButton, &QPushButton::clicked, this, &MainWindow::onSlopeCalibrationTool);

    // Densitometer interface update signals
    connect(densInterface_, &DensInterface::connectionError, this, &MainWindow::onConnectionError);
    connect(densInterface_, &DensInterface::densityReading, this, &MainWindow::onDensityReading);
    connect(densInterface_, &DensInterface::systemVersionResponse, this, &MainWindow::onSystemVersionResponse);
    connect(densInterface_, &DensInterface::systemBuildResponse, this, &MainWindow::onSystemBuildResponse);
    connect(densInterface_, &DensInterface::systemDeviceResponse, this, &MainWindow::onSystemDeviceResponse);
    connect(densInterface_, &DensInterface::systemUniqueId, this, &MainWindow::onSystemUniqueId);
    connect(densInterface_, &DensInterface::systemInternalSensors, this, &MainWindow::onSystemInternalSensors);
    connect(densInterface_, &DensInterface::diagDisplayScreenshot, this, &MainWindow::onDiagDisplayScreenshot);
    connect(densInterface_, &DensInterface::calGainResponse, this, &MainWindow::onCalGainResponse);
    connect(densInterface_, &DensInterface::calSlopeResponse, this, &MainWindow::onCalSlopeResponse);
    connect(densInterface_, &DensInterface::calReflectionResponse, this, &MainWindow::onCalReflectionResponse);
    connect(densInterface_, &DensInterface::calTransmissionResponse, this, &MainWindow::onCalTransmissionResponse);

    // Initialize all fields with blank values
    onSystemVersionResponse();
    onSystemBuildResponse();
    onSystemDeviceResponse();
    onSystemUniqueId();
    onSystemInternalSensors();

    refreshButtonState();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::openConnection()
{
    qDebug() << "Open connection";
    ConnectDialog *dialog = new ConnectDialog(this);
    connect(dialog, &QDialog::finished, this, &MainWindow::onOpenConnectionDialogFinished);
    dialog->show();
}

void MainWindow::onOpenConnectionDialogFinished(int result)
{
    ConnectDialog *dialog = dynamic_cast<ConnectDialog *>(sender());
    dialog->deleteLater();

    if (result == QDialog::Accepted) {
        const ConnectDialog::Settings p = dialog->settings();
        qDebug() << "Connecting to:" << p.name;
        serialPort_->setPortName(p.name);
        serialPort_->setBaudRate(p.baudRate);
        serialPort_->setDataBits(p.dataBits);
        serialPort_->setParity(p.parity);
        serialPort_->setStopBits(p.stopBits);
        serialPort_->setFlowControl(p.flowControl);
        if (serialPort_->open(QIODevice::ReadWrite)) {
            if (densInterface_->connectToDevice(serialPort_)) {
                ui->actionConnect->setEnabled(false);
                ui->actionDisconnect->setEnabled(true);
                statusLabel_->setText(tr("Connected to %1 : %2, %3, %4, %5, %6")
                                  .arg(p.name, p.stringBaudRate, p.stringDataBits,
                                       p.stringParity, p.stringStopBits,
                                       p.stringFlowControl));
                densInterface_->sendSetMeasurementFormat(DensInterface::FormatExtended);
                densInterface_->sendGetSystemBuild();
                densInterface_->sendGetSystemDeviceInfo();
                densInterface_->sendGetSystemUID();
                densInterface_->sendGetSystemInternalSensors();
                refreshButtonState();
            } else {
                serialPort_->close();
                QMessageBox::critical(this, tr("Error"), tr("Unrecognized device"));
                statusLabel_->setText(tr("Unrecognized device"));
            }
        } else {
            QMessageBox::critical(this, tr("Error"), serialPort_->errorString());
            statusLabel_->setText(tr("Open error"));
        }
    }
}

void MainWindow::closeConnection()
{
    qDebug() << "Close connection";
    densInterface_->disconnectFromDevice();
    if (serialPort_->isOpen()) {
        serialPort_->close();
    }
    statusLabel_->setText(tr("Disconnected"));
    refreshButtonState();
    ui->actionConnect->setEnabled(true);
    ui->actionDisconnect->setEnabled(false);
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About"),
                       tr("<b>Printalyzer Densitometer Desktop v0.1</b><br>"
                          "<br>"
                          "Copyright 2021 Dektronics, Inc. All rights reserved."));
}

void MainWindow::refreshButtonState()
{
    if (densInterface_->connected()) {
        ui->refreshSensorsPushButton->setEnabled(true);
        ui->screenshotButton->setEnabled(true);
        ui->calGetAllPushButton->setEnabled(true);
        ui->gainGetPushButton->setEnabled(true);
        ui->slopeGetPushButton->setEnabled(true);
        ui->reflGetPushButton->setEnabled(true);
        ui->tranGetPushButton->setEnabled(true);

        // Populate read-only edit fields that are only set
        // via the protocol for consistency of the data formats
        if (ui->low0LineEdit->text().isEmpty()) {
            ui->low0LineEdit->setText("1");
        }
        if (ui->low1LineEdit->text().isEmpty()) {
            ui->low1LineEdit->setText("1");
        }
        if (ui->tranLoDensityLineEdit->text().isEmpty()) {
            ui->tranLoDensityLineEdit->setText("0.00");
        }
    } else {
        ui->refreshSensorsPushButton->setEnabled(false);
        ui->screenshotButton->setEnabled(false);
        ui->calGetAllPushButton->setEnabled(false);
        ui->gainGetPushButton->setEnabled(false);
        ui->slopeGetPushButton->setEnabled(false);
        ui->reflGetPushButton->setEnabled(false);
        ui->tranGetPushButton->setEnabled(false);
    }

    onCalGainTextChanged();
    onCalSlopeTextChanged();
    onCalReflectionTextChanged();
    onCalTransmissionTextChanged();
}

void MainWindow::onConnectionError()
{
    closeConnection();
}

void MainWindow::onDensityReading(DensInterface::DensityType type, float dValue, float rawValue, float corrValue)
{
    Q_UNUSED(rawValue)
    Q_UNUSED(corrValue)

    // Update main tab contents
    if (type == DensInterface::DensityReflection) {
        ui->readingTypeLogoLabel->setPixmap(QPixmap(QString::fromUtf8(":/images/reflection-icon.png")));
        ui->readingTypeNameLabel->setText(tr("Reflection"));
    } else {
        ui->readingTypeLogoLabel->setPixmap(QPixmap(QString::fromUtf8(":/images/transmission-icon.png")));
        ui->readingTypeNameLabel->setText(tr("Transmission"));
    }

    ui->readingValueLineEdit->setText(QString("%1D").arg(dValue, 4, 'f', 2));

    // Update calibration tab fields, if focused
    if (type == DensInterface::DensityReflection) {
        if (ui->reflLoReadingLineEdit->hasFocus()) {
            ui->reflLoReadingLineEdit->setText(QString::number(corrValue, 'f'));
        } else if (ui->reflHiReadingLineEdit->hasFocus()) {
            ui->reflHiReadingLineEdit->setText(QString::number(corrValue, 'f'));
        }
    } else {
        if (ui->tranLoReadingLineEdit->hasFocus()) {
            ui->tranLoReadingLineEdit->setText(QString::number(corrValue, 'f'));
        } else if (ui->tranHiReadingLineEdit->hasFocus()) {
            ui->tranHiReadingLineEdit->setText(QString::number(corrValue, 'f'));
        }
    }
}

void MainWindow::onCalGetAllValues()
{
    densInterface_->sendGetCalGain();
    densInterface_->sendGetCalSlope();
    densInterface_->sendGetCalReflection();
    densInterface_->sendGetCalTransmission();
}

void MainWindow::onCalGainSetClicked()
{
    bool ok;
    float med0 = ui->med0LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float med1 = ui->med1LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float high0 = ui->high0LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float high1 = ui->high1LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float max0 = ui->max0LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float max1 = ui->max1LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    densInterface_->sentSetCalGain(med0, med1, high0, high1, max0, max1);
}

void MainWindow::onCalSlopeSetClicked()
{
    bool ok;
    float b0 = ui->b0LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float b1 = ui->b1LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float b2 = ui->b2LineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    densInterface_->sendSetCalSlope(b0, b1, b2);
}

void MainWindow::onCalReflectionSetClicked()
{
    bool ok;
    float loDensity = ui->reflLoDensityLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float loReading = ui->reflLoReadingLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float hiDensity = ui->reflHiDensityLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float hiReading = ui->reflHiReadingLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    densInterface_->sendSetCalReflection(loDensity, loReading, hiDensity, hiReading);
}

void MainWindow::onCalTransmissionSetClicked()
{
    bool ok;
    float loReading = ui->tranLoReadingLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float hiDensity = ui->tranHiDensityLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    float hiReading = ui->tranHiReadingLineEdit->text().toFloat(&ok);
    if (!ok) { return; }

    densInterface_->sendSetCalTransmission(0.0F, loReading, hiDensity, hiReading);
}

void MainWindow::onCalGainTextChanged()
{
    if (densInterface_->connected()
            && !ui->low0LineEdit->text().isEmpty()
            && !ui->low1LineEdit->text().isEmpty()
            && ui->med0LineEdit->hasAcceptableInput()
            && ui->med1LineEdit->hasAcceptableInput()
            && ui->high0LineEdit->hasAcceptableInput()
            && ui->high1LineEdit->hasAcceptableInput()
            && ui->max0LineEdit->hasAcceptableInput()
            && ui->max1LineEdit->hasAcceptableInput()) {
        ui->gainSetPushButton->setEnabled(true);
    } else {
        ui->gainSetPushButton->setEnabled(false);
    }
}

void MainWindow::onCalSlopeTextChanged()
{
    if (densInterface_->connected()
            && ui->b0LineEdit->hasAcceptableInput()
            && ui->b1LineEdit->hasAcceptableInput()
            && ui->b2LineEdit->hasAcceptableInput()) {
        ui->slopeSetPushButton->setEnabled(true);
    } else {
        ui->slopeSetPushButton->setEnabled(false);
    }
}

void MainWindow::onCalReflectionTextChanged()
{
    if (densInterface_->connected()
            && ui->reflLoDensityLineEdit->hasAcceptableInput()
            && ui->reflLoReadingLineEdit->hasAcceptableInput()
            && ui->reflHiDensityLineEdit->hasAcceptableInput()
            && ui->reflHiReadingLineEdit->hasAcceptableInput()) {
        ui->reflSetPushButton->setEnabled(true);
    } else {
        ui->reflSetPushButton->setEnabled(false);
    }
}

void MainWindow::onCalTransmissionTextChanged()
{
    if (densInterface_->connected()
            && !ui->tranLoDensityLineEdit->text().isEmpty()
            && ui->tranLoReadingLineEdit->hasAcceptableInput()
            && ui->tranHiDensityLineEdit->hasAcceptableInput()
            && ui->tranHiReadingLineEdit->hasAcceptableInput()) {
        ui->tranSetPushButton->setEnabled(true);
    } else {
        ui->tranSetPushButton->setEnabled(false);
    }
}

void MainWindow::onSystemVersionResponse()
{
    if (densInterface_->projectName().isEmpty()) {
        ui->nameLabel->setText("Printalyzer Densitometer");
    } else {
        ui->nameLabel->setText(QString("<b>%1</b>").arg(densInterface_->projectName()));
    }
    ui->versionLabel->setText(tr("Version: %1").arg(densInterface_->version()));
}

void MainWindow::onSystemBuildResponse()
{
    ui->buildDateLabel->setText(tr("Date: %1").arg(densInterface_->buildDate().toString("yyyy-MM-dd hh:mm")));
    ui->buildDescribeLabel->setText(tr("Commit: %1").arg(densInterface_->buildDescribe()));
    if (densInterface_->buildChecksum() == 0) {
        ui->checksumLabel->setText(tr("Checksum: %1").arg(""));
    } else {
        ui->checksumLabel->setText(tr("Checksum: %1").arg(densInterface_->buildChecksum(), 0, 16));
    }
}

void MainWindow::onSystemDeviceResponse()
{
    ui->halVersionLabel->setText(tr("HAL Version: %1").arg(densInterface_->halVersion()));
    ui->mcuDevIdLabel->setText(tr("MCU Device ID: %1").arg(densInterface_->mcuDeviceId()));
    ui->mcuRevIdLabel->setText(tr("MCU Revision ID: %1").arg(densInterface_->mcuRevisionId()));
    ui->mcuSysClockLabel->setText(tr("MCU SysClock: %1").arg(densInterface_->mcuSysClock()));
}

void MainWindow::onSystemUniqueId()
{
    ui->uniqueIdLabel->setText(tr("UID: %1").arg(densInterface_->uniqueId()));
}

void MainWindow::onSystemInternalSensors()
{
    ui->mcuVddaLabel->setText(tr("Vdda: %1").arg(densInterface_->mcuVdda()));
    ui->mcuTempLabel->setText(tr("Temperature: %1").arg(densInterface_->mcuTemp()));
}

void MainWindow::onDiagDisplayScreenshot(const QByteArray &data)
{
    qDebug() << "Got screenshot:" << data.size();
    QImage image = QImage::fromData(data, "XBM");
    if (!image.isNull()) {
        image = image.mirrored(true, true);
        image.invertPixels();

        QString fileName = QFileDialog::getSaveFileName(this, tr("Save Screenshot"),
                                   "screenshot.png",
                                   tr("Images (*.png *.jpg)"));
        if (!fileName.isEmpty()) {
            if (image.save(fileName)) {
                qDebug() << "Saved screenshot to:" << fileName;
            } else {
                qDebug() << "Error saving screenshot to:" << fileName;
            }
        }
    }
}

void MainWindow::onCalGainResponse()
{
    ui->low0LineEdit->setText(QString::number(densInterface_->calGainLow0(), 'f'));
    ui->low1LineEdit->setText(QString::number(densInterface_->calGainLow1(), 'f'));

    ui->med0LineEdit->setText(QString::number(densInterface_->calGainMedium0(), 'f'));
    ui->med1LineEdit->setText(QString::number(densInterface_->calGainMedium1(), 'f'));

    ui->high0LineEdit->setText(QString::number(densInterface_->calGainHigh0(), 'f'));
    ui->high1LineEdit->setText(QString::number(densInterface_->calGainHigh1(), 'f'));

    ui->max0LineEdit->setText(QString::number(densInterface_->calGainMaximum0(), 'f'));
    ui->max1LineEdit->setText(QString::number(densInterface_->calGainMaximum1(), 'f'));
}

void MainWindow::onCalSlopeResponse()
{
    ui->b0LineEdit->setText(QString::number(densInterface_->calSlopeB0(), 'f'));
    ui->b1LineEdit->setText(QString::number(densInterface_->calSlopeB1(), 'f'));
    ui->b2LineEdit->setText(QString::number(densInterface_->calSlopeB2(), 'f'));
}

void MainWindow::onCalReflectionResponse()
{
    ui->reflLoDensityLineEdit->setText(QString::number(densInterface_->calReflectionLoDensity(), 'f', 2));
    ui->reflLoReadingLineEdit->setText(QString::number(densInterface_->calReflectionLoReading(), 'f', 6));
    ui->reflHiDensityLineEdit->setText(QString::number(densInterface_->calReflectionHiDensity(), 'f', 2));
    ui->reflHiReadingLineEdit->setText(QString::number(densInterface_->calReflectionHiReading(), 'f', 6));
}

void MainWindow::onCalTransmissionResponse()
{
    ui->tranLoDensityLineEdit->setText(QString::number(densInterface_->calTransmissionLoDensity(), 'f', 2));
    ui->tranLoReadingLineEdit->setText(QString::number(densInterface_->calTransmissionLoReading(), 'f', 6));
    ui->tranHiDensityLineEdit->setText(QString::number(densInterface_->calTransmissionHiDensity(), 'f', 2));
    ui->tranHiReadingLineEdit->setText(QString::number(densInterface_->calTransmissionHiReading(), 'f', 6));
}

void MainWindow::onSlopeCalibrationTool()
{
    SlopeCalibrationDialog *dialog = new SlopeCalibrationDialog(densInterface_, this);
    connect(dialog, &QDialog::finished, this, &MainWindow::onSlopeCalibrationToolFinished);
    dialog->show();
}

void MainWindow::onSlopeCalibrationToolFinished(int result)
{
    SlopeCalibrationDialog *dialog = dynamic_cast<SlopeCalibrationDialog *>(sender());
    dialog->deleteLater();

    if (result == QDialog::Accepted) {
        auto result = dialog->calValues();
        ui->b0LineEdit->setText(QString::number(std::get<0>(result), 'f'));
        ui->b1LineEdit->setText(QString::number(std::get<1>(result), 'f'));
        ui->b2LineEdit->setText(QString::number(std::get<2>(result), 'f'));
    }
}