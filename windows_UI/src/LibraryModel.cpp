#include "LibraryModel.hpp"
#include "LightTableModel.hpp"
#include "Localization.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QUuid>

namespace spk::desktop::library {


namespace {

// The localization seam (task 6.1): user-facing errors resolve through the
// core at set time; the literals are the English keys.
QString L(const QString& key) { return spk::desktop::Localization::instance().L(key); }

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

/// A QVariantList of ids as a QStringList (the clipboard group ids ride
/// QML boundaries as variants).
QStringList stringListOf(const QVariantList& values) {
    QStringList out;
    out.reserve(int(values.size()));
    for (const QVariant& value : values) out << value.toString();
    return out;
}

/// The remembered clipboard group ticks — the ONE storage behind both
/// clipboardGroups() and setClipboardGroups() (review fix: they used to hold
/// two unrelated statics, so the setter's ticks never reached the getter or
/// the copy). Process-wide BY DECISION: a settings-page persistence tier
/// (and where it lives) is that page's business, and no library model may
/// write the user's real config from a test run. UI-thread only (the
/// property's own thread). The first read seeds "everything offered".
QStringList& clipboardGroupStorage() {
    static QStringList remembered;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        remembered = clip::groupIds(clip::defaultSelection());
    }
    return remembered;
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
    // The worker seam (task 4.4): exactly one worker — the thumbnail store
    // keeps its single-owner-thread contract with this thread, and the
    // import's chunk loop serialises on it too (thumbnail decodes asked
    // mid-import simply queue behind it).
    workerPool_.setMaxThreadCount(1);
    spawnHelper();
}

LibraryModel::~LibraryModel() {
    // No job may outlive the state it touches. The import host (if one was
    // ever made) lives on the worker thread and is destroyed there: the
    // delete rides one final job behind anything still running.
    if (importHost_ != nullptr) {
        workerPool_.start([this] {
            delete importHost_;
            importHost_ = nullptr;
        });
    }
    workerPool_.waitForDone();
    host_.shutdown();
}

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
    case SelectedRole: return selected_.contains(row.row.uuid());
    case HasThumbnailRole: return !row.thumbnail.isNull();
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
            {EditRevisionRole, "editRevision"},
            {SelectedRole, "selected"},
            {HasThumbnailRole, "hasThumbnail"}};
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

// --- the grid selection and the settings clipboard ----------------------------------

int LibraryModel::selectedCount() const { return int(selected_.size()); }

const QString& LibraryModel::clipboardSourceName() const {
    static const QString empty;
    return clipboard_.has_value() ? clipboard_->sourceName : empty;
}

QVariantList LibraryModel::clipboardGroups() const {
    // The remembered ticks, the Mac's clipboard.groups contract — kept in
    // the process, BY DECISION (see clipboardGroupStorage()). The getter
    // returns exactly what the setter stored: an empty tick list is stored
    // and read back empty — there is no "everything offered" fallback. The
    // copy sheet's 复制 button is disabled at 0 ticks, and the storage's
    // seeded full list only stands until the first setter call.
    const QStringList& remembered = clipboardGroupStorage();
    QVariantList out;
    for (const QString& id : remembered) out << id;
    return out;
}

void LibraryModel::setClipboardGroups(const QVariantList& ids) {
    QStringList parsed;
    for (const QVariant& id : ids) parsed << id.toString();
    // Only the offered groups' ids are kept, in the sheet's order.
    QStringList kept;
    for (const auto& group : clip::offeredGroups()) {
        const QString id = clip::groupId(group);
        if (parsed.contains(id)) kept << id;
    }
    QStringList& remembered = clipboardGroupStorage();
    if (kept == remembered) return;
    remembered = kept;
    emit clipboardGroupsChanged();
}

int LibraryModel::rowIndexForUuid(const QString& uuid) const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].row.uuid() == uuid) return int(i);
    return -1;
}

int LibraryModel::rowIndexForPath(const QString& path) const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].row.path() == path) return int(i);
    return -1;
}

