#include "NewDownloadDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "fdm/Paths.h"
#include "fdm/store/DownloadManager.h"

namespace fdm_gui {

using fdm::store::DownloadManager;
using fdm::store::ProbeResult;

namespace {

QString filenameFromUrl(const QString& url) {
    QUrl u(url);
    if (!u.isValid()) return {};
    const QString p = u.path();
    if (p.isEmpty() || p.endsWith('/')) return {};
    return QFileInfo(p).fileName();
}

// Returns the dot + extension (e.g., ".pdf") or "" if none. Leading-dot
// hidden files like ".bashrc" count as having no extension.
QString extensionOf(const QString& filename) {
    const int dot = filename.lastIndexOf('.');
    if (dot <= 0 || dot >= filename.size() - 1) return {};
    return filename.mid(dot);
}

// Merge what we know into a final leaf filename. Precedence:
//   1. User-typed name with its own extension -- respect verbatim.
//   2. User-typed name without an extension -- append the best extension we
//      can find from (serverName, finalUrl basename, original URL basename).
//   3. No user input -- prefer server-supplied name (Content-Disposition),
//      then final URL basename (after redirects), then original URL basename,
//      then "download.bin".
QString resolveFilename(const QString& userName, const QString& urlName,
                        const QString& serverName, const QString& finalUrlName) {
    if (!userName.isEmpty()) {
        if (!extensionOf(userName).isEmpty()) return userName;
        for (const QString& src : {serverName, finalUrlName, urlName}) {
            const QString ext = extensionOf(src);
            if (!ext.isEmpty()) return userName + ext;
        }
        return userName;
    }
    if (!serverName.isEmpty()) return serverName;
    if (!finalUrlName.isEmpty()) return finalUrlName;
    if (!urlName.isEmpty()) return urlName;
    return "download.bin";
}

}  // namespace

NewDownloadDialog::NewDownloadDialog(DownloadManager* manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
    setWindowTitle("New Download");
    buildUi();

    connect(urlEdit_, &QLineEdit::textChanged, this, &NewDownloadDialog::onUrlChanged);
    connect(browseBtn_, &QPushButton::clicked, this, &NewDownloadDialog::onBrowseClicked);
    connect(startBtn_, &QPushButton::clicked, this, &NewDownloadDialog::onAccept);
    connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
}

void NewDownloadDialog::buildUi() {
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
    nameEdit_->setPlaceholderText("filename (server-detected if blank)");

    auto* form = new QFormLayout;
    form->addRow("URL:", urlEdit_);
    form->addRow("Save to:", dirRow);
    form->addRow("Save as:", nameEdit_);

    statusLabel_ = new QLabel;
    statusLabel_->setStyleSheet("color: palette(mid);");
    statusLabel_->setVisible(false);

    startBtn_ = new QPushButton("Start");
    startBtn_->setDefault(true);
    cancelBtn_ = new QPushButton("Cancel");

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(statusLabel_);
    btnRow->addStretch();
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(cancelBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addLayout(btnRow);

    resize(580, sizeHint().height());
}

void NewDownloadDialog::onUrlChanged(const QString& url) {
    if (!nameEdit_->text().isEmpty()) return;
    const QString suggestion = filenameFromUrl(url);
    nameEdit_->setPlaceholderText(
        suggestion.isEmpty() ? "filename (server-detected if blank)"
                             : suggestion);
}

void NewDownloadDialog::onBrowseClicked() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Choose save folder",
                                                          dirEdit_->text());
    if (!dir.isEmpty()) dirEdit_->setText(dir);
}

void NewDownloadDialog::setFormEnabled(bool enabled) {
    urlEdit_->setEnabled(enabled);
    dirEdit_->setEnabled(enabled);
    nameEdit_->setEnabled(enabled);
    browseBtn_->setEnabled(enabled);
    startBtn_->setEnabled(enabled);
    // Leave Cancel always enabled so the user can back out of a slow probe.
}

void NewDownloadDialog::finishWithFilename(const QString& filename) {
    QString dir = dirEdit_->text().trimmed();
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    const QString candidate = QDir(dir).filePath(filename);
    resolvedPath_ = QString::fromStdString(fdm::findAvailablePath(candidate.toStdString()));
    accept();
}

void NewDownloadDialog::handleProbeResult(const ProbeResult& result) {
    const QString userName = nameEdit_->text().trimmed();
    const QString urlName = filenameFromUrl(urlEdit_->text().trimmed());

    if (!result.ok) {
        // Probe failed -- could be a transient network blip or a real 4xx.
        // Don't silently fall through: tell the user, let them retry or
        // proceed with whatever we can derive from the URL alone.
        statusLabel_->setVisible(false);
        setFormEnabled(true);
        QMessageBox::warning(this, "Probe failed",
                             QString("Could not reach the URL:\n%1").arg(result.error));
        return;
    }

    const QString serverName = result.suggestedFilename;
    const QString finalUrlName = filenameFromUrl(result.finalUrl);
    const QString filename = resolveFilename(userName, urlName, serverName, finalUrlName);
    finishWithFilename(filename);
}

void NewDownloadDialog::onAccept() {
    const QString url = urlEdit_->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, "Missing URL", "Please enter a URL.");
        return;
    }

    setFormEnabled(false);
    statusLabel_->setText("Detecting filename…");
    statusLabel_->setVisible(true);

    // Guard the callback with QPointer in case the user clicks Cancel (or
    // closes the dialog with Esc / [X]) while the probe is in flight. The
    // probe is fire-and-forget on the engine side; we just refuse to act on
    // it after the dialog is gone.
    QPointer<NewDownloadDialog> guard = this;
    manager_->probe(url, [guard](ProbeResult r) {
        if (!guard) return;
        guard->handleProbeResult(r);
    });
}

void NewDownloadDialog::prefill(const QString& url, const QString& directory,
                                const QString& filename) {
    if (!url.isEmpty()) urlEdit_->setText(url);
    if (!directory.isEmpty()) dirEdit_->setText(directory);
    if (!filename.isEmpty()) nameEdit_->setText(filename);
}

QString NewDownloadDialog::url() const { return urlEdit_->text().trimmed(); }

QString NewDownloadDialog::outputPath() const { return resolvedPath_; }

}  // namespace fdm_gui
