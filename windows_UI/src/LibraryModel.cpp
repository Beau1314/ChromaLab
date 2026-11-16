#include "LibraryModel.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace spk::desktop::library {


namespace {

QString uuidString(const QUuid& uuid) {
    return uuid.toString(QUuid::WithoutBraces).toUpper();
}

QVariantMap sourceRow(const CatalogSourcesStore::Source& source, const std::optional<QUuid>& selected) {
    QVariantMap map;
    map.insert(QStringLiteral("id"), uuidString(source.id));
    map.insert(QStringLiteral("kind"), CatalogSourcesStore::kindToWire(source.kind));
    map.insert(QStringLiteral("name"), source.name);
    map.insert(QStringLiteral("path"), source.path);
    if (!source.libraryPath.isEmpty()) map.insert(QStringLiteral("libraryPath"), source.libraryPath);
    map.insert(QStringLiteral("addedAt"),
               source.addedAt.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    if (source.openedAt.has_value())
        map.insert(QStringLiteral("openedAt"),
                   source.openedAt->toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    map.insert(QStringLiteral("selected"), selected.has_value() && *selected == source.id);
    return map;
}

/// An item's path, whether it came as a string or as a {path, metadata} map.
QString itemPath(const QVariant& item) {
    if (item.typeId() == QMetaType::QString) return item.toString();
    return item.toMap().value(QStringLiteral("path")).toString();
}

} // namespace

LibraryModel::LibraryModel(Deps deps, QObject* parent)
    : QAbstractListModel(parent),
      store_(deps.registryPath),
      edit_store_(deps.editDirectory),
      thumbnails_([&] {
          spk::desktop::ThumbnailStore::Options options;
          options.directory = deps.thumbnailDirectory;
          return options;
      }()),
      catalog_provider_(std::move(deps.catalog)) {
    storeRoot_ = deps.helper.storePath;
    if (storeRoot_.isEmpty()) storeRoot_ = CatalogSourcesStore::macLibraryRoot();
    deps.helper.storePath = storeRoot_;
    options_ = deps.helper;
    importChunkSize_ = deps.importChunkSize > 0 ? deps.importChunkSize : 200;
    spawnHelper();
}

LibraryModel::~LibraryModel() { host_.shutdown(); }

// --- the busy guard (see the header's notifications note) --------------------------

LibraryModel::BusyGuard::BusyGuard(LibraryModel* model) : model_(model) {
    if (model_->busyDepth_++ == 0) {
        model_->busy_ = true;
        emit model_->busyChanged();
    }
}

LibraryModel::BusyGuard::~BusyGuard() {
    if (--model_->busyDepth_ == 0) {
        model_->busy_ = false;
        emit model_->busyChanged();
    }
}

// --- QAbstractListModel ---------------------------------------------------------

int LibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(rows_.size())) return {};
    const Row& row = rows_[std::size_t(index.row())];
    switch (role) {
    case UuidRole: return row.row.uuid();
    case PathRole: return row.row.path();
    case FileNameRole: return row.row.fileName();
    case KindRole: return row.row.kind();
    case RatingRole: return row.row.rating();
    case ColorTagRole: return row.row.colorTag();
    case CapturedLocalRole: return row.row.capturedLocal();
    case ExistsRole: {
        const auto exists = row.row.exists();
        return exists.has_value() ? QVariant(*exists) : QVariant();
    }
    case TrashedRole: return row.row.trashed();
    case DevelopRole: return row.develop;
    case ThumbnailRole: return row.thumbnail;
    case VariantCountRole: return row.row.variantCount();
    case PrimaryVariantRole: return row.row.primaryVariant();
    case EditRevisionRole: return row.row.primaryEditRevision();
    default: return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {{UuidRole, "uuid"},         {PathRole, "path"},
            {FileNameRole, "fileName"}, {KindRole, "kind"},
            {RatingRole, "rating"},     {ColorTagRole, "colorTag"},
            {CapturedLocalRole, "capturedLocal"},
            {ExistsRole, "exists"},     {TrashedRole, "trashed"},
            {DevelopRole, "develop"},   {ThumbnailRole, "thumbnail"},
            {VariantCountRole, "variantCount"},
            {PrimaryVariantRole, "primaryVariant"},
            {EditRevisionRole, "editRevision"}};
}

// --- properties -------------------------------------------------------------------

QString LibraryModel::selectedSourceId() const {
    return registry_.selected.has_value() ? uuidString(*registry_.selected) : QString();
}

QVariantMap LibraryModel::selectedSource() const {
    const auto id = selectedSourceId();
    for (const auto& value : sources_)
        if (value.toMap().value(QStringLiteral("id")).toString() == id) return value.toMap();
    return {};
}

void LibraryModel::setSort(const QString& sort) {
    if (sort_ == sort) return;
    sort_ = sort;
    emit queryChanged();
}

void LibraryModel::setDescending(bool descending) {
    if (descending_ == descending) return;
    descending_ = descending;
    emit queryChanged();
}

void LibraryModel::setTextFilter(const QString& text) {
    if (textFilter_ == text) return;
    textFilter_ = text;
    emit queryChanged();
}

void LibraryModel::setRatingFilter(const QVariantList& ratings) {
    if (ratingFilter_ == ratings) return;
    ratingFilter_ = ratings;
    emit queryChanged();
}

void LibraryModel::setColorTagFilter(const QVariantList& colorTags) {
    if (colorTagFilter_ == colorTags) return;
    colorTagFilter_ = colorTags;
    emit queryChanged();
}

void LibraryModel::setKindFilter(const QVariantList& kinds) {
    if (kindFilter_ == kinds) return;
    kindFilter_ = kinds;
    emit queryChanged();
}

void LibraryModel::setPageSize(int size) {
    if (size <= 0 || pageSize_ == size) return;
    pageSize_ = size;
    emit queryChanged();
}

void LibraryModel::setError(const QString& message) {
    if (lastError_ == message) return;
    lastError_ = message;
    emit errorChanged();
}

void LibraryModel::setSelectedCollection(const QString& id) {
    if (selectedCollection_ == id) return;
    selectedCollection_ = id;
    // selectedCollectionId's declared NOTIFY (see the review fix): selectCollection
    // and the source switches go through here, so bindings cannot go stale.
    emit treeChanged();
}

// --- pure helpers --------------------------------------------------------------------

QVariantList LibraryModel::buildTreeRows(const std::vector<QVariantMap>& collections) {
    QVariantList rows;
    const auto append = [&](const std::vector<QVariantMap>& input, const QString& section) {
        for (const auto& row : input) {
            QVariantMap map;
            map.insert(QStringLiteral("id"), row.value(QStringLiteral("uuid")).toString());
            map.insert(QStringLiteral("parent"), row.value(QStringLiteral("parent")).toString());
            map.insert(QStringLiteral("kind"), row.value(QStringLiteral("kind")).toString());
            map.insert(QStringLiteral("title"),
                       row.value(QStringLiteral("title"), row.value(QStringLiteral("name")))
                           .toString());
            map.insert(QStringLiteral("imageCount"), row.value(QStringLiteral("image_count")));
            map.insert(QStringLiteral("editable"), row.value(QStringLiteral("editable")).toBool());
            map.insert(QStringLiteral("path"), row.value(QStringLiteral("folder")).toString());
            map.insert(QStringLiteral("section"), section);
            rows << map;
        }
    };
    const auto input = CatalogSourcesStore::treeInput(collections);
    append(input.fixedRows, QStringLiteral("fixed"));
    append(input.userRows, QStringLiteral("user"));
    append(input.folders, QStringLiteral("folders"));
    return rows;
}

QVariantMap LibraryModel::listQuery(const QString& collection, const QString& sort, bool descending,
                                    const QVariantMap& filter, int pageSize, int offset) {
    QVariantMap query;
    if (!collection.isEmpty()) query.insert(QStringLiteral("collection"), collection);
    query.insert(QStringLiteral("sort"), sort);
    query.insert(QStringLiteral("descending"), descending);
    query.insert(QStringLiteral("offset"), offset);
    query.insert(QStringLiteral("limit"), pageSize);
    if (!filter.isEmpty()) query.insert(QStringLiteral("filter"), filter);
    return query;
}

QVariantMap LibraryModel::filterMap(const QString& text, const QVariantList& ratings,
                                    const QVariantList& colorTags, const QVariantList& kinds) {
    QVariantMap filter;
    if (!text.isEmpty()) filter.insert(QStringLiteral("text"), text);
    if (!ratings.isEmpty()) filter.insert(QStringLiteral("ratings"), ratings);
    if (!colorTags.isEmpty()) filter.insert(QStringLiteral("color_tags"), colorTags);
    if (!kinds.isEmpty()) filter.insert(QStringLiteral("kinds"), kinds);
    return filter;
}

// --- the import dedupe vocabulary (see the header's IMPORT note) ---------------------

QString LibraryModel::importPathKey(const QString& path) {
    // Separators in, one spelling out; case folded where the filesystem
    // itself is case-blind (Windows), kept where it is not.
    QString key = QDir::fromNativeSeparators(QDir::cleanPath(path));
#ifdef Q_OS_WIN
    return key.toCaseFolded();
#else
    return key;
#endif
}

QString LibraryModel::importNameSizeKey(const QString& fileName, qlonglong size) {
    // The Mac's MacImportExisting.nameAndSize: one compound key, with a
    // separator no file name can carry (precomposed canonical mapping on the
    // Mac; QString NFC-ish comparison is close enough for a dedupe hint and
    // the helper re-checks every survivor by exact path anyway).
    return fileName.toCaseFolded() + QChar(u'\x1F') + QString::number(size);
}

bool LibraryModel::importDuplicate(const QString& path, const QString& fileName, qlonglong size,
                                   const QSet<QString>& paths, const QSet<QString>& namesAndSizes) {
    if (paths.contains(importPathKey(path))) return true;
    if (size < 0) return false;
    return namesAndSizes.contains(importNameSizeKey(fileName, size));
}

QString LibraryModel::stockLabel(const spk::desktop::Catalog* catalog, const std::string& id) {
    if (catalog == nullptr || id.empty()) return {};
    for (const auto& stock : catalog->films)
        if (stock.id == id) return QString::fromStdString(stock.label);
    for (const auto& stock : catalog->papers)
        if (stock.id == id) return QString::fromStdString(stock.label);
    return {};
}

QVariantMap LibraryModel::developState(const std::optional<editing::State>& state,
                                       const spk::desktop::Catalog* catalog) {
    QVariantMap map;
    if (state.has_value()) {
        map.insert(QStringLiteral("state"), QStringLiteral("developed"));
        map.insert(QStringLiteral("film"), stockLabel(catalog, state->settings.film_stock));
        map.insert(QStringLiteral("paper"), stockLabel(catalog, state->settings.print_stock));
    } else {
        map.insert(QStringLiteral("state"), QStringLiteral("notDeveloped"));
        map.insert(QStringLiteral("film"), QString());
        map.insert(QStringLiteral("paper"), QString());
    }
    return map;
}

// --- lifecycle -----------------------------------------------------------------------

bool LibraryModel::spawnHelper() {
    if (host_.isRunning()) return true;
    const auto failure = host_.start(options_);
    return !failure.has_value();
}

bool LibraryModel::start() {
    const BusyGuard guard(this);
    setError(QString());
    // A missing or refusing helper is start()'s failure and its reason is
    // the error the UI shows; the empty library state still stands.
    if (!spawnHelper()) {
        setError(host_.lastError());
        return false;
    }
    registry_ = store_.load();
    refreshSources();
    const auto* effective = CatalogSourcesStore::effectiveSource(registry_);
    if (effective == nullptr) return true; // helper up, no source yet: the empty state
    setSelectedCollection(QString());
    if (!openSource(effective->path)) return false;
    refresh();
    return true;
}

void LibraryModel::shutdown() {
    const BusyGuard guard(this); // the bounded waitForFinished is blocking work
    host_.shutdown();
}

// --- sources ---------------------------------------------------------------------------

void LibraryModel::refreshSources() {
    sources_.clear();
    for (const auto& source : registry_.sources)
        sources_ << sourceRow(source, registry_.selected);
    emit sourcesChanged();
}

bool LibraryModel::openSource(const QString& path) {
    if (!library_.isEmpty()) {
        const auto closed = host_.libraryClose(library_);
        if (!closed.ok) setError(closed.errorMessage);
        library_.clear();
    }
    const auto opened = host_.libraryOpen(path);
    if (!opened.ok) {
        setError(opened.errorMessage.isEmpty() ? QStringLiteral("cannot open the source")
                                               : opened.errorMessage);
        return false;
    }
    library_ = opened.value.library;
    return true;
}

bool LibraryModel::createCatalog(const QString& name, const QString& folder) {
    const BusyGuard guard(this);
    const auto dir = folder.isEmpty() ? storeRoot_ : folder;
    if (name.isEmpty()) {
        setError(QStringLiteral("a catalog needs a name"));
        return false;
    }
    if (!QDir().mkpath(dir)) {
        setError(QStringLiteral("cannot create the folder for the catalog"));
        return false;
    }
    const auto path = QDir(dir).filePath(name + QStringLiteral(".spektralab"));
    if (QFileInfo::exists(path)) {
        setError(QStringLiteral("a library already exists at ") + path);
        return false;
    }
    const auto created = host_.libraryCreate(path, QStringLiteral("catalog"), name);
    if (!created.ok) {
        setError(created.errorMessage);
        return false;
    }
    // The helper appends .spkcatalog (a directory holding Library.spkdb) to
    // the requested path; the source, the registry and the open call all use
    // the helper's real path, never the requested one.
    const auto realPath =
        created.value.path.isEmpty() ? path : created.value.path;
    CatalogSourcesStore::Source source;
    source.id = QUuid::createUuid();
    source.kind = CatalogSourcesStore::Kind::Catalog;
    source.name = created.value.name.isEmpty() ? name : created.value.name;
    source.path = realPath;
    source.addedAt = QDateTime::currentDateTimeUtc();
    registry_.sources.push_back(source);
    registry_.selected = source.id;
    if (!store_.save(registry_)) {
        setError(QStringLiteral("cannot write the sources registry"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(realPath)) return false;
    refresh();
    return true;
}

bool LibraryModel::createSession(const QString& name, const QString& folder) {
    const BusyGuard guard(this);
    const auto dir = folder.isEmpty() ? storeRoot_ : folder;
    if (name.isEmpty()) {
        setError(QStringLiteral("a session needs a name"));
        return false;
    }
    const auto path = QDir(dir).filePath(name);
    if (!QDir().mkpath(path)) {
        setError(QStringLiteral("cannot create the session folder"));
        return false;
    }
    const auto created = host_.libraryCreate(path, QStringLiteral("session"), name);
    if (!created.ok) {
        setError(created.errorMessage);
        return false;
    }
    const auto realPath =
        created.value.path.isEmpty() ? path : created.value.path;
    CatalogSourcesStore::Source source;
    source.id = QUuid::createUuid();
    source.kind = CatalogSourcesStore::Kind::Session;
    source.name = created.value.name.isEmpty() ? name : created.value.name;
    source.path = realPath;
    source.addedAt = QDateTime::currentDateTimeUtc();
    registry_.sources.push_back(source);
    registry_.selected = source.id;
    if (!store_.save(registry_)) {
        setError(QStringLiteral("cannot write the sources registry"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(realPath)) return false;
    refresh();
    return true;
}

bool LibraryModel::selectSource(const QString& id) {
    const BusyGuard guard(this);
    const auto found = std::find_if(registry_.sources.begin(), registry_.sources.end(),
                                    [&](const CatalogSourcesStore::Source& source) {
                                        return uuidString(source.id) == id;
                                    });
    if (found == registry_.sources.end()) {
        setError(QStringLiteral("no such source in the registry"));
        return false;
    }
    registry_.selected = found->id;
    if (!store_.save(registry_)) {
        setError(QStringLiteral("cannot write the sources registry"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(found->path)) return false;
    refresh();
    return true;
}

bool LibraryModel::addFolder(const QString& path) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    // folders.add IS a sync: its first walk already adds the folder's files
    // by reference, so the grid and the facets follow like after synchronize.
    const auto added = host_.foldersAdd(library_, path, true);
    if (!added.ok) {
        setError(added.errorMessage);
        return false;
    }
    refresh();
    refreshWatchedFolders();
    return true;
}

bool LibraryModel::buildExistingIndex(QSet<QString>& paths, QSet<QString>& namesAndSizes) {
    // The Mac's existingFiles(in:): every file the library holds, trashed
    // included (the helper's own duplicate check counts a trashed file as
    // there), full rows for file_size, paged.
    int offset = 0;
    for (;;) {
        QVariantMap query;
        query.insert(QStringLiteral("full"), true);
        query.insert(QStringLiteral("include_trashed"), true);
        query.insert(QStringLiteral("sort"), QStringLiteral("added"));
        query.insert(QStringLiteral("limit"), 5000);
        query.insert(QStringLiteral("offset"), offset);
        const auto page = host_.imagesList(library_, query);
        if (!page.ok) {
            setError(page.errorMessage);
            return false;
        }
        for (const auto& image : page.value.images) {
            paths.insert(importPathKey(image.path()));
            const QVariant size = image.map.value(QLatin1String("file_size"));
            if (size.isValid()) {
                bool ok = false;
                const qlonglong bytes = size.toLongLong(&ok);
                if (ok && bytes >= 0)
                    namesAndSizes.insert(importNameSizeKey(image.fileName(), bytes));
            }
        }
        if (page.value.images.empty()) break;
        offset += int(page.value.images.size());
        if (page.value.total >= 0 && offset >= page.value.total) break;
    }
    return true;
}

void LibraryModel::setProgress(const QString& phase, int done, int total) {
    QVariantMap frame;
    frame.insert(QStringLiteral("phase"), phase);
    frame.insert(QStringLiteral("done"), done);
    frame.insert(QStringLiteral("total"), total);
    if (progress_ == frame) return;
    progress_ = frame;
    emit progressChanged();
}

QVariantMap LibraryModel::importFiles(const QVariantList& items, const QString& mode) {
    const BusyGuard guard(this);
    QVariantMap out;
    out.insert(QStringLiteral("added"), 0);
    out.insert(QStringLiteral("existing"), 0);
    out.insert(QStringLiteral("duplicates"), 0);
    out.insert(QStringLiteral("failed"), 0);
    out.insert(QStringLiteral("chunks"), 0);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        out.insert(QStringLiteral("error"), lastError_);
        return out;
    }
    // 1. Duplicate exclusion, before anything is sent (the Mac's Exclude
    //    Duplicates): an exact path the library holds, or the same name and
    //    size at any path.
    QSet<QString> knownPaths;
    QSet<QString> knownNameSizes;
    if (!buildExistingIndex(knownPaths, knownNameSizes)) {
        out.insert(QStringLiteral("error"), lastError_);
        return out;
    }
    QVariantList planned;
    int duplicates = 0;
    for (const QVariant& item : items) {
        const QString path = itemPath(item);
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (importDuplicate(path, info.fileName(), info.size(), knownPaths, knownNameSizes)) {
            ++duplicates;
            continue;
        }
        planned << item;
    }
    out.insert(QStringLiteral("duplicates"), duplicates);
    if (planned.isEmpty()) return out; // an honest no-op: nothing added, no chunk, no album

    // 2. The run, in chunks (the Mac MacImportSession::run): the first chunk
    //    that adds anything makes the import's one Recent Imports album; the
    //    others go INTO it (`collection`) with recent_import false.
    const QString effectiveMode = mode.isEmpty() ? QStringLiteral("reference") : mode;
    const int total = int(planned.size());
    QString album;
    int done = 0;
    int added = 0;
    int existing = 0;
    int failed = 0;
    int chunks = 0;
    setProgress(QStringLiteral("adding"), 0, total);
    for (int start = 0; start < total; start += importChunkSize_) {
        QVariantList chunk;
        chunk.reserve(importChunkSize_);
        for (int i = start; i < qMin(start + importChunkSize_, total); ++i) chunk << planned.at(i);
        const auto result =
            host_.imagesAdd(library_, chunk, effectiveMode, album,
                            /*recentImportAt*/ QString(),
                            /*recentImport*/ album.isEmpty());
        if (!result.ok) {
            setError(result.errorMessage);
            out.insert(QStringLiteral("error"), result.errorMessage);
            out.insert(QStringLiteral("added"), added);
            out.insert(QStringLiteral("existing"), existing);
            out.insert(QStringLiteral("failed"), failed);
            out.insert(QStringLiteral("chunks"), chunks);
            out.insert(QStringLiteral("album"), album);
            return out;
        }
        ++chunks;
        added += result.value.added > 0 ? result.value.added : 0;
        for (const auto& row : result.value.rows) {
            if (!row.error.isEmpty()) ++failed;
            if (row.existing) ++existing;
        }
        if (album.isEmpty()) album = result.value.recentImport;
        done += int(chunk.size());
        setProgress(QStringLiteral("adding"), done, total);
    }
    setProgress(QStringLiteral("finishing"), done, total);
    // 3. The grid follows the import (the Mac's showImport re-lists too).
    refresh();
    out.insert(QStringLiteral("added"), added);
    out.insert(QStringLiteral("existing"), existing);
    out.insert(QStringLiteral("failed"), failed);
    out.insert(QStringLiteral("chunks"), chunks);
    out.insert(QStringLiteral("album"), album);
    return out;
}

QVariantMap LibraryModel::synchronize() {
    const BusyGuard guard(this);
    QVariantMap out;
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        out.insert(QStringLiteral("error"), lastError_);
        return out;
    }
    // The classification is the helper's (folders.sync): new files added by
    // reference, a move (same name and size as exactly one file that went)
    // keeps its row, gone files hidden with their rows kept, a folder that is
    // not there offline and untouched.
    const auto report = host_.foldersSync(library_);
    if (!report.ok) {
        setError(report.errorMessage);
        out.insert(QStringLiteral("error"), report.errorMessage);
        return out;
    }
    out.insert(QStringLiteral("added"), report.value.value(QLatin1String("added")).toInt());
    out.insert(QStringLiteral("moved"), report.value.value(QLatin1String("moved")).toInt());
    out.insert(QStringLiteral("removed"), report.value.value(QLatin1String("removed")).toInt());
    out.insert(QStringLiteral("restored"), report.value.value(QLatin1String("restored")).toInt());
    bool offline = false;
    const auto folders = report.value.value(QLatin1String("folders")).toList();
    for (const QVariant& folder : folders)
        if (folder.toMap().value(QLatin1String("offline")).toBool()) offline = true;
    const auto session = report.value.value(QLatin1String("session"));
    if (session.isValid() && session.toMap().value(QLatin1String("offline")).toBool()) offline = true;
    out.insert(QStringLiteral("offline"), offline);
    refresh();
    refreshWatchedFolders();
    return out;
}

bool LibraryModel::stopWatching(const QString& folder) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    // The uuid goes on the wire; a path is accepted because that is what a
    // user means — resolve it through the watched list.
    QString id = folder;
    if (id.size() != 36) {
        const auto watched = host_.foldersList(library_);
        if (watched.ok) {
            const QString wanted = QDir::fromNativeSeparators(QDir::cleanPath(folder));
            for (const auto& row : watched.value) {
                if (QDir::fromNativeSeparators(
                        QDir::cleanPath(row.value(QLatin1String("path")).toString()))
                        .compare(wanted, Qt::CaseInsensitive) == 0) {
                    id = row.value(QLatin1String("folder")).toString();
                    break;
                }
            }
        }
    }
    const auto removed = host_.foldersRemove(library_, id);
    if (!removed.ok) {
        setError(removed.errorMessage);
        return false;
    }
    // Its images stay: a re-list, not a removal.
    buildTree();
    refreshWatchedFolders();
    return true;
}

QImage LibraryModel::getThumbnail(const QString& path, int maxPixel) {
    if (path.isEmpty()) return {};
    const QImage image = thumbnails_.get(path, maxPixel);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].row.path() != path) continue;
        if (rows_[i].thumbnail.size() == image.size() && !image.isNull() &&
            rows_[i].thumbnail.cacheKey() == image.cacheKey())
            continue;
        rows_[i].thumbnail = image;
        const QModelIndex changed = createIndex(int(i), 0);
        emit dataChanged(changed, changed, {ThumbnailRole});
        break;
    }
    return image;
}

