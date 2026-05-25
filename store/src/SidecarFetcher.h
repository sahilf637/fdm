#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace fdm::store {

// One-shot, fire-and-forget sidecar fetcher. Tries <baseUrl>.sha256,
// <baseUrl>.sha1, <baseUrl>.md5 in that order. Each attempt has a hard
// timeout and aborts on bodies larger than a fixed cap (hash files are
// expected to be tiny). The first parsable line that matches the leaf
// filename (when a manifest-style line is present) wins; otherwise a single
// hex token on its own is accepted as the hash.
//
// Result is signalled exactly once via `result()` when:
//   - one of the variants returned a parsable hash, or
//   - all three variants have been tried and failed/404'd.
class SidecarFetcher : public QObject {
    Q_OBJECT
public:
    explicit SidecarFetcher(QObject* parent = nullptr);

    // baseUrl is the post-redirect URL of the main download (so sidecars
    // are looked up next to where the actual bytes lived). filename is the
    // basename of the saved file, used to match against manifest-style
    // sidecars ("<hex>  <filename>").
    void fetch(const QString& baseUrl, const QString& filename);

signals:
    // hashHex / algorithm / source empty when nothing was found.
    void result(QString hashHex, QString algorithm, QString source);

private slots:
    void onReplyFinished();
    void onTimeout();

private:
    void tryNext();
    void emitFailure();

    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* timeout_ = nullptr;
    QString baseUrl_;
    QString filename_;
    int attemptIndex_ = 0;  // 0=sha256, 1=sha1, 2=md5
};

}  // namespace fdm::store
