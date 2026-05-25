#include "SidecarFetcher.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace fdm::store {

namespace {

constexpr int kTimeoutMs = 3000;
constexpr qint64 kMaxBodyBytes = 4096;  // hash files are ~70 bytes; cap generously

struct Variant {
    const char* suffix;
    const char* algorithm;
    const char* source;
    int hexLen;
};
constexpr Variant kVariants[] = {
    {".sha256", "sha256", "sidecar-sha256", 64},
    {".sha1",   "sha1",   "sidecar-sha1",   40},
    {".md5",    "md5",    "sidecar-md5",    32},
};

bool isHexChar(QChar c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Pull a hash hex of the right length from one line of a sidecar.
// Accepts:
//   <hex>
//   <hex>  <filename>
//   <hex> *<filename>
// Returns the lowercased hex on match (and optionally requires the filename
// in the line to equal `filename` when a filename is present), "" otherwise.
QString tryParseLine(const QString& line, const QString& filename, int hexLen) {
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#')) return {};

    // First token = hex hash.
    int i = 0;
    while (i < trimmed.size() && !trimmed[i].isSpace()) ++i;
    if (i != hexLen) return {};
    QString hash = trimmed.left(hexLen).toLower();
    for (QChar c : hash) {
        if (!isHexChar(c)) return {};
    }

    // Optional filename column.
    while (i < trimmed.size() && trimmed[i].isSpace()) ++i;
    if (i >= trimmed.size()) return hash;  // bare hash
    // Skip "*" or "+" prefix used to mark binary/text mode.
    if (trimmed[i] == '*' || trimmed[i] == '+') ++i;
    const QString fileToken = trimmed.mid(i).trimmed();
    if (fileToken == filename) return hash;
    return {};
}

QString parseSidecar(const QByteArray& body, const QString& filename, int hexLen) {
    const QString text = QString::fromUtf8(body);
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    // First pass: prefer lines that explicitly name our file (handles
    // multi-entry sidecars).
    for (const QString& line : lines) {
        const QString h = tryParseLine(line, filename, hexLen);
        if (!h.isEmpty()) return h;
    }
    // Second pass: accept the bare-hash form (single-line .sha256 files).
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.size() != hexLen) continue;
        bool allHex = true;
        for (QChar c : trimmed) {
            if (!isHexChar(c)) { allHex = false; break; }
        }
        if (allHex) return trimmed.toLower();
    }
    return {};
}

}  // namespace

SidecarFetcher::SidecarFetcher(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, &SidecarFetcher::onTimeout);
}

void SidecarFetcher::fetch(const QString& baseUrl, const QString& filename) {
    baseUrl_ = baseUrl;
    filename_ = filename;
    attemptIndex_ = 0;
    tryNext();
}

void SidecarFetcher::tryNext() {
    if (attemptIndex_ >= static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]))) {
        emitFailure();
        return;
    }
    const Variant& v = kVariants[attemptIndex_];
    QNetworkRequest req(QUrl(baseUrl_ + v.suffix));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "fdm/0.1");
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::finished, this, &SidecarFetcher::onReplyFinished);
    // Defense against runaway bodies: abort as soon as the running total
    // exceeds our cap. A real hash file is well under this.
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (reply_ && reply_->bytesAvailable() > kMaxBodyBytes) reply_->abort();
    });
    timeout_->start(kTimeoutMs);
}

void SidecarFetcher::onTimeout() {
    if (reply_) reply_->abort();
    // onReplyFinished will fire next as a result of the abort.
}

void SidecarFetcher::onReplyFinished() {
    timeout_->stop();
    if (!reply_) return;
    QNetworkReply* r = reply_;
    reply_ = nullptr;
    r->deleteLater();

    const Variant& v = kVariants[attemptIndex_];
    if (r->error() == QNetworkReply::NoError) {
        const QByteArray body = r->readAll();
        if (body.size() <= kMaxBodyBytes) {
            const QString hash = parseSidecar(body, filename_, v.hexLen);
            if (!hash.isEmpty()) {
                emit result(hash, QString(v.algorithm), QString(v.source));
                return;
            }
        }
    }
    ++attemptIndex_;
    tryNext();
}

void SidecarFetcher::emitFailure() {
    emit result(QString(), QString(), QString());
}

}  // namespace fdm::store
