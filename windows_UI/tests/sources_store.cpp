// The sources.json registry, pure file I/O: round-trip against the Mac's
// exact key shape, the unreadable-file set-aside, ISO-8601 date precision,
// the selected-source semantics and the sidebar's tree input partition.
// Everything runs in a QTemporaryDir — this test never reads or writes the
// user's real library list and needs no helper process.
#include "CatalogSourcesStore.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

namespace lib = spk::desktop::library;

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "cannot read test file");
    return file.readAll();
}

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot write test file");
    check(file.write(bytes) == bytes.size(), "incomplete test write");
}

/// The Mac's persisted copied_from shape (MacCopiedFrom: path/format/
/// copiedAt/updatedAt, camelCase — the store keeps it verbatim).
QVariantMap macCopiedFrom() {
    return QVariantMap{
        {QLatin1String("path"), QLatin1String("/Volumes/C1/nature.cocatalog")},
        {QLatin1String("format"), QLatin1String("16.8.6.25 Pro Mac")},
        {QLatin1String("copiedAt"), QLatin1String("2026-11-10T09:00:00Z")},
        {QLatin1String("updatedAt"), QLatin1String("2026-11-11T09:00:00Z")}};
}

lib::CatalogSourcesStore::Source catalogSource() {
    lib::CatalogSourcesStore::Source source;
    source.id = QUuid(QStringLiteral("FF263318-46D5-4B19-99FD-F65AEF80D93D"));
    source.kind = lib::CatalogSourcesStore::Kind::Catalog;
    source.name = QStringLiteral("Nature");
    source.path = QStringLiteral("D:/Photos/Nature.spektralab");
    source.libraryPath = QStringLiteral("D:/Photos/Nature.spektralab");
    source.addedAt = QDateTime::fromString(QStringLiteral("2026-11-01T08:00:00Z"), Qt::ISODate);
    source.openedAt = QDateTime::fromString(QStringLiteral("2026-11-14T21:30:00Z"), Qt::ISODate);
    source.copiedFrom = macCopiedFrom();
    return source;
}

lib::CatalogSourcesStore::Source folderSource() {
    lib::CatalogSourcesStore::Source source;
    source.id = QUuid(QStringLiteral("0E10D9AC-5B74-4E67-9E04-3DCB4D0E1E11"));
    source.kind = lib::CatalogSourcesStore::Kind::Folder;
    source.name = QStringLiteral("Shooting");
    source.path = QStringLiteral("D:/Shooting");
    source.addedAt = QDateTime::fromString(QStringLiteral("2026-11-02T08:00:00Z"), Qt::ISODate);
    return source;
}

void expectSource(const lib::CatalogSourcesStore::Source& a,
                  const lib::CatalogSourcesStore::Source& b, const char* label) {
    check(a.id == b.id, (std::string(label) + ": id survives").c_str());
    check(a.kind == b.kind, (std::string(label) + ": kind survives").c_str());
    check(a.name == b.name, (std::string(label) + ": name survives").c_str());
    check(a.path == b.path, (std::string(label) + ": path survives").c_str());
    check(a.libraryPath == b.libraryPath, (std::string(label) + ": libraryPath survives").c_str());
    check(a.addedAt == b.addedAt, (std::string(label) + ": addedAt survives").c_str());
    check(a.openedAt == b.openedAt, (std::string(label) + ": openedAt survives").c_str());
    check(a.copiedFrom == b.copiedFrom, (std::string(label) + ": copiedFrom survives").c_str());
}

void testEmptyDefault() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    lib::CatalogSourcesStore store(dir.filePath(QStringLiteral("sources.json")));
    const lib::CatalogSourcesStore::Registry registry = store.load();
    check(registry.version == 1, "an absent file reads as the version-1 default");
    check(registry.sources.empty(), "an absent file has no sources");
    check(!registry.selected.has_value(), "an absent file has no selection");
    check(store.lastReadAside().isEmpty(), "a clean read sets nothing aside");
}

