#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <vector>

// The library helper shell: one spklib-host child process spoken to over
// stdio, one JSON request per line, one JSON reply per line (the repo's
// API-SPEC-library-helper.md). This is the thin shell only — process
// lifecycle, the ready handshake, the abi gate and id-routed replies. The
// full method surface lives in the library-workflow layer (task 4.1).
//
// The wire-format half (namespace protocol) is deliberately QProcess-free so
// it can be unit-tested as pure JSON in and out.
namespace spk::desktop::library {

// The protocol's own vocabulary, independent of any process. Everything here
// parses or builds one line of the wire, nothing more.
namespace protocol {

constexpr int kProtocolVersion = 1;
constexpr int kSupportedAbi = 1;

// The helper's error codes (the SDK's API-SPEC.md). The app maps these onto
// UI copy; an unknown code must survive verbatim rather than collapse into
// "internal" — a newer helper may invent codes the shell has never seen.
inline bool isKnownErrorCode(const QString& code) {
    static const QSet<QString> known = {
        QStringLiteral("not_found"), QStringLiteral("invalid_params"),
        QStringLiteral("read_only"), QStringLiteral("conflict"),
        QStringLiteral("source_busy"), QStringLiteral("unsupported"),
        QStringLiteral("exists"), QStringLiteral("in_use"),
        QStringLiteral("ambiguous"), QStringLiteral("cross_volume"),
        QStringLiteral("io"), QStringLiteral("internal"),
        QStringLiteral("unknown_method"), QStringLiteral("last_variant")};
    return known.contains(code);
}

struct ReadyEvent {
    bool valid = false;
    QString version;  // "spklib 0.4.0 (abi 1, schema 3)" or later
    int protocol = 0; // 1 on every helper the shell talks to
};

/// The first line the helper writes. Anything else on that line — an error
/// shape, a reply, garbage — comes back invalid and the shell refuses.
ReadyEvent parseReadyEvent(const QJsonDocument& document);

/// One request line. `params` is always written, even when empty: the
/// missing-params default is the helper's behaviour, not something the app
/// relies on.
QByteArray requestLine(qint64 id, const QString& method,
                       const QJsonObject& params = QJsonObject());

struct ParsedReply {
    bool valid = false;   // a well-formed envelope at all
    bool routed = false;  // the envelope's id answers this request's id
    bool isResult = false;
    QJsonObject result;      // valid && isResult
    QString errorCode;       // valid && !isResult (may be an unknown code)
    QString errorMessage;
    QJsonValue errorData;
};

/// One reply line against one outstanding request id. A reply whose id does
/// not match is parsed (so the caller can log it) but marked unrouted.
ParsedReply parseReply(const QJsonDocument& document, qint64 requestId);

/// The `sdk.version` result. The helper is refused unless abi matches
/// kSupportedAbi (schema differences are the SDK's problem, not the shell's).
struct SdkVersion {
    bool valid = false;
    QString version;
    int abi = 0;
    int schema = 0;
};
SdkVersion parseSdkVersion(const QJsonObject& result);

/// Reassembles stdout chunks into whole wire lines. A reply can be large and
/// arrive in many pipe reads: a chunk can end mid-line or hold several
/// complete ones (the Mac reference hosts this as LineSplitter). Without the
/// carried remainder a split reply reaches the parser as fragments, is
/// dropped as undecodable, and the waiting call times out. The buffer stays
/// bytes on purpose: a chunk boundary can fall inside a multi-byte UTF-8
/// character, which a QString decode would mangle.
class LineSplitter {
public:
    /// Feed one read's bytes; returns every line this chunk completed.
    /// Bytes after the last '\n' stay buffered until their line completes.
    std::vector<QByteArray> feed(const QByteArray& chunk) {
        buffer_ += chunk;
        std::vector<QByteArray> lines;
        for (;;) {
            const qint64 index = buffer_.indexOf('\n');
            if (index < 0) break;
            lines.push_back(buffer_.left(index));
            buffer_.remove(0, index + 1);
        }
        return lines;
    }

