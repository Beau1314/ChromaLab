#include "EditStore.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace spk::desktop::editing {
namespace {
constexpr qint64 maximumDocumentBytes = 64 * 1024;
constexpr qint64 sampleBytes = 64 * 1024;

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().toStdString());
}
void require(bool value, const QString& message) { if (!value) fail(message); }

QString sourcePath(const QUrl& url) {
    require(url.isLocalFile() && !url.toLocalFile().isEmpty(),
            QStringLiteral("Saved edits require a local source file"));
    const QFileInfo info(url.toLocalFile());
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
QString pathIdentity(const QString& path) {
#ifdef _WIN32
    return QDir::fromNativeSeparators(path).toCaseFolded();
#else
    return QDir::fromNativeSeparators(path);
#endif
}
QString observationKey(const QUrl& source) {
    require(source.isLocalFile() && !source.toLocalFile().isEmpty(),
            QStringLiteral("Saved edits require a local source file"));
    // Keep this key tied to the name the user opened. If a symlink is retargeted,
    // its canonical record path changes, but it must still fail the observation
    // check instead of looking like a never-before-opened photograph.
    return pathIdentity(QDir::cleanPath(QFileInfo(source.toLocalFile()).absoluteFilePath()));
}
void keys(const QJsonObject& object, std::initializer_list<const char*> expected,
          const QString& label) {
    require(object.size() == qsizetype(expected.size()),
            QStringLiteral("Saved edits contain missing or unsupported fields in ") + label);
    for (const char* key : expected)
        require(object.contains(QLatin1String(key)),
                QStringLiteral("Saved edits are missing ") + label + "." + QLatin1String(key));
}
// Same contract for a dynamically chosen key list (optional key groups).
void keys(const QJsonObject& object, const std::vector<const char*>& expected,
          const QString& label) {
    require(object.size() == qsizetype(expected.size()),
            QStringLiteral("Saved edits contain missing or unsupported fields in ") + label);
    for (const char* key : expected)
        require(object.contains(QLatin1String(key)),
                QStringLiteral("Saved edits are missing ") + label + "." + QLatin1String(key));
}
QJsonObject object(const QJsonObject& parent, const char* key) {
    const auto value = parent.value(QLatin1String(key));
    require(value.isObject(), QStringLiteral("Saved edit field must be an object: ") + QLatin1String(key));
    return value.toObject();
}
QString string(const QJsonObject& parent, const char* key) {
    const auto value = parent.value(QLatin1String(key));
    require(value.isString(), QStringLiteral("Saved edit field must be text: ") + QLatin1String(key));
    return value.toString();
}
double number(const QJsonObject& parent, const char* key, double low, double high) {
    const auto value = parent.value(QLatin1String(key));
    require(value.isDouble(), QStringLiteral("Saved edit field must be numeric: ") + QLatin1String(key));
    const double result = value.toDouble();
    require(std::isfinite(result) && result >= low && result <= high,
            QStringLiteral("Saved edit field is outside its valid range: ") + QLatin1String(key));
    return result;
}
bool boolean(const QJsonObject& parent, const char* key) {
    const auto value = parent.value(QLatin1String(key));
    require(value.isBool(), QStringLiteral("Saved edit field must be true or false: ") + QLatin1String(key));
    return value.toBool();
}
QString stockId(const QJsonObject& parent, const char* key,
                const std::vector<Stock>* catalog) {
    const QString id = string(parent, key);
    static const QRegularExpression syntax(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$"));
    require(syntax.match(id).hasMatch(), QStringLiteral("Saved edits contain an invalid stock ID"));
    if (catalog)
        require(std::any_of(catalog->begin(), catalog->end(), [&](const Stock& stock) {
                    return QString::fromStdString(stock.id) == id;
                }), QStringLiteral("Saved edits refer to an unavailable stock: ") + id);
    return id;
}

QJsonObject stateJson(const State& state) {
    const auto& s = state.settings;
    const auto& g = state.geometry;
    QString mode;
    switch (state.mode) {
    case DecodeMode::compatible16: mode = QStringLiteral("compatible16"); break;
    case DecodeMode::headroom: mode = QStringLiteral("headroom"); break;
    default: fail(QStringLiteral("Saved edits contain an unsupported RAW decode mode"));
    }
    return {{"settings", QJsonObject{
        {"film_stock", QString::fromStdString(s.film_stock)},
        {"print_stock", QString::fromStdString(s.print_stock)},
        {"print_exposure", s.print_exposure}, {"film_exposure_ev", s.film_exposure_ev},
        {"film_format_mm", s.film_format_mm}, {"grain_active", s.grain_active},
        {"grain_amount", s.grain_amount}, {"halation_active", s.halation_active},
        {"halation_amount", s.halation_amount}, {"glare_active", s.glare_active},
        {"glare_amount", s.glare_amount}, {"y_filter_shift", s.y_filter_shift},
        {"m_filter_shift", s.m_filter_shift},
        {"wb_temperature_k", s.wb_temperature_k}, {"wb_tint", s.wb_tint},
        {"auto_exposure", s.auto_exposure},
        {"di_active", s.di_active},
        {"di_blue_compensation", s.di_blue_compensation},
        {"scene_latitude_active", s.scene_latitude_active},
        {"scene_latitude_norm", QString::fromStdString(s.scene_latitude_norm)},
        {"scene_latitude_highlight_knee", s.scene_latitude_highlight_knee},
        {"scene_latitude_highlight_room", s.scene_latitude_highlight_room},
        {"scene_latitude_shadow_knee", s.scene_latitude_shadow_knee},
        {"scene_latitude_shadow_room", s.scene_latitude_shadow_room},
        {"scene_latitude_rolloff", s.scene_latitude_rolloff},
        {"scene_latitude_max_lift", s.scene_latitude_max_lift},
        {"contrast_mask_active", s.contrast_mask_active},
        {"contrast_mask_highlights", s.contrast_mask_highlights},
        {"contrast_mask_shadows", s.contrast_mask_shadows},
        {"contrast_mask_core", s.contrast_mask_core},
        {"contrast_mask_scale", s.contrast_mask_scale},
        {"scan_hdr_headroom", s.scan_hdr_headroom},
        {"scan_hdr_stretch", s.scan_hdr_stretch},
        {"scan_hdr_output_scale", s.scan_hdr_output_scale}}},
        {"decodeMode", mode}, {"geometry", QJsonObject{
            {"crop", QJsonObject{{"x", g.crop.x()}, {"y", g.crop.y()},
                                  {"width", g.crop.width()}, {"height", g.crop.height()}}},
            {"quarterTurns", g.quarterTurns}, {"flipHorizontal", g.flipHorizontal},
            {"flipVertical", g.flipVertical}, {"straightenDegrees", g.straightenDegrees}}},
        {"layer2", layer2::encode(state.layer2)}};
}
State parseState(const QJsonObject& value, const Catalog* catalog) {
    // Layer 2 arrived after the first records; its key is optional so an old
    // history still loads with the neutral grade (the document version did
    // not change). Anything else unknown is still rejected below.
    if (value.contains(QLatin1String("layer2")))
        keys(value, {"settings", "decodeMode", "geometry", "layer2"}, QStringLiteral("state"));
    else
        keys(value, {"settings", "decodeMode", "geometry"}, QStringLiteral("state"));
    State state;
    const auto s = object(value, "settings");
    // The white-balance row arrived after the first records (like layer2, its
    // keys are optional so an old history still loads); records that carry it
    // must carry all three of its keys. The digital-intermediate pair follows
    // the same rule: records that carry one key must carry both.
    const bool hasWhiteBalance = s.contains(QLatin1String("wb_temperature_k"));
    std::vector<const char*> setting_keys{
        "film_stock", "print_stock", "print_exposure", "film_exposure_ev", "film_format_mm",
        "grain_active", "grain_amount", "halation_active", "halation_amount", "glare_active",
        "glare_amount", "y_filter_shift", "m_filter_shift"};
    if (hasWhiteBalance)
        setting_keys.insert(setting_keys.end(), {"wb_temperature_k", "wb_tint", "auto_exposure"});
    const bool hasDigitalIntermediate = s.contains(QLatin1String("di_active"));
    if (hasDigitalIntermediate)
        setting_keys.insert(setting_keys.end(), {"di_active", "di_blue_compensation"});
    // RFC-023's resolved curve arrived with the phase-2 port (like the DI
    // pair, its keys are optional so an old history still loads); records
    // that carry one latitude key must carry all eight.
    const bool hasSceneLatitude = s.contains(QLatin1String("scene_latitude_active"));
    if (hasSceneLatitude)
        setting_keys.insert(setting_keys.end(),
                            {"scene_latitude_active", "scene_latitude_norm",
                             "scene_latitude_highlight_knee", "scene_latitude_highlight_room",
                             "scene_latitude_shadow_knee", "scene_latitude_shadow_room",
                             "scene_latitude_rolloff", "scene_latitude_max_lift"});
    // RFC-024's mask arrived with the phase-2 port (like the DI pair, its keys
    // are optional so an old history still loads); records that carry one mask
    // key must carry all five.
    const bool hasContrastMask = s.contains(QLatin1String("contrast_mask_active"));
    if (hasContrastMask)
        setting_keys.insert(setting_keys.end(),
                            {"contrast_mask_active", "contrast_mask_highlights",
                             "contrast_mask_shadows", "contrast_mask_core",
                             "contrast_mask_scale"});
    // The slide's HDR scan arrived with the same phase-2 port; records that
    // carry one of its keys must carry all three.
    const bool hasScanHdr = s.contains(QLatin1String("scan_hdr_headroom"));
    if (hasScanHdr)
        setting_keys.insert(setting_keys.end(),
                            {"scan_hdr_headroom", "scan_hdr_stretch",
                             "scan_hdr_output_scale"});
    keys(s, setting_keys, QStringLiteral("settings"));
    auto& out = state.settings;
    out.film_stock = stockId(s, "film_stock", catalog ? &catalog->films : nullptr).toStdString();
    out.print_stock = stockId(s, "print_stock", catalog ? &catalog->papers : nullptr).toStdString();
    out.print_exposure = number(s, "print_exposure", 0.05, 20.0);
    out.film_exposure_ev = number(s, "film_exposure_ev", -8.0, 8.0);
    out.film_format_mm = number(s, "film_format_mm", 4.0, 200.0);
    out.grain_active = boolean(s, "grain_active");
    out.grain_amount = number(s, "grain_amount", 0.0, 2.0);
    out.halation_active = boolean(s, "halation_active");
    out.halation_amount = number(s, "halation_amount", 0.0, 4.0);
    out.glare_active = boolean(s, "glare_active");
    out.glare_amount = number(s, "glare_amount", 0.0, 30.0);
    out.y_filter_shift = number(s, "y_filter_shift", -1.0, 1.0);
    out.m_filter_shift = number(s, "m_filter_shift", -1.0, 1.0);
    if (hasWhiteBalance) {
        out.wb_temperature_k = number(s, "wb_temperature_k", 0.0, 50000.0);
        require(out.wb_temperature_k == 0.0 || out.wb_temperature_k >= 2000.0,
                QStringLiteral("Saved white balance temperature is outside its valid range"));
        out.wb_tint = number(s, "wb_tint", -150.0, 150.0);
        // The two decode fields travel together: 0 kelvin is the As Shot
        // reset and carries no tint of its own.
        require(out.wb_temperature_k > 0.0 || out.wb_tint == 0.0,
                QStringLiteral("Saved white balance pair is inconsistent"));
        out.auto_exposure = boolean(s, "auto_exposure");
    }
    if (hasDigitalIntermediate) {
        out.di_active = boolean(s, "di_active");
        out.di_blue_compensation = boolean(s, "di_blue_compensation");
    }
    if (hasSceneLatitude) {
        out.scene_latitude_active = boolean(s, "scene_latitude_active");
        const QString norm = string(s, "scene_latitude_norm");
        require(norm == QLatin1String("power") || norm == QLatin1String("y") ||
                    norm == QLatin1String("max"),
                QStringLiteral("Saved scene latitude norm is unknown"));
        out.scene_latitude_norm = norm.toStdString();
        out.scene_latitude_highlight_knee = number(s, "scene_latitude_highlight_knee", -24.0, 24.0);
        out.scene_latitude_highlight_room = number(s, "scene_latitude_highlight_room", 0.0, 24.0);
        out.scene_latitude_shadow_knee = number(s, "scene_latitude_shadow_knee", -24.0, 24.0);
        out.scene_latitude_shadow_room = number(s, "scene_latitude_shadow_room", 0.0, 24.0);
        out.scene_latitude_rolloff = number(s, "scene_latitude_rolloff", 1.0, 4.0);
        out.scene_latitude_max_lift = number(s, "scene_latitude_max_lift", 0.25, 12.0);
    }
    if (hasContrastMask) {
        out.contrast_mask_active = boolean(s, "contrast_mask_active");
        out.contrast_mask_highlights = number(s, "contrast_mask_highlights", 0.0, 3.0);
        out.contrast_mask_shadows = number(s, "contrast_mask_shadows", 0.0, 3.0);
        out.contrast_mask_core = number(s, "contrast_mask_core", 0.0, 3.0);
        out.contrast_mask_scale = number(s, "contrast_mask_scale", 0.002, 0.12);
    }
    if (hasScanHdr) {
        out.scan_hdr_headroom = number(s, "scan_hdr_headroom", 0.0, 16.0);
        out.scan_hdr_stretch = boolean(s, "scan_hdr_stretch");
        out.scan_hdr_output_scale = number(s, "scan_hdr_output_scale", 0.0625, 1.0);
    }
    const auto mode = string(value, "decodeMode");
    require(mode == "compatible16" || mode == "headroom",
            QStringLiteral("Saved edits contain an unsupported RAW decode mode"));
    state.mode = mode == "headroom" ? DecodeMode::headroom : DecodeMode::compatible16;
    const auto g = object(value, "geometry");
    keys(g, {"crop", "quarterTurns", "flipHorizontal", "flipVertical", "straightenDegrees"},
         QStringLiteral("geometry"));
    const auto crop = object(g, "crop");
    keys(crop, {"x", "y", "width", "height"}, QStringLiteral("crop"));
    const double x = number(crop, "x", 0.0, 1.0), y = number(crop, "y", 0.0, 1.0);
    const double width = number(crop, "width", 0.0, 1.0), height = number(crop, "height", 0.0, 1.0);
    require(width > 0 && height > 0 && x + width <= 1.0 && y + height <= 1.0,
            QStringLiteral("Saved crop is empty or outside the image"));
    state.geometry.crop = QRectF(x, y, width, height);
    const double turns = number(g, "quarterTurns", 0.0, 3.0);
    require(std::floor(turns) == turns, QStringLiteral("Saved quarter turns must be an integer"));
    state.geometry.quarterTurns = int(turns);
    state.geometry.flipHorizontal = boolean(g, "flipHorizontal");
    state.geometry.flipVertical = boolean(g, "flipVertical");
    state.geometry.straightenDegrees = number(g, "straightenDegrees", -45.0, 45.0);
    if (value.contains(QLatin1String("layer2")))
        state.layer2 = layer2::decode(object(value, "layer2"));
    return state;
}

struct Identity {
    QString path;
    qint64 size = 0, mtime = 0;
    QString sample;
    bool operator==(const Identity&) const = default;
};
Identity identity(const QString& source) {
    const QFileInfo before(source);
    require(before.exists() && before.isFile(), QStringLiteral("Cannot find source while checking saved edits"));
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read source while checking saved edits: ") + file.errorString());
    Identity result{pathIdentity(source), before.size(), before.lastModified().toMSecsSinceEpoch(), {}};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto first = file.read(std::min(result.size, sampleBytes));
    require(first.size() == std::min(result.size, sampleBytes), QStringLiteral("Cannot fingerprint source for saved edits"));
    hash.addData(first);
    if (result.size > sampleBytes) {
        const qint64 tail = std::min(sampleBytes, result.size - sampleBytes);
        require(file.seek(result.size - tail), QStringLiteral("Cannot seek source while checking saved edits"));
        const auto last = file.read(tail);
        require(last.size() == tail, QStringLiteral("Cannot fingerprint the end of the source"));
        hash.addData(last);
    }
    const QFileInfo after(source);
    require(after.exists() && after.size() == result.size &&
                after.lastModified().toMSecsSinceEpoch() == result.mtime,
            QStringLiteral("Source changed while checking saved edits; try again when copying has finished"));
    result.sample = QString::fromLatin1(hash.result().toHex());
    return result;
}
QJsonObject identityJson(const Identity& value) {
    // Decimal strings keep 64-bit timestamps and file sizes exact in JSON.
    return {{"path", value.path}, {"size", QString::number(value.size)},
            {"mtimeMs", QString::number(value.mtime)}, {"sampleSha256", value.sample}};
}
Identity parseIdentity(const QJsonObject& value) {
    keys(value, {"path", "size", "mtimeMs", "sampleSha256"}, QStringLiteral("source"));
    Identity result;
    result.path = string(value, "path");
    require(!result.path.isEmpty() && QDir::isAbsolutePath(result.path),
            QStringLiteral("Saved edits have an invalid source path"));
    bool ok = false;
    const QString size = string(value, "size");
    result.size = size.toLongLong(&ok);
    require(ok && result.size >= 0 && QString::number(result.size) == size,
            QStringLiteral("Saved edits have an invalid source size"));
    const QString mtime = string(value, "mtimeMs");
    result.mtime = mtime.toLongLong(&ok);
    require(ok && QString::number(result.mtime) == mtime,
            QStringLiteral("Saved edits have an invalid source timestamp"));
    result.sample = string(value, "sampleSha256");
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    require(hex.match(result.sample).hasMatch(), QStringLiteral("Saved edits have an invalid source fingerprint"));
    return result;
}
QByteArray fingerprint(const Identity& value) {
    return QCryptographicHash::hash(QJsonDocument(identityJson(value)).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}
bool recordExists(const QString& path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}
QJsonObject readDocument(const QString& path, QByteArray* digest = nullptr) {
    const QFileInfo info(path);
    require(!info.isSymLink() && info.isFile(), QStringLiteral("Saved edit path is not a regular file"));
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read saved edits: ") + file.errorString());
    require(file.size() <= maximumDocumentBytes, QStringLiteral("Saved edit document exceeds the 64 KiB limit"));
    const auto bytes = file.read(maximumDocumentBytes + 1);
    require(bytes.size() == file.size() && bytes.size() <= maximumDocumentBytes,
            QStringLiteral("Cannot read the complete saved edit document"));
    if (digest) *digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            QStringLiteral("Saved edits contain invalid JSON: ") + error.errorString());
    const auto result = document.object();
    keys(result, {"format", "version", "source", "state"}, QStringLiteral("document"));
    require(string(result, "format") == "SpektraLab.Windows.Editor",
            QStringLiteral("Saved edits use an unsupported document format"));
    const auto version = result.value("version");
    require(version.isDouble() && version.toDouble() == 1.0,
            QStringLiteral("Saved edits use an unsupported version; existing file was preserved"));
    return result;
}
State checkDocument(const QJsonObject& document, const Identity& source, const Catalog* catalog) {
    const auto stored = parseIdentity(object(document, "source"));
    require(stored.path == source.path, QStringLiteral("Saved edits belong to a different source; existing file was preserved"));
    require(stored == source, QStringLiteral("The source file changed since these edits were saved; existing edits were preserved"));
    return parseState(object(document, "state"), catalog);
}
} // namespace

Store::Store(QString directory) : directory_(QDir::cleanPath(directory)) {
    require(!directory.isEmpty() && QDir::isAbsolutePath(directory),
            QStringLiteral("Saved edit store requires an explicit absolute directory"));
}
QString Store::filePath(const QUrl& source) const {
    const auto digest = QCryptographicHash::hash(pathIdentity(sourcePath(source)).toUtf8(),
                                                QCryptographicHash::Sha256).toHex();
    return QDir(directory_).filePath(QString::fromLatin1(digest) + ".json");
}
LoadResult Store::load(const QUrl& source, const Catalog& catalog) const {
    QString key;
    try {
        key = observationKey(source);
        const auto path = filePath(source);
        const auto sourceId = identity(sourcePath(source));
        if (!recordExists(path)) {
            observations_.insert(key, {fingerprint(sourceId), {}, {}, catalog});
            return {};
        }
        QByteArray recordDigest;
        const auto document = readDocument(path, &recordDigest);
        observations_.insert(key, {fingerprint(sourceId), recordDigest, {}, catalog});
        return {checkDocument(document, sourceId, &catalog), {}, false};
    } catch (const std::exception& error) {
        const auto warning = QString::fromUtf8(error.what());
        if (!key.isEmpty()) {
            auto& observation = observations_[key];
            observation.warning = warning;
            observation.catalog = catalog;
        }
        return {{}, warning, true};
    }
}
void Store::save(const QUrl& source, const State& state) const {
    const auto key = observationKey(source);
    const auto path = filePath(source);
    const auto observed = observations_.constFind(key);
    if (observed != observations_.cend() && !observed->warning.isEmpty()) fail(observed->warning);
    const auto value = stateJson(state);
    const auto catalog = observed != observations_.cend() ? observed->catalog : std::optional<Catalog>{};
    // Root callers load with a current catalogue first. Direct save callers
    // have no catalogue, but still receive complete shape/range validation.
    (void)parseState(value, catalog ? &*catalog : nullptr);
    const auto sourceFile = sourcePath(source);
    const auto sourceId = identity(sourceFile);
    if (observed != observations_.cend())
        require(observed->source == fingerprint(sourceId),
                QStringLiteral("The source file changed after it was opened; edits remain unsaved"));
    require(pathIdentity(QFileInfo(sourceFile).absolutePath()) !=
                pathIdentity(QFileInfo(directory_).canonicalFilePath().isEmpty()
                    ? directory_ : QFileInfo(directory_).canonicalFilePath()),
            QStringLiteral("Saved edit store must not be beside the source photograph"));
    require(QDir().mkpath(directory_), QStringLiteral("Cannot create the saved edit directory"));
    QLockFile lock(path + ".lock");
    lock.setStaleLockTime(30000);
    require(lock.tryLock(0), QStringLiteral("Saved edits are being written by another process; try again"));
    QByteArray previousDigest;
    if (recordExists(path)) {
        const auto previous = readDocument(path, &previousDigest);
        (void)checkDocument(previous, sourceId, nullptr);
    }
    if (observed != observations_.cend())
        require(observed->record == previousDigest,
                QStringLiteral("Saved edits changed in another instance; restart the app and reopen the photo before saving"));
    const auto bytes = QJsonDocument(QJsonObject{{"format", "SpektraLab.Windows.Editor"},
        {"version", 1}, {"source", identityJson(sourceId)}, {"state", value}}).toJson(QJsonDocument::Indented);
    require(bytes.size() <= maximumDocumentBytes, QStringLiteral("Saved edit document exceeds the 64 KiB limit"));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), QStringLiteral("Cannot open saved edits for atomic writing: ") + file.errorString());
    require(file.write(bytes) == bytes.size(), QStringLiteral("Cannot write saved edits: ") + file.errorString());
    require(file.commit(), QStringLiteral("Cannot atomically save edits: ") + file.errorString());
    observations_.insert(key, {fingerprint(sourceId),
                                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), {}, catalog});
}

} // namespace spk::desktop::editing