void testRoundTrip() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    lib::CatalogSourcesStore store(path);

    lib::CatalogSourcesStore::Registry registry;
    registry.sources.push_back(catalogSource());
    registry.sources.push_back(folderSource());
    registry.selected = folderSource().id;
    check(store.save(registry), "a save into a fresh directory succeeds");

    lib::CatalogSourcesStore reloaded(path);
    const lib::CatalogSourcesStore::Registry back = reloaded.load();
    check(back.version == 1, "the round-tripped registry is still version 1");
    check(back.sources.size() == 2, "both sources survive");
    check(back.selected.has_value() && *back.selected == folderSource().id,
          "the selection survives");
    expectSource(back.sources.at(0), catalogSource(), "catalog source round trip");
    expectSource(back.sources.at(1), folderSource(), "folder source round trip");
}

void testWrittenShape() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    lib::CatalogSourcesStore::Registry registry;
    registry.sources.push_back(catalogSource());
    registry.sources.push_back(folderSource());
    registry.selected = catalogSource().id;
    check(lib::CatalogSourcesStore(path).save(registry), "save succeeds");

    // Key-for-key with Mac CatalogSourcesStore.swift:142-187 — version/sources/
    // selected, a source carries id/kind/name/path/libraryPath/addedAt/
    // openedAt/copiedFrom, dates ISO 8601 with second precision.
    const QJsonObject object =
        QJsonDocument::fromJson(readBytes(path)).object();
    check(object.keys().size() == 3, "the registry keeps exactly three keys");
    check(object.value(QLatin1String("version")).toInt() == 1, "version is written");
    check(object.value(QLatin1String("selected")).toString() ==
              QLatin1String("FF263318-46D5-4B19-99FD-F65AEF80D93D"),
          "selected is written as the UUID string");
    const QJsonArray sources = object.value(QLatin1String("sources")).toArray();
    check(sources.size() == 2, "both sources are written");
    const QJsonObject written = sources.at(0).toObject();
    check(written.keys().size() == 8, "a source keeps exactly its eight keys");
    check(written.value(QLatin1String("id")).toString() ==
              QLatin1String("FF263318-46D5-4B19-99FD-F65AEF80D93D"),
          "id is written as the UUID string");
    check(written.value(QLatin1String("kind")).toString() == QLatin1String("catalog"),
          "kind is written as the Mac's raw string");
    check(written.value(QLatin1String("addedAt")).toString() ==
              QLatin1String("2026-11-01T08:00:00Z"),
          "dates are written ISO 8601 in UTC, no zone offset");
    check(written.value(QLatin1String("openedAt")).toString() ==
              QLatin1String("2026-11-14T21:30:00Z"),
          "openedAt is written ISO 8601");
    const QVariantMap copied = written.value(QLatin1String("copiedFrom")).toObject().toVariantMap();
    check(copied.size() == 4 && copied.value(QLatin1String("format")).toString() ==
                                       QLatin1String("16.8.6.25 Pro Mac"),
          "copiedFrom is written verbatim");
    check(!sources.at(1).toObject().contains(QLatin1String("openedAt")),
          "an absent optional key is omitted, not written null");
    check(!sources.at(1).toObject().contains(QLatin1String("copiedFrom")),
          "a folder source writes no copiedFrom");

    // `save` writes pretty and sorted (Mac's [.prettyPrinted, .sortedKeys]):
    // multi-line, and at every level the keys sit in lexicographic order.
    const QString text = QString::fromUtf8(readBytes(path));
    check(text.contains(QLatin1Char('\n')), "the file is pretty-printed, not one line");
    check(text.indexOf(QLatin1String("\"selected\"")) < text.indexOf(QLatin1String("\"sources\"")) &&
              text.indexOf(QLatin1String("\"sources\"")) < text.indexOf(QLatin1String("\"version\"")),
          "top-level keys are sorted");
    check(text.indexOf(QLatin1String("\"addedAt\"")) < text.indexOf(QLatin1String("\"id\"")) &&
              text.indexOf(QLatin1String("\"id\"")) < text.indexOf(QLatin1String("\"kind\"")) &&
              text.indexOf(QLatin1String("\"kind\"")) < text.indexOf(QLatin1String("\"name\"")),
          "source keys are sorted");
    check(!text.contains(QLatin1String(".000")), "no millisecond noise in the dates");
}

