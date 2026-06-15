#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace fdm::store {
class DownloadManager;
struct ProbeResult;
}  // namespace fdm::store

namespace fdm_gui {

// Modal "new download" form. The MainWindow owns this dialog; on accept() the
// caller reads url() / outputPath() and hands them to the DownloadManager.
// On accept the dialog probes the URL (HEAD-style) to pick up the server's
// Content-Disposition filename when the user hasn't typed one -- this is
// what stops URLs like /download?id=123 saving as "download.bin".
class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    // `manager` is borrowed for its probe() facility; must outlive the
    // dialog. (The dialog itself is short-lived -- exec() returns once the
    // user clicks Start/Cancel.)
    explicit NewDownloadDialog(fdm::store::DownloadManager* manager,
                               QWidget* parent = nullptr);

    // Optional pre-fill (e.g., when the user clicks "Redownload" on an
    // existing row). Empty strings leave that field at its default.
    void prefill(const QString& url, const QString& directory,
                 const QString& filename, const QString& expectedHash = QString());

    // Switch to "video" mode for a yt-dlp download: Start skips the HTTP HEAD
    // probe (the URL is a page/stream URL, not a file) and just resolves
    // folder + name. The hash row is hidden (a muxed video has nothing to
    // verify). The URL is read-only by default (the extension flow already
    // resolved it); pass urlEditable=true for the manual "Add video" entry
    // point where the user types the page URL. Call once, before exec().
    void setVideoMode(bool urlEditable = false);

    // Extra request headers (Cookie / Referer / User-Agent) sent on the probe
    // this dialog runs when the user clicks Start. Needed so an authenticated
    // URL still resolves its server filename / size. Not shown in the UI.
    void setRequestHeaders(const QList<QPair<QString, QString>>& headers) {
        requestHeaders_ = headers;
    }

    // Valid only after exec() returned QDialog::Accepted.
    QString url() const;
    QString outputPath() const;
    // The hash the user pasted into the optional field, trimmed and
    // lowercased. Empty if they didn't enter one (or entered something
    // that didn't validate).
    QString userHash() const { return userHash_; }
    // Algorithm inferred from the hash's length: "sha256" (64), "sha1"
    // (40), "md5" (32). Empty when no user hash.
    QString userHashAlgorithm() const { return userHashAlgorithm_; }

private slots:
    void onBrowseClicked();
    void onUrlChanged(const QString& url);
    void onAccept();

private:
    void buildUi();
    void setFormEnabled(bool enabled);
    void finishWithFilename(const QString& filename);
    void handleProbeResult(const fdm::store::ProbeResult& result);

    fdm::store::DownloadManager* manager_;
    QFormLayout* form_ = nullptr;
    bool videoMode_ = false;
    QLineEdit* urlEdit_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* hashEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QString resolvedPath_;
    QString userHash_;
    QString userHashAlgorithm_;
    QList<QPair<QString, QString>> requestHeaders_;
};

}  // namespace fdm_gui
