#include "slopecalibrationdialog.h"
#include "ui_slopecalibrationdialog.h"

#include <QClipboard>
#include <QMimeData>
#include <QStyledItemDelegate>
#include <QThread>
#include <QSettings>
#include <QDebug>
#include <cmath>
#include "floatitemdelegate.h"
#include "util.h"

//TODO Find a way to store the raw floats, rather than being limited by string formatting

SlopeCalibrationDialog::SlopeCalibrationDialog(DensInterface *densInterface, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SlopeCalibrationDialog),
    densInterface_(densInterface),
    calValues_{qSNaN(), qSNaN(), qSNaN()}
{
    ui->setupUi(this);

    ui->actionCut->setShortcut(QKeySequence::Cut);
    ui->actionCopy->setShortcut(QKeySequence::Copy);
    ui->actionPaste->setShortcut(QKeySequence::Paste);
    ui->actionDelete->setShortcut(QKeySequence::Delete);

    addAction(ui->actionCut);
    addAction(ui->actionCopy);
    addAction(ui->actionPaste);
    addAction(ui->actionDelete);

    connect(ui->actionCut, &QAction::triggered, this, &SlopeCalibrationDialog::onActionCut);
    connect(ui->actionCopy, &QAction::triggered, this, &SlopeCalibrationDialog::onActionCopy);
    connect(ui->actionPaste, &QAction::triggered, this, &SlopeCalibrationDialog::onActionPaste);
    connect(ui->actionDelete, &QAction::triggered, this, &SlopeCalibrationDialog::onActionDelete);

    connect(ui->calculatePushButton, &QPushButton::clicked, this, &SlopeCalibrationDialog::onCalculateResults);
    connect(ui->clearPushButton, &QPushButton::clicked, this, &SlopeCalibrationDialog::onClearReadings);

    model_ = new QStandardItemModel(22, 2, this);
    model_->setHorizontalHeaderLabels(QStringList() << tr("Density") << tr("Raw Reading"));

    QStringList verticalLabels;
    for (int i = 0; i < model_->rowCount(); i++) {
        verticalLabels.append(QString::number(i));
    }
    model_->setVerticalHeaderLabels(verticalLabels);

    ui->tableView->setModel(model_);
    ui->tableView->setColumnWidth(0, 80);
    ui->tableView->setColumnWidth(1, 150);
    ui->tableView->setItemDelegateForColumn(0, new FloatItemDelegate(0.0, 5.0, 2));
    ui->tableView->setItemDelegateForColumn(1, new FloatItemDelegate(0.0, 1000.0, 6));

    if (densInterface_) {
        connect(densInterface_, &DensInterface::densityReading, this, &SlopeCalibrationDialog::onDensityReading);
    }
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    // Preload calibrated numbers for the step wedge, with basic validation,
    // if they have been stored in app settings. As this is primarily intended
    // to help with device manufacturing use cases, no UI is currently provided
    // for populating this data.
    QSettings settings;
    QVariantList scaleList = settings.value("slope_calibration/scale").toList();
    if (!scaleList.isEmpty()) {
        int i = 0;
        for (const QVariant &entry : qAsConst(scaleList)) {
            bool ok;
            const QString entryStr = entry.toString();
            const float entryNum = entryStr.toFloat(&ok);
            if (ok && entryNum >= 0.0F && entryNum <= 5.0F) {
                model_->setItem(i++, 0, new QStandardItem(entryStr));
            }
        }
    }
}

SlopeCalibrationDialog::~SlopeCalibrationDialog()
{
    delete ui;
}

std::tuple<float, float, float> SlopeCalibrationDialog::calValues() const
{
    return calValues_;
}

void SlopeCalibrationDialog::onDensityReading(DensInterface::DensityType type, float dValue, float dZero, float rawValue, float corrValue)
{
    Q_UNUSED(dValue)
    Q_UNUSED(dZero)
    Q_UNUSED(corrValue)

    // Only using transmission readings for this
    if (type != DensInterface::DensityTransmission) {
        return;
    }

    if (qIsNaN(rawValue) || rawValue < 0.0F) {
        return;
    }

    QPair<int, int> ulIndex = upperLeftActiveIndex();
    int row = ulIndex.first;
    if (row < 0) { row = 0; }

    QString numStr = QString::number(rawValue, 'f', 6);
    model_->setItem(row, 1, new QStandardItem(numStr));

    if (row < model_->rowCount() - 1) {
        QModelIndex index = model_->index(row + 1, 1);
        ui->tableView->setCurrentIndex(index);
    }
}

void SlopeCalibrationDialog::onActionCut()
{
    onActionCopy();
    onActionDelete();
}