void LibraryModel::notifySelectionChanged() {
    if (!rows_.empty()) {
        const QModelIndex first = createIndex(0, 0);
        const QModelIndex last = createIndex(int(rows_.size()) - 1, 0);
        // SelectedRole rides with every repaint: the grid rebinds the
        // outline on the whole visible window anyway.
        emit dataChanged(first, last, {SelectedRole});
    }
    emit selectionChanged();
}

void LibraryModel::selectOnly(int row) {
    if (row < 0 || row >= int(rows_.size())) return;
    const bool changed = selected_.size() != 1 || !selected_.contains(rows_[std::size_t(row)].row.uuid());
    selectionAnchor_ = row;
    if (!changed) return;
    selected_.clear();
    selected_.insert(rows_[std::size_t(row)].row.uuid());
    notifySelectionChanged();
}

void LibraryModel::toggleSelected(int row) {
    if (row < 0 || row >= int(rows_.size())) return;
    const QString uuid = rows_[std::size_t(row)].row.uuid();
    if (selected_.remove(uuid)) {
        const QModelIndex changed = createIndex(row, 0);
        emit dataChanged(changed, changed, {SelectedRole});
        emit selectionChanged();
        return;
    }
    selected_.insert(uuid);
    const QModelIndex changed = createIndex(row, 0);
    emit dataChanged(changed, changed, {SelectedRole});
    emit selectionChanged();
}

void LibraryModel::selectRange(int row) {
    if (row < 0 || row >= int(rows_.size())) return selectOnly(row);
    if (selectionAnchor_ < 0 || selectionAnchor_ >= int(rows_.size())) return selectOnly(row);
    const int from = qMin(selectionAnchor_, row);
    const int to = qMax(selectionAnchor_, row);
    selected_.clear();
    for (int i = from; i <= to; ++i) selected_.insert(rows_[std::size_t(i)].row.uuid());
    notifySelectionChanged();
}

void LibraryModel::clearSelection() {
    if (selected_.isEmpty()) return;
    selected_.clear();
    notifySelectionChanged();
}

void LibraryModel::selectAllRows() {
    if (rows_.empty()) return;
    selected_.clear();
    for (const Row& row : rows_) selected_.insert(row.row.uuid());
    notifySelectionChanged();
}

QStringList LibraryModel::selectedUuids() const {
    // Row order: the library's order is the selection's meaning.
    QStringList out;
    for (const Row& row : rows_)
        if (selected_.contains(row.row.uuid())) out << row.row.uuid();
    return out;
}

void LibraryModel::pruneSelection() {
    QSet<QString> alive;
    for (const Row& row : rows_)
        if (selected_.contains(row.row.uuid())) alive.insert(row.row.uuid());
    if (alive.size() == selected_.size()) return;
    selected_ = std::move(alive);
    emit selectionChanged();
}

QVariantMap LibraryModel::rowInfo(const QString& image) const {
    const int index = rowIndexForUuid(image);
    if (index < 0) return {};
    const Row& row = rows_[std::size_t(index)];
    QVariantMap out;
    out.insert(QStringLiteral("uuid"), row.row.uuid());
    out.insert(QStringLiteral("path"), row.row.path());
    out.insert(QStringLiteral("fileName"), row.row.fileName());
    out.insert(QStringLiteral("kind"), row.row.kind());
    out.insert(QStringLiteral("rating"), row.row.rating());
    out.insert(QStringLiteral("colorTag"), row.row.colorTag());
    out.insert(QStringLiteral("capturedLocal"), row.row.capturedLocal());
    out.insert(QStringLiteral("develop"), row.develop);
    return out;
}

QString LibraryModel::uuidAt(int row) const {
    if (row < 0 || row >= int(rows_.size())) return {};
    return rows_[std::size_t(row)].row.uuid();
}

QStringList LibraryModel::offeredClipboardGroups() {
    return clip::groupIds(clip::defaultSelection());
}