void LibraryModel::refreshWatchedFolders() {
    watchedFolders_.clear();
    if (!library_.isEmpty()) {
        const auto watched = host_.foldersList(library_);
        if (!watched.ok) {
            setError(watched.errorMessage);
        } else {
            for (const auto& row : watched.value) {
                QVariantMap map = row;
                // The tree's id for this folder is its uuid.
                map.insert(QStringLiteral("id"), row.value(QLatin1String("folder")).toString());
                watchedFolders_ << map;
            }
        }
    }
    emit watchedFoldersChanged();
}

// --- collections -------------------------------------------------------------------------

bool LibraryModel::createAlbum(const QString& name, const QString& parent) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    const auto created = host_.collectionCreate(library_, QStringLiteral("album"), name, parent);
    if (!created.ok) {
        setError(created.errorMessage);
        return false;
    }
    buildTree();
    return true;
}

bool LibraryModel::addToCollection(const QString& collection, const QStringList& images) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    const auto added = host_.collectionAdd(library_, collection, images);
    if (!added.ok) {
        setError(added.errorMessage);
        return false;
    }
    buildTree();
    return true;
}

void LibraryModel::selectCollection(const QString& id) {
    const BusyGuard guard(this);
    // Through the setter so selectedCollectionId's NOTIFY (treeChanged) rides
    // with the id change — the review fix: the bare member write left QML
    // bindings stale until the next buildTree.
    setSelectedCollection(id);
    fetchPage(0, true);
    refreshFacets();
}

