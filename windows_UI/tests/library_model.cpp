// The library data model over the helper wire (task 4.2): two tiers, like
// the library-host test.
//
//   pure tier (always runs)  — the tree partition, the images.list query
//   mapping, the filter map and the EditStore develop-join naming are pure
//   functions, and the change notifications (errorChanged, busy, the
//   collection selection's NOTIFY) are assertable over a failed spawn:
//   none of it needs a helper process.
//
//   real tier (argv[1])      — the whole model workflow against a real
//   spklib-host: a fresh temp registry, a created catalog, thirty synthetic
//   PNG photographs imported, pagination, rating/colour-tag round trips,
//   facets, a local EditStore record joined into a row, an album, a watched
//   folder, the openInDevelop signal and a second source switched back and
//   forth. images.add wants a real file (the helper records size and
//   metadata, it never decodes), so the fixture is a tiny valid PNG built
//   byte by byte below (real CRC32/adler32, a stored deflate block) — a
//   few junk bytes named .png would ride as `other`, not `raster`.
//
// Without a host argument the pure tier still runs and the real tier prints
// an explicit skip line — a skip can never masquerade as a green run
// because the CMake invocation always passes the cached host path on this
// machine.
#include "LibraryModel.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace model = spk::desktop::library;
namespace editing = spk::desktop::editing;
namespace output = spk::desktop::output;

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

// --- the synthetic PNG fixture ------------------------------------------------

unsigned crc32(const unsigned char* data, std::size_t size) {
    unsigned crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

unsigned adler32(const unsigned char* data, std::size_t size) {
    unsigned a = 1, b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void appendChunk(QByteArray& out, const char* type, const QByteArray& body) {
    const quint32 length = quint32(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char((length >> shift) & 0xFF));
    QByteArray typed;
    typed.append(type, 4);
    typed.append(body);
    const unsigned crc = crc32(reinterpret_cast<const unsigned char*>(typed.constData()),
                               std::size_t(typed.size()));
    for (const unsigned char byte : typed) out.push_back(char(byte));
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char((crc >> shift) & 0xFF));
}

/// One valid 1x1 RGB PNG. The zlib stream is a stored (uncompressed) deflate
/// block over the single scanline `00 RR GG BB`, wrapped in the zlib header
/// and its Adler-32 — every checksum computed here, not hard-coded.
QByteArray onePixelPng() {
    const unsigned char signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    QByteArray png(reinterpret_cast<const char*>(signature), 8);
    QByteArray ihdr;
    const unsigned char header[13] = {0, 0, 0, 1, // width 1
                                      0, 0, 0, 1, // height 1
                                      8,          // bit depth
                                      2,          // colour type RGB
                                      0, 0, 0};   // compression, filter, interlace
    for (const unsigned char byte : header) ihdr.push_back(char(byte));
    appendChunk(png, "IHDR", ihdr);
    const unsigned char scanline[4] = {0x00, 0x10, 0x20, 0x30}; // filter 0, one RGB pixel
    QByteArray raw;
    for (const unsigned char byte : scanline) raw.push_back(char(byte));
    QByteArray idat;
    idat.push_back(char(0x78)); // CMF
    idat.push_back(char(0x01)); // FLG: 0x7801 % 31 == 0
    idat.push_back(char(0x01)); // BFINAL=1, BTYPE=00 (stored)
    idat.push_back(char(0x04)); // LEN = 4 (little endian)
    idat.push_back(char(0x00));
    idat.push_back(char(0xFB)); // NLEN = ~LEN
    idat.push_back(char(0xFF));
    idat.append(raw);
    const unsigned adler = adler32(reinterpret_cast<const unsigned char*>(scanline), 4);
    for (int shift = 24; shift >= 0; shift -= 8) idat.push_back(char((adler >> shift) & 0xFF));
    appendChunk(png, "IDAT", idat);
    appendChunk(png, "IEND", QByteArray());
    return png;
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "fixture file opens");
    check(file.write(bytes) == bytes.size(), "fixture file written whole");
}

// --- row accessors over the list model -----------------------------------------

QVariant cell(model::LibraryModel& model, int row, int role) {
    return model.data(model.index(row, 0), role);
}
QString uuidOf(model::LibraryModel& model, int row) {
    return cell(model, row, model::LibraryModel::UuidRole).toString();
}
QString pathOf(model::LibraryModel& model, int row) {
    return cell(model, row, model::LibraryModel::PathRole).toString();
}
QVariantMap developOf(model::LibraryModel& model, int row) {
    return cell(model, row, model::LibraryModel::DevelopRole).toMap();
}
QVariantMap rowById(const QVariantList& tree, const QString& id) {
    for (const auto& value : tree) {
        const auto map = value.toMap();
        if (map.value(QLatin1String("id")).toString() == id) return map;
    }
    return {};
}
QVariantMap rowByKind(const QVariantList& tree, const QString& kind, const QString& section) {
    for (const auto& value : tree) {
        const auto map = value.toMap();
        if (map.value(QLatin1String("section")).toString() == section &&
            map.value(QLatin1String("kind")).toString() == kind)
            return map;
    }
    return {};
}
int countFacet(const QVariantMap& facets, const char* facet, int value) {
    for (const auto& entry : facets.value(QLatin1String(facet)).toList()) {
        const auto map = entry.toMap();
        if (map.value(QLatin1String("value")).toInt() == value)
            return map.value(QLatin1String("count")).toInt();
    }
    return -1;
}

