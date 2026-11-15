#include "LibraryHost.hpp"

#include <QDateTime>
#include <QJsonParseError>
#include <QJsonValue>
#include <cstdio>

namespace spk::desktop::library {

namespace {

QJsonArray stringsArray(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values) array.append(value);
    return array;
}

} // namespace

namespace protocol {

ReadyEvent parseReadyEvent(const QJsonDocument& document) {
    ReadyEvent event;
    if (!document.isObject()) return event;
    const QJsonObject object = document.object();
    if (object.value(QLatin1String("event")).toString() != QLatin1String("ready")) return event;
    if (object.value(QLatin1String("protocol")).toInt() != kProtocolVersion) return event;
    event.valid = true;
    event.protocol = kProtocolVersion;
    event.version = object.value(QLatin1String("version")).toString();
    return event;
}

QByteArray requestLine(qint64 id, const QString& method, const QJsonObject& params) {
    QJsonObject request;
    request.insert(QLatin1String("id"), static_cast<double>(id));
    request.insert(QLatin1String("method"), method);
    request.insert(QLatin1String("params"), params);
    // QJsonDocument's compact form; the helper reads one JSON value per line,
    // so no newline belongs inside the bytes (writeLine adds it).
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

ParsedReply parseReply(const QJsonDocument& document, qint64 requestId) {
    ParsedReply reply;
    if (!document.isObject()) return reply;
    const QJsonObject object = document.object();
    const QJsonValue idValue = object.value(QLatin1String("id"));
    if (!idValue.isDouble()) return reply; // every envelope carries an id
    const qint64 id = static_cast<qint64>(idValue.toDouble());
    reply.valid = true;
    reply.routed = (id == requestId);
    const QJsonValue result = object.value(QLatin1String("result"));
    const QJsonValue error = object.value(QLatin1String("error"));
    if (result.isObject()) {
        reply.isResult = true;
        reply.result = result.toObject();
    } else if (error.isObject()) {
        const QJsonObject envelope = error.toObject();
        reply.errorCode = envelope.value(QLatin1String("code")).toString();
        reply.errorMessage = envelope.value(QLatin1String("message")).toString();
        reply.errorData = envelope.value(QLatin1String("data"));
    }
    return reply;
}

SdkVersion parseSdkVersion(const QJsonObject& result) {
    SdkVersion version;
    const QJsonValue abi = result.value(QLatin1String("abi"));
    if (!abi.isDouble()) return version;
    version.valid = true;
    version.version = result.value(QLatin1String("version")).toString();
    version.abi = abi.toInt();
    version.schema = result.value(QLatin1String("schema")).toInt();
    return version;
}

// --- the typed layer's wire params -------------------------------------------

QJsonObject imagesListParams(const QVariantMap& query) {
    // The spec's own keys, verbatim: the app renames nothing and default-
    // fills nothing — absence is the helper's default.
    return QJsonObject::fromVariantMap(query);
}

QJsonObject metaSetParams(const QStringList& variants, const QStringList& images,
                          MetaValue rating, MetaValue colorTag) {
    QJsonObject params;
    if (!variants.isEmpty()) params.insert(QLatin1String("variants"), stringsArray(variants));
    if (!images.isEmpty()) params.insert(QLatin1String("images"), stringsArray(images));
    const auto write = [&params](const char* key, MetaValue value) {
        if (value.isNumber()) params.insert(QLatin1String(key), value.number);
        else if (value.isNull()) params.insert(QLatin1String(key), QJsonValue::Null);
    };
    write("rating", rating);
    write("color_tag", colorTag);
    return params;
}

QJsonObject imagesTrashParams(const QStringList& images, TrashFlag trashed) {
    QJsonObject params;
    params.insert(QLatin1String("images"), stringsArray(images));
    // A JSON boolean or null, never a number: the helper validates the type
    // and refuses a 1 or a 0 (its set_trashed wants is_boolean() || is_null()).
    if (trashed.isTrue()) params.insert(QLatin1String("trashed"), true);
    else if (trashed.isNull()) params.insert(QLatin1String("trashed"), QJsonValue::Null);
    else params.insert(QLatin1String("trashed"), false);
    return params;
}

QJsonObject imagesAddParams(const QVariantList& items, const QString& mode,
                            const QString& collection, const QString& recentImportAt) {
    QJsonObject params;
    QJsonArray wireItems;
    for (const QVariant& item : items) {
        if (item.typeId() == QMetaType::QString) wireItems.append(item.toString());
        else wireItems.append(QJsonValue::fromVariant(item)); // {path, metadata{...}}
    }
    params.insert(QLatin1String("items"), wireItems);
    params.insert(QLatin1String("mode"), mode.isEmpty() ? QLatin1String("reference") : mode);
    if (!collection.isEmpty()) params.insert(QLatin1String("collection"), collection);
    if (!recentImportAt.isEmpty()) params.insert(QLatin1String("recent_import_at"), recentImportAt);
    return params;
}

QJsonObject collectionCreateParams(const QString& kind, const QString& name,
                                   const QString& parent, const QJsonObject& criteria) {
    QJsonObject params;
    params.insert(QLatin1String("kind"), kind);
    params.insert(QLatin1String("name"), name);
    if (!parent.isEmpty()) params.insert(QLatin1String("parent"), parent);
    if (!criteria.isEmpty()) params.insert(QLatin1String("criteria"), criteria);
    return params;
}

QJsonObject editPutParams(const QString& variant, const QJsonObject& edit, int baseRevision) {
    QJsonObject params;
    params.insert(QLatin1String("variant"), variant);
    params.insert(QLatin1String("edit"), edit);
    params.insert(QLatin1String("base_revision"), baseRevision);
    return params;
}

QJsonObject sessionMoveParams(const QStringList& images, const QString& to, const QString& onConflict) {
    QJsonObject params;
    params.insert(QLatin1String("images"), stringsArray(images));
    params.insert(QLatin1String("to"), to);
    if (!onConflict.isEmpty()) params.insert(QLatin1String("on_conflict"), onConflict);
    return params;
}

QJsonObject foldersAddParams(const QString& path, bool recursive, const QStringList& skip) {
    QJsonObject params;
    params.insert(QLatin1String("path"), path);
    params.insert(QLatin1String("recursive"), recursive);
    if (!skip.isEmpty()) params.insert(QLatin1String("skip"), stringsArray(skip));
    return params;
}

// --- the typed layer's result parsers ----------------------------------------

SourceInfo parseSourceInfo(const QJsonObject& result) {
    SourceInfo info;
    info.raw = result.toVariantMap();
    info.library = result.value(QLatin1String("library")).toString();
    if (info.library.isEmpty()) return info;
    const QJsonObject body = result.value(QLatin1String("info")).toObject();
    info.valid = !body.isEmpty();
    info.uuid = body.value(QLatin1String("uuid")).toString();
    info.kind = body.value(QLatin1String("kind")).toString();
    info.name = body.value(QLatin1String("name")).toString();
    info.path = body.value(QLatin1String("path")).toString();
    const QJsonObject counts = body.value(QLatin1String("counts")).toObject();
    const QJsonValue images = counts.value(QLatin1String("images"));
    const QJsonValue variants = counts.value(QLatin1String("variants"));
    const QJsonValue edits = counts.value(QLatin1String("edits"));
    if (images.isDouble()) info.images = images.toInt();
    if (variants.isDouble()) info.variants = variants.toInt();
    if (edits.isDouble()) info.edits = edits.toInt();
    const QJsonValue copied = body.value(QLatin1String("copied_from"));
    if (copied.isObject()) info.copiedFrom = copied.toObject().toVariantMap();
    return info;
}

ImagePage parseImagePage(const QJsonObject& result) {
    ImagePage page;
    page.total = result.value(QLatin1String("total")).toInt(-1);
    page.offset = result.value(QLatin1String("offset")).toInt(-1);
    const QJsonArray images = result.value(QLatin1String("images")).toArray();
    for (const QJsonValue& entry : images) {
        if (!entry.isObject()) continue;
        ImageRow row;
        row.map = entry.toObject().toVariantMap();
        row.valid = !row.uuid().isEmpty();
        page.images.push_back(std::move(row));
    }
    return page;
}

EditState parseEditState(const QJsonObject& result) {
    EditState state;
    const QJsonValue revision = result.value(QLatin1String("revision"));
    if (!revision.isDouble()) return state;
    state.valid = true;
    state.revision = revision.toInt();
    const QJsonValue edit = result.value(QLatin1String("edit"));
    state.hasEdit = edit.isObject();
    if (state.hasEdit) state.edit = edit.toObject();
    state.modifiedAt = result.value(QLatin1String("modified_at")).toString();
    return state;
}

EditWrite parseEditWrite(const QJsonObject& result) {
    EditWrite write;
    const QJsonValue revision = result.value(QLatin1String("revision"));
    if (!revision.isDouble()) return write;
    write.valid = true;
    write.revision = revision.toInt();
    write.modifiedAt = result.value(QLatin1String("modified_at")).toString();
    return write;
}

AddResult parseAddResult(const QJsonObject& result) {
    AddResult added;
    added.added = result.value(QLatin1String("added")).toInt(-1);
    const QJsonArray results = result.value(QLatin1String("results")).toArray();
    for (const QJsonValue& entry : results) {
        const QJsonObject object = entry.toObject();
        AddResult::Row row;
        row.path = object.value(QLatin1String("path")).toString();
        row.image = object.value(QLatin1String("image")).toString();
        row.variant = object.value(QLatin1String("variant")).toString();
        row.existing = object.value(QLatin1String("existing")).toBool();
        row.storedAt = object.value(QLatin1String("stored_at")).toString();
        row.error = object.value(QLatin1String("error")).toString();
        added.rows.push_back(std::move(row));
    }
    added.recentImport = result.value(QLatin1String("recent_import")).toString();
    return added;
}

RemoveResult parseRemoveResult(const QJsonObject& result) {
    RemoveResult removed;
    removed.removed = result.value(QLatin1String("removed")).toInt(-1);
    const QJsonArray refused = result.value(QLatin1String("refused")).toArray();
    for (const QJsonValue& entry : refused) {
        const QJsonObject object = entry.toObject();
        removed.refused.push_back({object.value(QLatin1String("image")).toString(),
                                   object.value(QLatin1String("code")).toString()});
    }
    return removed;
}

} // namespace protocol

namespace {

/// Read stdout until one whole line arrives or the budget runs out. A helper
/// that hangs at startup must fail start(), not the app.
std::optional<QByteArray> readLine(QProcess& process, std::chrono::milliseconds timeout) {
    QByteArray buffer;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() +
                            static_cast<qint64>(timeout.count());
    for (;;) {
        const qint64 index = buffer.indexOf('\n');
        if (index >= 0) return buffer.left(index); // a stray CR is JSON-safe
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now >= deadline) return std::nullopt;
        if (!process.waitForReadyRead(static_cast<int>(deadline - now))) {
            if (process.state() != QProcess::Running) return std::nullopt;
            continue;
        }
        buffer += process.readAllStandardOutput();
    }
}

} // namespace