void SlopeCalibrationDialog::onActionCopy()
{
    QModelIndexList selected = ui->tableView->selectionModel()->selectedIndexes();
    std::sort(selected.begin(), selected.end());

    int curRow = -1;
    QString num1;
    QString num2;
    QVector<QPair<QString,QString>> numList;
    bool hasCol1 = false;
    bool hasCol2 = false;

    for (const QModelIndex &index : qAsConst(selected)) {
        if (curRow != index.row()) {
            if (curRow != -1 && (!num1.isEmpty() || !num2.isEmpty())) {
                numList.append(qMakePair(num1, num2));
            }
            num1 = QString();
            num2 = QString();
            curRow = index.row();
        }

        QStandardItem *item = model_->itemFromIndex(index);
        if (item && index.column() == 0) {
            num1 = item->text();
            hasCol1 = true;
        } else if (item && index.column() == 1) {
            num2 = item->text();
            hasCol2 = true;
        }
    }
    if (curRow != -1 && (!num1.isEmpty() || !num2.isEmpty())) {
        numList.append(qMakePair(num1, num2));
    }

    QString copiedText;
    for (const auto &numElement : numList) {
        if ((hasCol1 || hasCol2) && !copiedText.isEmpty()) {
#if defined(Q_OS_WIN)
            copiedText.append(QLatin1String("\r\n"));
#else
            copiedText.append(QLatin1String("\n"));
#endif
        }
        if (hasCol1 && hasCol2) {
            copiedText.append(numElement.first);
            copiedText.append(QChar('\t'));
            copiedText.append(numElement.second);
        } else if (hasCol1) {
            copiedText.append(numElement.first);
        } else if (hasCol2) {
            copiedText.append(numElement.second);
        }
    }

    // Move to the clipboard
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(copiedText, QClipboard::Clipboard);

    if (clipboard->supportsSelection()) {
        clipboard->setText(copiedText, QClipboard::Selection);
    }

#if defined(Q_OS_UNIX)
    QThread::msleep(1);
#endif
}

void SlopeCalibrationDialog::onActionPaste()
{
    // Capture and split the text to be pasted
    const QClipboard *clipboard = QApplication::clipboard();
    const QMimeData *mimeData = clipboard->mimeData();
    QList<QPair<float,float>> numList;
    if (mimeData->hasText()) {
        const QString text = mimeData->text();
        const QStringList elements = text.split(QRegExp("\n|\r\n|\r"), Qt::SkipEmptyParts);
        for (const QString& element : elements) {
            QStringList rowElements = element.split(QRegExp("[,;]\\s*|\\s+"), Qt::SkipEmptyParts);
            bool ok;
            float num1 = qQNaN();
            float num2 = qQNaN();
            if (rowElements.size() > 0) {
                num1 = rowElements.at(0).toFloat(&ok);
                if (!ok) { num1 = qQNaN(); }
            }
            if (rowElements.size() > 1) {
                num2 = rowElements.at(1).toFloat(&ok);
                if (!ok) { num2 = qQNaN(); }
            }
            if (!qIsNaN(num1) || !qIsNaN(num2)) {
                numList.append(qMakePair(num1, num2));
            }
        }
    }

    // Find the upper-left corner of the paste area
    QPair<int, int> ulIndex = upperLeftActiveIndex();
    int row = ulIndex.first;
    int col = ulIndex.second;

    // Paste the values
    if (!numList.isEmpty() && row >= 0 && col >= 0) {
        for (auto numElement : numList) {
            QString numStr;
            if (col == 0) {
                if (!qIsNaN(numElement.first)) {
                    numStr = QString::number(numElement.first, 'f', 2);
                    model_->setItem(row, col, new QStandardItem(numStr));
                }
                if (!qIsNaN(numElement.second)) {
                    numStr = QString::number(numElement.second, 'f', 6);
                    model_->setItem(row, col + 1, new QStandardItem(numStr));
                }
            } else {
                if (!qIsNaN(numElement.first)) {
                    numStr = QString::number(numElement.first, 'f', 6);
                    model_->setItem(row, col, new QStandardItem(numStr));
                } else if (!qIsNaN(numElement.second)) {
                    numStr = QString::number(numElement.second, 'f', 6);
                    model_->setItem(row, col, new QStandardItem(numStr));
                }
            }
            row++;
            if (row >= model_->rowCount()) { break; }
        }
    }
}

void SlopeCalibrationDialog::onActionDelete()
{
    QModelIndexList selected = ui->tableView->selectionModel()->selectedIndexes();
    for (const QModelIndex &index : qAsConst(selected)) {
        QStandardItem *item = model_->itemFromIndex(index);
        if (item) {
            item->clearData();
        }
    }
}

