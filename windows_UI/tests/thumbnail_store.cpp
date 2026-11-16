// The persistent thumbnail store (task 4.3): a pure test — no helper, no
// engine session, no GPU. The fixtures are real PNGs painted and written by
// QImage itself, so the decode path (QImageReader) is exercised exactly as in
// production. The RAW branch is the brief's placeholder cut (the header
// records the judgment): a RAW-named file yields the deterministic
// placeholder tile, and a raster that will not decode yields null — both
// asserted below with plain fixtures.
//
// What is asserted, per the brief:
//   store/get   — decode at the asked long edge, never larger than the source
//   persistence — the disk tier survives a store rebuild (and a deleted source)
//   corrupt     — a corrupt cache entry is skipped and re-decoded; a corrupt
//                 raster yields a null image and caches nothing
//   RAW         — a RAW-named file yields the deterministic placeholder tile
//   LRU/budget  — the byte budget evicts least-recently-used entries (exact
//                 in-session order, no mtime rounding), get() touches
#include "ThumbnailStore.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {

int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

QString pathIn(const QDir& dir, const QString& name) { return dir.filePath(name); }

/// A painted WxH PNG: a diagonal gradient so every scale of it stays a real
/// picture (a flat fill would also survive a broken decode too easily).
QImage paint(int width, int height, int seed) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, QColor((x * 4 + seed) % 256, (y * 4 + seed) % 256,
                                        ((x + y) * 2 + seed) % 256)
                                   .rgb());
    return image;
}

void writePng(const QString& path, const QImage& image) {
    check(image.save(path, "PNG"), "fixture PNG written");
}

bool fileExists(const QString& path) { return QFileInfo::exists(path); }

void testDecodeAndScale() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto source = pathIn(QDir(photos), QStringLiteral("gradient.png"));
    writePng(source, paint(96, 72, 3));

    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    spk::desktop::ThumbnailStore store(options);

    const auto small = store.get(source, 32);
    check(!small.isNull(), "get decodes the PNG");
    const int longSmall = qMax(small.width(), small.height());
    const int shortSmall = qMin(small.width(), small.height());
    check(longSmall == 32 && shortSmall == 24,
          "the long edge is capped at maxPixel (96x72 -> 32x24)");
    const auto full = store.get(source, 96);
    check(full.width() == 96 && full.height() == 72,
          "a maxPixel at the source size decodes 1:1");
    const auto larger = store.get(source, 256);
    check(larger.width() == 96 && larger.height() == 72,
          "maxPixel never upscales past the source");
    check(fileExists(store.cachedFileFor(source, 32)) &&
              fileExists(store.cachedFileFor(source, 96)),
          "each (path, maxPixel) pair has its own disk entry");
    check(store.diskBytes() > 0, "the disk tier accounts bytes");
}

void testDiskPersistence() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto source = pathIn(QDir(photos), QStringLiteral("keep.png"));
    writePng(source, paint(64, 48, 7));
    const auto thumbs = scratch.filePath(QStringLiteral("thumbs"));

    spk::desktop::ThumbnailStore::Options options;
    options.directory = thumbs;
    const auto first = spk::desktop::ThumbnailStore(options).get(source, 48);
    check(!first.isNull(), "the first store decoded");
    const auto cacheFile =
        spk::desktop::ThumbnailStore(options).cachedFileFor(source, 48);
    check(fileExists(cacheFile), "the disk entry was written");

    // A rebuilt store over the same directory serves the disk tier even with
    // the source gone — the whole point of a persistent cache.
    check(QFile::remove(source), "the source file removed for the test");
    spk::desktop::ThumbnailStore rebuilt(options);
    const auto again = rebuilt.get(source, 48);
    check(!again.isNull(), "the rebuilt store still serves the disk hit");
    check(again.size() == first.size(), "the disk hit is the same picture");
    // And the (path, maxPixel) of another size is a miss: no source, no decode.
    check(rebuilt.get(source, 16).isNull(),
          "an uncached size with no source decodes to nothing (honest miss)");
}

void testCorruptEntrySkipped() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto source = pathIn(QDir(photos), QStringLiteral("real.png"));
    writePng(source, paint(40, 30, 11));
    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    spk::desktop::ThumbnailStore store(options);
    check(!store.get(source, 30).isNull(), "the first get decoded");
    const auto cacheFile = store.cachedFileFor(source, 30);
    check(fileExists(cacheFile), "the entry is on disk");

    // Corrupt the disk entry (a truncated write, a crashing save), then send
    // a fresh store (empty memory tier) after it: the store must not hand
    // back the junk — it skips the entry and decodes again.
    QFile junk(cacheFile);
    check(junk.open(QIODevice::WriteOnly | QIODevice::Truncate), "junk entry opened");
    junk.write("not a png at all");
    junk.close();
    spk::desktop::ThumbnailStore reentering(options);
    const auto recovered = reentering.get(source, 30);
    check(!recovered.isNull() && recovered.width() == 30,
          "the corrupt entry was skipped and the source decoded again");
    QImage healed(cacheFile);
    check(!healed.isNull(), "the entry was rewritten whole");
}

