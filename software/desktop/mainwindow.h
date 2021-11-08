#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "densinterface.h"

QT_BEGIN_NAMESPACE

class QLabel;
class QSerialPort;

namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void openConnection();
    void onOpenConnectionDialogFinished(int result);
    void closeConnection();
    void about();

    void onConnectionError();

    void onDensityReading(DensInterface::DensityType type, float dValue, float rawValue, float corrValue);

    void onCalGetAllValues();
    void onCalGainSetClicked();
    void onCalSlopeSetClicked();
    void onCalReflectionSetClicked();
    void onCalTransmissionSetClicked();

    void onCalGainTextChanged();
    void onCalSlopeTextChanged();
    void onCalReflectionTextChanged();
    void onCalTransmissionTextChanged();

    void onSystemVersionResponse();
    void onSystemBuildResponse();
    void onSystemDeviceResponse();
    void onSystemUniqueId();
    void onSystemInternalSensors();

    void onDiagDisplayScreenshot(const QByteArray &data);

    void onCalGainResponse();
    void onCalSlopeResponse();
    void onCalReflectionResponse();
    void onCalTransmissionResponse();

    void onSlopeCalibrationTool();
    void onSlopeCalibrationToolFinished(int result);

private:
    void refreshButtonState();

    Ui::MainWindow *ui = nullptr;
    QLabel *statusLabel_ = nullptr;
    QSerialPort *serialPort_ = nullptr;
    DensInterface *densInterface_ = nullptr;
};

#endif // MAINWINDOW_H