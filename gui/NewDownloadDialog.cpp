#include "NewDownloadDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "fdm/Paths.h"

namespace fdm_gui {

namespace {

QString filenameFromUrl(const QString& url) {
    QUrl u(url);
    if (!u.isValid()) return {};
    const QString p = u.path();
    if (p.isEmpty() || p.endsWith('/')) return {};
    return QFileInfo(p).fileName();
}

QString extensionOf(const QString& filename) {
    const int dot = filename.lastIndexOf('.');
    if (dot <= 0 || dot >= filename.size() - 1) return {};
    return filename.mid(dot);
}

}  // namespace

NewDownloadDialog::NewDownloadDialog(QWidget* parent) : QDialog(parent) {
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
    nameEdit_->setPlaceholderText("filename (extension auto-filled from URL)");

    auto* form = new QFormLayout;
    form->addRow("URL:", urlEdit_);
    form->addRow("Save to:", dirRow);
    form->addRow("Save as:", nameEdit_);

    startBtn_ = new QPushButton("Start");
    startBtn_->setDefault(true);
    cancelBtn_ = new QPushButton("Cancel");

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(cancelBtn_);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addLayout(btnRow);

    resize(560, sizeHint().height());
}

void NewDownloadDialog::onUrlChanged(const QString& url) {
    if (!nameEdit_->text().isEmpty()) return;
    const QString suggestion = filenameFromUrl(url);
    nameEdit_->setPlaceholderText(
        suggestion.isEmpty() ? "filename (extension auto-filled from URL)"
                             : suggestion);
}

void NewDownloadDialog::onBrowseClicked() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Choose save folder",
                                                          dirEdit_->text());
    if (!dir.isEmpty()) dirEdit_->setText(dir);
}

QString NewDownloadDialog::resolveSaveName() const {
    const QString userName = nameEdit_->text().trimmed();
    const QString urlName = filenameFromUrl(urlEdit_->text().trimmed());
    if (userName.isEmpty()) {
        return urlName.isEmpty() ? QString("download.bin") : urlName;
    }
    if (!extensionOf(userName).isEmpty()) return userName;
    const QString ext = extensionOf(urlName);
    return userName + ext;
}

void NewDownloadDialog::onAccept() {
    const QString url = urlEdit_->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, "Missing URL", "Please enter a URL.");
        return;
    }
    QString dir = dirEdit_->text().trimmed();
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    const QString candidate = QDir(dir).filePath(resolveSaveName());
    resolvedPath_ = QString::fromStdString(fdm::findAvailablePath(candidate.toStdString()));
    accept();
}

QString NewDownloadDialog::url() const { return urlEdit_->text().trimmed(); }

QString NewDownloadDialog::outputPath() const { return resolvedPath_; }

}  // namespace fdm_gui