void SlopeCalibrationDialog::onCalculateResults()
{
    qDebug() << "Calculate Results";
    QList<float> xList;
    QList<float> yList;
    float base_measurement = qSNaN();

    for (int row = 0; row < model_->rowCount(); row++) {
        float density = itemValueAsFloat(row, 0);
        float measurement = itemValueAsFloat(row, 1);
        if (qIsNaN(density) || qIsNaN(measurement)) {
            break;
        }
        if (row == 0) {
            if (density < 0.0F || density > 0.001F) {
                qDebug() << "First row density must be zero:" << density;
                break;
            }
            float x = std::log10(measurement);
            xList.append(x);
            yList.append(x);
            base_measurement = measurement;
        } else {
            float x = std::log10(measurement);
            float y = std::log10(base_measurement / std::pow(10.0F, density));
            xList.append(x);
            yList.append(y);
        }
    }
    qDebug() << "Have" << xList.size() << "rows of data";
    if (xList.size() < 5) {
        qDebug() << "Not enough rows of data";
        return;
    }

    auto beta = polyfit(xList, yList);
    ui->b0LineEdit->setText(QString::number(std::get<0>(beta), 'f'));
    ui->b1LineEdit->setText(QString::number(std::get<1>(beta), 'f'));
    ui->b2LineEdit->setText(QString::number(std::get<2>(beta), 'f'));
    calValues_ = beta;
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

std::tuple<float, float, float> SlopeCalibrationDialog::polyfit(const QList<float> &xList, const QList<float> &yList)
{
    // Polynomial Fitting, based on this implementation:
    // https://www.bragitoff.com/2018/06/polynomial-fitting-c-program/

    if (xList.isEmpty() || xList.size() != yList.size()) {
        return {qSNaN(), qSNaN(), qSNaN()};
    }

    // Number of data points
    const int N = xList.size();

    // Degree of polynomial
    const int n = 2;

    // An array of size 2*n+1 for storing N, Sig xi, Sig xi^2, ....
    // which are the independent components of the normal matrix
    double X[2*n+1];
    for (int i=0; i <= 2 * n; i++) {
        X[i] = 0;
        for (int j=0; j < N; j++) {
            X[i] = X[i] + std::pow((double)xList[j], i);
        }
    }

    // The normal augmented matrix
    //double B[n+1][n+2];
    double **B = util::make2DArray(n+1, n+2);
    // rhs
    double Y[n+1];
    for (int i = 0; i <= n; i++) {
        Y[i] = 0;
        for (int j=0; j < N; j++) {
            Y[i] = Y[i] + std::pow((double)xList[j], i) * (double)yList[j];
        }
    }
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= n; j++) {
            B[i][j] = X[i + j];
        }
    }
    for (int i = 0; i <= n; i++) {
        B[i][n + 1] = Y[i];
    }

    double A[n+1];
    gaussEliminationLS(n+1, n+2, B, A);

    for(int i = 0; i <= n; i++) {
        qDebug().nospace() << "B[" << i << "] = " << A[i];
    }

    util::free2DArray(B, n+1);

    return {(float)A[0], (float)A[1], (float)A[2]};
}

void SlopeCalibrationDialog::gaussEliminationLS(int m, int n, double **a /*[m][n]*/, double *x /*[n-1]*/)
{
    for (int i = 0; i < m-1; i++) {
        // Partial Pivoting
        for (int k = i+1; k < m; k++) {
            // If diagonal element(absolute vallue) is smaller than any of the terms below it
            if (std::abs(a[i][i]) < std::abs(a[k][i])) {
                // Swap the rows
                for (int j=0; j < n; j++) {
                    double temp;
                    temp = a[i][j];
                    a[i][j] = a[k][j];
                    a[k][j] = temp;
                }
            }
        }
        // Begin Gauss Elimination
        for (int k = i + 1; k < m; k++) {
            double term = a[k][i] / a[i][i];
            for (int j = 0; j < n; j++) {
                a[k][j] = a[k][j] - term * a[i][j];
            }
        }
    }
    // Begin Back-substitution
    for (int i = m-1; i >= 0; i--) {
        x[i] = a[i][n-1];
        for (int j = i+1; j < n-1; j++) {
            x[i] = x[i] - a[i][j] * x[j];
        }
        x[i] = x[i] / a[i][i];
    }
}

void SlopeCalibrationDialog::onClearReadings()
{
    for (int i = 0; i < model_->rowCount(); i++) {
        model_->setItem(i, 1, nullptr);
    }
    QModelIndex index = model_->index(0, 0);
    ui->tableView->setCurrentIndex(index);
    ui->tableView->selectionModel()->clearSelection();
    ui->tableView->setColumnWidth(0, 80);
    ui->tableView->setColumnWidth(1, 150);
    ui->tableView->scrollToTop();
}

QPair<int, int> SlopeCalibrationDialog::upperLeftActiveIndex() const
{
    int row = -1;
    int col = -1;
    QModelIndexList selected = ui->tableView->selectionModel()->selectedIndexes();
    selected.append(ui->tableView->selectionModel()->currentIndex());
    for (const QModelIndex &index : qAsConst(selected)) {
        if (row == -1 || index.row() < row) {
            row = index.row();
        }
        if (col == -1 || index.column() < col) {
            col = index.column();
        }
    }
    return qMakePair(row, col);
}

float SlopeCalibrationDialog::itemValueAsFloat(int row, int col) const
{
    float value;
    bool ok = false;
    QStandardItem *item = model_->item(row, col);
    if (item) {
        value = item->text().toFloat(&ok);
    }

    if (!ok) {
        value = qQNaN();
    }
    return value;
}
