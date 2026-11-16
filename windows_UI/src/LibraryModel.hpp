#pragma once

#include "CatalogSourcesStore.hpp"
#include "EditStore.hpp"
#include "LibraryHost.hpp"
#include "ThumbnailStore.hpp"

#include <QAbstractListModel>
#include <QImage>
#include <QSet>
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
// + state) joined from the EditStore by path, and a QImage thumbnail role the
// grid asks for per row through getThumbnail() (task 4.4's delegates decide
// when; the store caches, so an ask is cheap and repeatable). `exists` rides
// because the base query sets check_files.
//
// IMPORT (task 4.3), the Mac MacImportSession's run over this model's wire:
// importFiles(items) excludes duplicates first (the Mac's Exclude Duplicates:
// an exact path the library already holds, or the same file name and size at
// any path — the same photograph twice never enters twice), then sends the
// rest in chunks of Deps::importChunkSize (200 in production, the Mac's
// chunkSize). The helper's images.add makes the import's one Recent Imports
// album on the first chunk that adds anything; later chunks ride INTO that
// album (`collection`) with `recent_import: false`, so one import is one
// album however many chunks it takes. The result map carries {added, existing,
// duplicates, failed, chunks, album, error}; importProgress carries
// {phase, done, total} as it runs.
//
// FOLDER SYNC (task 4.3), MAC-LIBRARY-DATA §1's kept-in-step contract, whose
// classification the helper's folders.sync owns: a NEW file is added by
// reference; a MOVED file (a new file whose name and size match exactly one
// file that went) keeps its row — rating, colour tag, albums and edit ride
// with it; a GONE file is hidden, its row kept until the file comes back;
// a folder that is NOT THERE (a disk away) is offline and nothing in it
// changes. synchronize() runs that over every watched folder and returns the
// counts; watchedFolders lists them with their online state; stopWatching()
// leaves the watched list without removing images.
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
    // OBSERVABILITY, BY DECISION: importProgress is terminal-state observable
    // only — the chunk loop runs synchronously on the UI thread inside
    // BusyGuard (the header's BLOCKING note), so QML never repaints
    // mid-import and reads the final {phase, done, total} only after
    // importFiles returns. Live progress lands with the Task 4.4/4.5
    // worker-queue seam (the same seam the blocking note names).
    Q_PROPERTY(QVariantMap importProgress READ importProgress NOTIFY progressChanged)
    Q_PROPERTY(QVariantList watchedFolders READ watchedFolders NOTIFY watchedFoldersChanged)
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
        QString thumbnailDirectory;        // the thumbnail cache; empty = the
                                           // MacLibrary layout's Cache/Thumbnails
        int importChunkSize = 200;         // files per images.add (the Mac's chunkSize)
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
    QVariantMap importProgress() const { return progress_; }
    QVariantList watchedFolders() const { return watchedFolders_; }

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
    /// The import (see the header's IMPORT note): duplicate exclusion, chunks
    /// of importChunkSize through images.add, one Recent Imports album, the
    /// progress property along the way, then a full re-list. `items` are path
    /// strings or {path, metadata} maps; `mode` is the helper's ("reference",
    /// or "copy" into the library's own storage). Returns {added, existing,
    /// duplicates, failed, chunks, album, error} — album is the Recent
    /// Imports album's uuid when anything was added, error the failure reason.
    Q_INVOKABLE QVariantMap importFiles(const QVariantList& items,
                                        const QString& mode = {});
    /// folders.sync over every watched folder, then a full re-list (see the
    /// header's FOLDER SYNC note). Returns {added, moved, removed, restored,
    /// offline}; offline is true when any watched folder was not there.
    Q_INVOKABLE QVariantMap synchronize();
    /// Stop watching a folder (its uuid, or its path as watchedFolders shows
    /// it). Its images stay, as referenced images no sync looks after.
    Q_INVOKABLE bool stopWatching(const QString& folder);
    /// The row's picture for the grid, through the ThumbnailStore: decodes
    /// (or serves from cache), fills that row's thumbnail role and notifies
    /// dataChanged(ThumbnailRole), and returns the image. The grid calls it
    /// per delegate; an empty result leaves the placeholder in charge.
    Q_INVOKABLE QImage getThumbnail(const QString& path, int maxPixel = 256);

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

    // --- the import dedupe vocabulary (pure; the Mac's MacImportExisting) ---
    /// One path's key in the import's existing index: separators normalised,
    /// case folded on Windows (the same file said two ways is one photograph).
    static QString importPathKey(const QString& path);
    /// The name+size compound key: "the same photograph" by content size.
    static QString importNameSizeKey(const QString& fileName, qlonglong size);
    /// The Mac's MacImportExisting.holds: an exact path the library holds, or
    /// the same name AND size at any path. An unknown size (-1) never matches.
    static bool importDuplicate(const QString& path, const QString& fileName, qlonglong size,
                                const QSet<QString>& paths, const QSet<QString>& namesAndSizes);

signals:
    void sourcesChanged();
    void treeChanged();
    void facetsChanged();
    void queryChanged();
    void rowsChanged();
    void busyChanged();
    void progressChanged();
    void watchedFoldersChanged();
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
        QImage thumbnail; // filled by getThumbnail (task 4.3), empty until asked
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
    void refreshWatchedFolders();
    QVariantMap developFor(const ImageRow& row);
    QVariantMap listQueryFor(int offset) const;
    /// The import's existing index: every file the library holds, trashed
    /// ones included, as path keys and name+size keys (the Mac's
    /// existingFiles — full rows, paged).
    bool buildExistingIndex(QSet<QString>& paths, QSet<QString>& namesAndSizes);
    /// The only writer of progress_; notifies progressChanged.
    void setProgress(const QString& phase, int done, int total);
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
    spk::desktop::ThumbnailStore thumbnails_;
    CatalogProvider catalog_provider_;
    CatalogSourcesStore::Registry registry_;
    QString library_; // the helper handle
    QString storeRoot_;
    QVariantList sources_;
    QVariantList tree_;
    std::vector<Row> rows_;
    QVariantMap facets_;
    QVariantList watchedFolders_;
    QString selectedCollection_;
    QString sort_ = QStringLiteral("captured");
    bool descending_ = false;
    QString textFilter_;
    QVariantList ratingFilter_;
    QVariantList colorTagFilter_;
    QVariantList kindFilter_;
    int pageSize_ = 1000;
    int importChunkSize_ = 200;
    int total_ = 0;
    bool busy_ = false;
    int busyDepth_ = 0; // BusyGuard nesting depth (UI thread only)
    QString lastError_;
    QVariantMap progress_;
};

} // namespace spk::desktop::library
