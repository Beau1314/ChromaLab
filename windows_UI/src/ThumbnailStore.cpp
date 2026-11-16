#include "ThumbnailStore.hpp"

#include "CatalogSourcesStore.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>

namespace spk::desktop {

namespace {

constexpr const char* kEntrySuffix = ".png";

// The RAW extensions that get the placeholder tile (the scope cut recorded in
// the header). Canon/Nikon/Sony/Fuji/Olympus/Panasonic/Pentax/Leica and the
// open DNG; a kind this list misses decodes-or-fails like any other file.
const QSet<QString>& rawExtensions() {
    static const QSet<QString> extensions = {
        QStringLiteral("nef"), QStringLiteral("nrw"), QStringLiteral("cr2"),
        QStringLiteral("cr3"), QStringLiteral("crw"), QStringLiteral("arw"),
        QStringLiteral("srf"), QStringLiteral("sr2"), QStringLiteral("dng"),
        QStringLiteral("raf"), QStringLiteral("orf"), QStringLiteral("rw2"),
        QStringLiteral("raw"), QStringLiteral("pef"), QStringLiteral("ptx"),
        QStringLiteral("srw"), QStringLiteral("3fr"), QStringLiteral("erf"),
        QStringLiteral("mef"), QStringLiteral("mos"), QStringLiteral("iiq"),
        QStringLiteral("kdc"), QStringLiteral("dcr"), QStringLiteral("x3f"),
        QStringLiteral("rwl"), QStringLiteral("mrw"), QStringLiteral("r3d"),
        QStringLiteral("fff"), QStringLiteral("gpr"), QStringLiteral("braw")};
    return extensions;
}

bool isRawByExtension(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    return rawExtensions().contains(suffix.toCaseFolded());
}

qint64 imageBytes(const QImage& image) {
    return image.isNull() ? 0 : qint64(image.sizeInBytes());
}

} // namespace

ThumbnailStore::ThumbnailStore() : ThumbnailStore(Options{}) {}

ThumbnailStore::ThumbnailStore(const Options& options) : options_(options) {
    if (options_.directory.isEmpty()) options_.directory = defaultDirectory();
    if (options_.budgetBytes <= 0) options_.budgetBytes = 64 << 20;
}

QString ThumbnailStore::defaultDirectory() {
    return QDir(library::CatalogSourcesStore::macLibraryRoot())
        .filePath(QLatin1String("Cache/Thumbnails"));
}

void ThumbnailStore::setBudget(qint64 bytes) {
    options_.budgetBytes = bytes > 0 ? bytes : 64 << 20;
}

QString ThumbnailStore::entryName(const QString& path, int maxPixel) const {
    const QByteArray seed = path.toUtf8() + '@' + QByteArray::number(maxPixel);
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex()) +
           QLatin1String(kEntrySuffix);
}

QString ThumbnailStore::cachedFileFor(const QString& path, int maxPixel) const {
    return QDir(options_.directory).filePath(entryName(path, maxPixel));
}

// --- the disk index -------------------------------------------------------------

void ThumbnailStore::scanDisk() {
    if (diskScanned_) return;
    diskScanned_ = true;
    disk_.clear();
    diskBytes_ = 0;
    diskLru_.clear();
    diskPos_.clear();
    const QDir dir(options_.directory);
    if (!dir.exists()) QDir().mkpath(options_.directory);
    const auto entries = dir.entryList(QDir::Files);
    for (const QString& name : entries) {
        if (!name.endsWith(QLatin1String(kEntrySuffix))) continue;
        QFileInfo info(dir, name);
        if (info.size() <= 0) continue; // a zero-byte husk is corruption to be
        disk_.insert(name, info.size());
        diskBytes_ += info.size();
        diskPos_.insert(name, diskLru_.insert(diskLru_.end(), name));
    }
}

void ThumbnailStore::touchDisk(const QString& fileName) {
    // A hit rewrites the entry's in-session recency. Nothing else changes.
    if (const auto it = diskPos_.find(fileName); it != diskPos_.end()) {
        diskLru_.splice(diskLru_.begin(), diskLru_, *it);
        return;
    }
    if (!disk_.contains(fileName)) return;
    diskPos_.insert(fileName, diskLru_.insert(diskLru_.begin(), fileName));
}

void ThumbnailStore::enforceDiskBudget(const QString& keep) {
    scanDisk();
    while (diskBytes_ > options_.budgetBytes && disk_.size() > 1) {
        // The oldest entry this session's list still knows; entries the list
        // never heard of (a scan that found a previous session's leftovers)
        // sit at the back and go first.
        QString victim;
        for (auto it = diskLru_.rbegin(); it != diskLru_.rend(); ++it) {
            if (*it == keep) continue;
            if (disk_.contains(*it)) { victim = *it; break; }
        }
        if (victim.isEmpty()) break;
        QFile::remove(QDir(options_.directory).filePath(victim));
        diskBytes_ -= disk_.value(victim);
        disk_.remove(victim);
        if (const auto pos = diskPos_.constFind(victim); pos != diskPos_.cend()) {
            diskLru_.erase(*pos);
            diskPos_.erase(pos);
        }
    }
}

// --- the memory tier ------------------------------------------------------------

