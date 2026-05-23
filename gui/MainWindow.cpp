#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace fdm_gui {

namespace {

constexpr int kColIndex = 0;
constexpr int kColRange = 1;
constexpr int kColReceived = 2;
constexpr int kColPercent = 3;
constexpr int kColAttempts = 4;
constexpr int kColStatus = 5;
constexpr int kColCount = 6;

// Smooth solid-fill progress style: overrides whatever the platform theme
// would otherwise paint (often a segmented "chunked" look on KDE/Fusion).
const char* const kSmoothProgressStyle = R"(
QProgressBar {
    border: 1px solid palette(mid);
    border-radius: 4px;
    background-color: palette(base);
    text-align: center;
    min-height: 14px;
}
QProgressBar::chunk {
    background-color: #3a82e6;
    border-radius: 3px;
    margin: 0px;
}
)";

QString humanBytes(qint64 bytes) {
    if (bytes < 0) return "?";
    constexpr qint64 KiB = 1024;
    constexpr qint64 MiB = 1024 * KiB;
    constexpr qint64 GiB = 1024 * MiB;
    if (bytes >= GiB)
        return QString::number(static_cast<double>(bytes) / GiB, 'f', 2) + " GiB";
    if (bytes >= MiB)
        return QString::number(static_cast<double>(bytes) / MiB, 'f', 2) + " MiB";
    if (bytes >= KiB)
        return QString::number(static_cast<double>(bytes) / KiB, 'f', 1) + " KiB";
    return QString::number(bytes) + " B";
}

QString humanRate(double bytesPerSec) {
    return humanBytes(static_cast<qint64>(bytesPerSec)) + "/s";
}

QString statusText(int status) {
    switch (status) {
        case 0: return "Active";
        case 1: return "Done";
        case 2: return "Failed";
        default: return "?";
    }
}

QString filenameFromUrl(const QString& url) {
    QUrl u(url);
    if (!u.isValid()) return {};
    QString p = u.path();
    if (p.isEmpty() || p.endsWith('/')) return {};
    return QFileInfo(p).fileName();
}

// "" if no dot (or trailing dot). Includes the leading dot.
QString extensionOf(const QString& filename) {
    const int dot = filename.lastIndexOf('.');
    if (dot <= 0 || dot >= filename.size() - 1) return {};
    return filename.mid(dot);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    bridge_ = new EngineBridge(this);
    buildUi();
    buildMenuAndToolbar();

    connect(urlEdit_, &QLineEdit::textChanged, this, &MainWindow::onUrlChanged);
    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);

    connect(bridge_, &EngineBridge::started, this, &MainWindow::onStarted);
    connect(bridge_, &EngineBridge::progressUpdated, this, &MainWindow::onProgressUpdated);
    connect(bridge_, &EngineBridge::downloadFinished, this, &MainWindow::onFinished);
    connect(bridge_, &EngineBridge::downloadFailed, this, &MainWindow::onFailed);
}

void MainWindow::buildMenuAndToolbar() {
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* newAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_FileDialogNewFolder), "&New Download");
    newAction->setShortcut(QKeySequence::New);
    newAction->setStatusTip("Open another download window");
    connect(newAction, &QAction::triggered, this, &MainWindow::onNewDownloadWindow);

    fileMenu->addSeparator();

    auto* quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // Top toolbar -- visible buttons for the most common action.
    auto* toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(newAction);

    statusBar()->showMessage("Ready");
}

void MainWindow::applyProgressBarStyle(QProgressBar* bar) {
    bar->setStyleSheet(kSmoothProgressStyle);
}