    /// Bytes carried across reads — a truncated tail (EOF mid-line, mostly).
    const QByteArray& remainder() const { return buffer_; }

    /// Drop carried bytes: a fresh child must not inherit the last one's tail.
    void reset() { buffer_.clear(); }

private:
    QByteArray buffer_;
};

} // namespace protocol

/// `meta.set`/`images.trash`'s tri-state value.
struct MetaValue {
    enum class Kind { Omit, Null, Number };
    Kind kind = Kind::Omit;
    int number = 0;
    static MetaValue omit() { return {}; }
    static MetaValue clear() { MetaValue v; v.kind = Kind::Null; return v; }
    static MetaValue of(int number) { MetaValue v; v.kind = Kind::Number; v.number = number; return v; }
    bool isOmit() const { return kind == Kind::Omit; }
    bool isNull() const { return kind == Kind::Null; }
    bool isNumber() const { return kind == Kind::Number; }
};

// --- the typed layer's wire params -------------------------------------------
//
// Every builder writes the helper's vocabulary verbatim — snake_case keys,
// `null` exactly where the spec says a null changes behaviour, and a key left
// off the wire where absence means "leave as is". These are pure so the wire
// shapes are assertable without a helper process.
namespace protocol {

/// `images.list`/`images.facets` scope: the query keys are the spec's own
/// (collection, recursive, filter, sort, descending, offset, limit,
/// check_files, full, include_trashed, ...), passed through untouched.
QJsonObject imagesListParams(const QVariantMap& query);

/// `meta.set`'s rating/colour tag: omitted (key left off — leave the stored
/// value), null (explicit JSON null — hand the value back to Capture One),
/// or a number. Exactly one of `variants`/`images` goes on the wire.
QJsonObject metaSetParams(const QStringList& variants, const QStringList& images,
                          MetaValue rating, MetaValue colorTag);

/// `images.trash`: the flag is the same tri-state as meta.set's values.
QJsonObject imagesTrashParams(const QStringList& images, MetaValue trashed);

/// `images.add`: an item is a path string or a {path, metadata} object
/// (metadata exactly as the app's decoder read it); `recent_import_at` rides
/// only when given.
QJsonObject imagesAddParams(const QVariantList& items, const QString& mode,
                            const QString& collection, const QString& recentImportAt);

/// `collection.create`: parent and criteria ride only when given.
QJsonObject collectionCreateParams(const QString& kind, const QString& name,
                                   const QString& parent, const QJsonObject& criteria);

/// `edit.put`: the Sidecar JSON object goes through as-is with its base
/// revision — the revision gate lives on the helper's side.
QJsonObject editPutParams(const QString& variant, const QJsonObject& edit, int baseRevision);

/// `session.move`/`session.restore`: on_conflict rides only when given.
QJsonObject sessionMoveParams(const QStringList& images, const QString& to,
                              const QString& onConflict);

/// `folders.add`: skip rides only when non-empty.
QJsonObject foldersAddParams(const QString& path, bool recursive, const QStringList& skip);

} // namespace protocol

// --- the typed layer ----------------------------------------------------------
//
// One typed method per helper method (API-SPEC-library-helper.md's table; the
// SDK's own API-SPEC.md is authoritative for the shapes). Each method builds
// its params, sends one request and blocks on its reply, returning the parsed
// value or the helper's error envelope.
//
// Single-flight on the wire. One request in flight at a time, by
// construction: a typed method runs to its reply before returning, so no
// second line from the app ever overtakes it. This is the engine's render
// single-flight carried over; callers with more work than one request queue
// in their own layer (the library controller) and hand the host one call at
// a time. Nothing here re-enters another typed method.

/// One typed call's outcome: the parsed value, or the helper's error. A
/// transport failure (host not running, timeout, lost reply) is ok=false
/// with an empty errorCode — "the helper said no" stays distinguishable
/// from "the wire said nothing".
template <typename T> struct Reply {
    bool ok = false;
    T value{};
    QString errorCode;    // !ok — the helper's error code (may be unknown)
    QString errorMessage;
    QJsonValue errorData; // e.g. a conflict's {revision: n}
};

/// library.create/open's reply: the handle every library method takes, plus
/// the info the sources store keeps (uuid, kind, name, path, counts and a
/// copy's Capture One origin). The rest of the reply rides in `raw`.
struct SourceInfo {
    bool valid = false;
    QString library; // the handle ("L1")
    QString uuid;
    QString kind;
    QString name;
    QString path;
    int images = -1;   // counts.images (-1 = absent)
    int variants = -1; // counts.variants
    int edits = -1;    // counts.edits
    QVariantMap copiedFrom; // a copy's origin, verbatim; empty = none
    QVariantMap raw;        // the whole result, passthrough
};
SourceInfo parseSourceInfo(const QJsonObject& result);

/// One images.list row: the full row (variants included) rides in `map`; the
/// accessors are the keys the grid and Info rail read on every row.
struct ImageRow {
    QVariantMap map;
    bool valid = false;
    QString uuid() const { return map.value(QLatin1String("uuid")).toString(); }
    QString path() const { return map.value(QLatin1String("path")).toString(); }
    QString fileName() const { return map.value(QLatin1String("file_name")).toString(); }
    QString kind() const { return map.value(QLatin1String("kind")).toString(); }
    int rating() const { return map.value(QLatin1String("rating")).toInt(); }
    int colorTag() const { return map.value(QLatin1String("color_tag")).toInt(); }
    bool trashed() const { return map.value(QLatin1String("trashed")).toBool(); }
    QString capturedLocal() const { return map.value(QLatin1String("captured_local")).toString(); }
    /// exists rides only with check_files; absent = never asked.
    std::optional<bool> exists() const {
        const QVariant value = map.value(QLatin1String("exists"));
        if (!value.isValid()) return std::nullopt;
        return value.toBool();
    }
    int variantCount() const { return map.value(QLatin1String("variants")).toList().size(); }
    QString primaryVariant() const {
        const QVariantList variants = map.value(QLatin1String("variants")).toList();
        return variants.isEmpty() ? QString()
                                  : variants.front().toMap().value(QLatin1String("uuid")).toString();
    }
    int primaryEditRevision() const {
        const QVariantList variants = map.value(QLatin1String("variants")).toList();
        return variants.isEmpty()
                   ? -1
                   : variants.front().toMap().value(QLatin1String("edit_revision")).toInt();
    }
};

/// One images.list page.
struct ImagePage {
    int total = -1;
    int offset = -1;
    std::vector<ImageRow> images;
};
ImagePage parseImagePage(const QJsonObject& result);

/// edit.get's answer: revision 0 = this variant has never been edited.
struct EditState {
    bool valid = false;
    int revision = -1;
    bool hasEdit = false;
    QJsonObject edit;
    QString modifiedAt;
};
EditState parseEditState(const QJsonObject& result);

/// edit.put's answer: the revision to send back as the next base_revision.
struct EditWrite {
    bool valid = false;
    int revision = -1;
    QString modifiedAt;
};
EditWrite parseEditWrite(const QJsonObject& result);

/// images.add's answer: per-item results and the new Recent Imports album.
struct AddResult {
    struct Row {
        QString path;
        QString image;
        QString variant;
        bool existing = false;
        QString storedAt; // copy mode only
        QString error;    // present when the item failed
    };
    int added = -1;
    std::vector<Row> rows;
    QString recentImport; // the import's album, absent on a trimmed old import
};
AddResult parseAddResult(const QJsonObject& result);

/// images.remove's answer: rows forgotten (never files) and the refusals.
struct RemoveResult {
    struct Refused {
        QString image;
        QString code;
    };
    int removed = -1;
    std::vector<Refused> refused;
};
RemoveResult parseRemoveResult(const QJsonObject& result);

class LibraryHost : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString helperPath; // spklib-host(.exe)
        QString storePath;  // --store DIR, the libraries root
        // Extra argv after --store. Production passes nothing; the tests
        // point helperPath at the test binary itself in a scripted fake-peer
        // mode (silent or split-reply) to drive the timeout and reassembly
        // paths against a real pipe.
        QStringList helperArguments;
        std::chrono::milliseconds startTimeout{10000};
        std::chrono::milliseconds callTimeout{30000};
    };

    explicit LibraryHost(QObject* parent = nullptr);
    ~LibraryHost() override;

    /// Spawn the helper, wait for the ready event and gate on abi. Returns
    /// nullopt on success, or the failure reason (start, handshake, abi).
    std::optional<QString> start(const Options& options);

    /// host.shutdown and a bounded wait; the destructor also kills a helper
    /// that will not leave.
    void shutdown();

    bool isRunning() const { return process_ && process_->state() == QProcess::Running; }
    const protocol::SdkVersion& sdkVersion() const { return sdk_; }

    /// Send one request; returns its id, or -1 when the host is not running.
    /// Replies are routed by id to exactly one of: the registered handler,
    /// waitForReply's waiter, or (neither) dropped with a warning on stderr.
    qint64 call(const QString& method, const QJsonObject& params,
                std::function<void(const protocol::ParsedReply&)> handler = {});

    /// Synchronous convenience for setup-time calls: send and block (pumping
    /// QProcess events) until this id's reply or the timeout.
    std::optional<protocol::ParsedReply> waitForReply(qint64 id, const QString& method,
                                                      const QJsonObject& params,
                                                      std::chrono::milliseconds timeout);

    // --- the typed layer (single-flight; see the header's comment above) ---
    // Every method: build params, one request, block on the reply, return the
    // parsed value or the helper's error. `library` is the handle a create/
    // open/link returned.

    Reply<SourceInfo> libraryCreate(const QString& path, const QString& kind, const QString& name = {});
    Reply<SourceInfo> libraryOpen(const QString& path);
    Reply<bool> libraryClose(const QString& library);
    /// library.info as a passthrough map (uuid, kind, name, path, origin,
    /// copied_from, counts).
    Reply<QVariantMap> libraryInfo(const QString& library);
    /// library.sync: a per-table report, or {skipped: true} when unchanged.
    Reply<QVariantMap> librarySync(const QString& library, bool force = false);

    /// The sidebar's rows, each a passthrough map (uuid, parent, kind, name,
    /// title, position, origin, editable, created_at, image_count, folder,
    /// criteria, supported).
    Reply<std::vector<QVariantMap>> collectionsList(const QString& library);
    /// images.list: `query` keys are the spec's, verbatim.
    Reply<ImagePage> imagesList(const QString& library, const QVariantMap& query = {});
    /// image.get: the full image map (variants, albums, capture_one values).
    Reply<QVariantMap> imageGet(const QString& library, const QString& image);
    /// images.lookup: one result map per path ({path, found, match{image,variant}}).
    Reply<std::vector<QVariantMap>> imagesLookup(const QString& library, const QStringList& paths);
    /// images.facets: the scope rides as in images.list; the facet map comes
    /// back whole (total, rating[], color_tag[], kind[], captured, camera[],
    /// lens[], keyword[], edited, undescribed).
    Reply<QVariantMap> imagesFacets(const QString& library, const QVariantMap& scope = {});

    /// meta.set on exactly one of variants (or images, whose first variant is
    /// meant); both or neither is refused before the wire.
    Reply<bool> metaSet(const QString& library, const QStringList& variants,
                        MetaValue rating = {}, MetaValue colorTag = {},
                        const QStringList& images = {});
    /// images.add: an item is a path string or {path, metadata};
    /// `recentImportAt` (a UTC ISO 8601 stamp) backdates the import's album
    /// — the 1.x-sidecar adoption path.
    Reply<AddResult> imagesAdd(const QString& library, const QVariantList& items,
                               const QString& mode = QLatin1String("reference"),
                               const QString& collection = {},
                               const QString& recentImportAt = {});
    Reply<bool> imagesTrash(const QString& library, const QStringList& images, MetaValue trashed = {});
    Reply<RemoveResult> imagesRemove(const QString& library, const QStringList& images);

    Reply<QString> collectionCreate(const QString& library, const QString& kind, const QString& name,
                                    const QString& parent = {}, const QJsonObject& criteria = {});
    Reply<bool> collectionUpdate(const QString& library, const QString& collection, const QString& name);
    Reply<bool> collectionDelete(const QString& library, const QString& collection,
                                 bool recursive = false);
    Reply<int> collectionAdd(const QString& library, const QString& collection, const QStringList& images);
    Reply<int> collectionRemove(const QString& library, const QString& collection,
                                const QStringList& images);

    Reply<QVariantMap> foldersAdd(const QString& library, const QString& path, bool recursive = true,
                                  const QStringList& skip = {});
    Reply<std::vector<QVariantMap>> foldersList(const QString& library);
    Reply<QVariantMap> foldersSync(const QString& library, const QString& folder = {});
    Reply<bool> foldersRemove(const QString& library, const QString& folder);

    Reply<QVariantMap> sessionFolders(const QString& library);
    Reply<QVariantMap> sessionScan(const QString& library);
    Reply<QVariantMap> sessionMove(const QString& library, const QStringList& images, const QString& to,
                                   const QString& onConflict = {});
    Reply<QVariantMap> sessionRestore(const QString& library, const QStringList& images,
                                      const QString& onConflict = {});
    Reply<std::vector<QVariantMap>> sessionTrashPlan(const QString& library);
    Reply<QVariantMap> sessionTrashCommit(const QString& library, const QStringList& removedPaths);

    Reply<EditState> editGet(const QString& library, const QString& variant);
    Reply<EditWrite> editPut(const QString& library, const QString& variant, const QJsonObject& edit,
                             int baseRevision);
    Reply<bool> editDelete(const QString& library, const QString& variant);

    /// The failure reason from start(), for UI copy.
    const QString& lastError() const { return error_; }