LibraryHost::LibraryHost(QObject* parent) : QObject(parent) {}

LibraryHost::~LibraryHost() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(3000);
    }
}

std::optional<QString> LibraryHost::start(const Options& options) {
    options_ = options;
    error_.clear();
    process_ = std::make_unique<QProcess>(this);
    process_->setProgram(options.helperPath);
    QStringList arguments;
    if (!options.storePath.isEmpty()) {
        arguments << QStringLiteral("--store") << options.storePath;
    }
    arguments << options.helperArguments;
    process_->setArguments(arguments);
    splitter_.reset(); // a fresh child must not inherit the last one's tail
    process_->start();
    if (!process_->waitForStarted(5000)) {
        fail(QStringLiteral("cannot start %1: %2").arg(options.helperPath, process_->errorString()));
        return error_;
    }
    // 1. the ready event, the first line the helper writes
    const auto line = readLine(*process_, options_.startTimeout);
    if (!line) {
        fail(QStringLiteral("no ready event within %1 ms").arg(options_.startTimeout.count()));
        return error_;
    }
    const protocol::ReadyEvent event =
        protocol::parseReadyEvent(QJsonDocument::fromJson(*line));
    if (!event.valid) {
        fail(QStringLiteral("the helper's first line is not a protocol-%1 ready event")
                 .arg(protocol::kProtocolVersion));
        return error_;
    }
    // 2. the abi gate: refuse before any library call
    const auto version =
        waitForReply(nextId_, QStringLiteral("sdk.version"), {}, options_.callTimeout);
    if (!version) {
        fail(QStringLiteral("sdk.version timed out"));
        return error_;
    }
    if (!version->isResult) {
        fail(QStringLiteral("sdk.version failed: %1 (%2)")
                 .arg(version->errorCode, version->errorMessage));
        return error_;
    }
    sdk_ = protocol::parseSdkVersion(version->result);
    if (!sdk_.valid || sdk_.abi != protocol::kSupportedAbi) {
        fail(QStringLiteral("unsupported helper abi %1 (want %2)")
                 .arg(sdk_.abi)
                 .arg(protocol::kSupportedAbi));
        return error_;
    }
    return std::nullopt;
}