bool LibraryModel::copySettings() {
    const auto uuids = selectedUuids();
    if (uuids.isEmpty()) {
        setError(L("No photos selected to copy"));
        return false;
    }
    const int index = rowIndexForUuid(uuids.first());
    if (index < 0) return false;
    const Row& row = rows_[std::size_t(index)];
    const auto* catalog = catalog_provider_ ? catalog_provider_() : nullptr;
    static const spk::desktop::Catalog emptyCatalog;
    const auto loaded = edit_store_.load(QUrl::fromLocalFile(row.row.path()),
                                         catalog ? *catalog : emptyCatalog);
    clip::Clip taken;
    taken.groups = clip::groupSetFromIds(stringListOf(clipboardGroups()));
    taken.settings = loaded.state.value_or(editing::State{});
    taken.sourceName = row.row.fileName();
    clipboard_ = std::move(taken);
    emit clipboardChanged();
    return true;
}

bool LibraryModel::pasteSettings() {
    if (importBlocked()) return false;
    if (!clipboard_.has_value()) {
        setError(L("Nothing is copied yet"));
        return false;
    }
    const BusyGuard guard(this);
    const auto uuids = selectedUuids();
    if (uuids.isEmpty()) {
        setError(L("No photos selected to paste"));
        return false;
    }
    const auto* catalog = catalog_provider_ ? catalog_provider_() : nullptr;
    static const spk::desktop::Catalog emptyCatalog;
    int written = 0;
    for (const QString& uuid : uuids) {
        const int index = rowIndexForUuid(uuid);
        if (index < 0) continue;
        const Row& row = rows_[std::size_t(index)];
        const QUrl source = QUrl::fromLocalFile(row.row.path());
        const auto loaded = edit_store_.load(source, catalog ? *catalog : emptyCatalog);
        if (loaded.blocked) {
            // The store refuses an unsafe replacement; that photo keeps its
            // record and the reason is reported once at the end.
            setError(loaded.warning.isEmpty() ? L("The settings record cannot be replaced")
                                              : loaded.warning);
            continue;
        }
        editing::State next = clipboard_->appliedTo(loaded.state.value_or(editing::State{}));
        if (loaded.state.has_value() && next == *loaded.state) continue;
        try {
            edit_store_.save(source, next);
        } catch (const std::exception& e) {
            setError(QString::fromUtf8(e.what()));
            continue;
        }
        ++written;
        // The develop badge follows the record it was joined from.
        rows_[std::size_t(index)].develop = developFor(rows_[std::size_t(index)].row);
        const QModelIndex changed = createIndex(index, 0);
        emit dataChanged(changed, changed, {DevelopRole});
    }
    if (written > 0) refreshFacets(); // the edited facet follows the records
    return written > 0;
}

// --- the worker seam (task 4.4; see the header's BLOCKING note) ---------------------

void LibraryModel::postProgressFromWorker(const QString& phase, int done, int total) {
    QMetaObject::invokeMethod(this, [this, phase, done, total] { setProgress(phase, done, total); },
                              Qt::QueuedConnection);
}

QImage LibraryModel::getThumbnail(const QString& path, int maxPixel) {
    if (path.isEmpty() || maxPixel <= 0) return {};
    // The row's tile serves at once, whatever size it was decoded at — the
    // grid asks one size per session, and the delegate scales what it gets
    // (the store never upscales, so a small source decodes small).
    const int index = rowIndexForPath(path);
    if (index >= 0 && !rows_[std::size_t(index)].thumbnail.isNull())
        return rows_[std::size_t(index)].thumbnail;
    const QString key = path + QLatin1Char('@') + QString::number(maxPixel);
    if (pendingThumbnails_.contains(key)) return {};
    pendingThumbnails_.insert(key);
    // The pool's one thread is the store's owner; the picture returns as a
    // dataChanged(ThumbnailRole) on the UI thread.
    workerPool_.start([this, path, maxPixel] {
        const QImage image = thumbnails_.get(path, maxPixel);
        QMetaObject::invokeMethod(this,
                                  [this, path, maxPixel, image] {
                                      thumbnailDecoded(path, maxPixel, image);
                                  },
                                  Qt::QueuedConnection);
    });
    return {};
}

