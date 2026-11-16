#pragma once

#include "CatalogSourcesStore.hpp"
#include "EditStore.hpp"
#include "LibraryHost.hpp"
#include "SettingsClipboard.hpp"
#include "ThumbnailStore.hpp"

#include <QAbstractListModel>
#include <QImage>
#include <QSet>
#include <QString>
#include <QThreadPool>
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
// BLOCKING CALLS, REVISED IN TASK 4.4 (was: everything synchronous on the
// UI thread). The helper round trips (page fetch, facets, meta.set, the
// tree) stay synchronous UI-thread calls — the helper reads a small local
// SQLite database and lands in single-digit milliseconds, so the queue that
// would buy nothing stays unbuilt. The two seams that DID hold the UI thread
// hostage moved off it:
//   * THUMBNAILS: decode now runs on a one-thread QThreadPool (the store's
//     single-owner-thread contract lives on — the pool's thread is the
//     owner; see ThumbnailStore.hpp). getThumbnail() returns the row's
//     cached tile at once and otherwise schedules the decode and returns an
//     empty image; the picture arrives through dataChanged(ThumbnailRole)
//     marshalled back onto the UI thread, so the grid never blocks on a
//     scroll.
//   * IMPORT: the chunk loop runs on the same pool. importFiles() keeps its
//     synchronous signature by pumping an event loop
//     (ExcludeUserInputEvents — clicks and keys are not delivered while the
//     import runs) until the worker finishes, so the returned result map is
//     complete AND importProgress is live: each chunk posts a progress
//     frame onto the UI thread, and QML repaints it mid-import. The pump
//     has NO deadline valve, BY REVIEW: every helper call the worker makes
//     is itself timeout-bounded, so the run always converges — and a valve
//     that expired would return while the worker lambda still held
//     references into the returned frame (a dangling write) with
//     importActive_ already false under a live import. Revising the 4.2
//     decision cost this one rule: while importActive_, every other public
//     wire call is refused with lastError set ("an import is running") —
//     the LibraryHost pipe is single-conversation and must not interleave.
// The helper calls themselves stay single-flight by construction, so no
// re-entrancy can reorder a page.
//
// NOTIFICATIONS, BY DECISION (review fix): lastError has its own errorChanged
// NOTIFY — emitted only when the reason actually changes (a new failure, or
// start() clearing the previous one) — instead of piggybacking on busyChanged.
// busy is wired around the blocking work with a scoped guard (BusyGuard):
// every public operation flips busy true on entry and false on exit, one
// window per operation even when the public calls nest (start -> refresh).
// The import holds its guard across the pumped wait, so busy stays true for
// the whole run and a spinner bound to busy reads it live (the QML thread
// keeps spinning the event loop inside the window).
//
// SELECTION, BY DECISION: the grid's selection is a set of image uuids on
// the model (SelectedRole), surviving re-lists row-for-row (pruned to rows
// still present). Click semantics are the library's: a plain click selects
// only that row, Ctrl-click toggles, Shift-click extends from the anchor
// (the last plain-clicked row). Copy/paste of develop settings (the
// SettingsClip semantics of SettingsClipboard.hpp) rides the same
// selection: copySettings() reads the first selected row's stored record,
// pasteSettings() writes the clip's ticked groups into every selected row's
// record — settings only: the target keeps its own crop, decode mode,
// metering (re-done at its next render), As Shot white balance and
// placement. Records are written through this model's own editing::Store
// (the same directory the editor persists into, by main.cpp's hand-off);
// a photo currently open in the editor keeps its editor-side state until
// that state is re-saved — the full shared-browser unification (one model
// of open photos for both pages) is future work, recorded here.
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
// + state) joined from the EditStore by path, the grid's selected flag, and
// a QImage thumbnail role the grid asks for per row through getThumbnail()
// (which now schedules the decode on the worker pool and delivers the
// picture via dataChanged(ThumbnailRole) — see the BLOCKING note). `exists`
// rides because the base query sets check_files.
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
    // OBSERVABILITY, REVISED IN TASK 4.4 (was: terminal-state observable
    // only): the chunk loop runs on the worker pool now, and each chunk
    // posts a progress frame onto the UI thread — QML repaints {phase,
    // done, total} live while importFiles is still in flight. The events
    // pumped inside that window exclude user input, so the run is not
    // interrupted mid-chunk.
    Q_PROPERTY(QVariantMap importProgress READ importProgress NOTIFY progressChanged)
    // The grid's selection (see the SELECTION note) and the settings
    // clipboard it feeds (SettingsClipboard.hpp's group semantics).
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(bool hasClipboard READ hasClipboard NOTIFY clipboardChanged)
    Q_PROPERTY(QString clipboardSourceName READ clipboardSourceName NOTIFY clipboardChanged)
    /// The ticked groups, as the ids SettingsClipboard::groupId gives.
    /// Remembered in the process (the Mac's rememberedKey contract; the
    /// settings page owns any persistence tier).
    Q_PROPERTY(QVariantList clipboardGroups READ clipboardGroups WRITE setClipboardGroups
                   NOTIFY clipboardGroupsChanged)
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
        EditRevisionRole,
        SelectedRole,
        HasThumbnailRole
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
    int selectedCount() const;
    bool hasClipboard() const { return clipboard_.has_value(); }
    const QString& clipboardSourceName() const;
    QVariantList clipboardGroups() const;
    void setClipboardGroups(const QVariantList& ids);

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
    /// The row's picture for the grid, through the ThumbnailStore — REVISED
    /// IN TASK 4.4 (was a synchronous decode on the calling thread): the
    /// row's cached tile (any size — the delegate scales what it gets)
    /// returns at once; anything else schedules the decode on the one-thread
    /// worker pool (the store's owner) and returns an empty image. The
    /// filled picture lands through dataChanged(ThumbnailRole) on the UI
    /// thread; the delegate repaints then. Repeated asks while a decode is
    /// in flight are coalesced.
    Q_INVOKABLE QImage getThumbnail(const QString& path, int maxPixel = 256);

    // --- grid selection (see the header's SELECTION note) -----------------------
    /// A plain click: the row is the whole selection (and the extend anchor).
    Q_INVOKABLE void selectOnly(int row);
    /// A Ctrl-click: the row toggles; the anchor does not move.
    Q_INVOKABLE void toggleSelected(int row);
    /// A Shift-click: the run from the anchor to the row replaces the
    /// selection. With no anchor yet, behaves as selectOnly.
    Q_INVOKABLE void selectRange(int row);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectAllRows();
    /// The selection in row order — copy/paste's targets.
    Q_INVOKABLE QStringList selectedUuids() const;
    /// One row's display fields for the sidebar's Info tab: fileName, kind,
    /// rating, colorTag, capturedLocal, path, uuid and the develop map.
    /// Empty when the uuid is not on the grid.
    Q_INVOKABLE QVariantMap rowInfo(const QString& image) const;
    /// The row's uuid by index — the QML grid's current row resolves to its
    /// data through this (rows are model-addressable only by uuid).
    /// Empty past the end.
    Q_INVOKABLE QString uuidAt(int row) const;
    /// The sheet's offered group ids, in the sheet's order (the QML copy
    /// sheet's rows). Static: the vocabulary is the host's, not a model's.
    Q_INVOKABLE static QStringList offeredClipboardGroups();

    // --- the settings clipboard (SettingsClipboard.hpp's semantics) ---------------
    /// Reads the FIRST selected row's stored record (a record-less photo is
    /// its neutral default) into the clipboard under the ticked
    /// clipboardGroups. False with lastError set when nothing is selected.
    Q_INVOKABLE bool copySettings();
    /// Writes the clipboard's ticked groups into every selected row's
    /// record — settings only (the clip's appliedTo). Records that cannot
    /// be replaced (the store's blocked verdict) are skipped with their
    /// reason in lastError. True when at least one record was written.
    Q_INVOKABLE bool pasteSettings();

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
    void selectionChanged();
    void clipboardChanged();
    void clipboardGroupsChanged();
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
        QImage thumbnail; // filled by the worker pool's decode (via
                          // getThumbnail), empty until it lands
    };

    /// Scoped guard over the blocking helper round trips: the outermost
    /// guard flips busy and notifies, the last one off flips it back.
    /// Copy-protected; public calls nest through busyDepth_, so an operation
    /// is one busy window, not one per wire call. The import holds its
    /// guard across the pumped wait (see the BLOCKING note), so busy reads
    /// true for the whole run.
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
    /// existingFiles — full rows, paged). Runs on the worker thread inside
    /// runImport over the import host — no property writes; the failure
    /// reason is the return (empty = success).
    QString buildExistingIndex(LibraryHost& host, const QString& library,
                               QSet<QString>& paths, QSet<QString>& namesAndSizes);
    /// The only writer of progress_; notifies progressChanged.
    void setProgress(const QString& phase, int done, int total);
    /// The only writer of lastError_; notifies errorChanged when the reason
    /// actually changes (so re-reporting the same failure is silent).
    void setError(const QString& message);
    /// The only writer of selectedCollection_ outside buildTree's default
    /// selection (whose closing treeChanged covers it); notifies treeChanged
    /// so QML bindings on selectedCollectionId cannot go stale.
    void setSelectedCollection(const QString& id);

    // --- the worker seam (task 4.4; see the BLOCKING note) ------------------
    /// The import's chunk loop, run on workerPool_'s thread: the duplicate
    /// plan (the caller hands the pre-validated items), the chunks through
    /// imagesAdd, the one Recent Imports album. Every observable it would
    /// touch (progress_) is marshalled onto the UI thread with
    /// postProgressFromWorker; a failure rides the returned map's `error`
    /// key, which importFiles surfaces through the UI thread's setError.
    /// The returned map is the importFiles result, taken at the end.
    ///
    /// The import talks to the helper through ITS OWN LibraryHost
    /// (importHost_, created and used only on the worker thread): a QProcess
    /// may only be waited on from the thread it lives in, and the UI
    /// thread's host_ stays where the synchronous calls need it. Two
    /// processes over one SQLite library is safe because they never overlap
    /// — the whole point of importBlocked() is that no UI wire call runs
    /// while the worker holds the pipe.
    QVariantMap runImport(const QString& libraryPath, QVariantList items, const QString& mode,
                          int chunkSize);
    /// Worker-side progress marshal: queued onto the UI thread, the only
    /// writer of progress_. (The failure reason rides the returned map's
    /// `error` key — importFiles sets lastError on the UI thread.)
    void postProgressFromWorker(const QString& phase, int done, int total);
    /// The UI-thread landing of one pool decode: fills the row's thumbnail,
    /// notifies dataChanged(ThumbnailRole), releases the pending key.
    void thumbnailDecoded(const QString& path, int maxPixel, const QImage& image);
    /// The row index by path, or -1.
    int rowIndexForPath(const QString& path) const;
    /// The row index by uuid, or -1.
    int rowIndexForUuid(const QString& uuid) const;
    /// Drops selection entries whose rows are gone (after a re-list) and
    /// notifies the selected role where the flags actually moved.
    void pruneSelection();
    void notifySelectionChanged();
    /// True when a worker import is in flight: public wire calls refuse
    /// (the LibraryHost pipe is single-conversation; see the BLOCKING note).
    /// `importBlocked()` is the shared refusal — true with lastError set.
    bool importBlocked();

    LibraryHost host_;
    /// The import's helper process (see runImport): created, used and
    /// destroyed only on workerPool_'s thread; null until the first import.
    /// `importLibrary_` is that process's current library handle (worker
    /// thread only, too).
    LibraryHost* importHost_ = nullptr;
    QString importLibrary_;
    LibraryHost::Options options_;
    CatalogSourcesStore store_;
    editing::Store edit_store_;
    spk::desktop::ThumbnailStore thumbnails_;
    CatalogProvider catalog_provider_;
    CatalogSourcesStore::Registry registry_;
    QString library_; // the helper handle
    /// The open source's path — the import host re-opens the same library
    /// in its own process (runImport).
    QString sourcePath_;
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
    // --- task 4.4: the worker seam, the grid's selection, the clipboard ------
    /// One decode/import at a time (the store keeps its single-owner-thread
    /// contract; the pool's thread is the owner). Declared last: destroyed
    /// first, so no job outlives the state it touches.
    QThreadPool workerPool_;
    QSet<QString> pendingThumbnails_; // "path@size" keys in flight
    QSet<QString> selected_;          // the grid's selection, by image uuid
    int selectionAnchor_ = -1;        // the last plain-clicked row
    std::optional<clip::Clip> clipboard_;
    bool importActive_ = false;       // set only on the UI thread
};

} // namespace spk::desktop::library
