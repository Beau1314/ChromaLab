// Temporary source files and an explicit store root: this test never reads or
// writes the user's real edit history and needs neither a RAW decoder nor GPU.
#include "EditStore.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

namespace editing = spk::desktop::editing;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action, const char* message) {
    try { action(); } catch (const std::exception&) { ++checks; return; }
    throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "cannot read test file");
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot write test file");
    check(file.write(bytes) == bytes.size(), "incomplete test write");
}
QByteArray digest(const QString& path) {
    return QCryptographicHash::hash(read(path), QCryptographicHash::Sha256);
}
void mutateObject(QJsonObject& parent, const char* name,
                  const std::function<void(QJsonObject&)>& change) {
    auto child = parent.value(QLatin1String(name)).toObject();
    change(child);
    parent.insert(QLatin1String(name), child);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "temporary root unavailable");
        const auto sources = temporary.filePath(QString::fromUtf8("照片 文件夹"));
        check(QDir().mkpath(sources), "cannot create source folder");
        const auto source = QDir(sources).filePath(QString::fromUtf8("海边 日落.ARW"));
        QByteArray input(200000, 'R');
        for (qsizetype i = 0; i < input.size(); ++i) input[i] = char((i * 31 + i / 257) % 251);
        write(source, input);
        const auto originalHash = digest(source);
        const auto originalTime = QFileInfo(source).lastModified();
        const QUrl url = QUrl::fromLocalFile(source);
        const auto root = temporary.filePath("editor-state");
        const editing::Store store(root);
        const spk::desktop::Catalog catalog{
            {{"kodak_portra_400", "Portra", false}, {"fuji_provia_100f", "Provia", true}},
            {{"kodak_portra_endura", "Portra paper", false}, {"fuji_crystal", "Crystal", false}}};
        check(!store.load(url, catalog).state && !QFileInfo::exists(root),
              "reading missing state must not create a directory");
        check(!store.load(url, catalog).blocked, "missing state is not blocked");
        rejects([&] { editing::Store invalid(QString{}); }, "empty store directory accepted");
        rejects([&] { editing::Store invalid("relative/path"); }, "relative store directory accepted");
        rejects([&] { (void)store.filePath(QUrl("https://example.com/photo.ARW")); },
                "remote source URL accepted");
        editing::State state;
        state.settings.film_stock = "fuji_provia_100f";
        state.settings.print_stock = "fuji_crystal";
        state.settings.print_exposure = 0.625;
        state.settings.film_exposure_ev = -2.25;
        state.settings.film_format_mm = 61.5;
        state.settings.grain_active = false;
        state.settings.grain_amount = 0.3125;
        state.settings.halation_active = false;
        state.settings.halation_amount = 2.625;
        state.settings.glare_active = false;
        state.settings.glare_amount = 11.25;
        state.settings.y_filter_shift = -0.3125;
        state.settings.m_filter_shift = 0.375;
        // The digital-intermediate pair rides the same strict record schema.
        state.settings.di_active = true;
        state.settings.di_blue_compensation = true;
        state.mode = spk::desktop::DecodeMode::headroom;
        state.geometry.crop = QRectF(0.125, 0.25, 0.5, 0.625);
        state.geometry.quarterTurns = 3;
        state.geometry.flipHorizontal = true;
        state.geometry.flipVertical = true;
        state.geometry.straightenDegrees = -12.75;
        // Every Layer 2 family, with dyadic values so the JSON roundtrip must
        // be exact rather than merely close.
        namespace layer2 = spk::desktop::layer2;
        state.layer2.enabled = false;
        state.layer2.temperature = -47.5;
        state.layer2.tint = 31.25;
        state.layer2.exposure = 1.5;
        state.layer2.contrast = -18.0;
        state.layer2.brightness = 12.0;
        state.layer2.saturation = -42.0;
        state.layer2.highlights = 66.0;
        state.layer2.shadows = -9.5;
        state.layer2.blackPoint = 4.5;
        state.layer2.whitePoint = 3.25;
        state.layer2.curves.rgb.points = {QPointF(0, 0), QPointF(0.5, 0.75), QPointF(1, 1)};
        state.layer2.curves.luma.points = {QPointF(0, 0.25), QPointF(1, 0.875)};
        state.layer2.colorBalance.shadows = {15.0, 0.25, -0.5};
        state.layer2.colorBalance.highlights = {300.0, 0.125, 0.5};
        state.layer2.vignette = {-62.5, 62.5};
        check(!layer2::isNeutral(state.layer2), "layer 2 fixture must carry a visible grade");
        store.save(url, state);
        const auto path = store.filePath(url);
        const auto result = store.load(url, catalog);
        check(result.state && *result.state == state && result.warning.isEmpty() && !result.blocked,
              "complete state did not roundtrip exactly");
        check(QFileInfo(path).absolutePath() == root && QFileInfo(path).fileName().size() == 69,
              "edit file must be a hash inside the explicit store");
        check(QDir(sources).entryList(QDir::Files).size() == 1,
              "edit store wrote next to the source photograph");
        check(digest(source) == originalHash && QFileInfo(source).lastModified() == originalTime,
              "saving edits touched source bytes or modification time");
        const editing::Store reopened(root);
        check(reopened.load(url, catalog).state == result.state,
              "new store instance did not restore saved edits");