// The worker seam's waits (task 4.4): thumbnail decodes and import progress
// land as queued UI-thread events, so the test pumps the loop until the
// condition holds — a bounded wait, and the check trips on expiry.
void spinUntil(const std::function<bool()>& condition, const char* message,
               int timeoutMs = 10000) {
    QDeadlineTimer timer(timeoutMs);
    while (!condition() && !timer.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(condition(), message);
}

// --- the pure tier ---------------------------------------------------------------

void testTreeRows() {
    // The helper's rows verbatim, root included; the model's tree excludes
    // the root and tags each row fixed/user/folders in that order.
    const std::vector<QVariantMap> collections = {
        {{"uuid", QLatin1String("R")}, {"kind", QLatin1String("root")},
         {"title", QLatin1String("Root")}, {"editable", false}},
        {{"uuid", QLatin1String("A")}, {"kind", QLatin1String("all_images")},
         {"title", QLatin1String("All Images")}, {"editable", false}, {"image_count", 7}},
        {{"uuid", QLatin1String("RI")}, {"kind", QLatin1String("recent_imports")},
         {"title", QLatin1String("Recent Imports")}, {"editable", false}, {"image_count", 2}},
        {{"uuid", QLatin1String("T")}, {"kind", QLatin1String("trash")},
         {"title", QLatin1String("Trash")}, {"editable", false}},
        {{"uuid", QLatin1String("IL")}, {"kind", QLatin1String("in_library")},
         {"title", QLatin1String("In Catalog")}, {"editable", false}},
        {{"uuid", QLatin1String("P1")}, {"kind", QLatin1String("project")},
         {"title", QLatin1String("Trip")}, {"editable", true}},
        {{"uuid", QLatin1String("AL1")}, {"kind", QLatin1String("album")},
         {"title", QLatin1String("Keepers")}, {"parent", QLatin1String("P1")},
         {"editable", true}, {"image_count", 3}},
        {{"uuid", QLatin1String("F1")}, {"kind", QLatin1String("folder")},
         {"title", QLatin1String("D:/Photos/Nature")}, {"editable", false},
         {"folder", QLatin1String("D:/Photos/Nature")}},
    };
    const auto tree = model::LibraryModel::buildTreeRows(collections);
    check(tree.size() == 7, "the root row is excluded, everything else listed once");
    check(rowByKind(tree, QLatin1String("all_images"),
                    QLatin1String("fixed"))
              .value(QLatin1String("imageCount"))
              .toInt() == 7,
          "a fixed row carries its image_count");
    check(rowByKind(tree, QLatin1String("album"), QLatin1String("user"))
                  .value(QLatin1String("parent"))
                  .toString() == QLatin1String("P1") &&
              rowByKind(tree, QLatin1String("album"), QLatin1String("user"))
                  .value(QLatin1String("editable"))
                  .toBool(),
          "a user row keeps its parent and editable flag");
    check(rowByKind(tree, QLatin1String("folder"), QLatin1String("folders"))
                  .value(QLatin1String("path"))
                  .toString() == QLatin1String("D:/Photos/Nature"),
          "a folder row keeps its folder path");
    bool orderHeld = true;
    QString lastSection;
    for (const auto& value : tree) {
        const auto section = value.toMap().value(QLatin1String("section")).toString();
        static const QStringList sections{QLatin1String("fixed"), QLatin1String("user"),
                                          QLatin1String("folders")};
        if (lastSection.isEmpty() ||
            sections.indexOf(section) >= sections.indexOf(lastSection))
            lastSection = section;
        else
            orderHeld = false;
    }
    check(orderHeld, "the sections ride in fixed, user, folders order");
    // An empty catalog (the helper's five fixed rows only, root excluded).
    const auto fresh = model::LibraryModel::buildTreeRows({collections[0], collections[1]});
    check(fresh.size() == 1 &&
              fresh.front().toMap().value(QLatin1String("section")).toString() ==
                  QLatin1String("fixed"),
          "a fresh catalog shows just the All Images fixed row");
}

void testListQuery() {
    const auto plain = model::LibraryModel::listQuery({}, QLatin1String("captured"), false, {},
                                                      1000, 0);
    check(!plain.contains(QLatin1String("collection")), "no collection key when none selected");
    check(plain.value(QLatin1String("sort")).toString() == QLatin1String("captured") &&
              plain.value(QLatin1String("descending")).toBool() == false &&
              plain.value(QLatin1String("offset")).toInt() == 0 &&
              plain.value(QLatin1String("limit")).toInt() == 1000,
          "the base query carries sort, order, offset and the page size");
    const auto full = model::LibraryModel::listQuery(
        QLatin1String("C9"), QLatin1String("name"), true,
        QVariantMap{{QLatin1String("text"), QLatin1String("sea")}}, 25, 50);
    check(full.value(QLatin1String("collection")).toString() == QLatin1String("C9") &&
              full.value(QLatin1String("sort")).toString() == QLatin1String("name") &&
              full.value(QLatin1String("descending")).toBool() &&
              full.value(QLatin1String("offset")).toInt() == 50 &&
              full.value(QLatin1String("limit")).toInt() == 25 &&
              full.value(QLatin1String("filter")).toMap().value(QLatin1String("text")).toString() ==
                  QLatin1String("sea"),
          "the full query rides collection, order, paging and the filter whole");
}

void testFilterMap() {
    const auto full = model::LibraryModel::filterMap(QLatin1String("sea"), {2, 5}, {1},
                                                     {QLatin1String("raw"), QLatin1String("raster")});
    check(full.value(QLatin1String("text")).toString() == QLatin1String("sea") &&
              full.value(QLatin1String("ratings")).toList().size() == 2 &&
              full.value(QLatin1String("color_tags")).toList().front().toInt() == 1 &&
              full.value(QLatin1String("kinds")).toList().size() == 2,
          "the filter map uses the helper's vocabulary verbatim");
    const auto empty = model::LibraryModel::filterMap({}, {}, {}, {});
    check(empty.isEmpty(), "every tick off: no filter key at all");
    const auto partial = model::LibraryModel::filterMap({}, {3}, {}, {});
    check(partial.size() == 1 && partial.contains(QLatin1String("ratings")),
          "empty filter keys stay off the wire, only the ticked ones ride");
}

void testDevelopJoinHelpers() {
    const spk::desktop::Catalog catalog{
        {{"test_film_a", "Test Film A", false}, {"test_film_b", "Test Film B", true}},
        {{"test_paper_a", "Test Paper A", false}, {"test_paper_b", "Test Paper B", false}}};
    editing::State state;
    state.settings.film_stock = "test_film_b";
    state.settings.print_stock = "test_paper_a";
    const auto developed = model::LibraryModel::developState(state, &catalog);
    check(developed.value(QLatin1String("state")).toString() == QLatin1String("developed") &&
              developed.value(QLatin1String("film")).toString() == QLatin1String("Test Film B") &&
              developed.value(QLatin1String("paper")).toString() == QLatin1String("Test Paper A"),
          "a stored state reads developed with its film and paper names");
    const auto none = model::LibraryModel::developState(std::nullopt, &catalog);
    check(none.value(QLatin1String("state")).toString() == QLatin1String("notDeveloped"),
          "no stored state reads notDeveloped");
    check(model::LibraryModel::developState(state, nullptr)
                  .value(QLatin1String("film"))
                  .toString()
              .isEmpty(),
          "no engine catalog yet: the ids stay unresolved, not invented");
    state.settings.film_stock = "not_in_catalog";
    check(model::LibraryModel::developState(state, &catalog)
                  .value(QLatin1String("film"))
                  .toString()
              .isEmpty(),
          "an unknown stock id stays an empty name");
}

// --- change notifications (review fixes, hostless) ---------------------------------

void testNotificationsWithoutHost() {
    // An empty helper path makes the spawn fail fast: start() reports its
    // reason through lastError's own errorChanged (review fix 1 — busyChanged
    // never carried it), with busy bracketing the failed attempt; and
    // selectCollection notifies selectedCollectionId's declared NOTIFY
    // (review fix 2) even before any source exists. Nothing is written
    // anywhere: the failed start() returns before the registry is read.
    model::LibraryModel::Deps deps; // helperPath empty: the spawn fails fast
    // The stores validate their paths up front (the edit store demands an
    // explicit absolute directory), so hand isolated temp paths — nothing
    // is ever written: the failed start() returns before the registry read.
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    deps.editDirectory = scratch.filePath(QString::fromUtf8("edits"));
    deps.registryPath = scratch.filePath(QString::fromUtf8("sources.json"));
    model::LibraryModel library(deps);
    int busySignals = 0;
    int errorSignals = 0;
    int treeSignals = 0;
    QObject::connect(&library, &model::LibraryModel::busyChanged,
                     [&busySignals] { ++busySignals; });
    QObject::connect(&library, &model::LibraryModel::errorChanged,
                     [&errorSignals] { ++errorSignals; });
    QObject::connect(&library, &model::LibraryModel::treeChanged,
                     [&treeSignals] { ++treeSignals; });
    check(!library.start(), "start fails without a helper");
    check(errorSignals >= 1, "the failed start notifies errorChanged");
    check(!library.lastError().isEmpty(), "the failed start carries its reason");
    check(!library.busy(), "busy is idle again after the failed start");
    check(busySignals >= 2, "the failed start still bracketed busy true..false");
    treeSignals = 0;
    library.selectCollection(QStringLiteral("X1"));
    check(treeSignals == 1, "selectCollection notifies treeChanged");
    check(library.selectedCollectionId() == QLatin1String("X1"), "the selection stuck");
    treeSignals = 0;
    library.selectCollection(QStringLiteral("X1"));
    check(treeSignals == 0, "re-selecting the same id is no new notification");
}

// --- the real tier -----------------------------------------------------------------

void testModelWorkflow(const QString& helperPath) {
    QTemporaryDir root;
    check(root.isValid(), "temporary root unavailable");
    // ASCII on purpose: the helper takes --store through narrow argv and
    // parses it as UTF-8 — a CJK directory name arrives as GBK bytes on
    // Windows and kills the child before its ready line. CJK catalog and
    // session NAMES are safe: they ride library.create's JSON params.
    const auto store = root.filePath(QStringLiteral("library-store"));
    // ASCII on purpose as well: the helper's check_files stat is not UTF-8
    // aware on Windows (std::filesystem in images.add is; ::stat in
    // images.list is not), so a CJK folder here would read exists=false —
    // an upstream finding this task documents, not models.
    const auto photos = root.filePath(QStringLiteral("photos"));
    const auto edits = root.filePath(QString::fromUtf8("edits"));
    check(QDir().mkpath(photos), "the photos folder is made");

    // The engine catalog stand-in: the model resolves EditStore film/paper
    // ids through the provider, exactly as main.cpp hands it
    // PreviewController's catalog once the engine has answered init.
    const spk::desktop::Catalog catalog{
        {{"test_film_a", "Test Film A", false}, {"test_film_b", "Test Film B", true}},
        {{"test_paper_a", "Test Paper A", false}, {"test_paper_b", "Test Paper B", false}}};

    model::LibraryModel::Deps deps;
    deps.helper.helperPath = helperPath;
    deps.helper.storePath = store;
    deps.registryPath = root.filePath(QString::fromUtf8("sources.json"));
    deps.editDirectory = edits;
    deps.thumbnailDirectory = root.filePath(QString::fromUtf8("thumbs"));
    deps.catalog = [&catalog] { return &catalog; };
    model::LibraryModel library(deps);

    // First launch before start(): an absent sources.json reads as the empty
    // library state and nothing is written behind the user's back.
    check(library.sources().isEmpty(), "an absent registry reads as the empty library state");
    check(library.selectedSourceId().isEmpty(), "no source is selected");
    check(!QFileInfo::exists(deps.registryPath), "nothing written before start()");

    const bool started = library.start();
    check(started, (std::string("start: ") + library.lastError().toStdString()).c_str());
    check(library.sources().isEmpty(), "first launch still lists no sources");
    check(library.collectionsTree().isEmpty(), "no source: no tree");
    check(library.total() == 0 && library.rowCount() == 0, "no source: no rows");

    // --- create a catalog ------------------------------------------------------
    const bool catalogMade = library.createCatalog(QString::fromUtf8("测试图库"));
    check(catalogMade, (std::string("createCatalog: ") +
                        library.lastError().toStdString()).c_str());
    check(library.sources().size() == 1, "the new catalog is the one source");
    const auto source = library.sources().front().toMap();
    check(source.value(QLatin1String("kind")).toString() == QLatin1String("catalog") &&
              source.value(QLatin1String("name")).toString() == QString::fromUtf8("测试图库") &&
              source.value(QLatin1String("selected")).toBool() &&
              source.value(QLatin1String("id")).toString().size() == 36,
          "the source is a selected catalog with an uppercase uuid");
    check(QFileInfo::exists(source.value(QLatin1String("path")).toString()),
          "the helper's catalog path exists on disk under the store root");
    check(QFileInfo::exists(deps.registryPath), "the registry file is written");

    const auto tree = library.collectionsTree();
    check(tree.size() == 4, "a fresh catalog's tree is the four fixed rows");
    const auto allImages = rowByKind(tree, QLatin1String("all_images"), QLatin1String("fixed"));
    check(!allImages.isEmpty(), "the All Images fixed row is in the tree");
    check(library.selectedCollectionId() == allImages.value(QLatin1String("id")).toString(),
          "All Images is the default selection");

    // --- import thirty synthetic PNGs ------------------------------------------
    const QByteArray png = onePixelPng();
    QVariantList items;
    for (int i = 0; i < 30; ++i) {
        const auto name = QStringLiteral("photo-%1.png").arg(i, 2, 10, QChar('0'));
        writeFile(QDir(photos).filePath(name), png);
        items << QVariantMap{
            {QLatin1String("path"), QDir(photos).filePath(name)},
            {QLatin1String("metadata"),
             QVariantMap{{QLatin1String("camera_make"), QLatin1String("Fixture")},
                         {QLatin1String("camera_model"), QLatin1String("Gate Cam")},
                         {QLatin1String("captured_local"),
                          i % 2 == 0 ? QLatin1String("2024-03-08T10:00:00")
                                     : QLatin1String("2025-01-02T09:30:00")}}}};
    }
    const auto imported = library.importFiles(items);
    check(imported.value(QLatin1String("error")).toString().isEmpty(),
          (std::string("importFiles: ") +
           library.lastError().toStdString()).c_str());
    check(imported.value(QLatin1String("added")).toInt() == 30,
          "the import result counts thirty added");
    check(library.total() == 30 && library.rowCount() == 30,
          "thirty imports list as thirty rows");
    check(uuidOf(library, 0).size() == 36, "rows carry helper image uuids");
    check(cell(library, 0, model::LibraryModel::KindRole).toString() == QLatin1String("raster"),
          "a .png fixture lands as raster, not other");
    check(cell(library, 0, model::LibraryModel::FileNameRole).toString() ==
              QLatin1String("photo-00.png"),
          "rows carry the file name");
    check(cell(library, 0, model::LibraryModel::CapturedLocalRole).toString() ==
              QLatin1String("2024-03-08T10:00:00"),
          "rows carry the stored captured_local");
    const auto exists = cell(library, 0, model::LibraryModel::ExistsRole);
    check(exists.isValid() && exists.toBool(), "rows carry check_files' exists");
    check(cell(library, 0, model::LibraryModel::ThumbnailRole).value<QImage>().isNull(),
          "the thumbnail role starts empty — the grid asks per row (task 4.4)");
    // The row thumbnail wiring, REVISED by the worker seam (task 4.4):
    // getThumbnail schedules the decode on the pool (returns empty), the
    // picture lands as dataChanged(ThumbnailRole) on the UI thread, and a
    // repeat ask serves the row's cached tile at once.
    const auto rowPath = pathOf(library, 0);
    int thumbnailUpdates = 0;
    QObject::connect(&library, &QAbstractItemModel::dataChanged,
                     [&thumbnailUpdates](const QModelIndex&, const QModelIndex&,
                                         const QList<int>& roles) {
                         if (roles.contains(model::LibraryModel::ThumbnailRole))
                             ++thumbnailUpdates;
                     });
    const auto thumb = library.getThumbnail(rowPath, 32);
    check(thumb.isNull(),
          "the first getThumbnail schedules the decode and returns empty (worker seam)");
    spinUntil([&thumbnailUpdates] { return thumbnailUpdates >= 1; },
              "the decoded thumbnail landed as dataChanged(ThumbnailRole)");
    const auto filled = cell(library, 0, model::LibraryModel::ThumbnailRole).value<QImage>();
    check(!filled.isNull(), "the row's thumbnail role carries the decoded picture");
    check(thumbnailUpdates == 1, "the filled thumbnail notified dataChanged(ThumbnailRole) once");
    const auto thumbAgain = library.getThumbnail(rowPath, 32);
    check(!thumbAgain.isNull() && thumbAgain.size() == filled.size(),
          "a repeat getThumbnail serves the row's cache synchronously");
    check(thumbnailUpdates == 1, "the cached call did not re-notify");

    const auto facets = library.facets();
    check(facets.value(QLatin1String("total")).toInt() == 30, "facets carry the total");
    bool rasterCounted = false;
    for (const auto& entry : facets.value(QLatin1String("kind")).toList()) {
        const auto kind = entry.toMap();
        if (kind.value(QLatin1String("value")).toString() == QLatin1String("raster") &&
            kind.value(QLatin1String("count")).toInt() == 30)
            rasterCounted = true;
    }
    check(rasterCounted, "the kind facet counts all thirty rasters");
    check(facets.value(QLatin1String("captured")).toMap()
                  .value(QLatin1String("years"))
                  .toList()
                  .size() == 2,
          "the captured facet spans both fixture years");
    check(rowById(library.collectionsTree(),
                   allImages.value(QLatin1String("id")).toString())
              .value(QLatin1String("imageCount"))
              .toInt() == 30,
          "the tree's All Images row counts thirty after the import");

    // --- pagination -------------------------------------------------------------
    library.setPageSize(25);
    library.refresh();
    check(library.rowCount() == 25 && library.total() == 30,
          "the first page holds pageSize rows, total 30");
    check(uuidOf(library, 0) == uuidOf(library, 0) &&
              cell(library, 24, model::LibraryModel::FileNameRole).toString() !=
                  QLatin1String(""),
          "the page is a real slice of rows");
    library.loadMore();
    check(library.rowCount() == 30, "loadMore fetches the remaining five");
    library.loadMore();
    check(library.rowCount() == 30, "loadMore past the end appends nothing");
    library.setPageSize(1000);
    library.refresh();

    // --- rating and colour tag round trip --------------------------------------
    const auto first = uuidOf(library, 0);
    const auto second = uuidOf(library, 1);
    check(library.setRating(first, 5),
          (std::string("setRating: ") + library.lastError().toStdString()).c_str());
    check(cell(library, 0, model::LibraryModel::RatingRole).toInt() == 5,
          "the rating updates in the row immediately");
    check(library.setColorTag(first, 3), "setColorTag acks");
    check(cell(library, 0, model::LibraryModel::ColorTagRole).toInt() == 3,
          "the colour tag updates in the row immediately");
    check(library.setRating(second, 3), "a second rating acks");
    check(library.setRating(first, 5), "re-setting the same rating still acks");
    const auto after = library.facets();
    check(countFacet(after, "rating", 5) == 1 && countFacet(after, "rating", 3) == 1 &&
              countFacet(after, "color_tag", 3) == 1,
          "facets follow the ratings and colour tag");
    library.refresh();
    check(cell(library, 0, model::LibraryModel::RatingRole).toInt() == 5 &&
              cell(library, 0, model::LibraryModel::ColorTagRole).toInt() == 3,
          "a fresh list from the helper keeps the stored values (round trip)");

    // --- filters ride the wire ---------------------------------------------------
    library.setRatingFilter(QVariantList{5});
    library.refresh();
    check(library.total() == 1 && library.rowCount() == 1 &&
              uuidOf(library, 0) == first,
          "the ratings filter narrows to the five-star row");
    library.setRatingFilter({});
    library.setTextFilter(QLatin1String("photo-1"));
    library.refresh();
    // The zero-padded names: photo-1 is a prefix of photo-10..photo-19 only.
    check(library.total() == 10, "the text filter matches photo-10..photo-19");
    library.setTextFilter({});
    library.setKindFilter(QVariantList{QLatin1String("raw")});
    library.refresh();
    check(library.total() == 0, "the kinds filter drops every raster");
    library.setKindFilter({});
    library.refresh();
    check(library.total() == 30, "clearing the filters restores the whole collection");

    // --- the EditStore develop join ----------------------------------------------
    editing::Store writer(edits);
    editing::State stored;
    stored.settings.film_stock = "test_film_b";
    stored.settings.print_stock = "test_paper_a";
    writer.save(QUrl::fromLocalFile(pathOf(library, 0)), stored);
    library.refresh();
    const auto dev0 = developOf(library, 0);
    check(dev0.value(QLatin1String("state")).toString() == QLatin1String("developed") &&
              dev0.value(QLatin1String("film")).toString() == QLatin1String("Test Film B") &&
              dev0.value(QLatin1String("paper")).toString() == QLatin1String("Test Paper A"),
          "the edited row joins to developed with its film and paper names");
    check(developOf(library, 1).value(QLatin1String("state")).toString() ==
              QLatin1String("notDeveloped"),
          "an unedited row reads notDeveloped");

    // --- an album, collection.add and selecting it --------------------------------
    check(library.createAlbum(QString::fromUtf8("精选")), "createAlbum acks");
    const auto refreshedTree = library.collectionsTree();
    QVariantMap album;
    for (const auto& value : refreshedTree) {
        const auto map = value.toMap();
        if (map.value(QLatin1String("kind")).toString() == QLatin1String("album") &&
            map.value(QLatin1String("title")).toString() == QString::fromUtf8("精选"))
            album = map;
    }
    check(!album.isEmpty() &&
              album.value(QLatin1String("section")).toString() == QLatin1String("user") &&
              album.value(QLatin1String("editable")).toBool(),
          "the new album rides the tree's user section as editable");
    check(library.addToCollection(album.value(QLatin1String("id")).toString(),
                                  QStringList{first}),
          "collection.add acks");
    library.refresh();
    const auto albumAfter = rowById(library.collectionsTree(),
                                    album.value(QLatin1String("id")).toString());
    check(albumAfter.value(QLatin1String("imageCount")).toInt() == 1,
          "the album's tree row counts its member");
    library.selectCollection(album.value(QLatin1String("id")).toString());
    check(library.total() == 1 && library.rowCount() == 1 && uuidOf(library, 0) == first,
          "selecting the album lists exactly its member");
    library.selectCollection(allImages.value(QLatin1String("id")).toString());
    check(library.total() == 30, "selecting All Images lists the catalog again");

    // --- a watched folder ---------------------------------------------------------
    const bool folderAdded = library.addFolder(photos);
    check(folderAdded, (std::string("addFolder: ") +
                        library.lastError().toStdString()).c_str());
    const auto withFolder = library.collectionsTree();
    QVariantMap folderRow;
    for (const auto& value : withFolder) {
        const auto map = value.toMap();
        if (map.value(QLatin1String("section")).toString() == QLatin1String("folders"))
            folderRow = map;
    }
    check(!folderRow.isEmpty(), "the watched folder rides the tree's folders section");
    library.selectCollection(folderRow.value(QLatin1String("id")).toString());
    check(library.total() == 30, "selecting the folder lists its images recursively");

    // --- change notifications (the review fixes) -----------------------------------
    // lastError's own NOTIFY: a failure reports through errorChanged, and
    // re-reporting the same reason is silent.
    int errorSignals = 0;
    QObject::connect(&library, &model::LibraryModel::errorChanged,
                     [&errorSignals] { ++errorSignals; });
    check(!library.selectSource(QStringLiteral("no-such-source")),
          "a bogus source id fails the selection");
    check(!library.lastError().isEmpty(), "the failure carries its reason");
    check(errorSignals == 1, "the failure notified errorChanged");
    check(!library.selectSource(QStringLiteral("no-such-source")),
          "the same bogus id fails again");
    check(errorSignals == 1, "the same reason set again notifies nothing");

    // busy brackets the blocking wire round trips: directly connected slots
    // observe busy == true inside the window, idle false after it.
    int busySignals = 0;
    bool busyTrueSeen = false;
    QObject::connect(&library, &model::LibraryModel::busyChanged, [&] {
        ++busySignals;
        if (library.busy()) busyTrueSeen = true;
    });
    library.refresh();
    check(busySignals >= 2 && busyTrueSeen,
          "a wire round trip brackets busy true..false");
    check(!library.busy(), "busy settles idle after the operation");

    // selectCollection notifies selectedCollectionId's declared NOTIFY, so
    // QML bindings on it cannot go stale between tree rebuilds.
    int treeSignals = 0;
    QObject::connect(&library, &model::LibraryModel::treeChanged,
                     [&treeSignals] { ++treeSignals; });
    library.selectCollection(allImages.value(QLatin1String("id")).toString());
    check(treeSignals == 1, "selectCollection notifies treeChanged");
    library.selectCollection(allImages.value(QLatin1String("id")).toString());
    check(treeSignals == 1, "re-selecting the same collection notifies nothing new");

    // --- openInDevelop -------------------------------------------------------------
    bool openSeen = false;
    QUrl openUrl;
    QObject::connect(&library, &model::LibraryModel::openRequested,
                     [&openSeen, &openUrl](const QUrl& url) {
                         openSeen = true;
                         openUrl = url;
                     });
    library.openInDevelop(pathOf(library, 0));
    check(openSeen && openUrl == QUrl::fromLocalFile(pathOf(library, 0)),
          "openInDevelop emits the row's local file URL for the openRaw channel");

    // --- a second source and switching back ----------------------------------------
    const bool sessionMade = library.createSession(QString::fromUtf8("会话甲"));
    check(sessionMade, (std::string("createSession: ") +
                        library.lastError().toStdString()).c_str());
    check(library.sources().size() == 2, "both sources are listed");
    check(library.selectedSource().value(QLatin1String("kind")).toString() == QLatin1String("session"),
          "the new session became the selection");
    check(library.total() == 0 && library.rowCount() == 0, "the fresh session holds no images");
    // A session carries only three fixed rows: In Catalog is a catalog's own.
    check(library.collectionsTree().size() == 3, "the session's tree is its three fixed rows");
    library.selectSource(source.value(QLatin1String("id")).toString());
    check(library.selectedSourceId() == source.value(QLatin1String("id")).toString(),
          "selectSource switches the selection");
    check(library.total() == 30, "the catalog's rows come back after the switch");
    check(library.collectionsTree().size() >= 5,
          "the catalog's tree (with album and folder) comes back too");

    // --- selectSource on the registry only (no helper writes) ----------------------
    library.selectSource(library.sources().at(1).toMap().value(QLatin1String("id")).toString());
    check(library.selectedSource().value(QLatin1String("kind")).toString() == QLatin1String("session"),
          "the registry switch selects the session again");

    std::printf("model workflow: catalog, 30 raster imports, paging, facets, meta round trip, "
                "filters, EditStore join, album, folder, openInDevelop, source switch all held\n");
}

// --- the import dedupe vocabulary (pure, hostless) ---------------------------------

void testImportDedupeKeys() {
    // Paths: same file said three ways collapses onto one key (case and
    // separators are the same path on Windows; the helper's lookup compares
    // in Unicode normalisation, the app's index in the OS's own fold).
    const auto a = model::LibraryModel::importPathKey(QStringLiteral("D:/Photos/a.png"));
    check(a == model::LibraryModel::importPathKey(QStringLiteral("D:\\Photos\\a.png")),
          "separator spelling does not change a path key");
    check(a == model::LibraryModel::importPathKey(QStringLiteral("d:/photos/A.PNG")),
          "Windows path comparison folds case");
    check(a != model::LibraryModel::importPathKey(QStringLiteral("D:/Photos/b.png")),
          "different files keep different keys");
    // Name+size: the Mac's MacImportExisting.nameAndSize — one compound key.
    check(model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 123) ==
              model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 123),
          "name+size keys are stable");
    check(model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 123) !=
              model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 124),
          "a size difference is a different photograph");
    check(model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 123) !=
              model::LibraryModel::importNameSizeKey(QStringLiteral("b.png"), 123),
          "a name difference is a different photograph");
    // holds(): exact path OR name+size — the Mac's MacImportExisting.holds.
    QSet<QString> paths{model::LibraryModel::importPathKey(QStringLiteral("D:/Photos/a.png"))};
    QSet<QString> nameSizes{model::LibraryModel::importNameSizeKey(QStringLiteral("a.png"), 123)};
    check(model::LibraryModel::importDuplicate(
              QStringLiteral("D:/photos/../Photos/a.png"), QStringLiteral("a.png"), 123, paths,
              nameSizes),
          "an exact path (said differently) is a duplicate");
    check(model::LibraryModel::importDuplicate(
              QStringLiteral("D:/Elsewhere/a.png"), QStringLiteral("a.png"), 123, paths, nameSizes),
          "the same name and size at a new path is a duplicate");
    check(model::LibraryModel::importDuplicate(
              QStringLiteral("D:/Elsewhere/a.png"), QStringLiteral("a.png"), 124, paths, nameSizes) ==
              false,
          "the same name at a different size is not");
    check(model::LibraryModel::importDuplicate(
              QStringLiteral("D:/Elsewhere/b.png"), QStringLiteral("b.png"), 123, paths,
              nameSizes) == false,
          "a different name at the same size is not");
    check(model::LibraryModel::importDuplicate(QStringLiteral("D:/Elsewhere/b.png"),
                                               QStringLiteral("b.png"), -1, {}, {}) == false,
          "no index at all: nothing is a duplicate");
}

