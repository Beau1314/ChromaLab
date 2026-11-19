#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace spk::desktop {

// The self-update channel over the repository's public Releases
// (github.com/Beau1314/SpektraLab-Windows): the running copy compares its
// compiled-in version with releases/latest, offers the newer zip, then
// downloads, verifies, extracts and swaps the application directory in
// place, and restarts. The user's own data (Edits, lighttable.json,
// MacLibrary) lives under AppData and is never touched.
//
// The Windows swap of a RUNNING application: the exe and the loaded DLLs
// are memory-mapped — they cannot be deleted or overwritten, but they CAN
// be renamed within the volume. Every locked file therefore moves aside as
// <name>.spk-old, the new copy takes its place, and the next launch deletes
// the leftovers (cleanupOldFiles).
class Updater final : public QObject {
    Q_OBJECT
    // idle → checking → (uptodate | available) → downloading → installing →
    // restarting | failed. QML drives the dialog off these.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT FINAL)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)

public:
    explicit Updater(QObject* parent = nullptr);
    ~Updater() override;

    // "v1.2.3"/"1.2.3" → {1,2,3}; a non-numeric tail ("1.2.3-rc1") is cut at
    // the dash. Empty when nothing numeric leads.
    static QList<long long> parseVersion(const QString& version);
    // >0 when a is newer, <0 when b is, 0 on equality or when either side
    // does not parse (an unreadable channel never looks newer).
    static int compareVersions(const QString& a, const QString& b);

    QString state() const { return state_; }
    QString latestVersion() const { return latest_; }
    QString releaseNotes() const { return notes_; }
    QString currentVersion() const;
    double progress() const { return progress_; }
    QString error() const { return error_; }

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void startDownloadAndInstall();

    // Delete <appdir>/*.spk-old from a previous swap. Call once at startup
    // before any DLL could be loaded from those names.
    static void cleanupOldFiles();

signals:
    void stateChanged();

private:
    void setState(const QString& next);
    void fail(const QString& message);
    QString stageArchive() const;      // %TEMP%/spk-update/download.zip
    QString stageExtract() const;      // %TEMP%/spk-update/extracted
    void applyExtractedFiles();        // the swap, then restart
    void restart();

    QNetworkAccessManager* network_ = nullptr;
    QString state_ = QStringLiteral("idle");
    QString latest_;
    QString notes_;
    QString downloadUrl_;
    QString expectedSha256_;           // the asset digest when GitHub sent one
    double progress_ = -1.0;           // -1: indeterminate
    QString error_;
    QMetaObject::Connection progressHook_;
};

} // namespace spk::desktop