private:
    /// One typed request: build, send, wait, and split the reply into the
    /// value or the helper's error envelope.
    template <typename T, typename Parse>
    Reply<T> callTyped(const QString& method, const QJsonObject& params, Parse parse) {
        Reply<T> reply;
        const auto parsed = waitForReply(nextId_, method, params, options_.callTimeout);
        if (!parsed) return reply; // transport failure: an empty code says so
        if (!parsed->isResult) {
            reply.errorCode = parsed->errorCode;
            reply.errorMessage = parsed->errorMessage;
            reply.errorData = parsed->errorData;
            return reply;
        }
        reply.value = parse(parsed->result);
        reply.ok = true;
        return reply;
    }

    void writeLine(const QByteArray& line);
    void handleLine(const QByteArray& line);
    void fail(const QString& why);

    std::unique_ptr<QProcess> process_;
    protocol::LineSplitter splitter_; // stdout reassembly across pipe reads
    protocol::SdkVersion sdk_;
    Options options_;
    QString error_;
    qint64 nextId_ = 1;
    // id -> the handler call() registered, and replies handleLine has routed
    // before a waiting loop got to consume them.
    std::map<qint64, std::function<void(const protocol::ParsedReply&)>> handlers_;
    std::map<qint64, protocol::ParsedReply> pending_;
};

} // namespace spk::desktop::library