void MainWindow::buildUi() {
    setWindowTitle("Fresh Download Manager");
    resize(860, 560);

    urlEdit_ = new QLineEdit;
    urlEdit_->setPlaceholderText("https://example.com/file.iso");

    dirEdit_ = new QLineEdit;
    dirEdit_->setText(
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    browseBtn_ = new QPushButton("Browse…");

    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(dirEdit_);
    dirRow->addWidget(browseBtn_);

    nameEdit_ = new QLineEdit;
    nameEdit_->setPlaceholderText("filename (extension auto-filled from URL)");

    startBtn_ = new QPushButton("Start download");
    startBtn_->setDefault(true);

    auto* form = new QFormLayout;
    form->addRow("URL:", urlEdit_);
    form->addRow("Save to:", dirRow);
    form->addRow("Save as:", nameEdit_);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(startBtn_);

    statusLabel_ = new QLabel("Idle.");
    speedLabel_ = new QLabel("—");
    overallBar_ = new QProgressBar;
    overallBar_->setRange(0, 1000);
    overallBar_->setValue(0);
    overallBar_->setFormat("%p%");
    applyProgressBarStyle(overallBar_);

    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(statusLabel_, 1);
    statusRow->addWidget(speedLabel_);

    chunkTable_ = new QTableWidget(0, kColCount);
    chunkTable_->setHorizontalHeaderLabels(
        {"#", "Range", "Received", "Progress", "Attempts", "Status"});
    chunkTable_->verticalHeader()->setVisible(false);
    chunkTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    chunkTable_->setSelectionMode(QAbstractItemView::NoSelection);
    chunkTable_->setFocusPolicy(Qt::NoFocus);
    chunkTable_->setShowGrid(false);
    chunkTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    chunkTable_->horizontalHeader()->setSectionResizeMode(kColPercent, QHeaderView::Stretch);
    chunkTable_->setAlternatingRowColors(true);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->addLayout(form);
    root->addLayout(btnRow);
    root->addSpacing(8);
    root->addLayout(statusRow);
    root->addWidget(overallBar_);
    root->addSpacing(4);
    root->addWidget(new QLabel("Chunks:"));
    root->addWidget(chunkTable_, 1);
    setCentralWidget(central);
}

void MainWindow::setFormEnabled(bool enabled) {
    urlEdit_->setEnabled(enabled);
    dirEdit_->setEnabled(enabled);
    browseBtn_->setEnabled(enabled);
    nameEdit_->setEnabled(enabled);
    startBtn_->setEnabled(enabled);
}

void MainWindow::refreshFilenameSuggestion(const QString& url) {
    if (!nameEdit_->text().isEmpty()) return;
    const QString suggestion = filenameFromUrl(url);
    if (!suggestion.isEmpty()) {
        nameEdit_->setPlaceholderText(suggestion);
    } else {
        nameEdit_->setPlaceholderText("filename (extension auto-filled from URL)");
    }
}

QString MainWindow::resolveSaveName(const QString& userName, const QString& url) const {
    const QString urlName = filenameFromUrl(url);
    // No user input -> use the URL's basename verbatim (with extension).
    if (userName.isEmpty()) {
        return urlName.isEmpty() ? QString("download.bin") : urlName;
    }
    // User typed something with a dot anywhere except as a leading char -> they
    // own the extension; respect it as-is.
    if (extensionOf(userName).size() > 0) return userName;
    // User typed a bare name -> append the URL's extension if any.
    const QString ext = extensionOf(urlName);
    return userName + ext;  // ext is "" if URL has no extension, which is fine
}

void MainWindow::onUrlChanged(const QString& url) {
    refreshFilenameSuggestion(url);
}

void MainWindow::onBrowseClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Choose save folder", dirEdit_->text());
    if (!dir.isEmpty()) dirEdit_->setText(dir);
}

void MainWindow::onNewDownloadWindow() {
    auto* w = new MainWindow();
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::onStartClicked() {
    const QString url = urlEdit_->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, "Missing URL", "Please enter a URL.");
        return;
    }
    QString dir = dirEdit_->text().trimmed();
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString name = resolveSaveName(nameEdit_->text().trimmed(), url);
    const QString path = QDir(dir).filePath(name);

    statusLabel_->setText(QString("Probing %1…").arg(url));
    speedLabel_->setText("—");
    overallBar_->setRange(0, 0);  // indeterminate / busy
    overallBar_->setValue(0);
    chunkTable_->setRowCount(0);
    setFormEnabled(false);
    statusBar()->showMessage("Probing…");

    bridge_->startDownload(url, path);
}