// --- rows -----------------------------------------------------------------------------------

QVariantMap LibraryModel::listQueryFor(int offset) const {
    return listQuery(selectedCollection_, sort_, descending_,
                     filterMap(textFilter_, ratingFilter_, colorTagFilter_, kindFilter_), pageSize_,
                     offset);
}

bool LibraryModel::fetchPage(int offset, bool reset) {
    if (library_.isEmpty()) {
        beginResetModel();
        rows_.clear();
        total_ = 0;
        endResetModel();
        emit rowsChanged();
        return true;
    }
    auto query = listQueryFor(offset);
    query.insert(QStringLiteral("check_files"), true);
    const auto page = host_.imagesList(library_, query);
    if (!page.ok) {
        setError(page.errorMessage);
        return false;
    }
    std::vector<Row> fetched;
    fetched.reserve(page.value.images.size());
    for (auto& image : page.value.images) {
        Row row;
        row.row = std::move(image);
        row.develop = developFor(row.row);
        fetched.push_back(std::move(row));
    }
    if (reset) {
        beginResetModel();
        rows_ = std::move(fetched);
        total_ = page.value.total;
        endResetModel();
    } else {
        if (rows_.empty()) {
            beginResetModel();
            rows_ = std::move(fetched);
            total_ = page.value.total;
            endResetModel();
        } else {
            beginInsertRows({}, int(rows_.size()), int(rows_.size() + fetched.size()) - 1);
            for (auto& row : fetched) rows_.push_back(std::move(row));
            endInsertRows();
        }
    }
    emit rowsChanged();
    return true;
}