void testCorruptSource() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto source = pathIn(QDir(photos), QStringLiteral("broken.png"));
    QFile junk(source);
    check(junk.open(QIODevice::WriteOnly | QIODevice::Truncate), "broken source opened");
    junk.write("\x89\x50\x4E\x47 garbage, not a decodable image");
    junk.close();
    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    spk::desktop::ThumbnailStore store(options);
    check(store.get(source, 32).isNull(),
          "an undecodable source yields a null image (the grid's placeholder)");
    check(!fileExists(store.cachedFileFor(source, 32)),
          "a failed decode caches nothing");
    check(store.diskBytes() == 0, "the store stayed empty");
    check(store.get(QString(), 32).isNull(), "an empty path yields null");
    check(store.get(scratch.filePath(QStringLiteral("absent.png")), 32).isNull(),
          "a missing file yields null");
}

void testBudgetLruEviction() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto a = pathIn(QDir(photos), QStringLiteral("a.png"));
    const auto b = pathIn(QDir(photos), QStringLiteral("b.png"));
    const auto c = pathIn(QDir(photos), QStringLiteral("c.png"));
    // Identical pixels under three names: identical PNG bytes, so the byte
    // budget arithmetic is exact (one entry = diskBytes()/2).
    writePng(a, paint(48, 36, 9));
    writePng(b, paint(48, 36, 9));
    writePng(c, paint(48, 36, 9));
    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    options.budgetBytes = 1 << 30; // unbounded while the fixtures go in
    spk::desktop::ThumbnailStore store(options);
    check(!store.get(a, 24).isNull(), "a decoded");
    check(!store.get(b, 24).isNull(), "b decoded");
    const qint64 entry = store.diskBytes() / 2;
    check(entry > 0, "two entries on disk");
    store.setBudget(2 * entry); // room for exactly two
    // get(a) makes a the most recently used; b is now the oldest.
    store.get(a, 24);
    check(!store.get(c, 24).isNull(), "c decoded under the two-entry budget");
    check(!fileExists(store.cachedFileFor(b, 24)), "the least recently used entry was evicted");
    check(fileExists(store.cachedFileFor(a, 24)), "the touched entry survived");
    check(fileExists(store.cachedFileFor(c, 24)), "the newcomer survived");
    check(store.diskBytes() <= 2 * entry, "the disk tier settled at or under its budget");
    // b is evicted but its source lives: get re-decodes it back in, evicting
    // the next-oldest (a) — the cache is a cache, not a database.
    check(!store.get(b, 24).isNull(), "an evicted entry re-decodes from its source");
    check(fileExists(store.cachedFileFor(b, 24)), "b is cached again");
    check(!fileExists(store.cachedFileFor(a, 24)), "a made room for it");
    store.clear();
    check(store.diskBytes() == 0, "clear empties the disk tier");
    check(QDir(options.directory).entryList(QDir::Files).isEmpty(),
          "the cache directory holds no files after clear");
}

void testRawPlaceholder() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    // A RAW-named file whose bytes are junk: 4.3 has no RAW decode (the
    // documented scope cut), so it stands in its deterministic placeholder —
    // a real picture to the cache, never a render of the photograph.
    const auto raw = pathIn(QDir(photos), QStringLiteral("sample.NEF"));
    QFile junk(raw);
    check(junk.open(QIODevice::WriteOnly | QIODevice::Truncate), "raw fixture opened");
    junk.write("junk bytes, no camera ever made this");
    junk.close();
    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    spk::desktop::ThumbnailStore store(options);
    const auto tile = store.get(raw, 48);
    check(!tile.isNull() && tile.width() == 48 && tile.height() == 48,
          "a RAW yields the placeholder tile at the asked size");
    const auto again = store.get(raw, 48);
    check(again.size() == tile.size(), "the placeholder is cached like any tile");
    check(fileExists(store.cachedFileFor(raw, 48)), "the placeholder rides the disk tier");
    // A case-folded extension counts too (a card's .nef, a copy's .NEF).
    const auto lower = pathIn(QDir(photos), QStringLiteral("sample2.dng"));
    QFile junk2(lower);
    check(junk2.open(QIODevice::WriteOnly | QIODevice::Truncate), "dng fixture opened");
    junk2.write("junk bytes");
    junk2.close();
    check(!store.get(lower, 32).isNull(), "the raw list is case-folded and covers DNG");
    // A missing RAW is still nothing at all.
    check(store.get(pathIn(QDir(photos), QStringLiteral("absent.nef")), 32).isNull(),
          "a missing RAW yields null, no placeholder out of thin air");
}

void testMemoryTier() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary scratch root unavailable");
    const auto photos = scratch.filePath(QStringLiteral("photos"));
    check(QDir().mkpath(photos), "the photos folder is made");
    const auto source = pathIn(QDir(photos), QStringLiteral("mem.png"));
    writePng(source, paint(32, 32, 5));
    spk::desktop::ThumbnailStore::Options options;
    options.directory = scratch.filePath(QStringLiteral("thumbs"));
    spk::desktop::ThumbnailStore store(options);
    const auto first = store.get(source, 32);
    check(!first.isNull(), "decoded once");
    check(store.memoryCount() == 1, "the decode sits in the memory tier");
    const auto second = store.get(source, 32);
    check(second.size() == first.size() && store.memoryCount() == 1,
          "a repeat get serves the memory tier, one entry");
    store.clear();
    check(store.memoryCount() == 0, "clear empties the memory tier");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testDecodeAndScale();
        testDiskPersistence();
        testCorruptEntrySkipped();
        testCorruptSource();
        testRawPlaceholder();
        testBudgetLruEviction();
        testMemoryTier();
        std::printf("thumbnail_store: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("thumbnail_store: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
