#include "Updater.hpp"
#include "Localization.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>

namespace spk::desktop {
namespace {

// The public releases channel. The repository is public, so every installed
// copy checks and downloads anonymously — no token ships inside the exe.
const char kReleasesLatest[] =
    "https://api.github.com/repos/Beau1314/SpektraLab-Windows/releases/latest";

QString tempRoot() {
    return QDir(QDir::temp()).filePath(QStringLiteral("spk-update"));
}

} // namespace

QList<long long> Updater::parseVersion(const QString& version) {
    QString cleaned = version.trimmed();
    // The release tag's conventional leading v.
    if (cleaned.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        cleaned = cleaned.mid(1);
    const int dash = cleaned.indexOf(QLatin1Char('-'));
    const QString body = dash > 0 ? cleaned.left(dash) : cleaned;
    QList<long long> parts;
    for (const QString& chunk : body.split(QLatin1Char('.'))) {
        bool ok = false;
        const long long value = chunk.toLongLong(&ok);
        if (!ok) return parts;
        parts.append(value);
    }
    return parts;
}

int Updater::compareVersions(const QString& a, const QString& b) {
    const auto left = parseVersion(a);
    const auto right = parseVersion(b);
    if (left.isEmpty() || right.isEmpty()) return 0;
    const int longest = qMax(left.size(), right.size());
    for (int i = 0; i < longest; ++i) {
        const long long l = i < left.size() ? left.at(i) : 0;
        const long long r = i < right.size() ? right.at(i) : 0;
        if (l != r) return l > r ? 1 : -1;
    }
    return 0;
}

Updater::Updater(QObject* parent) : QObject(parent) {
    network_ = new QNetworkAccessManager(this);
}

Updater::~Updater() {
    // The staged download survives an aborted update (a resumed one reuses
    // it); only a torn extraction dir is junk.
    QDir(stageExtract()).removeRecursively();
}

QString Updater::currentVersion() const {
    const QByteArray fake = qgetenv("SPK_UPDATE_FAKE_VERSION");
    if (!fake.isEmpty()) return QString::fromUtf8(fake);
    return QStringLiteral(SPK_APP_VERSION);
}

QString Updater::stageArchive() const {
    return QDir(tempRoot()).filePath(QStringLiteral("download.zip"));
}

QString Updater::stageExtract() const {
    return QDir(tempRoot()).filePath(QStringLiteral("extracted"));
}

void Updater::setState(const QString& next) {
    if (state_ == next) return;
    state_ = next;
    emit stateChanged();
}

void Updater::fail(const QString& message) {
    error_ = message;
    setState(QStringLiteral("failed"));
}

void Updater::checkNow() {
    if (state_ == QStringLiteral("checking") ||
        state_ == QStringLiteral("downloading") ||
        state_ == QStringLiteral("installing"))
        return;
    error_.clear();
    latest_.clear();
    notes_.clear();
    downloadUrl_.clear();
    expectedSha256_.clear();
    progress_ = -1.0;
    setState(QStringLiteral("checking"));

    QNetworkRequest request{QUrl(QString::fromLatin1(kReleasesLatest))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SpektraLab-Windows-Updater"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Offline machines are the ordinary case, not a failure worth a
            // dialog: the manual check surfaces it, the silent one stays put.
            fail(QString::fromUtf8("Network: ") + reply->errorString());
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) {
            fail(QString::fromUtf8("The update channel returned an invalid answer"));
            return;
        }
        const auto release = document.object();
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            // No release has been published yet: the channel is simply empty.
            setState(QStringLiteral("uptodate"));
            return;
        }
        latest_ = tag;
        notes_ = release.value(QStringLiteral("body")).toString();
        // The one Windows archive in the release is the payload.
        for (const auto& value : release.value(QStringLiteral("assets")).toArray()) {
            const auto asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!name.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) continue;
            downloadUrl_ = asset.value(QStringLiteral("browser_download_url")).toString();
            const QString digest = asset.value(QStringLiteral("digest")).toString();
            if (digest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive))
                expectedSha256_ = digest.mid(7).toLower();
            break;
        }
        if (downloadUrl_.isEmpty()) {
            fail(QString::fromUtf8("The release carries no Windows archive"));
            return;
        }
        setState(compareVersions(latest_, currentVersion()) > 0
                     ? QStringLiteral("available")
                     : QStringLiteral("uptodate"));
    });
}