void LibraryModel::loadMore() {
    const BusyGuard guard(this);
    if (library_.isEmpty() || rows_.size() >= total_) return;
    fetchPage(int(rows_.size()), false);
}

void LibraryModel::refresh() {
    const BusyGuard guard(this); // nests: start and the source switches call this too
    buildTree();
    fetchPage(0, true);
    refreshFacets();
}

void LibraryModel::buildTree() {
    tree_.clear();
    if (!library_.isEmpty()) {
        const auto collections = host_.collectionsList(library_);
        if (collections.ok) {
            tree_ = buildTreeRows(collections.value);
            // The default selection: All Images, else the first fixed row.
            // These writes stay direct — the closing treeChanged below covers
            // them, and the setter would only double-notify.
            const auto stillThere = std::any_of(tree_.cbegin(), tree_.cend(),
                                                [&](const QVariant& value) {
                                                    return value.toMap().value(
                                                               QStringLiteral("id"))
                                                            .toString() ==
                                                        selectedCollection_;
                                                });
            if (selectedCollection_.isEmpty() || !stillThere) {
                selectedCollection_.clear();
                for (const auto& value : tree_) {
                    const auto map = value.toMap();
                    if (map.value(QStringLiteral("section")).toString() == QStringLiteral("fixed")) {
                        if (selectedCollection_.isEmpty() ||
                            map.value(QStringLiteral("kind")).toString() ==
                                QStringLiteral("all_images"))
                            selectedCollection_ =
                                map.value(QStringLiteral("id")).toString();
                    }
                }
            }
        } else {
            setError(collections.errorMessage);
        }
    }
    emit treeChanged();
}

