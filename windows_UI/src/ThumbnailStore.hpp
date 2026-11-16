#pragma once

#include <QImage>
#include <QHash>
#include <QString>
#include <list>

// The persistent thumbnail store (task 4.3) — the Windows port of the Mac's
// Services/ThumbnailStore.swift, sharing its one contract: a grid tile is a
// picture already on disk (a camera's embedded preview or a plain decode),
// NEVER a develop render.
//
// DECODING, BY DECISION — rasters only; RAW is a documented scope cut:
//   * raster files (PNG/JPEG/TIFF/WebP/…) decode through QImageReader, at the
//     asked long edge (setScaledSize keeps the full bitmap from ever being
//     held; autoTransform applies EXIF orientation as ImageIO does);
//   * a RAW (by extension, the list below) yields a deterministic placeholder
//     tile instead of a picture. The brief offered two roads and asked for
//     the placeholder if the engine's LibRaw was not cheaply reachable; it is
//     not: the embedded-preview extractor (spkhost::thumbnail in engine/
//     host/image_io.cpp) is compiled into the spektralab-host EXECUTABLE —
//     engine/host/CMakeLists.txt's add_executable, not into the linked
//     spk_image_io library — so reaching it means an engine CMake refactor
//     (extract a host static lib) or duplicating host translation units into
//     the Qt targets, and spk_image_io's own raw_decoder entry points do a
//     full develop, exactly what a thumbnail must never do. Real RAW
//     thumbnails therefore arrive with real RAW usage, through a future
//     engine seam (unpack_thumb behind a small library target). The
//     placeholder is deterministic per size so the cache treats it like any
//     other tile.
//
// CACHING. Path-keyed, two tiers:
//   * memory: decoded QImages bounded by the byte budget, exact in-session
//     LRU (an intrusive list; a hit moves the entry to the front);
//   * disk: one PNG per (path, maxPixel), file name = SHA-1 of the key,
//     under options.directory (default <AppLocalData>/…/MacLibrary/Cache/
//     Thumbnails — beside sources.json, the MacLibrary layout's Cache tier).
//     The budget (default 64 MB, the Mac's byte limit carried over) evicts
//     least-recently-used files first; a hit rewrites the entry's place in
//     the in-session order. The index rebuilds lazily from the directory on
//     first use (mtime order approximates the previous session's order —
//     good enough for an eviction heuristic, and honest about it).
//
// CORRUPTION. A disk entry that does not load is deleted and the source is
// decoded again (a truncated write must never reach the grid). A raster that
// decodes nowhere returns a null QImage and caches nothing — the grid draws
// its own placeholder over the empty role. A RAW (above) keeps its
// placeholder tile, cached like any other.
//
// THREADS, BY DECISION: get() is synchronous and documented as UI-thread
// work, the same recorded decision as LibraryModel's blocking helper round
// trips. A raster tile decodes in single-digit milliseconds. The store is
// therefore deliberately not thread-safe: one owner thread. If 4.4's grid
// ever wants scrolling decodes off the UI thread, the seam is a worker queue
// calling this same class under a lock — the cache logic need not change.
namespace spk::desktop {

class ThumbnailStore final {
public:
    struct Options {
        QString directory;              // disk cache directory; empty = default
        qint64 budgetBytes = 64 << 20;  // 64 MB, the Mac store's byte limit
    };

    ThumbnailStore();
    explicit ThumbnailStore(const Options& options);

    /// <AppLocalData>/<org>/<app>/MacLibrary/Cache/Thumbnails — beside the
    /// sources registry, the Windows MacLibrary layout's cache tier.
    static QString defaultDirectory();

    /// The picture with its long edge at most `maxPixel` (never upscaled).
    /// Synchronous; see the header note on threads.
    QImage get(const QString& path, int maxPixel);

    /// Drop every cached entry, memory and disk. The directory keeps standing.
    void clear();

    /// Where (path, maxPixel) lives on disk — diagnostics and the tests' eyes.
    QString cachedFileFor(const QString& path, int maxPixel) const;

    /// Bytes the disk tier currently accounts.
    qint64 diskBytes() const { return diskBytes_; }

    /// Entries held in memory.
    int memoryCount() const { return int(memory_.size()); }

    /// Raise or lower the byte budget (the tests tighten it; a settings page
    /// may one day too). Applies from the next insert or hit.
    void setBudget(qint64 bytes);

private:
    QString entryName(const QString& path, int maxPixel) const;
    QImage decode(const QString& path, int maxPixel) const;
    QImage decodeRaster(const QString& path, int maxPixel) const;
    static QImage placeholder(int maxPixel);
    void scanDisk();
    void remember(const QImage& image, const QString& path, int maxPixel);
    void enforceDiskBudget(const QString& keep);
    void touchDisk(const QString& fileName);

    Options options_;
    // memory tier: name -> image; its own recency list, front = most recent.
    // The tiers keep SEPARATE lists on purpose: a memory hit must not
    // resurrect a disk entry's standing (and the other way round), or a
    // budget-tight cache evicts one tier's bookkeeping out from under the
    // other's.
    QHash<QString, QImage> memory_;
    std::list<QString> memoryLru_;
    QHash<QString, std::list<QString>::iterator> memoryPos_;
    qint64 memoryBytes_ = 0;
    // disk tier: name -> bytes; diskBytes_ is their sum; its own recency.
    QHash<QString, qint64> disk_;
    std::list<QString> diskLru_;
    QHash<QString, std::list<QString>::iterator> diskPos_;
    qint64 diskBytes_ = 0;
    bool diskScanned_ = false;
};

} // namespace spk::desktop
