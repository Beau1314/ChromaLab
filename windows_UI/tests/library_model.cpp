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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace model = spk::desktop::library;
namespace editing = spk::desktop::editing;

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
    const bool imported = library.importFiles(items);
    check(imported, (std::string("importFiles: ") +
                     library.lastError().toStdString()).c_str());
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
          "the thumbnail role is present for task 4.3 and empty for now");

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

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testTreeRows();
        testListQuery();
        testFilterMap();
        testDevelopJoinHelpers();
        testNotificationsWithoutHost();
        if (argc > 1 && QFileInfo(argv[1]).isFile()) {
            testModelWorkflow(QString::fromUtf8(argv[1]));
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