void LibraryModel::refreshFacets() {
    facets_.clear();
    if (!library_.isEmpty()) {
        QVariantMap scope;
        if (!selectedCollection_.isEmpty())
            scope.insert(QStringLiteral("collection"), selectedCollection_);
        const auto filter =
            filterMap(textFilter_, ratingFilter_, colorTagFilter_, kindFilter_);
        if (!filter.isEmpty()) scope.insert(QStringLiteral("filter"), filter);
        const auto counted = host_.imagesFacets(library_, scope);
        if (counted.ok)
            facets_ = counted.value;
        else
            setError(counted.errorMessage);
    }
    emit facetsChanged();
}

QVariantMap LibraryModel::developFor(const ImageRow& row) {
    const QString path = row.path();
    if (path.isEmpty()) return developState(std::nullopt, nullptr);
    const spk::desktop::Catalog* catalog =
        catalog_provider_ ? catalog_provider_() : nullptr;
    static const spk::desktop::Catalog empty;
    const auto loaded = edit_store_.load(QUrl::fromLocalFile(path),
                                         catalog ? *catalog : empty);
    return developState(loaded.state, catalog);
}

bool LibraryModel::setRating(const QString& image, int rating) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    const auto reply = host_.metaSet(library_, {}, MetaValue::of(rating), MetaValue::omit(),
                                     QStringList{image});
    if (!reply.ok) {
        setError(reply.errorMessage);
        return false;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].row.uuid() != image) continue;
        rows_[i].row.map.insert(QStringLiteral("rating"), rating);
        const QModelIndex changed = createIndex(int(i), 0);
        emit dataChanged(changed, changed, {RatingRole});
        break;
    }
    refreshFacets();
    return true;
}

bool LibraryModel::setColorTag(const QString& image, int colorTag) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    const auto reply = host_.metaSet(library_, {}, MetaValue::omit(), MetaValue::of(colorTag),
                                     QStringList{image});
    if (!reply.ok) {
        setError(reply.errorMessage);
        return false;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].row.uuid() != image) continue;
        rows_[i].row.map.insert(QStringLiteral("color_tag"), colorTag);
        const QModelIndex changed = createIndex(int(i), 0);
        emit dataChanged(changed, changed, {ColorTagRole});
        break;
    }
    refreshFacets();
    return true;
}

void LibraryModel::openInDevelop(const QString& path) {
    if (path.isEmpty()) return;
    emit openRequested(QUrl::fromLocalFile(path));
}

} // namespace spk::desktop::library
