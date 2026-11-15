#include "CatalogSourcesStore.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace spk::desktop::library {

namespace {

/// ISO 8601 UTC at second precision — Mac's `.iso8601` strategy: no offset,
/// no milliseconds, a literal `Z`.
QString formatStamp(const QDateTime& moment) {
    return moment.toUTC().toString(QLatin1String("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

std::optional<QDateTime> parseStamp(const QString& raw) {
    const QDateTime moment = QDateTime::fromString(raw, Qt::ISODate);
    if (!moment.isValid()) return std::nullopt;
    return moment.toUTC();
}

QString uuidString(const QUuid& id) {
    // Swift's UUID string — and so the Mac's sources.json — is upper case.
    return id.toString(QUuid::WithoutBraces).toUpper();
}

std::optional<QUuid> parseUuid(const QString& raw) {
    const QUuid id = QUuid::fromString(raw);
    if (id.isNull()) return std::nullopt;
    return id;
}

/// One source row, strict: every persisted key must carry the shape Mac's
/// synthesized Codable decodes, unknown extra keys are ignored, absent
/// optionals stay absent.
std::optional<CatalogSourcesStore::Source> parseSource(const QJsonObject& object) {
    CatalogSourcesStore::Source source;
    const auto id = parseUuid(object.value(QLatin1String("id")).toString());
    const auto kind = CatalogSourcesStore::kindFromWire(object.value(QLatin1String("kind")).toString());
    const auto addedAt = parseStamp(object.value(QLatin1String("addedAt")).toString());
    if (!id.has_value() || !kind.has_value() || !addedAt.has_value()) return std::nullopt;
    const QJsonValue name = object.value(QLatin1String("name"));
    if (!name.isString()) return std::nullopt;
    source.id = *id;
    source.kind = *kind;
    source.name = name.toString();
    source.addedAt = *addedAt;
    const QJsonValue path = object.value(QLatin1String("path"));
    if (path.isString()) source.path = path.toString();
    else if (!path.isUndefined() && !path.isNull()) return std::nullopt;
    const QJsonValue libraryPath = object.value(QLatin1String("libraryPath"));
    if (libraryPath.isString()) source.libraryPath = libraryPath.toString();
    else if (!libraryPath.isUndefined() && !libraryPath.isNull()) return std::nullopt;
    const QJsonValue openedAt = object.value(QLatin1String("openedAt"));
    if (openedAt.isString()) {
        const auto stamp = parseStamp(openedAt.toString());
        if (!stamp.has_value()) return std::nullopt;
        source.openedAt = *stamp;
    } else if (!openedAt.isUndefined() && !openedAt.isNull()) {
        return std::nullopt;
    }
    const QJsonValue copiedFrom = object.value(QLatin1String("copiedFrom"));
    if (copiedFrom.isObject()) {
        source.copiedFrom = copiedFrom.toObject().toVariantMap();
    } else if (!copiedFrom.isUndefined() && !copiedFrom.isNull()) {
        return std::nullopt;
    }
    return source;
}

} // namespace

std::optional<CatalogSourcesStore::Kind> CatalogSourcesStore::kindFromWire(const QString& raw) {
    if (raw == QLatin1String("appLibrary")) return Kind::AppLibrary;
    if (raw == QLatin1String("catalog")) return Kind::Catalog;
    if (raw == QLatin1String("session")) return Kind::Session;
    if (raw == QLatin1String("captureOneCatalog")) return Kind::CaptureOneCatalog;
    if (raw == QLatin1String("captureOneSession")) return Kind::CaptureOneSession;
    if (raw == QLatin1String("folder")) return Kind::Folder;
    return std::nullopt;
}

QString CatalogSourcesStore::kindToWire(Kind kind) {
    switch (kind) {
    case Kind::AppLibrary: return QLatin1String("appLibrary");
    case Kind::Catalog: return QLatin1String("catalog");
    case Kind::Session: return QLatin1String("session");
    case Kind::CaptureOneCatalog: return QLatin1String("captureOneCatalog");
    case Kind::CaptureOneSession: return QLatin1String("captureOneSession");
    case Kind::Folder: return QLatin1String("folder");
    }
    return QLatin1String("catalog");
}

QString CatalogSourcesStore::macLibraryRoot() {
    // QStandardPaths::AppLocalDataLocation is Windows' counterpart of the
    // Mac's Application Support; the MacLibrary folder name mirrors the Mac
    // layout so sources.json sits at the same relative place.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QLatin1String("MacLibrary"));
}

QString CatalogSourcesStore::defaultRegistryPath() {
    return QDir(macLibraryRoot()).filePath(QLatin1String("sources.json"));
}

CatalogSourcesStore::CatalogSourcesStore(QString registryPath)
    : registryPath_(std::move(registryPath)) {}

std::optional<CatalogSourcesStore::Registry> CatalogSourcesStore::parseRegistry(const QByteArray& bytes) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) return std::nullopt;
    const QJsonObject object = document.object();

    Registry registry;
    // Mac's `var version = 1` decodes with its default when the key is
    // absent; a present non-number is unreadable.
    const QJsonValue version = object.value(QLatin1String("version"));
    if (version.isDouble()) registry.version = version.toInt();
    else if (!version.isUndefined() && !version.isNull()) return std::nullopt;

    const QJsonValue sources = object.value(QLatin1String("sources"));
    if (!sources.isArray()) return std::nullopt;
    for (const QJsonValue& entry : sources.toArray()) {
        if (!entry.isObject()) return std::nullopt;
        const auto source = parseSource(entry.toObject());
        if (!source.has_value()) return std::nullopt;
        registry.sources.push_back(*source);
    }

    const QJsonValue selected = object.value(QLatin1String("selected"));
    if (selected.isString()) {
        const auto id = parseUuid(selected.toString());
        if (!id.has_value()) return std::nullopt;
        registry.selected = *id;
    } else if (!selected.isUndefined() && !selected.isNull()) {
        return std::nullopt;
    }
    return registry;
}

QString CatalogSourcesStore::setAside(const QString& path) {
    // The Mac's KeptFileAside: the unreadable file is copied beside itself as
    // `<name>.unreadable-<seconds since 1970>.<ext>`, never written over,
    // and a copy already holding the same bytes is reused (the same unreadable
    // file passes through here on every change's read).
    QFile unreadable(path);
    if (!unreadable.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = unreadable.readAll();
    unreadable.close();

    const QFileInfo info(path);
    const QDir folder = info.dir();
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString prefix = stem + QLatin1String(".unreadable-");
    const QFileInfoList existing =
        folder.entryInfoList(QStringList() << QLatin1String("*.unreadable-*.json"), QDir::Files);
    for (const QFileInfo& candidate : existing) {
        if (!candidate.fileName().startsWith(prefix)) continue;
        QFile kept(candidate.filePath());
        if (kept.open(QIODevice::ReadOnly) && kept.readAll() == bytes) return candidate.filePath();
    }
    // The Mac names a copy by the second; two different unreadable contents
    // within one second get -1, -2, ... rather than the Mac's outcome (the
    // second copy silently not made). An existing copy is never overwritten.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QString aside;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const QString stem = prefix + QString::number(now) +
                             (attempt == 0 ? QString() : QLatin1String("-") + QString::number(attempt));
        const QString candidate = folder.filePath(stem + (suffix.isEmpty() ? QString() : QLatin1String(".") + suffix));
        if (!QFileInfo::exists(candidate)) {
            aside = candidate;
            break;
        }
        QFile existing(candidate);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) {
            return candidate; // a copy already holding these bytes is reused
        }
    }
    if (aside.isEmpty()) return {};
    if (!QFile::copy(path, aside)) return {}; // never overwritten, never dropped
    return aside;
}