#ifdef _WIN32
        check(store.filePath(QUrl::fromLocalFile(source.toUpper())) == path,
              "Windows path case created two histories");
#endif
        const auto relativeEquivalent = QDir(sources).filePath("../" + QFileInfo(sources).fileName() + "/" + QFileInfo(source).fileName());
        check(store.filePath(QUrl::fromLocalFile(relativeEquivalent)) == path,
              "equivalent local paths created two histories");
        state.settings.print_exposure = 1.25;
        state.geometry.quarterTurns = 1;
        store.save(url, state);
        check(store.load(url, catalog).state == std::optional(state), "atomic update did not replace prior state");
        const auto validBytes = read(path);
        const auto validJson = QJsonDocument::fromJson(validBytes).object();
        rejects([&] { reopened.save(url, state); },
                "second instance overwrote edits saved since its load");
        check(read(path) == validBytes, "optimistic conflict changed existing edits");
        check(reopened.load(url, catalog).state == std::optional(state),
              "reopening did not refresh conflict observation");
        reopened.save(url, state);

        auto corrupt = [&](const std::function<void(QJsonObject&)>& change) {
            auto document = validJson;
            change(document);
            const auto bytes = QJsonDocument(document).toJson();
            write(path, bytes);
            const auto rejected = store.load(url, catalog);
            check(!rejected.state && rejected.blocked && !rejected.warning.isEmpty(),
                  "invalid document loaded or failed without a warning");
            rejects([&] { store.save(url, state); }, "save replaced an incompatible or corrupt record");
            check(read(path) == bytes, "rejected save changed existing edit history");
            write(path, validBytes);
        };
        corrupt([](auto& d) { d["version"] = 2; });
        corrupt([](auto& d) { d["version"] = "1"; });
        corrupt([](auto& d) { d["format"] = "Unrelated.Application"; });
        corrupt([](auto& d) { d["futureField"] = true; });
        corrupt([](auto& d) { d.remove("source"); });
        corrupt([](auto& d) { mutateObject(d, "source", [](auto& s) { s["path"] = "D:/different.ARW"; }); });
        corrupt([](auto& d) { mutateObject(d, "source", [](auto& s) { s["size"] = 123; }); });
        corrupt([](auto& d) { mutateObject(d, "source", [](auto& s) { s["sampleSha256"] = "broken"; }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) { s["decodeMode"] = "future-decoder"; }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f.remove("film_format_mm"); }); }); });
        // The DI pair must travel together: a record with one key alone is
        // incomplete and cannot be interpreted as an edit.
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f.remove("di_blue_compensation"); }); }); });
        // The HDR scan's triple as well.
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f.remove("scan_hdr_output_scale"); }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f["grain_active"] = 1; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f["print_exposure"] = "1.0"; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f["film_exposure_ev"] = 8.01; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f["film_stock"] = "../bad"; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "geometry", [](auto& g) { g["quarterTurns"] = 0.5; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "geometry", [](auto& g) { g["straightenDegrees"] = -45.1; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "geometry", [](auto& g) { g["flipVertical"] = "false"; }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "geometry", [](auto& g) {
                mutateObject(g, "crop", [](auto& c) { c["width"] = 0; }); }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) {
            mutateObject(s, "geometry", [](auto& g) {
                mutateObject(g, "crop", [](auto& c) { c["width"] = 1; }); }); }); });
        corrupt([](auto& d) { mutateObject(d, "state", [](auto& s) { s["layer2"] = 7; }); });

        auto noStocks = catalog;
        noStocks.films.clear();
        const auto unavailable = store.load(url, noStocks);
        check(!unavailable.state && unavailable.blocked && !unavailable.warning.isEmpty(),
              "unavailable catalog stock silently fell back to defaults");
        rejects([&] { store.save(url, state); }, "blocked catalog load could still overwrite its history");
        check(read(path) == validBytes, "catalog validation modified saved data");
        for (const QByteArray& bytes : {QByteArray("{\"version\":1,"), QByteArray(65537, ' ')}) {
            write(path, bytes);
            const auto bad = store.load(url, catalog);
            check(!bad.state && bad.blocked && !bad.warning.isEmpty(), "bad JSON or oversize document loaded");
            rejects([&] { store.save(url, state); }, "save overwrote truncated or oversize history");
            check(read(path) == bytes, "failed save modified unreadable history");
        }
        write(path, validBytes);
        check(store.load(url, catalog).state == std::optional(state), "reopen did not clear a resolved load warning");

        // A record written before Layer 2 existed (the JSON version never
        // changed) must still load, restoring the neutral grade; the next
        // save rewrites the record in the current shape.
        auto legacy = validJson;
        mutateObject(legacy, "state", [](auto& s) { s.remove("layer2"); });
        const auto legacyBytes = QJsonDocument(legacy).toJson();
        write(path, legacyBytes);
        const auto restored = store.load(url, catalog);
        check(restored.state && restored.state->layer2 == layer2::Adjustments{} && !restored.blocked,
              "legacy record without a layer 2 grade did not load with defaults");
        store.save(url, state);
        check(read(path) != legacyBytes && store.load(url, catalog).state == std::optional(state),
              "saving after a legacy record did not restore the layer 2 fields");
        write(path, validBytes);

        // A record written before the digital intermediate existed must load
        // with the pair off, and the next save rewrites the current shape.
        auto legacyDi = validJson;
        mutateObject(legacyDi, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) { f.remove("di_active"); f.remove("di_blue_compensation"); }); });
        const auto legacyDiBytes = QJsonDocument(legacyDi).toJson();
        write(path, legacyDiBytes);
        const auto restoredDi = store.load(url, catalog);
        check(restoredDi.state && restoredDi.state->settings.di_active == false &&
              restoredDi.state->settings.di_blue_compensation == false && !restoredDi.blocked,
              "legacy record without the digital intermediate did not load with defaults");
        store.save(url, state);
        check(read(path) != legacyDiBytes && store.load(url, catalog).state == std::optional(state),
              "saving after a legacy DI record did not restore the DI fields");
        write(path, validBytes);

        // The slide's HDR scan follows the same rule: a record from before the
        // phase-2 port loads with the scan off, and a record carrying one of
        // its keys without the others is incomplete.
        auto legacyHdr = validJson;
        mutateObject(legacyHdr, "state", [](auto& s) {
            mutateObject(s, "settings", [](auto& f) {
                f.remove("scan_hdr_headroom"); f.remove("scan_hdr_stretch"); f.remove("scan_hdr_output_scale"); }); });
        const auto legacyHdrBytes = QJsonDocument(legacyHdr).toJson();
        write(path, legacyHdrBytes);
        const auto restoredHdr = store.load(url, catalog);
        check(restoredHdr.state && restoredHdr.state->settings.scan_hdr_headroom == 0.0 &&
              restoredHdr.state->settings.scan_hdr_stretch == false &&
              restoredHdr.state->settings.scan_hdr_output_scale == 1.0 && !restoredHdr.blocked,
              "legacy record without the HDR scan did not load with defaults");
        store.save(url, state);
        check(read(path) != legacyHdrBytes && store.load(url, catalog).state == std::optional(state),
              "saving after a legacy HDR scan record did not restore the scan fields");
        write(path, validBytes);

        auto invalidState = [&](const std::function<void(editing::State&)>& change) {
            auto candidate = state;
            change(candidate);
            rejects([&] { store.save(url, candidate); }, "invalid in-memory state was saved");
            check(read(path) == validBytes, "invalid state modified valid history");
        };
        invalidState([](auto& s) { s.settings.grain_amount = std::numeric_limits<double>::quiet_NaN(); });
        invalidState([](auto& s) { s.settings.glare_amount = std::numeric_limits<double>::infinity(); });
        invalidState([](auto& s) { s.settings.film_format_mm = 3.99; });
        invalidState([](auto& s) { s.settings.print_exposure = 0.049; });
        invalidState([](auto& s) { s.settings.halation_amount = 4.01; });
        invalidState([](auto& s) { s.settings.m_filter_shift = 1.01; });
        invalidState([](auto& s) { s.settings.scan_hdr_headroom = 16.01; });
        invalidState([](auto& s) { s.settings.scan_hdr_output_scale = 0.06; });
        invalidState([](auto& s) { s.settings.film_stock = "syntactically_valid_but_missing"; });
        invalidState([](auto& s) { s.mode = static_cast<spk::desktop::DecodeMode>(17); });
        invalidState([](auto& s) { s.geometry.quarterTurns = 4; });
        invalidState([](auto& s) { s.geometry.crop = QRectF(-0.1, 0, 1, 1); });
        invalidState([](auto& s) { s.geometry.straightenDegrees = std::numeric_limits<double>::quiet_NaN(); });

        QLockFile lock(path + ".lock");
        check(lock.tryLock(0), "test could not acquire store lock");
        rejects([&] { store.save(url, state); }, "concurrent writer bypassed store lock");
        check(read(path) == validBytes, "locked save changed history");
        lock.unlock();

        // Same name, size and timestamp, but changed sampled bytes: the hash
        // prevents an unrelated replacement from inheriting these edits.
        QByteArray replaced = input;
        replaced[replaced.size() - 9] = char(replaced[replaced.size() - 9] ^ 63);
        write(source, replaced);
        QFile sourceHandle(source);
        check(sourceHandle.open(QIODevice::ReadWrite), "cannot open owned source timestamp");
        check(sourceHandle.setFileTime(originalTime, QFileDevice::FileModificationTime),
              "cannot restore owned source timestamp");
        sourceHandle.close();
        const auto stale = store.load(url, catalog);
        check(!stale.state && stale.blocked && stale.warning.contains("changed"),
              "changed sampled source content was not detected");
        rejects([&] { store.save(url, state); }, "save overwrote edits after the source changed");
        check(read(path) == validBytes, "stale source check discarded old edits");

        const auto another = QDir(sources).filePath("separate.ARW");
        write(another, QByteArray("independent source"));
        const auto otherUrl = QUrl::fromLocalFile(another);
        check(store.filePath(otherUrl) != path && !store.load(otherUrl, catalog).state,
              "separate source inherited another photograph's settings");
        store.save(otherUrl, editing::State{});
        check(store.load(otherUrl, catalog).state == std::optional(editing::State{}),
              "default state failed to roundtrip independently");
        const auto firstSaveSource = QDir(sources).filePath("changed-before-first-save.ARW");
        write(firstSaveSource, "first source contents");
        const auto firstSaveUrl = QUrl::fromLocalFile(firstSaveSource);
        check(!store.load(firstSaveUrl, catalog).state,
              "new source unexpectedly had edit history");
        write(firstSaveSource, "replacement contents");
        rejects([&] { store.save(firstSaveUrl, state); },
                "first save adopted a replaced source file");
        check(!QFileInfo::exists(store.filePath(firstSaveUrl)),
              "first-save source conflict created an edit record");
        const auto contested = QDir(sources).filePath("two-first-writers.ARW");
        write(contested, "contested source");
        const auto contestedUrl = QUrl::fromLocalFile(contested);
        editing::Store firstWriter(root), secondWriter(root);
        check(!firstWriter.load(contestedUrl, catalog).state && !secondWriter.load(contestedUrl, catalog).state,
              "contested source had prior state");
        firstWriter.save(contestedUrl, state);
        const auto firstBytes = read(firstWriter.filePath(contestedUrl));
        rejects([&] { secondWriter.save(contestedUrl, editing::State{}); },
                "second first-save writer replaced new history from another instance");
        check(read(firstWriter.filePath(contestedUrl)) == firstBytes,
              "competing first save modified the winner's edits");
        const auto blockedRoot = temporary.filePath("store-is-a-file");
        write(blockedRoot, "keep");
        rejects([&] { editing::Store(blockedRoot).save(otherUrl, state); },
                "save succeeded with a regular file as root");
        check(read(blockedRoot) == "keep", "failed directory creation changed its obstacle");
        rejects([&] { editing::Store(sources).save(otherUrl, state); },
                "store wrote edits beside the source image");
        check(QDir(root).entryList({"*.lock", "*.tmp"}, QDir::Files).isEmpty(),
              "store left lock or temporary files behind");

        // --- Pair records (Task 3.2): an independent file keyed by the union
        // hash of BOTH source URLs, carrying version/sources/hole states/look
        // with the same atomicity and conflict discipline as a photo record.
        {
            const auto leftSource = QDir(sources).filePath(QString::fromUtf8("左 半格.ARW"));
            const auto rightSource = QDir(sources).filePath(QString::fromUtf8("右 半格.ARW"));
            write(leftSource, QByteArray("left half-frame negative"));
            write(rightSource, QByteArray("right half-frame negative"));
            const auto leftUrl = QUrl::fromLocalFile(leftSource);
            const auto rightUrl = QUrl::fromLocalFile(rightSource);
            // A missing record is a new pair, and a load must not create the
            // store directory as a side effect.
            check(!store.loadPair(leftUrl, rightUrl, catalog).state,
                  "missing pair record loaded a state");
            check(!store.loadPair(leftUrl, rightUrl, catalog).blocked,
                  "missing pair record is not blocked");
            check(!QFileInfo::exists(store.pairFilePath(leftUrl, rightUrl)),
                  "pair load created a record file");
            // The record file is a hash inside the store, and the union hash
            // is ordered: the first hole is positional, so the swapped order
            // is a different pair and never shares a file.
            const auto pairPath = store.pairFilePath(leftUrl, rightUrl);
            check(QFileInfo(pairPath).absolutePath() == root &&
                      QFileInfo(pairPath).suffix() == QLatin1String("json") &&
                      QFileInfo(pairPath).completeBaseName().size() == 64,
                  "pair record must be a hash file inside the explicit store");
            check(store.pairFilePath(rightUrl, leftUrl) != pairPath,
                  "swapped pair sources shared one record file");
            check(pairPath != path && !store.load(leftUrl, catalog).blocked,
                  "pair records must not collide with photo records");

            editing::PairState pair;
            pair.spacing_mm = 1.5;
            pair.turned = true;
            pair.holes[0].placement = {1.5, 0.125, -0.0625, 1};
            pair.holes[0].exposure_ev = -0.75;
            pair.holes[0].dated = true;
            pair.holes[0].date_text = QString::fromUtf8("'26 11 15");
            pair.holes[0].trim = {0.5, -0.125, 0.25, 1.5};
            pair.holes[1].placement = {1.0, 0.0, 0.0, 0};
            pair.holes[1].exposure_ev = 1.25;
            pair.holes[1].dated = false;
            pair.holes[1].trim = {-1.0, 0.5, -0.5, 0.0};
            pair.look.settings.film_stock = "fuji_provia_100f";
            pair.look.settings.print_stock = "fuji_crystal";
            pair.look.settings.print_exposure = 0.875;
            pair.look.settings.auto_exposure = false; // the pair's own wire (§3)
            pair.look.settings.film_format_mm = 37.5;
            // The pair wire's second-frame rows ride the look: the split at
            // the gap's middle and a non-default B curve must roundtrip.
            pair.look.settings.scene_latitude_split = 0.5;
            pair.look.settings.scene_latitude_b_active = true;
            pair.look.settings.scene_latitude_b_highlight_knee = 1.5;
            pair.look.settings.scene_latitude_b_highlight_room = 0.75;
            pair.look.settings.scene_latitude_b_shadow_knee = -1.25;
            pair.look.settings.scene_latitude_b_shadow_room = 0.25;
            pair.look.geometry.crop = QRectF(0, 0, 1, 1);
            namespace layer2 = spk::desktop::layer2;
            pair.look.layer2.exposure = 0.5;
            store.savePair(leftUrl, rightUrl, pair);
            const auto round = store.loadPair(leftUrl, rightUrl, catalog);
            check(round.state && *round.state == pair && round.warning.isEmpty() && !round.blocked,
                  "pair record did not roundtrip exactly");
            // The swapped order is a different record: no state, not blocked.
            check(!store.loadPair(rightUrl, leftUrl, catalog).state &&
                      !store.loadPair(rightUrl, leftUrl, catalog).blocked,
                  "swapped sources inherited the ordered pair's record");
            // A fresh store instance restores the saved pair (reopen path).
            const editing::Store reopenedPair(root);
            check(reopenedPair.loadPair(leftUrl, rightUrl, catalog).state == std::optional(pair),
                  "a new store instance did not restore the pair record");

            // The record is bound to BOTH sources with the same identity
            // discipline as a photo record: a changed byte in either source
            // blocks the load and refuses saves.
            auto pairRecordBytes = read(pairPath);
            const auto pairJson = QJsonDocument::fromJson(pairRecordBytes).object();
            // Same name, size and timestamp, but a changed sampled byte in the
            // LEFT source alone: the fingerprint prevents the pair from
            // inheriting a replaced negative.
            const auto leftOriginalTime = QFileInfo(leftSource).lastModified();
            QByteArray replacedLeft = QByteArray("left half-frame negative");
            replacedLeft[4] = '?';
            write(leftSource, replacedLeft);
            QFile leftHandle(leftSource);
            check(leftHandle.open(QIODevice::ReadWrite), "cannot open owned source timestamp");
            check(leftHandle.setFileTime(leftOriginalTime, QFileDevice::FileModificationTime),
                  "cannot restore owned source timestamp");
            leftHandle.close();
            const auto staleLeft = store.loadPair(leftUrl, rightUrl, catalog);
            check(!staleLeft.state && staleLeft.blocked && !staleLeft.warning.isEmpty(),
                  "a changed left source silently kept its pair record");
            rejects([&] { store.savePair(leftUrl, rightUrl, pair); },
                    "save replaced a pair record after its left source changed");
            check(read(pairPath) == pairRecordBytes,
                  "refused pair save modified the existing record");
            write(leftSource, QByteArray("left half-frame negative"));
            QFile leftRestore(leftSource);
            check(leftRestore.open(QIODevice::ReadWrite), "cannot reopen owned source timestamp");
            check(leftRestore.setFileTime(leftOriginalTime, QFileDevice::FileModificationTime),
                  "cannot restore owned source timestamp again");
            leftRestore.close();
            check(store.loadPair(leftUrl, rightUrl, catalog).state == std::optional(pair),
                  "restoring the left source did not clear the pair warning");

            auto corruptPair = [&](const std::function<void(QJsonObject&)>& change) {
                auto document = pairJson;
                change(document);
                const auto bytes = QJsonDocument(document).toJson();
                write(pairPath, bytes);
                const auto rejected = store.loadPair(leftUrl, rightUrl, catalog);
                check(!rejected.state && rejected.blocked && !rejected.warning.isEmpty(),
                      "invalid pair document loaded or failed without a warning");
                rejects([&] { store.savePair(leftUrl, rightUrl, pair); },
                        "save replaced an incompatible pair record");
                check(read(pairPath) == bytes, "rejected pair save changed the record");
                write(pairPath, pairRecordBytes);
            };
            corruptPair([](auto& d) { d["version"] = 2; });
            corruptPair([](auto& d) { d["format"] = "SpektraLab.Windows.Editor"; });
            corruptPair([](auto& d) { d["futureField"] = true; });
            corruptPair([](auto& d) { d.remove("sources"); });
            corruptPair([](auto& d) { d["sources"] = QJsonArray{d["sources"]}; });
            corruptPair([](auto& d) { d.remove("pair"); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) { p.remove("spacingMM"); }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) { p["holes"] = QJsonArray{}; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) { p.remove("look"); }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[0].toObject();
                    hole.remove("trim");
                    holes[0] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[1].toObject();
                    mutateObject(hole, "trim", [](auto& t) { t["preflashExposure"] = "0"; });
                    holes[1] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[0].toObject();
                    mutateObject(hole, "placement", [](auto& pl) { pl["scale"] = 0.5; });
                    holes[0] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[1].toObject();
                    mutateObject(hole, "placement", [](auto& pl) { pl["quarterTurns"] = 4; });
                    holes[1] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[0].toObject();
                    hole["exposureEv"] = 5.01;
                    holes[0] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    auto holes = p["holes"].toArray();
                    auto hole = holes[0].toObject();
                    hole["dated"] = 1;
                    holes[0] = hole;
                    p["holes"] = holes; }); });
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) { p["spacingMM"] = 4.01; }); });
            // The look block shares the photo state's strict schema, so a
            // missing settings key is corrupt there as well.
            corruptPair([](auto& d) {
                mutateObject(d, "pair", [](auto& p) {
                    mutateObject(p, "look", [](auto& l) {
                        mutateObject(l, "settings", [](auto& f) { f.remove("print_stock"); }); }); }); });

            // In-memory validation refuses an out-of-range pair before any
            // record is touched. The reload first clears the warning the last
            // corrupt load left on the observation, so the rejections below
            // are the genuine range refusals.
            check(store.loadPair(leftUrl, rightUrl, catalog).state == std::optional(pair),
                  "reopen did not clear a resolved pair load warning");
            auto invalidPair = [&](const std::function<void(editing::PairState&)>& change) {
                auto candidate = pair;
                change(candidate);
                rejects([&] { store.savePair(leftUrl, rightUrl, candidate); },
                        "invalid in-memory pair state was saved");
                check(read(pairPath) == pairRecordBytes, "invalid pair state modified the record");
            };
            invalidPair([](auto& p) { p.spacing_mm = std::numeric_limits<double>::quiet_NaN(); });
            invalidPair([](auto& p) { p.holes[0].placement.scale = 8.5; });
            invalidPair([](auto& p) { p.holes[1].placement.x = -2.5; });
            invalidPair([](auto& p) { p.holes[0].trim.preflash_exposure = -0.5; });
            invalidPair([](auto& p) { p.holes[1].trim.y_filter_shift = 1.5; });
            invalidPair([](auto& p) { p.look.settings.film_format_mm = 3.0; });

            // The same optimistic-conflict discipline: a store that loaded
            // before the save cannot replace the newer record.
            editing::Store secondPairWriter(root);
            check(secondPairWriter.loadPair(leftUrl, rightUrl, catalog).state == std::optional(pair),
                  "second writer could not observe the saved pair");
            store.savePair(leftUrl, rightUrl, pair); // refresh the observation digest
            auto newer = pair;
            newer.holes[0].exposure_ev = -1.5;
            store.savePair(leftUrl, rightUrl, newer);
            rejects([&] { secondPairWriter.savePair(leftUrl, rightUrl, pair); },
                    "second pair writer replaced a record saved since its load");
            check(store.loadPair(leftUrl, rightUrl, catalog).state == std::optional(newer),
                  "the ordered record did not keep the newest state");
            check(read(pairPath) != pairRecordBytes, "the newer pair record was not written");

            // Photo records and pair records stay independent files.
            check(store.load(otherUrl, catalog).state == std::optional(editing::State{}),
                  "pair saves disturbed the photo record's store");
            check(QDir(root).entryList({"*.lock", "*.tmp"}, QDir::Files).isEmpty(),
                  "pair store left lock or temporary files behind");
        }

        std::printf("Edit store: %d checks passed; explicit temporary files only.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Edit store FAILED after %d checks: %s\n", checks, error.what());
        return 1;
    }
}