void LibraryHost::shutdown() {
    if (!process_ || process_->state() != QProcess::Running) return;
    writeLine(protocol::requestLine(nextId_++, QStringLiteral("host.shutdown")));
    if (!process_->waitForFinished(5000)) {
        process_->terminate();
        if (!process_->waitForFinished(2000)) process_->kill();
    }
}

void LibraryHost::writeLine(const QByteArray& line) {
    if (!process_) return;
    process_->write(line + '\n');
    process_->waitForBytesWritten(2000);
}

qint64 LibraryHost::call(const QString& method, const QJsonObject& params,
                         std::function<void(const protocol::ParsedReply&)> handler) {
    if (!isRunning()) return -1;
    const qint64 id = nextId_++;
    if (handler) handlers_[id] = std::move(handler);
    writeLine(protocol::requestLine(id, method, params));
    return id;
}

std::optional<protocol::ParsedReply> LibraryHost::waitForReply(
    qint64 id, const QString& method, const QJsonObject& params,
    std::chrono::milliseconds timeout) {
    if (!isRunning()) return std::nullopt;
    const qint64 deadline =
        QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(timeout.count());
    const qint64 sentId = call(method, params);
    if (sentId != id) {
        // The caller's id is a promise about the wire; the shell numbers the
        // requests itself, so a mismatch means a misuse worth catching in tests.
        return std::nullopt;
    }
    for (;;) {
        const auto delivered = pending_.extract(id);
        if (delivered) return delivered.mapped();
        if (!isRunning()) return std::nullopt;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now >= deadline) return std::nullopt;
        if (!process_->waitForReadyRead(static_cast<int>(deadline - now))) continue;
        // One pipe read is not one line: a chunk can end mid-line or hold
        // several (the Mac LineSplitter's exact concern). Reassemble before
        // parsing — feeding fragments to handleLine drops a split reply as
        // undecodable and the call times out even though the helper answered.
        for (const QByteArray& line : splitter_.feed(process_->readAllStandardOutput()))
            handleLine(line);
    }
}

