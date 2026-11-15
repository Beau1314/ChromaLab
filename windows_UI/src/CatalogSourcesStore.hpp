#pragma once

#include <QDateTime>
#include <QDir>
#include <QUuid>
#include <QVariantMap>
#include <optional>
#include <vector>

// The sources.json registry — the Mac's kept library list (Mac's
// CatalogSourcesStore.swift:142-187, key for key), ported for the Windows
// library window.
//
// The file sits at `<AppLocalData>/<org>/<app>/MacLibrary/sources.json`:
// QStandardPaths::AppLocalDataLocation is the Windows counterpart of Mac's
// `~/Library/Application Support/SpektraLab`, and the MacLibrary folder name
// is kept so a later cross-platform sync stays byte-compatible. The store
// itself never touches that default path unless handed it — tests (and the
// caller) pass the path explicitly, so nothing here writes the user's real
// list by accident.
//
// Shape (the Mac's Registry, verbatim): `version` (1, decoded with its
// default when absent), `sources[]` with `id`/`kind`/`name`/`path`/
// `libraryPath`/`addedAt`/`openedAt`/`copiedFrom` (the two paths and the last
// three optional), `selected` (a UUID string). Dates are ISO 8601 UTC at
// second precision — Mac's `.iso8601` strategy. Saves are pretty-printed with
// sorted keys (Mac's [.prettyPrinted, .sortedKeys]) and replace the file
// atomically. The decoding is strict like Swift's generated Codable: an
// unknown kind, a malformed UUID or a wrong-typed date makes the whole file
// unreadable, and an unreadable file is set aside beside itself as
// `sources.unreadable-<time>.json` (once per content, never overwritten;
// two different contents within one second get -1, -2 suffixes rather than
// the second copy being skipped) —
// the reader then carries on with the empty default, exactly as the Mac's
// KeptFileAside does.
namespace spk::desktop::library {

class CatalogSourcesStore {
public:
    /// What a source is (the Mac's MacSource.Kind raw values). `appLibrary`
    /// is kept for old files; the Windows library never lists it.
    enum class Kind { AppLibrary, Catalog, Session, CaptureOneCatalog, CaptureOneSession, Folder };
    static std::optional<Kind> kindFromWire(const QString& raw);
    static QString kindToWire(Kind kind);

    /// One entry of sources.json — the keys the file keeps. `copiedFrom` is
    /// the Capture One origin a copy remembers, verbatim in the Mac's
    /// persisted keys (path/format/copiedAt/updatedAt); empty = none.
    struct Source {
        QUuid id;
        Kind kind = Kind::Catalog;
        QString name;
        QString path;
        QString libraryPath;
        QDateTime addedAt; // UTC
        std::optional<QDateTime> openedAt; // UTC
        QVariantMap copiedFrom;

        bool operator==(const Source& other) const {
            return id == other.id && kind == other.kind && name == other.name &&
                   path == other.path && libraryPath == other.libraryPath &&
                   addedAt == other.addedAt && openedAt == other.openedAt &&
                   copiedFrom == other.copiedFrom;
        }
    };

    struct Registry {
        int version = 1;
        std::vector<Source> sources;
        std::optional<QUuid> selected;
    };

    /// The Windows MacLibrary root: <AppLocalData>/<org>/<app>/MacLibrary.
    static QString macLibraryRoot();
    /// sources.json beside that root — the production path.
    static QString defaultRegistryPath();

    explicit CatalogSourcesStore(QString registryPath);

    /// Read the file. An absent file reads as the empty default; an
    /// unreadable one is set aside first (lastReadAside() tells where) and
    /// also reads as the empty default. Never throws.
    Registry load();
    /// The file the last load() set aside, or empty on a clean read.
    const QString& lastReadAside() const { return aside_; }

    /// Write the registry whole: pretty + sorted keys, second-precision ISO
    /// 8601 UTC dates, atomic replacement, the folder made when missing.
    bool save(const Registry& registry) const;

    /// The source a launch opens: the selected one, else the most recently
    /// opened (or, never opened, added) — Mac's first-pick rule. Null when no
    /// source qualifies.
    static const Source* effectiveSource(const Registry& registry);

    /// The sidebar's input before any tree exists (the library-workflow task
    /// builds the tree on these): the helper's fixed rows (all_images,
    /// recent_imports, trash, in_library), the user rows (projects, groups,
    /// albums, smart albums — and anything this build does not classify), and
    /// the folder rows, each bucket in list order. The `root` row is the
    /// tree's own root and is not listed. Rows ride through verbatim.
    struct TreeInput {
        std::vector<QVariantMap> fixedRows;
        std::vector<QVariantMap> userRows;
        std::vector<QVariantMap> folders;
    };
    static TreeInput treeInput(const std::vector<QVariantMap>& collections);

private:
    /// Swift-Codable-strict: nullopt when anything is not the shape above.
    static std::optional<Registry> parseRegistry(const QByteArray& bytes);
    static QString setAside(const QString& path); // once per content; empty on failure
    QString registryPath_;
    QString aside_;
};

} // namespace spk::desktop::library