void ThumbnailStore::remember(const QImage& image, const QString& path, int maxPixel) {
    const QString name = entryName(path, maxPixel);
    const qint64 bytes = imageBytes(image);
    if (const auto pos = memoryPos_.constFind(name); pos != memoryPos_.cend()) {
        // An insert that replaces an existing entry keeps its place.
        memoryBytes_ += bytes - imageBytes(memory_.value(name));
        memory_.insert(name, image);
        return;
    }
    memory_.insert(name, image);
    memoryBytes_ += bytes;
    memoryPos_.insert(name, memoryLru_.insert(memoryLru_.begin(), name));
    // Same byte budget on the memory tier: the decoded pixels are the
    // expensive half; the disk tier survives a trim, the Mac's store only
    // ever had this one.
    while (memoryBytes_ > options_.budgetBytes && memoryLru_.size() > 1) {
        const QString victim = memoryLru_.back();
        if (victim == name) break;
        if (const auto img = memory_.constFind(victim); img != memory_.cend()) {
            memoryBytes_ -= imageBytes(*img);
            memory_.remove(victim);
        }
        memoryPos_.remove(victim);
        memoryLru_.pop_back();
    }
}

// --- decoding --------------------------------------------------------------------

QImage ThumbnailStore::decodeRaster(const QString& path, int maxPixel) const {
    QImageReader reader(path);
    reader.setAutoTransform(true); // EXIF orientation, as ImageIO does
    const QSize size = reader.size();
    if (size.isValid() && size.width() > 0 && size.height() > 0) {
        const int longEdge = qMax(size.width(), size.height());
        if (longEdge > maxPixel) {
            const double scale = double(maxPixel) / double(longEdge);
            reader.setScaledSize(QSize(qMax(1, int(std::lround(size.width() * scale))),
                                       qMax(1, int(std::lround(size.height() * scale)))));
        }
    }
    QImage image;
    if (!reader.read(&image) || image.isNull()) return {};
    return image;
}

QImage ThumbnailStore::placeholder(int maxPixel) {
    // The RAW tile: a quiet two-tone diagonal, deterministic per size (the
    // cache treats it like any other picture). It reads as "no preview yet",
    // not as corruption, and never as a render of the photograph.
    QImage tile(maxPixel, maxPixel, QImage::Format_RGB32);
    const QRgb base = QColor(38, 38, 40).rgb();
    const QRgb stripe = QColor(52, 52, 55).rgb();
    for (int y = 0; y < maxPixel; ++y)
        for (int x = 0; x < maxPixel; ++x)
            tile.setPixel(x, y, ((x + y) / 8) % 2 == 0 ? base : stripe);
    return tile;
}

QImage ThumbnailStore::decode(const QString& path, int maxPixel) const {
    if (path.isEmpty() || maxPixel <= 0 || !QFileInfo::exists(path)) return {};
    // Qt decodes the rasters (including their EXIF orientation). A RAW gets
    // the placeholder — the scope cut recorded in the header. Anything else
    // that will not decode (a corrupt file) is an honest null.
    QImage image = decodeRaster(path, maxPixel);
    if (image.isNull() && isRawByExtension(path)) image = placeholder(maxPixel);
    if (image.isNull()) return {};
    const int longEdge = qMax(image.width(), image.height());
    if (longEdge > maxPixel)
        image = image.scaled(maxPixel, maxPixel, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// --- the one public read -----------------------------------------------------------

QImage ThumbnailStore::get(const QString& path, int maxPixel) {
    if (path.isEmpty() || maxPixel <= 0) return {};
    const QString name = entryName(path, maxPixel);

    if (const auto hit = memory_.constFind(name); hit != memory_.cend()) {
        if (const auto pos = memoryPos_.find(name); pos != memoryPos_.end())
            memoryLru_.splice(memoryLru_.begin(), memoryLru_, *pos);
        // A key's recency is the key's, in both tiers: the ask refreshed the
        // tile the user wants, and its disk entry (when one stands) rides
        // along or a budget-tight cache would evict exactly what is hot.
        if (disk_.contains(name)) touchDisk(name);
        return *hit;
    }

    scanDisk();
    if (disk_.contains(name)) {
        QImage image(QDir(options_.directory).filePath(name));
        if (!image.isNull()) {
            touchDisk(name);
            remember(image, path, maxPixel);
            return image;
        }
        // Corrupt entry (a truncated write): skip it, decode again.
        QFile::remove(QDir(options_.directory).filePath(name));
        diskBytes_ -= disk_.value(name);
        disk_.remove(name);
        if (const auto pos = diskPos_.constFind(name); pos != diskPos_.cend()) {
            diskLru_.erase(*pos);
            diskPos_.erase(pos);
        }
    }

    QImage image = decode(path, maxPixel);
    if (image.isNull()) return {}; // a failed decode caches nothing

    const QString file = QDir(options_.directory).filePath(name);
    QDir().mkpath(options_.directory);
    if (!image.save(file, "PNG")) return image; // memory-only if the disk refuses
    QFileInfo info(file);
    if (disk_.contains(name)) {
        diskBytes_ -= disk_.value(name);
        if (const auto pos = diskPos_.constFind(name); pos != diskPos_.cend()) {
            diskLru_.erase(*pos);
            diskPos_.erase(pos);
        }
    }
    disk_.insert(name, info.size());
    diskBytes_ += info.size();
    diskPos_.insert(name, diskLru_.insert(diskLru_.begin(), name));
    enforceDiskBudget(name);
    remember(image, path, maxPixel);
    return image;
}

void ThumbnailStore::clear() {
    memory_.clear();
    memoryLru_.clear();
    memoryPos_.clear();
    memoryBytes_ = 0;
    scanDisk();
    const QDir dir(options_.directory);
    const auto entries = dir.entryList(QDir::Files);
    for (const QString& name : entries)
        if (name.endsWith(QLatin1String(kEntrySuffix))) QFile::remove(dir.filePath(name));
    disk_.clear();
    diskLru_.clear();
    diskPos_.clear();
    diskBytes_ = 0;
}

} // namespace spk::desktop