void LibraryHost::handleLine(const QByteArray& line) {
    if (line.trimmed().isEmpty()) return;
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        std::fprintf(stderr, "spklib-host: undecodable line: %s\n", line.constData());
        return;
    }
    const qint64 id = static_cast<qint64>(
        document.object().value(QLatin1String("id")).toDouble(-1));
    const auto handler = handlers_.find(id);
    if (handler == handlers_.end()) {
        // No handler: only waitForReply is waiting, and it has not consumed
        // this id yet — park the reply where its loop will find it.
        pending_[id] = protocol::parseReply(document, id);
        return;
    }
    const auto taken = handlers_.extract(handler);
    taken.mapped()(protocol::parseReply(document, id));
}

void LibraryHost::fail(const QString& why) {
    error_ = why;
    std::fprintf(stderr, "LibraryHost: %s\n", why.toUtf8().constData());
}

// --- the typed layer ----------------------------------------------------------

Reply<SourceInfo> LibraryHost::libraryCreate(const QString& path, const QString& kind,
                                             const QString& name) {
    QJsonObject params;
    params.insert(QLatin1String("path"), path);
    params.insert(QLatin1String("kind"), kind);
    if (!name.isEmpty()) params.insert(QLatin1String("name"), name);
    return callTyped<SourceInfo>(QStringLiteral("library.create"), params, protocol::parseSourceInfo);
}

Reply<SourceInfo> LibraryHost::libraryOpen(const QString& path) {
    QJsonObject params;
    params.insert(QLatin1String("path"), path);
    return callTyped<SourceInfo>(QStringLiteral("library.open"), params, protocol::parseSourceInfo);
}

Reply<bool> LibraryHost::libraryClose(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<bool>(QStringLiteral("library.close"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("closed")).toBool();
                           });
}