void testSecondPrecision() {
    // Mac's ISO 8601 strategy writes seconds: a date carrying milliseconds in
    // memory must not leak them into the file.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    lib::CatalogSourcesStore::Registry registry;
    lib::CatalogSourcesStore::Source source = catalogSource();
    source.addedAt = QDateTime::fromString(QStringLiteral("2026-11-01T08:00:00Z"), Qt::ISODate);
    source.addedAt = source.addedAt.addMSecs(123);
    registry.sources.push_back(source);
    check(lib::CatalogSourcesStore(path).save(registry), "save succeeds");
    check(readBytes(path).contains("\"addedAt\": \"2026-11-01T08:00:00Z\""),
          "milliseconds are truncated to the Mac's second precision");
}

void testCorruptionAside() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    const QByteArray garbage = QByteArray("{ not json at all");
    writeBytes(path, garbage);

    lib::CatalogSourcesStore store(path);
    const lib::CatalogSourcesStore::Registry registry = store.load();
    check(registry.sources.empty() && registry.version == 1,
          "an unreadable file reads as the empty default");
    const QString aside = store.lastReadAside();
    check(!aside.isEmpty(), "an unreadable file is set aside");
    check(QFileInfo(aside).fileName().startsWith(QLatin1String("sources.unreadable-")) &&
              QFileInfo(aside).fileName().endsWith(QLatin1String(".json")),
          "the aside copy is named sources.unreadable-<time>.json");
    check(readBytes(aside) == garbage, "the aside copy holds the unreadable bytes");
    check(readBytes(path) == garbage, "the original is never written over");

    // Once per content: the same unreadable bytes pass through load again and
    // no second copy appears beside the first.
    const lib::CatalogSourcesStore::Registry again = store.load();
    check(again.sources.empty(), "the second read is again the empty default");
    check(store.lastReadAside() == aside, "the second read finds the first copy, not a new one");
    QDir folder(dir.path());
    check(folder.entryList(QStringList() << QStringLiteral("sources.unreadable-*.json")).size() == 1,
          "exactly one aside copy exists for one unreadable content");
}

void testAsidePerContent() {
    // Two different unreadable contents get two copies; the first copy's
    // bytes are never overwritten by the second.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    writeBytes(path, QByteArray("one bad thing"));
    lib::CatalogSourcesStore first(path);
    first.load();
    const QString asideOne = first.lastReadAside();
    check(!asideOne.isEmpty(), "the first aside exists");

    writeBytes(path, QByteArray("a different bad thing"));
    lib::CatalogSourcesStore second(path);
    second.load();
    const QString asideTwo = second.lastReadAside();
    check(!asideTwo.isEmpty() && asideTwo != asideOne, "a different content gets its own copy");
    check(readBytes(asideOne) == QByteArray("one bad thing"),
          "the first aside copy keeps its bytes");
}