CatalogSourcesStore::Registry CatalogSourcesStore::load() {
    aside_.clear();
    Registry registry;
    QFile file(registryPath_);
    if (!file.open(QIODevice::ReadOnly)) return registry; // absent: the empty default
    const QByteArray bytes = file.readAll();
    file.close();

    const auto parsed = parseRegistry(bytes);
    if (parsed.has_value()) return *parsed;
    // Unreadable: set aside beside itself, never written over, and carry on
    // as the Mac's reader does — the empty default.
    aside_ = setAside(registryPath_);
    return registry;
}

bool CatalogSourcesStore::save(const Registry& registry) const {
    QJsonObject object;
    object.insert(QLatin1String("version"), registry.version);
    QJsonArray sources;
    for (const Source& source : registry.sources) {
        QJsonObject entry;
        entry.insert(QLatin1String("id"), uuidString(source.id));
        entry.insert(QLatin1String("kind"), kindToWire(source.kind));
        entry.insert(QLatin1String("name"), source.name);
        if (!source.path.isEmpty()) entry.insert(QLatin1String("path"), source.path);
        if (!source.libraryPath.isEmpty()) entry.insert(QLatin1String("libraryPath"), source.libraryPath);
        entry.insert(QLatin1String("addedAt"), formatStamp(source.addedAt));
        if (source.openedAt.has_value()) entry.insert(QLatin1String("openedAt"), formatStamp(*source.openedAt));
        if (!source.copiedFrom.isEmpty())
            entry.insert(QLatin1String("copiedFrom"), QJsonObject::fromVariantMap(source.copiedFrom));
        sources.append(entry);
    }
    object.insert(QLatin1String("sources"), sources);
    if (registry.selected.has_value())
        object.insert(QLatin1String("selected"), uuidString(*registry.selected));
    // QJsonObject keeps its keys sorted, so the indented form is the Mac's
    // [.prettyPrinted, .sortedKeys].
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);

    const QFileInfo info(registryPath_);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QSaveFile file(registryPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit(); // atomic replacement: the reader sees all or nothing
}

const CatalogSourcesStore::Source* CatalogSourcesStore::effectiveSource(const Registry& registry) {
    // Mac's launch rule: the selected source, or — a stale selection or none
    // at all — the one opened last (`openedAt ?? addedAt`), else nothing.
    if (registry.selected.has_value()) {
        for (const Source& source : registry.sources)
            if (source.id == *registry.selected) return &source;
    }
    const Source* best = nullptr;
    QDateTime bestStamp;
    for (const Source& source : registry.sources) {
        const QDateTime stamp = source.openedAt.value_or(source.addedAt);
        if (!best || stamp > bestStamp) {
            best = &source;
            bestStamp = stamp;
        }
    }
    return best;
}

CatalogSourcesStore::TreeInput CatalogSourcesStore::treeInput(const std::vector<QVariantMap>& collections) {
    static const QSet<QString> fixed = {
        QLatin1String("all_images"), QLatin1String("recent_imports"),
        QLatin1String("trash"), QLatin1String("in_library")};
    TreeInput tree;
    for (const QVariantMap& row : collections) {
        const QString kind = row.value(QLatin1String("kind")).toString();
        if (kind == QLatin1String("root")) continue; // the tree's own root
        if (fixed.contains(kind)) tree.fixedRows.push_back(row);
        else if (kind == QLatin1String("folder")) tree.folders.push_back(row);
        else tree.userRows.push_back(row); // projects, groups, albums, smart albums — and the unclassified
    }
    return tree;
}

} // namespace spk::desktop::library
