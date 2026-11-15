#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <chrono>
#include <functional>
#include <map>
#include <optional>

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

} // namespace protocol

class LibraryHost : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString helperPath; // spklib-host(.exe)
        QString storePath;  // --store DIR, the libraries root
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

    /// The failure reason from start(), for UI copy.
    const QString& lastError() const { return error_; }

private:
    void writeLine(const QByteArray& line);
    void handleLine(const QByteArray& line);
    void fail(const QString& why);

    std::unique_ptr<QProcess> process_;
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