void testStrictDecode() {
    // Mac's Codable is strict: a kind this build does not know, a malformed
    // UUID, a wrong-typed date — any of them makes the whole file unreadable,
    // which the reader answers with the set-aside, not a partial list.
    const QByteArray unknownKind =
        R"json({"version":1,"sources":[{"id":"FF263318-46D5-4B19-99FD-F65AEF80D93D","kind":"lighttable","name":"X","addedAt":"2026-11-01T08:00:00Z"}],"selected":null})json";
    const QByteArray badUuid =
        R"json({"version":1,"sources":[{"id":"not-a-uuid","kind":"catalog","name":"X","addedAt":"2026-11-01T08:00:00Z"}]})json";
    const QByteArray badDate =
        R"json({"version":1,"sources":[{"id":"FF263318-46D5-4B19-99FD-F65AEF80D93D","kind":"catalog","name":"X","addedAt":"yesterday"}]})json";
    const QByteArray missingName =
        R"json({"version":1,"sources":[{"id":"FF263318-46D5-4B19-99FD-F65AEF80D93D","kind":"catalog","addedAt":"2026-11-01T08:00:00Z"}]})json";
    for (const char* bytes : {unknownKind, badUuid, badDate, missingName}) {
        QTemporaryDir dir;
        check(dir.isValid(), "temporary directory unavailable");
        const QString path = dir.filePath(QStringLiteral("sources.json"));
        writeBytes(path, bytes);
        lib::CatalogSourcesStore store(path);
        check(store.load().sources.empty(), "a strict-decode failure reads as the empty default");
        check(!store.lastReadAside().isEmpty(), "the unreadable file is set aside");
    }

    // What must still read: a file without version (Mac's decodeIfPresent
    // default 1), without selected, with null optionals and unknown extra
    // keys (Codable ignores what it does not know).
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory unavailable");
    const QString path = dir.filePath(QStringLiteral("sources.json"));
    writeBytes(path,
               QByteArray(R"json({"sources":[{"id":"0E10D9AC-5B74-4E67-9E04-3DCB4D0E1E11","kind":"session","name":"S","path":"D:/S","addedAt":"2026-11-01T08:00:00Z","openedAt":null,"imageCount":3}],"selected":null})json"));
    lib::CatalogSourcesStore store(path);
    const lib::CatalogSourcesStore::Registry registry = store.load();
    check(store.lastReadAside().isEmpty(), "a tolerable file is read, not set aside");
    check(registry.version == 1, "a missing version reads as 1");
    check(registry.sources.size() == 1 && registry.sources.front().kind ==
                                                lib::CatalogSourcesStore::Kind::Session,
          "the session kind reads back");
    check(!registry.sources.front().openedAt.has_value(), "a null openedAt reads as absent");
    check(registry.sources.front().path == QLatin1String("D:/S"), "path survives");
}

void testKindEnum() {
    using K = lib::CatalogSourcesStore::Kind;
    const std::vector<std::pair<QString, K>> pairs = {
        {QStringLiteral("appLibrary"), K::AppLibrary},
        {QStringLiteral("catalog"), K::Catalog},
        {QStringLiteral("session"), K::Session},
        {QStringLiteral("captureOneCatalog"), K::CaptureOneCatalog},
        {QStringLiteral("captureOneSession"), K::CaptureOneSession},
        {QStringLiteral("folder"), K::Folder}};
    for (const auto& pair : pairs) {
        const auto kind = lib::CatalogSourcesStore::kindFromWire(pair.first);
        check(kind.has_value() && *kind == pair.second,
              (std::string("kind reads: ") + pair.first.toStdString()).c_str());
        check(lib::CatalogSourcesStore::kindToWire(pair.second) == pair.first,
              (std::string("kind writes: ") + pair.first.toStdString()).c_str());
    }
    check(!lib::CatalogSourcesStore::kindFromWire(QStringLiteral("Lighttable")).has_value(),
          "an unknown kind string does not decode");
}