// --- the chunked import and the folder sync (real tier) ------------------------------
//
// One fresh catalog with its own model (importChunkSize 3 so chunking is
// observable without 200 fixtures), then:
//   import — 10 PNGs in 4 chunks, one Recent Imports album, progress property;
//   dedupe — the same ten again (path), one same-name-same-size copy (name+size);
//   sync   — a watched folder: new / move-keeps-rating / gone-hides / the
//            folder gone is offline and untouched / back is restored.
void testImportSyncWorkflow(const QString& helperPath) {
    QTemporaryDir root;
    check(root.isValid(), "temporary root unavailable");
    const auto store = root.filePath(QStringLiteral("library-store"));
    const auto photos = root.filePath(QStringLiteral("photos"));
    const auto watch = root.filePath(QStringLiteral("watch"));
    const auto edits = root.filePath(QString::fromUtf8("edits"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const QByteArray png = onePixelPng();

    const spk::desktop::Catalog catalog{{}, {}};
    model::LibraryModel::Deps deps;
    deps.helper.helperPath = helperPath;
    deps.helper.storePath = store;
    deps.registryPath = root.filePath(QString::fromUtf8("sources.json"));
    deps.editDirectory = edits;
    deps.thumbnailDirectory = root.filePath(QString::fromUtf8("thumbs"));
    deps.importChunkSize = 3; // observable chunking
    deps.catalog = [&catalog] { return &catalog; };
    model::LibraryModel library(deps);
    check(library.start(), (std::string("start: ") + library.lastError().toStdString()).c_str());
    check(library.createCatalog(QStringLiteral("import-sync")),
          (std::string("createCatalog: ") + library.lastError().toStdString()).c_str());

    // --- the chunked import ---------------------------------------------------
    QVariantList items;
    for (int i = 0; i < 10; ++i) {
        const auto name = QStringLiteral("photo-%1.png").arg(i, 2, 10, QChar('0'));
        writeFile(QDir(photos).filePath(name), png);
        items << QDir(photos).filePath(name); // plain path strings, as a drop gives them
    }
    int progressFrames = 0;
    QObject::connect(&library, &model::LibraryModel::progressChanged,
                     [&progressFrames] { ++progressFrames; });
    // Live observability (the worker seam, task 4.4): a timer samples the
    // progress property while importFiles is still on the stack — its
    // events are processed inside the pumped window (user input excluded),
    // so a sample reading done>0 proves the property moved mid-run.
    QVariantMap midRun;
    QTimer sampler;
    sampler.setInterval(1);
    QObject::connect(&sampler, &QTimer::timeout,
                     [&library, &midRun] { midRun = library.importProgress(); });
    sampler.start();
    const auto result = library.importFiles(items);
    sampler.stop();
    check(result.value(QLatin1String("error")).toString().isEmpty(),
          (std::string("importFiles: ") + library.lastError().toStdString()).c_str());
    check(result.value(QLatin1String("added")).toInt() == 10, "ten files added");
    check(result.value(QLatin1String("duplicates")).toInt() == 0, "nothing excluded first time");
    check(result.value(QLatin1String("chunks")).toInt() == 4,
          "ten files at chunk size 3 rode in four chunks");
    const auto album = result.value(QLatin1String("album")).toString();
    check(album.size() == 36, "the import's Recent Imports album uuid came back");
    const auto progress = library.importProgress();
    check(progress.value(QLatin1String("phase")).toString() == QLatin1String("finishing") &&
              progress.value(QLatin1String("done")).toInt() == 10 &&
              progress.value(QLatin1String("total")).toInt() == 10,
          "the progress property ended at done==total, phase finishing");
    check(progressFrames >= 1, "the progress property notified");
    check(!midRun.isEmpty() && midRun.value(QLatin1String("done")).toInt() > 0 &&
              progressFrames >= 2,
          "progress frames landed while importFiles was still in flight (worker seam)");
    check(library.total() == 10, "the grid lists the ten imports");
    // Exactly ONE album under Recent Imports: the later chunks joined the
    // first chunk's album, they did not each make their own.
    int albums = 0;
    for (const auto& value : library.collectionsTree())
        if (value.toMap().value(QLatin1String("kind")).toString() == QLatin1String("album"))
            ++albums;
    check(albums == 1, "four chunks made one album, not four");
    check(library.addToCollection(album, QStringList{uuidOf(library, 0)}), "the album takes images");
    check(library.selectSource(library.selectedSourceId()), "the source stays selected");

    // --- the dedupe (path, then name+size) --------------------------------------
    const auto again = library.importFiles(items);
    check(again.value(QLatin1String("error")).toString().isEmpty(), "the re-import ran");
    check(again.value(QLatin1String("added")).toInt() == 0 &&
              again.value(QLatin1String("duplicates")).toInt() == 10 &&
              again.value(QLatin1String("chunks")).toInt() == 0 &&
              again.value(QLatin1String("album")).toString().isEmpty(),
          "the same ten paths are all duplicates: nothing added, no chunk, no album");
    check(library.total() == 10, "the grid did not grow");
    // A copy under a new path with the same name and size is a duplicate too
    // (the Mac's Exclude Duplicates), so a re-imported card never doubles.
    const auto sub = root.filePath(QStringLiteral("card-copy"));
    check(QDir().mkpath(sub), "the copy folder is made");
    const auto sameBytes = QDir(sub).filePath(QStringLiteral("photo-00.png"));
    writeFile(sameBytes, png); // same content, same size, different path
    const auto third = library.importFiles(QVariantList{sameBytes});
    check(third.value(QLatin1String("duplicates")).toInt() == 1 &&
              third.value(QLatin1String("added")).toInt() == 0,
          "a same-name-same-size copy at a new path is a duplicate");
    albums = 0;
    for (const auto& value : library.collectionsTree())
        if (value.toMap().value(QLatin1String("kind")).toString() == QLatin1String("album"))
            ++albums;
    check(albums == 1, "a fully-duplicated import made no new album");
    // An import with nothing to add still returns honestly.
    const auto empty = library.importFiles({});
    check(empty.value(QLatin1String("added")).toInt() == 0 &&
              empty.value(QLatin1String("error")).toString().isEmpty(),
          "an empty import list is an honest no-op");

    // --- the watched folder and its sync ----------------------------------------
    check(QDir().mkpath(watch), "the watched folder is made");
    const auto watchA = QDir(watch).filePath(QStringLiteral("watch-a.png"));
    const auto watchB = QDir(watch).filePath(QStringLiteral("watch-b.png"));
    const auto watchC = QDir(watch).filePath(QStringLiteral("watch-c.png"));
    writeFile(watchA, png);
    writeFile(watchB, png);
    writeFile(watchC, png);
    check(library.addFolder(watch), (std::string("addFolder: ") +
                                     library.lastError().toStdString()).c_str());
    check(library.total() == 13, "the folder's three files joined by reference on add");
    const auto watched = library.watchedFolders();
    check(watched.size() == 1, "the watched folder is listed once");
    const auto watchedRow = watched.front().toMap();
    // The helper spells paths natively (std::filesystem's backslashes on
    // Windows); the comparison normalizes both sides.
    check(QDir::fromNativeSeparators(
              watchedRow.value(QLatin1String("path")).toString()) ==
              QDir::cleanPath(watch) &&
              watchedRow.value(QLatin1String("id")).toString().size() == 36 &&
              watchedRow.value(QLatin1String("online")).toBool(),
          "the watched folder row carries its path, id and online state");
    // A five-star rating on one watched row: the move below must keep it.
    QString ratedUuid;
    for (int i = 0; i < library.rowCount(); ++i)
        if (QDir::fromNativeSeparators(pathOf(library, i)) == QDir::cleanPath(watchA))
            ratedUuid = uuidOf(library, i);
    check(!ratedUuid.isEmpty(), "the watched row to rate was found");
    check(library.setRating(ratedUuid, 5), "the watched row was rated");

    // A NEW file: added by reference, counted.
    const auto watchNew = QDir(watch).filePath(QStringLiteral("watch-new.png"));
    writeFile(watchNew, png);
    auto report = library.synchronize();
    check(report.value(QLatin1String("error")).toString().isEmpty(),
          (std::string("synchronize: ") + library.lastError().toStdString()).c_str());
    check(report.value(QLatin1String("added")).toInt() == 1 &&
              report.value(QLatin1String("moved")).toInt() == 0 &&
              report.value(QLatin1String("removed")).toInt() == 0 &&
              report.value(QLatin1String("restored")).toInt() == 0 &&
              !report.value(QLatin1String("offline")).toBool(),
          "the sync counted one new file and nothing else");
    check(library.total() == 14, "the grid shows the new file");

    // A MOVE: watch-a to a subfolder, same name, same size — exactly one
    // candidate that went. The row follows: the five-star rating stays.
    const auto movedDir = QDir(watch).filePath(QStringLiteral("moved"));
    check(QDir().mkpath(movedDir), "the move destination folder is made");
    const auto watchMoved = QDir(movedDir).filePath(QStringLiteral("watch-a.png"));
    check(QFile::rename(watchA, watchMoved), "the fixture file was moved");
    report = library.synchronize();
    check(report.value(QLatin1String("moved")).toInt() == 1 &&
              report.value(QLatin1String("added")).toInt() == 0 &&
              report.value(QLatin1String("removed")).toInt() == 0,
          "the sync classified the rename as a move");
    check(library.total() == 14, "a move does not change the grid's size");
    QString movedUuid;
    for (int i = 0; i < library.rowCount(); ++i)
        if (QDir::fromNativeSeparators(pathOf(library, i)) == QDir::cleanPath(watchMoved))
            movedUuid = uuidOf(library, i);
    check(!movedUuid.isEmpty(), "the moved file's row is listed at its new path");
    check(movedUuid == ratedUuid, "the move kept the row (same uuid)");
    int ratedSeen = 0;
    for (int i = 0; i < library.rowCount(); ++i) {
        const auto map = library.index(i, 0).data(model::LibraryModel::UuidRole);
        if (map.toString() == ratedUuid &&
            cell(library, i, model::LibraryModel::RatingRole).toInt() == 5)
            ++ratedSeen;
    }
    check(ratedSeen == 1, "the moved row kept its five-star rating");

    // GONE: a deleted file is hidden, not forgotten.
    check(QFile::remove(watchB), "the fixture file was deleted");
    report = library.synchronize();
    check(report.value(QLatin1String("removed")).toInt() == 1 &&
              report.value(QLatin1String("added")).toInt() == 0,
          "the sync classified the deletion as gone");
    check(library.total() == 13, "the gone file's row is hidden from the grid");
    // The row is kept: the file back again brings the row back (restored).
    writeFile(watchB, png);
    report = library.synchronize();
    check(report.value(QLatin1String("restored")).toInt() == 1,
          "the sync restored the row when the file came back");
    check(library.total() == 14, "the restored row is listed again");
    ratedSeen = 0;
    for (int i = 0; i < library.rowCount(); ++i)
        if (uuidOf(library, i) == ratedUuid &&
            cell(library, i, model::LibraryModel::RatingRole).toInt() == 5)
            ++ratedSeen;
    check(ratedSeen == 1, "the rated row survived the gone-and-back round trip");

    // OFFLINE: a folder that is not there is a disk away, never a deletion.
    check(QDir(watch).removeRecursively(), "the watched folder was removed");
    report = library.synchronize();
    check(report.value(QLatin1String("offline")).toBool(),
          "the sync reports the offline folder");
    check(report.value(QLatin1String("removed")).toInt() == 0 &&
              report.value(QLatin1String("added")).toInt() == 0 &&
              report.value(QLatin1String("moved")).toInt() == 0,
          "nothing in an offline folder is marked");
    check(library.total() == 14, "the offline folder's rows stay listed");
    check(!library.watchedFolders().front().toMap().value(QLatin1String("online")).toBool(),
          "the watched folder row reads offline");
    // The disk comes back: everything is restored.
    check(QDir().mkpath(movedDir), "the watched folder is made again");
    writeFile(watchMoved, png);
    writeFile(watchB, png);
    writeFile(watchC, png);
    writeFile(watchNew, png);
    report = library.synchronize();
    // Nothing was marked while the folder was away (the offline contract),
    // so the returning disk restores nothing — it re-sees four unchanged
    // files and the grid never blinked.
    check(!report.value(QLatin1String("offline")).toBool() &&
              report.value(QLatin1String("restored")).toInt() == 0 &&
              report.value(QLatin1String("added")).toInt() == 0 &&
              report.value(QLatin1String("removed")).toInt() == 0 &&
              report.value(QLatin1String("moved")).toInt() == 0,
          "the coming disk re-sees all four files and marks nothing");
    check(library.total() == 14, "the grid is whole again after the disk came back");
    check(library.watchedFolders().front().toMap().value(QLatin1String("online")).toBool(),
          "the watched folder row reads online again");

    // stopWatching: the folder leaves the watched list, its images stay.
    const auto folderId = library.watchedFolders().front().toMap()
                              .value(QLatin1String("id"))
                              .toString();
    check(library.stopWatching(folderId), (std::string("stopWatching: ") +
                                           library.lastError().toStdString()).c_str());
    check(library.watchedFolders().isEmpty(), "the watched list is empty after stopWatching");
    check(library.total() == 14, "stopWatching removed no images");
    check(!library.stopWatching(QStringLiteral("no-such-folder")),
          "an unknown folder fails the stop");
    check(!library.lastError().isEmpty(), "the failed stop carries its reason");

    std::printf("import/sync workflow: chunked import (4 chunks, one album), dedupe (path, "
                "name+size), progress, folder sync (new/move-keeps-rating/gone-hides/offline/"
                "restored), stopWatching all held\n");
}

// The grid's selection and the settings clipboard over it (task 4.4): the
// Mac's copy/paste on the library — records written through the model's
// editing::Store, only the ticked groups' fields, the target's own crop,
// decode mode and As Shot white balance kept.
void testClipboardWorkflow(const QString& helperPath) {
    QTemporaryDir root;
    check(root.isValid(), "the clipboard workflow's temp root is valid");
    const auto store = root.filePath(QStringLiteral("store"));
    const auto edits = root.filePath(QStringLiteral("edits"));
    const auto photos = root.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const QByteArray png = onePixelPng();
    for (int i = 0; i < 3; ++i)
        writeFile(QDir(photos).filePath(QStringLiteral("photo-%1.png").arg(i, 2, 10, QChar('0'))),
                  png);

    // The clipboard workflow's catalog: the test stocks the records name —
    // Store::save refuses a record naming a stock the catalog lacks.
    const spk::desktop::Catalog catalog{
        {{"test_film_a", "Test Film A", false}, {"test_film_b", "Test Film B", true}},
        {{"test_paper_a", "Test Paper A", false}, {"test_paper_b", "Test Paper B", false}}};
    model::LibraryModel::Deps deps;
    deps.helper.helperPath = helperPath;
    deps.helper.storePath = store;
    deps.registryPath = root.filePath(QStringLiteral("sources.json"));
    deps.editDirectory = edits;
    deps.thumbnailDirectory = root.filePath(QStringLiteral("thumbs"));
    deps.catalog = [&catalog] { return &catalog; };
    model::LibraryModel library(deps);
    check(library.start(), (std::string("start: ") + library.lastError().toStdString()).c_str());
    check(library.createCatalog(QStringLiteral("clipboard")),
          (std::string("createCatalog: ") + library.lastError().toStdString()).c_str());
    QVariantList items;
    for (int i = 0; i < 3; ++i)
        items << QDir(photos).filePath(QStringLiteral("photo-%1.png").arg(i, 2, 10, QChar('0')));
    check(library.importFiles(items).value(QLatin1String("added")).toInt() == 3,
          "three photos imported for the clipboard workflow");

    // --- the selection vocabulary ------------------------------------------------
    check(library.selectedCount() == 0, "the grid starts unselected");
    library.selectOnly(0);
    check(library.selectedCount() == 1 &&
              cell(library, 0, model::LibraryModel::SelectedRole).toBool() &&
              !cell(library, 1, model::LibraryModel::SelectedRole).toBool(),
          "a plain click selects only its row");
    library.toggleSelected(1);
    check(library.selectedCount() == 2 &&
              cell(library, 1, model::LibraryModel::SelectedRole).toBool(),
          "a Ctrl-click adds a row");
    library.toggleSelected(1);
    check(library.selectedCount() == 1, "a second Ctrl-click takes the row back off");
    library.selectOnly(2);
    library.selectRange(0);
    check(library.selectedCount() == 3,
          "a Shift-click extends from the anchor over the run");
    library.clearSelection();
    check(library.selectedCount() == 0, "clear empties the selection");
    library.selectAllRows();
    check(library.selectedCount() == 3, "select-all takes every row");
    check(library.selectedUuids().size() == 3, "the uuid list follows the selection");
    library.refresh();
    check(library.selectedCount() == 3,
          "the selection survives a re-list row for row");

    // --- copy from a stored record ------------------------------------------------
    // Photo A's record, written the way the editor writes records — the
    // model's clipboard reads the same store (main.cpp hands both the one
    // directory).
    const QString editsPath = QDir(photos).filePath(QStringLiteral("photo-00.png"));
    editing::Store records(edits);
    const QUrl sourceA = QUrl::fromLocalFile(editsPath);
    editing::State edited;
    edited.settings.film_stock = "test_film_b";
    edited.settings.print_stock = "test_paper_a";
    edited.settings.di_active = true;
    edited.settings.di_blue_compensation = true;
    edited.settings.auto_exposure = false;
    edited.settings.film_exposure_ev = 0.3;
    edited.settings.print_exposure = 1.2;
    edited.settings.y_filter_shift = 0.05;
    edited.settings.m_filter_shift = -0.02;
    edited.settings.film_format_mm = 56.0;
    edited.settings.grain_amount = 1.4;
    edited.settings.halation_amount = 2.5;
    edited.settings.wb_temperature_k = 6500.0;
    edited.settings.wb_tint = 10.0;
    edited.layer2.exposure = 0.5;
    edited.layer2.curves.rgb.points = {QPointF(0, 0), QPointF(0.5, 0.6), QPointF(1, 1)};
    edited.layer2.colorBalance.shadows.saturation = 0.3;
    edited.geometry.quarterTurns = 1;
    edited.mode = spk::desktop::DecodeMode::headroom;
    records.save(sourceA, edited);

    library.selectOnly(0);
    library.setClipboardGroups(QVariantList{QStringLiteral("filmAndPaper"),
                                            QStringLiteral("filmExposure"),
                                            QStringLiteral("format"), QStringLiteral("decode"),
                                            QStringLiteral("enlarger"), QStringLiteral("light"),
                                            QStringLiteral("curve"), QStringLiteral("colour")});
    check(library.clipboardGroups().size() == 8,
          "the offered groups ride the sheet's ticks");
    check(library.copySettings(), (std::string("copySettings: ") +
                                   library.lastError().toStdString()).c_str());
    check(library.hasClipboard(), "the clipboard holds a clip");
    check(library.clipboardSourceName() == QLatin1String("photo-00.png"),
          "the clip remembers its source's name");
    // Paste into photo B: only the groups' fields cross; B's crop, decode
    // mode, mask, glare and view stay its own.
    const QString pathB = QDir(photos).filePath(QStringLiteral("photo-01.png"));
    library.selectOnly(1);
    check(library.pasteSettings(), (std::string("pasteSettings: ") +
                                    library.lastError().toStdString()).c_str());
    const auto reloaded = records.load(QUrl::fromLocalFile(pathB), catalog);
    check(reloaded.state.has_value(), "the paste wrote photo B a record");
    const editing::State& b = *reloaded.state;
    check(b.settings.film_stock == edited.settings.film_stock &&
              b.settings.print_stock == edited.settings.print_stock &&
              b.settings.di_active && b.settings.di_blue_compensation,
          "film, paper and the DI traveled");
    check(!b.settings.auto_exposure &&
              b.settings.film_exposure_ev == edited.settings.film_exposure_ev,
          "the meter switch and film placement traveled as settings");
    check(b.settings.print_exposure == edited.settings.print_exposure &&
              b.settings.y_filter_shift == edited.settings.y_filter_shift &&
              b.settings.m_filter_shift == edited.settings.m_filter_shift,
          "the enlarger's three rows traveled");
    check(b.settings.film_format_mm == edited.settings.film_format_mm &&
              b.settings.grain_amount == edited.settings.grain_amount &&
              b.settings.halation_amount == edited.settings.halation_amount,
          "the format group traveled");
    check(b.settings.wb_temperature_k == edited.settings.wb_temperature_k &&
              b.settings.wb_tint == edited.settings.wb_tint,
          "the measured white balance traveled with its numbers");
    check(b.layer2.exposure == edited.layer2.exposure &&
              b.layer2.curves == edited.layer2.curves &&
              b.layer2.colorBalance.shadows.saturation == 0.3,
          "light, curve and colour traveled");
    check(b.geometry == output::Geometry{} && b.mode == spk::desktop::DecodeMode::compatible16,
          "photo B kept its own crop and decode mode");
    check(!b.settings.contrast_mask_active && b.settings.glare_amount == 1.0,
          "the mask, glare and every ungrouped row stayed photo B's own");
    check(developOf(library, 1).value(QLatin1String("state")).toString() ==
              QLatin1String("developed"),
          "the pasted row's develop badge turned developed");

    // --- the As Shot rule over the wire ---------------------------------------------
    // A clip copied from an As Shot frame keeps the target's own pair.
    edited.settings.wb_temperature_k = 0.0;
    edited.settings.wb_tint = 0.0;
    records.save(sourceA, edited);
    const QString pathC = QDir(photos).filePath(QStringLiteral("photo-02.png"));
    editing::State warmed;
    warmed.settings.film_stock = "test_film_a";
    warmed.settings.print_stock = "test_paper_b";
    warmed.settings.wb_temperature_k = 4800.0;
    warmed.settings.wb_tint = -7.0;
    records.save(QUrl::fromLocalFile(pathC), warmed);
    library.selectOnly(0);
    check(library.copySettings(), "the As Shot clip copied");
    library.selectOnly(2);
    check(library.pasteSettings(), "the As Shot clip pasted");
    const auto reloadedC = records.load(QUrl::fromLocalFile(pathC), catalog);
    check(reloadedC.state.has_value() &&
              reloadedC.state->settings.wb_temperature_k == 4800.0 &&
              reloadedC.state->settings.wb_tint == -7.0,
          "an As Shot clip kept the target's own white balance");

    // --- the refusals -----------------------------------------------------------------
    library.clearSelection();
    check(!library.copySettings() && !library.lastError().isEmpty(),
          "copying with nothing selected fails with its reason");
    library.selectOnly(1);
    check(library.rowInfo(library.selectedUuids().front())
              .value(QLatin1String("fileName"))
              .toString() == QLatin1String("photo-01.png"),
          "rowInfo serves the selected row's fields");

    std::printf("clipboard workflow: selection vocabulary, copy groups from a stored record, "
                "paste writes only the groups (target keeps crop/mode/WB), As Shot rule over "
                "the wire, refusals all held\n");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testTreeRows();
        testListQuery();
        testFilterMap();
        testDevelopJoinHelpers();
        testImportDedupeKeys();
        testNotificationsWithoutHost();
        if (argc > 1 && QFileInfo(argv[1]).isFile()) {
            testModelWorkflow(QString::fromUtf8(argv[1]));
            testImportSyncWorkflow(QString::fromUtf8(argv[1]));
            testClipboardWorkflow(QString::fromUtf8(argv[1]));
        } else {
            std::printf("model workflow skipped: no spklib-host given "
                        "(pure tier still verified)\n");
        }
        std::printf("library_model: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("library_model: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