Reply<QVariantMap> LibraryHost::libraryInfo(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("library.info"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<QVariantMap> LibraryHost::librarySync(const QString& library, bool force) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    if (force) params.insert(QLatin1String("force"), true);
    return callTyped<QVariantMap>(QStringLiteral("library.sync"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<std::vector<QVariantMap>> LibraryHost::collectionsList(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<std::vector<QVariantMap>>(
        QStringLiteral("collections.list"), params, [](const QJsonObject& result) {
            std::vector<QVariantMap> rows;
            const QJsonArray collections = result.value(QLatin1String("collections")).toArray();
            for (const QJsonValue& entry : collections)
                if (entry.isObject()) rows.push_back(entry.toObject().toVariantMap());
            return rows;
        });
}

Reply<ImagePage> LibraryHost::imagesList(const QString& library, const QVariantMap& query) {
    QJsonObject params = protocol::imagesListParams(query);
    params.insert(QLatin1String("library"), library);
    return callTyped<ImagePage>(QStringLiteral("images.list"), params, protocol::parseImagePage);
}

Reply<QVariantMap> LibraryHost::imageGet(const QString& library, const QString& image) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("image"), image);
    return callTyped<QVariantMap>(QStringLiteral("image.get"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<std::vector<QVariantMap>> LibraryHost::imagesLookup(const QString& library,
                                                          const QStringList& paths) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("paths"), stringsArray(paths));
    return callTyped<std::vector<QVariantMap>>(
        QStringLiteral("images.lookup"), params, [](const QJsonObject& result) {
            std::vector<QVariantMap> results;
            const QJsonArray entries = result.value(QLatin1String("results")).toArray();
            for (const QJsonValue& entry : entries)
                if (entry.isObject()) results.push_back(entry.toObject().toVariantMap());
            return results;
        });
}

Reply<QVariantMap> LibraryHost::imagesFacets(const QString& library, const QVariantMap& scope) {
    QJsonObject params = protocol::imagesListParams(scope);
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("images.facets"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<bool> LibraryHost::metaSet(const QString& library, const QStringList& variants,
                                 MetaValue rating, MetaValue colorTag, const QStringList& images) {
    if (variants.isEmpty() == images.isEmpty()) {
        // A caller bug, not a helper answer: refuse before the wire.
        Reply<bool> refused;
        refused.errorCode = QStringLiteral("invalid_params");
        refused.errorMessage = QStringLiteral("meta.set takes exactly one of variants/images");
        return refused;
    }
    QJsonObject params = protocol::metaSetParams(variants, images, rating, colorTag);
    params.insert(QLatin1String("library"), library);
    return callTyped<bool>(QStringLiteral("meta.set"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("updated")).toBool();
                           });
}

Reply<AddResult> LibraryHost::imagesAdd(const QString& library, const QVariantList& items,
                                        const QString& mode, const QString& collection,
                                        const QString& recentImportAt) {
    QJsonObject params = protocol::imagesAddParams(items, mode, collection, recentImportAt);
    params.insert(QLatin1String("library"), library);
    return callTyped<AddResult>(QStringLiteral("images.add"), params, protocol::parseAddResult);
}

Reply<bool> LibraryHost::imagesTrash(const QString& library, const QStringList& images,
                                     TrashFlag trashed) {
    QJsonObject params = protocol::imagesTrashParams(images, trashed);
    params.insert(QLatin1String("library"), library);
    return callTyped<bool>(QStringLiteral("images.trash"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("updated")).toBool();
                           });
}

Reply<RemoveResult> LibraryHost::imagesRemove(const QString& library, const QStringList& images) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("images"), stringsArray(images));
    return callTyped<RemoveResult>(QStringLiteral("images.remove"), params,
                                   protocol::parseRemoveResult);
}

Reply<QString> LibraryHost::collectionCreate(const QString& library, const QString& kind,
                                             const QString& name, const QString& parent,
                                             const QJsonObject& criteria) {
    QJsonObject params =
        protocol::collectionCreateParams(kind, name, parent, criteria);
    params.insert(QLatin1String("library"), library);
    return callTyped<QString>(QStringLiteral("collection.create"), params,
                              [](const QJsonObject& result) {
                                  return result.value(QLatin1String("collection")).toString();
                              });
}

Reply<bool> LibraryHost::collectionUpdate(const QString& library, const QString& collection,
                                          const QString& name) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("collection"), collection);
    params.insert(QLatin1String("name"), name);
    return callTyped<bool>(QStringLiteral("collection.update"), params, [](const QJsonObject&) {
        return true; // the spec's result is the empty object
    });
}

Reply<bool> LibraryHost::collectionDelete(const QString& library, const QString& collection,
                                          bool recursive) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("collection"), collection);
    if (recursive) params.insert(QLatin1String("recursive"), true);
    return callTyped<bool>(QStringLiteral("collection.delete"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("deleted")).toBool();
                           });
}

Reply<int> LibraryHost::collectionAdd(const QString& library, const QString& collection,
                                      const QStringList& images) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("collection"), collection);
    params.insert(QLatin1String("images"), stringsArray(images));
    return callTyped<int>(QStringLiteral("collection.add"), params,
                          [](const QJsonObject& result) {
                              return result.value(QLatin1String("added")).toInt(-1);
                          });
}