void Updater::startDownloadAndInstall() {
    if (downloadUrl_.isEmpty()) {
        fail(QString::fromUtf8("No update was found to install"));
        return;
    }
    if (state_ == QStringLiteral("downloading") || state_ == QStringLiteral("installing"))
        return;
    error_.clear();
    progress_ = 0.0;
    setState(QStringLiteral("downloading"));

    QDir().mkpath(tempRoot());
    const QString archive = stageArchive();
    {
        QFile previous(archive);
        if (previous.exists()) QFile::remove(archive);
    }
    QFile* sink = new QFile(archive, this);
    if (!sink->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QString::fromUtf8("Cannot write the staged download"));
        return;
    }

    QNetworkRequest request{QUrl(downloadUrl_)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SpektraLab-Windows-Updater"));
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                progress_ = total > 0 ? double(received) / double(total) : -1.0;
                emit stateChanged();
            });
    connect(reply, &QNetworkReply::readyRead, this, [reply, sink] {
        sink->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, sink] {
        sink->flush();
        sink->close();
        sink->deleteLater();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QString::fromUtf8("Download: ") + reply->errorString());
            return;
        }
        // The digest GitHub signed the asset with, when it sent one.
        if (!expectedSha256_.isEmpty()) {
            QFile file(stageArchive());
            if (!file.open(QIODevice::ReadOnly)) {
                fail(QString::fromUtf8("Cannot read the staged download"));
                return;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&file)) {
                fail(QString::fromUtf8("Cannot hash the staged download"));
                return;
            }
            if (hash.result().toHex() != expectedSha256_) {
                fail(QString::fromUtf8("The downloaded archive failed its checksum"));
                return;
            }
        }
        // Windows ships bsdtar as tar.exe (Win10+): it reads zip archives
        // without adding a compression dependency to the app.
        QDir(stageExtract()).removeRecursively();
        QDir().mkpath(stageExtract());
        const int tar = QProcess::execute(
            QStringLiteral("tar"),
            {QStringLiteral("-xf"), stageArchive(), QStringLiteral("-C"), stageExtract()});
        if (tar != 0) {
            fail(QString::fromUtf8("Cannot unpack the downloaded archive"));
            return;
        }
        setState(QStringLiteral("installing"));
        applyExtractedFiles();
    });
}

void Updater::applyExtractedFiles() {
    // The payload's root may be a single wrapping folder (the packaging
    // script zips the app directory as a prefix) — the walk starts at the
    // first directory that actually holds the exe.
    QString root = stageExtract();
    while (true) {
        const QDir dir(root);
        if (QFileInfo::exists(dir.filePath(QStringLiteral("SpektraLabQt.exe")))) break;
        const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (entries.size() == 1) {
            root = dir.filePath(entries.first());
            continue;
        }
        fail(QString::fromUtf8("The downloaded archive has no application inside"));
        return;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString relative = QDir(root).relativeFilePath(source);
        // Leftovers of an earlier swap ride back inside a packaged zip
        // sometimes; they are never part of the application.
        if (relative.endsWith(QStringLiteral(".spk-old"))) continue;
        const QString target = QDir(appDir).filePath(relative);
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (QFileInfo::exists(target)) QFile::remove(target);
        if (QFile::copy(source, target)) continue;
        // Locked (the running exe, the loaded DLLs): move the old file aside
        // — a memory-mapped file renames within its volume — and land the
        // new copy under the original name.
        const QString aside = target + QStringLiteral(".spk-old");
        QFile::remove(aside);
        if (!QFile::rename(target, aside) ||
            !QFile::copy(source, target)) {
            fail(QString::fromUtf8("Cannot replace: ") + relative);
            return;
        }
    }
    setState(QStringLiteral("restarting"));
    restart();
}

void Updater::restart() {
    // The detached new copy starts before this one exits, so the shortcut,
    // the taskbar and the window all come straight back.
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    QCoreApplication::quit();
}

void Updater::cleanupOldFiles() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList stale =
        QDir(appDir).entryList({QStringLiteral("*.spk-old")}, QDir::Files);
    for (const QString& name : stale)
        QFile::remove(QDir(appDir).filePath(name));
}

} // namespace spk::desktop