void LibraryModel::thumbnailDecoded(const QString& path, int maxPixel, const QImage& image) {
    pendingThumbnails_.remove(path + QLatin1Char('@') + QString::number(maxPixel));
    const int index = rowIndexForPath(path);
    if (index < 0) return; // the row scrolled off a re-list; the cache keeps the tile
    Row& row = rows_[std::size_t(index)];
    if (!image.isNull() && row.thumbnail.cacheKey() == image.cacheKey()) return;
    row.thumbnail = image;
    const QModelIndex changed = createIndex(index, 0);
    emit dataChanged(changed, changed, {ThumbnailRole});
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

bool LibraryModel::importBlocked() {
    // The public wire calls' shared refusal while a worker import holds
    // the helper pipe (see the header's BLOCKING note). True = refused,
    // with the reason in lastError.
    if (!importActive_) return false;
    setError(L("An import is running; wait for it to finish and try again"));
    return true;
}

bool LibraryModel::start() {
    if (importBlocked()) return false;
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
    if (importBlocked()) return;
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
    sourcePath_ = path;
    if (!library_.isEmpty()) {
        const auto closed = host_.libraryClose(library_);
        if (!closed.ok) setError(closed.errorMessage);
        library_.clear();
    }
    const auto opened = host_.libraryOpen(path);
    if (!opened.ok) {
        setError(opened.errorMessage.isEmpty() ? L("The source cannot be opened")
                                               : opened.errorMessage);
        return false;
    }
    library_ = opened.value.library;
    return true;
}

bool LibraryModel::createCatalog(const QString& name, const QString& folder) {
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    const auto dir = folder.isEmpty() ? storeRoot_ : folder;
    if (name.isEmpty()) {
        setError(L("A catalog needs a name"));
        return false;
    }
    if (!QDir().mkpath(dir)) {
        setError(L("The catalog folder cannot be created"));
        return false;
    }
    const auto path = QDir(dir).filePath(name + QStringLiteral(".spektralab"));
    if (QFileInfo::exists(path)) {
        setError(L("A library already exists at this location: ") + path);
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
        setError(L("The sources registry file cannot be written"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(realPath)) return false;
    refresh();
    return true;
}

bool LibraryModel::createSession(const QString& name, const QString& folder) {
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    const auto dir = folder.isEmpty() ? storeRoot_ : folder;
    if (name.isEmpty()) {
        setError(L("A session needs a name"));
        return false;
    }
    const auto path = QDir(dir).filePath(name);
    if (!QDir().mkpath(path)) {
        setError(L("The session folder cannot be created"));
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
        setError(L("The sources registry file cannot be written"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(realPath)) return false;
    refresh();
    return true;
}

bool LibraryModel::selectSource(const QString& id) {
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    const auto found = std::find_if(registry_.sources.begin(), registry_.sources.end(),
                                    [&](const CatalogSourcesStore::Source& source) {
                                        return uuidString(source.id) == id;
                                    });
    if (found == registry_.sources.end()) {
        setError(L("The source is not in the sources registry file"));
        return false;
    }
    registry_.selected = found->id;
    if (!store_.save(registry_)) {
        setError(L("The sources registry file cannot be written"));
        return false;
    }
    refreshSources();
    setSelectedCollection(QString());
    if (!openSource(found->path)) return false;
    refresh();
    return true;
}

bool LibraryModel::addFolder(const QString& path) {
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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

QString LibraryModel::buildExistingIndex(LibraryHost& host, const QString& library,
                                         QSet<QString>& paths, QSet<QString>& namesAndSizes) {
    // Runs on the worker thread inside runImport now (task 4.4) over the
    // import host: no property writes here — the failure reason returns to
    // the caller, which surfaces it through the UI thread
    // (importFiles's post-spin setError). Empty string = success.
    int offset = 0;
    for (;;) {
        QVariantMap query;
        query.insert(QStringLiteral("full"), true);
        query.insert(QStringLiteral("include_trashed"), true);
        query.insert(QStringLiteral("sort"), QStringLiteral("added"));
        query.insert(QStringLiteral("limit"), 5000);
        query.insert(QStringLiteral("offset"), offset);
        const auto page = host.imagesList(library, query);
        if (!page.ok) return page.errorMessage;
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
    return {};
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

int LibraryModel::importDroppedUrls(const QVariantList& urls) {
    // A drop imports by reference into the open source; with no source the
    // grid's empty state already says what to do first.
    if (!importActive_ && selectedSourceId().isEmpty()) {
        setError(L("Create a catalog or session, then import photos"));
        return -1;
    }
    QVariantList paths;
    for (const auto& u : urls) {
        const QString local = QUrl(u.toString()).toLocalFile();
        if (!local.isEmpty() && QFileInfo::exists(local)) paths.append(local);
    }
    if (paths.isEmpty()) {
        setError(L("Nothing droppable in the drag — use image files"));
        return -1;
    }
    const auto out = importFiles(paths, QStringLiteral("reference"));
    return out.value(QStringLiteral("added")).toInt();
}

int LibraryModel::trashImages(const QStringList& uuids) {
    // The helper's trash is a reversible flag on the row (never a file
    // deletion); the default images.list excludes trashed rows, so the
    // grid loses them and the rows survive in the source.
    if (uuids.isEmpty()) return 0;
    auto reply = host_.imagesTrash(library_, uuids, spk::desktop::library::TrashFlag::of(true));
    if (!reply.ok) {
        setError(reply.errorCode.isEmpty() ? L("The library helper is not running")
                                           : reply.errorCode);
        return -1;
    }
    refresh();
    return int(uuids.size());
}

QVariantMap LibraryModel::importFiles(const QVariantList& items, const QString& mode) {
    QVariantMap out;
    out.insert(QStringLiteral("added"), 0);
    out.insert(QStringLiteral("existing"), 0);
    out.insert(QStringLiteral("duplicates"), 0);
    out.insert(QStringLiteral("failed"), 0);
    out.insert(QStringLiteral("chunks"), 0);
    if (importActive_) {
        // Re-entry is refused: the worker owns the helper's pipe for the
        // whole run (see the header's BLOCKING note).
        setError(L("An import is already running"));
        out.insert(QStringLiteral("error"), lastError_);
        return out;
    }
    if (library_.isEmpty()) {
        setError(L("No library is open"));
        out.insert(QStringLiteral("error"), lastError_);
        return out;
    }
    // The whole run moves to the worker pool (task 4.4): duplicate
    // exclusion, the chunk loop, the one Recent Imports album. This frame
    // stays on the UI thread holding the BusyGuard and pumping a bounded
    // event loop — user input excluded, so clicks and keys are not
    // delivered while the run holds the pipe; progress frames and the
    // completion are queued onto this thread and land inside the pump, so
    // QML repaints importProgress live. The synchronous result contract is
    // kept: runImport fills the map before the pump's exit condition.
    const BusyGuard guard(this);
    importActive_ = true;
    QVariantMap result;
    bool done = false;
    const QString libraryPath = sourcePath_;
    const int chunkSize = importChunkSize_;
    workerPool_.start([this, libraryPath, items, mode, chunkSize, &result, &done] {
        result = runImport(libraryPath, items, mode, chunkSize);
        QMetaObject::invokeMethod(this, [&done] { done = true; }, Qt::QueuedConnection);
    });
    // NO DEADLINE VALVE HERE (review fix — the 600 s valve was removed): the
    // valve's expiry returned while the worker lambda still held &result and
    // &done — stack locals of THIS frame — so the worker's landing wrote
    // freed memory; importActive_ also dropped false under a still-running
    // import, letting public wire calls interleave with the worker's helper
    // pipe (exactly what importBlocked() refuses), and the returned map (no
    // error key) read as a successful 0-added import. None of that is worth
    // a deadline: every call the worker makes is itself timeout-bounded
    // (LibraryHost's wire round trips), so the run always converges and the
    // pump has no other exit than done.
    while (!done)
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 30);
    importActive_ = false;
    const QString error = result.value(QLatin1String("error")).toString();
    if (!error.isEmpty())
        setError(error); // the worker's reasons surface through the UI thread
    // 3. The grid follows the import (the Mac's showImport re-lists too).
    if (error.isEmpty()) refresh();
    return result;
}

QVariantMap LibraryModel::runImport(const QString& libraryPath, QVariantList items,
                                    const QString& mode, int chunkSize) {
    // WORKER THREAD (workerPool_'s single thread). Touches only the import
    // host and locals; every observable (progress, error) is posted to the
    // UI thread. `items` and `mode` are by-value copies — the caller's
    // frame stays alive for the pump, but nothing here reads its members.
    //
    // The import's own helper process (see the header note on runImport):
    // a QProcess is waited on from its own thread only, so this job speaks
    // to a host created on this thread. The two processes never overlap —
    // the UI wire is gated off for the whole run.
    if (importHost_ == nullptr) importHost_ = new LibraryHost(nullptr);
    if (!importHost_->isRunning()) {
        const auto failure = importHost_->start(options_);
        if (failure.has_value()) {
            QVariantMap out;
            out.insert(QStringLiteral("added"), 0);
            out.insert(QStringLiteral("existing"), 0);
            out.insert(QStringLiteral("duplicates"), 0);
            out.insert(QStringLiteral("failed"), 0);
            out.insert(QStringLiteral("chunks"), 0);
            out.insert(QStringLiteral("error"), *failure);
            return out;
        }
    }
    QString library;
    if (!importLibrary_.isEmpty()) {
        importHost_->libraryClose(importLibrary_); // the previous run's handle
        importLibrary_.clear();
    }
    if (!libraryPath.isEmpty()) {
        const auto opened = importHost_->libraryOpen(libraryPath);
        if (opened.ok) {
            library = opened.value.library;
            importLibrary_ = library;
        }
    }
    QVariantMap out;
    out.insert(QStringLiteral("added"), 0);
    out.insert(QStringLiteral("existing"), 0);
    out.insert(QStringLiteral("duplicates"), 0);
    out.insert(QStringLiteral("failed"), 0);
    out.insert(QStringLiteral("chunks"), 0);
    if (library.isEmpty()) {
        out.insert(QStringLiteral("error"),
                   importHost_->lastError().isEmpty()
                       ? L("The source cannot be opened for import")
                       : importHost_->lastError());
        return out;
    }
    // 1. Duplicate exclusion, before anything is sent (the Mac's Exclude
    //    Duplicates): an exact path the library holds, or the same name and
    //    size at any path.
    QSet<QString> knownPaths;
    QSet<QString> knownNameSizes;
    const QString indexError = buildExistingIndex(*importHost_, library, knownPaths, knownNameSizes);
    if (!indexError.isEmpty()) {
        out.insert(QStringLiteral("error"), indexError); // surfaced by importFiles
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
    postProgressFromWorker(QStringLiteral("adding"), 0, total);
    for (int start = 0; start < total; start += chunkSize) {
        QVariantList chunk;
        chunk.reserve(chunkSize);
        for (int i = start; i < qMin(start + chunkSize, total); ++i) chunk << planned.at(i);
        const auto result = importHost_->imagesAdd(library, chunk, effectiveMode, album,
                                                   /*recentImportAt*/ QString(),
                                                   /*recentImport*/ album.isEmpty());
        if (!result.ok) {
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
        postProgressFromWorker(QStringLiteral("adding"), done, total);
    }
    postProgressFromWorker(QStringLiteral("finishing"), done, total);
    out.insert(QStringLiteral("added"), added);
    out.insert(QStringLiteral("existing"), existing);
    out.insert(QStringLiteral("failed"), failed);
    out.insert(QStringLiteral("chunks"), chunks);
    out.insert(QStringLiteral("album"), album);
    return out;
}

QVariantMap LibraryModel::synchronize() {
    if (importBlocked()) return { {QStringLiteral("error"), lastError_} };
    const BusyGuard guard(this);
    QVariantMap out;
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
    if (importBlocked()) return;
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
        pruneSelection();
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
            } else if (!fetched.empty()) {
                // An empty page must not announce a -1..-1 insert range
                // (beginInsertRows with first > last is undefined).
                beginInsertRows({}, int(rows_.size()), int(rows_.size() + fetched.size()) - 1);
                for (auto& row : fetched) rows_.push_back(std::move(row));
                endInsertRows();
            }
        }
    // The selection survives a re-list row for row (pruned to rows that
    // still stand — the header's SELECTION note).
    pruneSelection();
    emit rowsChanged();
    return true;
}

void LibraryModel::loadMore() {
    if (importBlocked()) return;
    const BusyGuard guard(this);
    if (library_.isEmpty() || rows_.size() >= total_) return;
    fetchPage(int(rows_.size()), false);
}

void LibraryModel::refresh() {
    if (importBlocked()) return;
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

// The light table's frames (task 6.4): the admission filter and the true-mm
// sizes over this model's own rows. The pixels are the thumbnail store's
// decodes — the same ones the grid shows — so a row the grid has not decoded
// yet carries no size here; the page's next sync (after the decode lands)
// carries it. The film-edge arm prices the canvas with the engine's own
// format table (Carry::filmFormat); the bare-develop arm scales the shown
// crop by the record's film_format_mm. Never a guess: an unsizable frame is
// simply not offered to the table.
QVariantList LibraryModel::lightTableFrames(const QStringList& uuids) {
    namespace lt = spk::desktop::lighttable;
    QVariantList out;
    for (const Row& row : rows_) {
        if (!uuids.isEmpty() && !uuids.contains(row.row.uuid())) continue;
        if (row.row.trashed()) continue;
        const QString path = row.row.path();
        if (path.isEmpty()) continue;
        // the uncut frame's pixels (EXIF applied): the aspect the sizes price
        const QImage original = getThumbnail(path, 256);
        if (original.isNull()) continue;
        const spk::desktop::Catalog* catalog =
            catalog_provider_ ? catalog_provider_() : nullptr;
        static const spk::desktop::Catalog empty;
        const auto loaded = edit_store_.load(QUrl::fromLocalFile(path),
                                             catalog ? *catalog : empty);
        lt::Carry::Spec spec;
        spec.developed = loaded.state.has_value();
        if (spec.developed) {
            spec.filmEdgeActive = loaded.state->settings.overscan_active;
            spec.filmEdgeMode = QString::fromStdString(loaded.state->settings.overscan_mode);
            spec.filmEdgeFormat = QString::fromStdString(loaded.state->settings.overscan_format);
            spec.filmFormatMM = loaded.state->settings.film_format_mm;
            // the shown picture: the crop fraction of the original, then the
            // quarter turns (odd turns swap the axes)
            spec.displayPx = QSizeF(loaded.state->geometry.crop.width() * original.width(),
                                    loaded.state->geometry.crop.height() * original.height());
            if (loaded.state->geometry.quarterTurns % 2 == 1) spec.displayPx.transpose();
        }
        spec.originalPx = QSizeF(original.width(), original.height());
        if (!lt::Carry::takes(spec.developed, spec.filmEdgeActive, spec.pair)) continue;
        const std::optional<QSizeF> mm = lt::Carry::frameSizeMM(spec);
        if (!mm.has_value()) continue;
        QVariantMap frame;
        frame.insert(QStringLiteral("uuid"), row.row.uuid());
        frame.insert(QStringLiteral("path"), path);
        frame.insert(QStringLiteral("mmw"), mm->width());
        frame.insert(QStringLiteral("mmh"), mm->height());
        frame.insert(QStringLiteral("image"), getThumbnail(path, 1024));
        out.append(frame);
    }
    return out;
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
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
    if (importBlocked()) return false;
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(L("No library is open"));
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