void MainWindow::onStarted(qint64 totalBytes, bool supportsRanges, int chunkCount,
                           const QList<ChunkRow>& chunks) {
    overallBar_->setRange(0, 1000);
    overallBar_->setValue(0);
    statusLabel_->setText(QString("Downloading %1 in %2 chunk(s)%3")
                              .arg(humanBytes(totalBytes))
                              .arg(chunkCount)
                              .arg(supportsRanges ? "" : " (no range support)"));
    statusBar()->showMessage("Downloading");

    chunkTable_->setRowCount(chunkCount);
    for (const auto& c : chunks) {
        const int r = c.index;
        chunkTable_->setItem(r, kColIndex, new QTableWidgetItem(QString::number(c.index)));
        QString range = c.endByte >= 0
                            ? QString("%1 – %2").arg(humanBytes(c.startByte), humanBytes(c.endByte))
                            : QString("from %1").arg(humanBytes(c.startByte));
        chunkTable_->setItem(r, kColRange, new QTableWidgetItem(range));
        chunkTable_->setItem(r, kColReceived, new QTableWidgetItem(humanBytes(0)));
        auto* bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setValue(0);
        bar->setFormat("%p%");
        applyProgressBarStyle(bar);
        chunkTable_->setCellWidget(r, kColPercent, bar);
        chunkTable_->setItem(r, kColAttempts, new QTableWidgetItem("1"));
        chunkTable_->setItem(r, kColStatus, new QTableWidgetItem(statusText(0)));
    }
}

void MainWindow::onProgressUpdated(qint64 received, qint64 total, double bytesPerSec,
                                   const QList<ChunkRow>& chunks) {
    if (total > 0) {
        const int permille = static_cast<int>((received * 1000) / total);
        overallBar_->setRange(0, 1000);
        overallBar_->setValue(permille);
        statusLabel_->setText(QString("%1 / %2  (%3%)")
                                  .arg(humanBytes(received), humanBytes(total))
                                  .arg(QString::number(permille / 10.0, 'f', 1)));
    } else {
        overallBar_->setRange(0, 0);  // indeterminate
        statusLabel_->setText(QString("%1 received").arg(humanBytes(received)));
    }
    speedLabel_->setText(humanRate(bytesPerSec));

    for (const auto& c : chunks) {
        const int r = c.index;
        if (r < 0 || r >= chunkTable_->rowCount()) continue;
        const qint64 chunkSize =
            (c.endByte >= 0) ? (c.endByte - c.startByte + 1) : -1;
        if (auto* item = chunkTable_->item(r, kColReceived))
            item->setText(humanBytes(c.bytesReceived));
        if (auto* bar = qobject_cast<QProgressBar*>(chunkTable_->cellWidget(r, kColPercent))) {
            if (chunkSize > 0) {
                const int permille = static_cast<int>((c.bytesReceived * 1000) / chunkSize);
                bar->setRange(0, 1000);
                bar->setValue(permille);
            } else {
                bar->setRange(0, 0);
            }
        }
        if (auto* item = chunkTable_->item(r, kColAttempts))
            item->setText(QString::number(c.attempts));
        if (auto* item = chunkTable_->item(r, kColStatus))
            item->setText(statusText(c.status));
    }
}

void MainWindow::onFinished() {
    statusLabel_->setText("Finished.");
    overallBar_->setRange(0, 1000);
    overallBar_->setValue(1000);
    speedLabel_->setText("—");
    statusBar()->showMessage("Finished", 5000);
    setFormEnabled(true);
}

void MainWindow::onFailed(const QString& reason) {
    statusLabel_->setText(QString("Failed: %1").arg(reason));
    overallBar_->setRange(0, 1000);
    overallBar_->setValue(0);
    speedLabel_->setText("—");
    statusBar()->showMessage("Failed", 5000);
    setFormEnabled(true);
}

}  // namespace fdm_gui
