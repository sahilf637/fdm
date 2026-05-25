#pragma once

#include <QDialog>
#include <QString>

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
                 const QString& filename);

    // Valid only after exec() returned QDialog::Accepted.
    QString url() const;
    QString outputPath() const;

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
    QLineEdit* urlEdit_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QString resolvedPath_;
};

}  // namespace fdm_gui
