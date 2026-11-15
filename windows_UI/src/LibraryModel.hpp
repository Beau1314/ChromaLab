#pragma once

#include "CatalogSourcesStore.hpp"
#include "EditStore.hpp"
#include "LibraryHost.hpp"

#include <QAbstractListModel>
#include <QImage>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <optional>
#include <vector>

// The library window's data model (task 4.2) — the Windows port of the Mac's
// MacLibraryData surface (MacLibraryModels.swift:465-507), grown over the
// LibraryHost wire. One model owns: the helper process (LibraryHost), the
// kept-sources registry (CatalogSourcesStore over sources.json), the grid's
// rows (QAbstractListModel), the sidebar's collection tree, the facets, and
// the develop-join that reads the app's own per-photo EditStore records.
//
// BLOCKING CALLS, BY DECISION: every helper round trip here is synchronous
// (the typed layer blocks on its reply; the model calls it on the UI thread).
// The helper reads a small local SQLite database and adds no decode work, so
// a page fetch, a facets call or a meta.set land in single-digit
// milliseconds — the same reasoning as the Mac's early builds before async
// was worth its queue. The brief mandates this first shape and asks only
// that the decision be recorded: if a real catalog ever makes the grid
// stutter, the seam to move is these same typed calls behind a worker
// queue; the model surface (properties, roles, signals) need not change.
// The calls themselves stay single-flight by construction, so no re-entrancy
// can reorder a page.
//
// NOTIFICATIONS, BY DECISION (review fix): lastError has its own errorChanged
// NOTIFY — emitted only when the reason actually changes (a new failure, or
// start() clearing the previous one) — instead of piggybacking on busyChanged.
// busy is wired around the same blocking round trips with a scoped guard
// (BusyGuard): every public operation flips busy true on entry and false on
// exit, one window per operation even when the public calls nest
// (start -> refresh). Because the calls are synchronous the QML thread never
// re-enters mid-operation, so a re-evaluated binding only ever reads busy
// after the window closes; slots connected directly (any C++ observer, or a
// QML onErrorChanged handler reading busy) do observe busy == true inside the
// window. Task 4.4 can bind a spinner to busy unchanged if the calls ever
// move behind the worker queue.
//
// Seams, on purpose:
//   * openInDevelop does NOT hold the PreviewController. It emits
//     openRequested(path) and main.cpp connects that to the existing
//     openRaw channel (whose addFile already de-duplicates by canonical
//     path). A signal keeps this model engine-free — the tests run without
//     a GPU — and keeps the library able to outlive a failed preview.
//   * The engine catalog (needed to resolve an EditStore record's film and
//     paper ids into names) arrives as a std::function pointer provider,
//     not a pointer to the controller. main.cpp hands
//     `[&controller]{ return &controller.catalog(); }`; the tests hand a
//     stub. Before the engine answers init the provider returns nullptr and
//     the develop join leaves the names empty rather than inventing them.
//   * The model owns its own editing::Store over the same edit directory the
//     PreviewController persists into (main.cpp passes one directory to
//     both), so a develop badge reads exactly what the editor writes.
//   * Filter and sort setters store state and emit queryChanged; they do not
//     auto-refresh. Selection changes (selectCollection/selectSource) DO
//     re-list immediately. The QML layer (task 4.4) wires queryChanged to
//     refresh(); keeping the model dumb makes the wire shape assertable.
//
// Rows carry fileName/kind/rating/colorTag/capturedLocal/exists/trashed/
// variantCount/primaryVariant/editRevision, a develop map (film/paper names
// + state) joined from the EditStore by path, and an empty QImage thumbnail
// role held for task 4.3's ThumbnailStore — the grid shows placeholders
// until that lands. `exists` rides because the base query sets check_files.
namespace spk::desktop::library {

class LibraryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QString selectedSourceId READ selectedSourceId NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap selectedSource READ selectedSource NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList collectionsTree READ collectionsTree NOTIFY treeChanged)
    Q_PROPERTY(QString selectedCollectionId READ selectedCollectionId NOTIFY treeChanged)
    Q_PROPERTY(QVariantMap facets READ facets NOTIFY facetsChanged)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY queryChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY queryChanged)
    Q_PROPERTY(QString textFilter READ textFilter WRITE setTextFilter NOTIFY queryChanged)
    Q_PROPERTY(QVariantList ratingFilter READ ratingFilter WRITE setRatingFilter NOTIFY queryChanged)
    Q_PROPERTY(QVariantList colorTagFilter READ colorTagFilter WRITE setColorTagFilter NOTIFY queryChanged)
    Q_PROPERTY(QVariantList kindFilter READ kindFilter WRITE setKindFilter NOTIFY queryChanged)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY queryChanged)
    Q_PROPERTY(int total READ total NOTIFY rowsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
public:
    /// The engine catalog lookup (nullptr before the engine's init reply, or
    /// in tests). Everything else is an explicit path — nothing here touches
    /// the user's real registry or edit store unless handed them.
    using CatalogProvider = std::function<const spk::desktop::Catalog*()>;
    struct Deps {
        LibraryHost::Options helper; // helperPath (+ storePath; an empty
                                           // storePath defaults to macLibraryRoot())
        QString registryPath;              // sources.json
        QString editDirectory;             // the per-photo edit records directory
        CatalogProvider catalog;
    };

    explicit LibraryModel(Deps deps, QObject* parent = nullptr);
    ~LibraryModel() override;

    // --- QAbstractListModel -----------------------------------------------------
    enum Role {
        UuidRole = Qt::UserRole + 1,
        PathRole,
        FileNameRole,
        KindRole,
        RatingRole,
        ColorTagRole,
        CapturedLocalRole,
        ExistsRole,
        TrashedRole,
        DevelopRole,
        ThumbnailRole,
        VariantCountRole,
        PrimaryVariantRole,
        EditRevisionRole
    };
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // --- properties ---------------------------------------------------------------
    QVariantList sources() const { return sources_; }
    QString selectedSourceId() const;
    QVariantMap selectedSource() const;
    QVariantList collectionsTree() const { return tree_; }
    QString selectedCollectionId() const { return selectedCollection_; }
    QVariantMap facets() const { return facets_; }
    QString sort() const { return sort_; }
    bool descending() const { return descending_; }
    QString textFilter() const { return textFilter_; }
    QVariantList ratingFilter() const { return ratingFilter_; }
    QVariantList colorTagFilter() const { return colorTagFilter_; }
    QVariantList kindFilter() const { return kindFilter_; }
    int pageSize() const { return pageSize_; }
    int total() const { return total_; }
    bool busy() const { return busy_; }
    const QString& lastError() const { return lastError_; }

    void setSort(const QString& sort);
    void setDescending(bool descending);
    void setTextFilter(const QString& text);
    void setRatingFilter(const QVariantList& ratings);
    void setColorTagFilter(const QVariantList& colorTags);
    void setKindFilter(const QVariantList& kinds);
    void setPageSize(int size);

    // --- lifecycle ------------------------------------------------------------------
    /// Spawn the helper, read the registry, open the effective source and list
    /// it. Synchronous (see the header note). False on a helper failure — the
    /// empty library state (no sources, no tree) still stands, so the UI can
    /// show its guidance; lastError says why.
    Q_INVOKABLE bool start();
    Q_INVOKABLE void shutdown();

    // --- sources ---------------------------------------------------------------------
    /// A new `.spektralab` catalog under `folder` (default: the store root),
    /// registered, selected and opened.
    Q_INVOKABLE bool createCatalog(const QString& name, const QString& folder = {});
    /// A new session folder under `folder` (default: the store root) — the
    /// helper creates the `.spksession` inside. Registered, selected, opened.
    Q_INVOKABLE bool createSession(const QString& name, const QString& folder = {});
    /// Select an existing source from the registry: saved, opened, re-listed.
    Q_INVOKABLE bool selectSource(const QString& id);
    /// Watch a folder on the open library (folders.add) and re-list the tree.
    Q_INVOKABLE bool addFolder(const QString& path);
    /// images.add in reference mode, then a full re-list. Items ride through
    /// verbatim: path strings or {path, metadata} maps.
    Q_INVOKABLE bool importFiles(const QVariantList& items);

    // --- collections -------------------------------------------------------------------
    Q_INVOKABLE bool createAlbum(const QString& name, const QString& parent = {});
    Q_INVOKABLE bool addToCollection(const QString& collection, const QStringList& images);
    /// Re-lists the grid and the facets for this collection immediately.
    Q_INVOKABLE void selectCollection(const QString& id);

    // --- rows ----------------------------------------------------------------------------
    /// Fetch the next page (pageSize rows) and append.
    Q_INVOKABLE void loadMore();
    /// Re-list the tree, the first page and the facets from scratch.
    Q_INVOKABLE void refresh();
    /// meta.set through the image uuid (→ first variant); the row updates in
    /// place and the facets follow.
    Q_INVOKABLE bool setRating(const QString& image, int rating);
    Q_INVOKABLE bool setColorTag(const QString& image, int colorTag);
    /// The library row → the existing open channel. Emits openRequested with
    /// the row's local file URL; main.cpp connects it to openRaw.
    Q_INVOKABLE void openInDevelop(const QString& path);

    // --- pure helpers (assertable without a helper process) --------------------------------
    /// The sidebar's flat rows: the helper's collections.list verbatim, the
    /// root excluded, each row tagged fixed/user/folders (CatalogSourcesStore's
    /// treeInput partition) in that order, with id/title/kind/imageCount/
    /// parent/editable/path exposed for QML.
    static QVariantList buildTreeRows(const std::vector<QVariantMap>& collections);
    /// The images.list query for a grid page. The collection key stays off
    /// when none is selected; the filter rides whole when non-empty.
    static QVariantMap listQuery(const QString& collection, const QString& sort, bool descending,
                                 const QVariantMap& filter, int pageSize, int offset);
    /// The helper's filter vocabulary: text, ratings[], color_tags[], kinds[] —
    /// only the ticked keys ride; every tick off is no filter key at all.
    static QVariantMap filterMap(const QString& text, const QVariantList& ratings,
                                 const QVariantList& colorTags, const QVariantList& kinds);
    /// The develop badge joined from the EditStore record: developed with the
    /// film and paper NAMES resolved through the engine catalog, or
    /// notDeveloped. A null catalog (engine not ready) leaves the names empty;
    /// an id the catalog does not know stays empty rather than invented.
    /// (The Mac's inQueue/failed states have no Windows counterpart yet — a
    /// record exists or it does not.)
    static QVariantMap developState(const std::optional<editing::State>& state,
                                    const spk::desktop::Catalog* catalog);
    static QString stockLabel(const spk::desktop::Catalog* catalog, const std::string& id);

signals:
    void sourcesChanged();
    void treeChanged();
    void facetsChanged();
    void queryChanged();
    void rowsChanged();
    void busyChanged();
    /// lastError's own NOTIFY: fired whenever the reported reason actually
    /// changes, so QML observes error state after startup without depending
    /// on a busy transition.
    void errorChanged();
    /// openInDevelop's hand-off to the existing openRaw channel.
    void openRequested(const QUrl& path);

private:
    struct Row {
        ImageRow row;
        QVariantMap develop;
        QImage thumbnail; // task 4.3 fills this; empty until then
    };

    /// Scoped guard over the blocking helper round trips: the outermost
    /// guard flips busy and notifies, the last one off flips it back.
    /// Copy-protected; public calls nest through busyDepth_, so an operation
    /// is one busy window, not one per wire call.
    struct BusyGuard {
        explicit BusyGuard(LibraryModel* model);
        ~BusyGuard();
        BusyGuard(const BusyGuard&) = delete;
        BusyGuard& operator=(const BusyGuard&) = delete;
        LibraryModel* model_;
    };

    bool spawnHelper();
    void refreshSources();
    bool openSource(const QString& path);
    void buildTree();
    bool fetchPage(int offset, bool reset);
    void refreshFacets();
    QVariantMap developFor(const ImageRow& row);
    QVariantMap listQueryFor(int offset) const;
    /// The only writer of lastError_; notifies errorChanged when the reason
    /// actually changes (so re-reporting the same failure is silent).
    void setError(const QString& message);
    /// The only writer of selectedCollection_ outside buildTree's default
    /// selection (whose closing treeChanged covers it); notifies treeChanged
    /// so QML bindings on selectedCollectionId cannot go stale.
    void setSelectedCollection(const QString& id);

    LibraryHost host_;
    LibraryHost::Options options_;
    CatalogSourcesStore store_;
    editing::Store edit_store_;
    CatalogProvider catalog_provider_;
    CatalogSourcesStore::Registry registry_;
    QString library_; // the helper handle
    QString storeRoot_;
    QVariantList sources_;
    QVariantList tree_;
    std::vector<Row> rows_;
    QVariantMap facets_;
    QString selectedCollection_;
    QString sort_ = QStringLiteral("captured");
    bool descending_ = false;
    QString textFilter_;
    QVariantList ratingFilter_;
    QVariantList colorTagFilter_;
    QVariantList kindFilter_;
    int pageSize_ = 1000;
    int total_ = 0;
    bool busy_ = false;
    int busyDepth_ = 0; // BusyGuard nesting depth (UI thread only)
    QString lastError_;
};

} // namespace spk::desktop::library
