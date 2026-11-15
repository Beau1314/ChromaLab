#include "LibraryHost.hpp"

#include <QDateTime>
#include <QJsonParseError>
#include <QJsonValue>
#include <cstdio>

namespace spk::desktop::library {

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

} // namespace spk::desktop::library
