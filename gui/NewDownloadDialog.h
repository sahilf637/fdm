#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace fdm_gui {

// Modal "new download" form. The MainWindow owns this dialog; on accept() the
// caller reads url() / outputPath() and hands them to the DownloadManager.
class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDownloadDialog(QWidget* parent = nullptr);

    // Valid only after exec() returned QDialog::Accepted.
    QString url() const;
    QString outputPath() const;

private slots:
    void onBrowseClicked();
    void onUrlChanged(const QString& url);
    void onAccept();

private:
    void buildUi();
    QString resolveSaveName() const;

    QLineEdit* urlEdit_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;

    QString resolvedPath_;
};

}  // namespace fdm_gui
