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

} // namespace

LibraryModel::LibraryModel(Deps deps, QObject* parent)
    : QAbstractListModel(parent),
      store_(deps.registryPath),
      edit_store_(deps.editDirectory),
      catalog_provider_(std::move(deps.catalog)) {
    storeRoot_ = deps.helper.storePath;
    if (storeRoot_.isEmpty()) storeRoot_ = CatalogSourcesStore::macLibraryRoot();
    deps.helper.storePath = storeRoot_;
    options_ = deps.helper;
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
    const auto added = host_.foldersAdd(library_, path, true);
    if (!added.ok) {
        setError(added.errorMessage);
        return false;
    }
    buildTree();
    return true;
}

bool LibraryModel::importFiles(const QVariantList& items) {
    const BusyGuard guard(this);
    if (library_.isEmpty()) {
        setError(QStringLiteral("no library is open"));
        return false;
    }
    const auto added = host_.imagesAdd(library_, items);
    if (!added.ok) {
        setError(added.errorMessage);
        return false;
    }
    refresh();
    return true;
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