void testSelectedSemantics() {
    using Registry = lib::CatalogSourcesStore::Registry;
    Registry registry;
    registry.sources.push_back(catalogSource()); // added 11-01, opened 11-14
    registry.sources.push_back(folderSource());  // added 11-02, never opened
    check(*lib::CatalogSourcesStore::effectiveSource(registry) == registry.sources.front(),
          "with no selection, the most recently opened source wins");
    registry.sources.back().openedAt =
        QDateTime::fromString(QStringLiteral("2026-11-15T06:00:00Z"), Qt::ISODate);
    check(*lib::CatalogSourcesStore::effectiveSource(registry) == registry.sources.back(),
          "a later openedAt takes the spot");
    registry.sources.back().openedAt.reset();
    check(*lib::CatalogSourcesStore::effectiveSource(registry) == registry.sources.front(),
          "an unopened source falls back to its addedAt");
    registry.selected = registry.sources.back().id;
    check(*lib::CatalogSourcesStore::effectiveSource(registry) == registry.sources.back(),
          "the selection wins over recency");
    registry.sources.clear();
    check(lib::CatalogSourcesStore::effectiveSource(registry) == nullptr,
          "an empty list has no effective source");
    registry.selected = QUuid::createUuid();
    registry.sources.push_back(folderSource());
    check(*lib::CatalogSourcesStore::effectiveSource(registry) == registry.sources.front(),
          "a selection naming a gone source falls through to recency (the Mac's ??)");
}

void testTreeInput() {
    auto row = [](const char* kind, const char* name, const char* uuid) {
        return QVariantMap{{QLatin1String("kind"), QLatin1String(kind)},
                           {QLatin1String("name"), QLatin1String(name)},
                           {QLatin1String("uuid"), QLatin1String(uuid)},
                           {QLatin1String("editable"), true}};
    };
    const std::vector<QVariantMap> collections = {
        row("root", "root", "R"),
        row("all_images", "All Images", "A"),
        row("project", "Weddings", "P1"),
        row("album", "Best of", "AL1"),
        row("recent_imports", "2026-11-14", "RI1"),
        row("folder", "D:/Shooting", "F1"),
        row("trash", "Trash", "T"),
        row("album", "Second album", "AL2"),
        row("smart_album", "4+ stars", "SA1"),
        row("in_library", "In Catalog", "IL"),
    };
    const lib::CatalogSourcesStore::TreeInput tree =
        lib::CatalogSourcesStore::treeInput(collections);
    check(tree.fixedRows.size() == 4, "the four fixed rows partition together");
    check(tree.fixedRows.front().value(QLatin1String("uuid")).toString() == QLatin1String("A"),
          "fixed rows keep list order (All Images first here)");
    check(tree.folders.size() == 1 &&
              tree.folders.front().value(QLatin1String("uuid")).toString() == QLatin1String("F1"),
          "folder rows partition to the folders bucket");
    check(tree.userRows.size() == 4, "user rows are the rest");
    check(tree.userRows.front().value(QLatin1String("uuid")).toString() == QLatin1String("P1") &&
              tree.userRows.back().value(QLatin1String("uuid")).toString() == QLatin1String("SA1"),
          "user rows keep list order");
    // The rows ride through verbatim: the map is the row, not a re-encoding.
    check(tree.fixedRows.front().value(QLatin1String("editable")).toBool(),
          "a partitioned row keeps its fields");
    check(lib::CatalogSourcesStore::treeInput({}).fixedRows.empty() &&
              lib::CatalogSourcesStore::treeInput({}).userRows.empty(),
          "an empty collection list partitions to nothing");
}

void testDefaultPath() {
    const QString root = lib::CatalogSourcesStore::macLibraryRoot();
    check(root.endsWith(QLatin1String("MacLibrary"), Qt::CaseInsensitive),
          "the app root mirrors the Mac's MacLibrary folder");
    check(lib::CatalogSourcesStore::defaultRegistryPath() ==
              root + QLatin1String("/sources.json"),
          "sources.json sits at <AppLocalData>/MacLibrary/sources.json");
    check(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) != QString(),
          "QStandardPaths resolves the app-local data root");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("SpektraLab"));
    application.setApplicationName(QStringLiteral("SpektraLabQt"));
    try {
        testEmptyDefault();
        testRoundTrip();
        testWrittenShape();
        testSecondPrecision();
        testCorruptionAside();
        testAsidePerContent();
        testStrictDecode();
        testKindEnum();
        testSelectedSemantics();
        testTreeInput();
        testDefaultPath();
        std::printf("sources_store: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("sources_store: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