Reply<int> LibraryHost::collectionRemove(const QString& library, const QString& collection,
                                         const QStringList& images) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("collection"), collection);
    params.insert(QLatin1String("images"), stringsArray(images));
    return callTyped<int>(QStringLiteral("collection.remove"), params,
                          [](const QJsonObject& result) {
                              return result.value(QLatin1String("removed")).toInt(-1);
                          });
}

Reply<QVariantMap> LibraryHost::foldersAdd(const QString& library, const QString& path,
                                           bool recursive, const QStringList& skip) {
    QJsonObject params = protocol::foldersAddParams(path, recursive, skip);
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("folders.add"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<std::vector<QVariantMap>> LibraryHost::foldersList(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<std::vector<QVariantMap>>(
        QStringLiteral("folders.list"), params, [](const QJsonObject& result) {
            std::vector<QVariantMap> rows;
            const QJsonArray folders = result.value(QLatin1String("folders")).toArray();
            for (const QJsonValue& entry : folders)
                if (entry.isObject()) rows.push_back(entry.toObject().toVariantMap());
            return rows;
        });
}

Reply<QVariantMap> LibraryHost::foldersSync(const QString& library, const QString& folder) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    if (!folder.isEmpty()) params.insert(QLatin1String("folder"), folder);
    return callTyped<QVariantMap>(QStringLiteral("folders.sync"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<bool> LibraryHost::foldersRemove(const QString& library, const QString& folder) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("folder"), folder);
    return callTyped<bool>(QStringLiteral("folders.remove"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("removed")).toBool();
                           });
}

Reply<QVariantMap> LibraryHost::sessionFolders(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("session.folders"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<QVariantMap> LibraryHost::sessionScan(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("session.scan"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<QVariantMap> LibraryHost::sessionMove(const QString& library, const QStringList& images,
                                            const QString& to, const QString& onConflict) {
    QJsonObject params = protocol::sessionMoveParams(images, to, onConflict);
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("session.move"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<QVariantMap> LibraryHost::sessionRestore(const QString& library, const QStringList& images,
                                               const QString& onConflict) {
    QJsonObject params;
    params.insert(QLatin1String("images"), stringsArray(images));
    if (!onConflict.isEmpty()) params.insert(QLatin1String("on_conflict"), onConflict);
    params.insert(QLatin1String("library"), library);
    return callTyped<QVariantMap>(QStringLiteral("session.restore"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<std::vector<QVariantMap>> LibraryHost::sessionTrashPlan(const QString& library) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    return callTyped<std::vector<QVariantMap>>(
        QStringLiteral("session.trash.plan"), params, [](const QJsonObject& result) {
            std::vector<QVariantMap> rows;
            const QJsonArray files = result.value(QLatin1String("files")).toArray();
            for (const QJsonValue& entry : files)
                if (entry.isObject()) rows.push_back(entry.toObject().toVariantMap());
            return rows;
        });
}

Reply<QVariantMap> LibraryHost::sessionTrashCommit(const QString& library,
                                                   const QStringList& removedPaths) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("removed_paths"), stringsArray(removedPaths));
    return callTyped<QVariantMap>(QStringLiteral("session.trash.commit"), params,
                                  [](const QJsonObject& result) { return result.toVariantMap(); });
}

Reply<EditState> LibraryHost::editGet(const QString& library, const QString& variant) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("variant"), variant);
    return callTyped<EditState>(QStringLiteral("edit.get"), params, protocol::parseEditState);
}

Reply<EditWrite> LibraryHost::editPut(const QString& library, const QString& variant,
                                      const QJsonObject& edit, int baseRevision) {
    QJsonObject params = protocol::editPutParams(variant, edit, baseRevision);
    params.insert(QLatin1String("library"), library);
    return callTyped<EditWrite>(QStringLiteral("edit.put"), params, protocol::parseEditWrite);
}

Reply<bool> LibraryHost::editDelete(const QString& library, const QString& variant) {
    QJsonObject params;
    params.insert(QLatin1String("library"), library);
    params.insert(QLatin1String("variant"), variant);
    return callTyped<bool>(QStringLiteral("edit.delete"), params,
                           [](const QJsonObject& result) {
                               return result.value(QLatin1String("deleted")).toBool();
                           });
}

} // namespace spk::desktop::library
