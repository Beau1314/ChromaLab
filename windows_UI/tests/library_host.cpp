// The library helper's wire, at two tiers. The protocol half is pure JSON in
// and JSON out — request lines, reply envelopes, the ready event, the abi
// gate, id routing — with no QProcess anywhere. The host half is the smoke
// gate: it runs only when a built spklib-host is passed as argv[1] (the
// build points it at the local spklib checkout's build/spklib-host.exe);
// without one the protocol tier still compiles and runs, so the gate can
// never masquerade as broken when it merely has no helper to talk to.
#include "LibraryHost.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>

namespace host = spk::desktop::library;
namespace protocol = spk::desktop::library::protocol;

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
    } catch (const std::exception&) {
        ++checks;
        return;
    }
    throw std::runtime_error(message);
}

// --- the protocol tier -------------------------------------------------------

void testReadyEvent() {
    // Plain string literals: QByteArrayLiteral's macro does not survive the
    // commas inside a raw-string JSON payload.
    const QByteArray readyLine =
        R"json({"event":"ready","version":"spklib 0.4.0 (abi 1, schema 3)","protocol":1})json";
    const auto good = protocol::parseReadyEvent(QJsonDocument::fromJson(readyLine));
    check(good.valid, "a correct ready event must parse");
    check(good.protocol == 1, "ready protocol is 1");
    check(good.version.startsWith(QLatin1String("spklib ")), "ready carries the sdk version");

    check(!protocol::parseReadyEvent(QJsonDocument::fromJson(
               QByteArray(R"json({"id":7,"result":{"pong":true}})json")))
               .valid,
           "a reply line is not a ready event");
    check(!protocol::parseReadyEvent(QJsonDocument::fromJson(
               QByteArray(R"json({"event":"ready","protocol":2})json")))
               .valid,
           "a ready event of another protocol is refused");
    check(!protocol::parseReadyEvent(QJsonDocument::fromJson(
               QByteArray(R"json({"event":"ready"})json")))
               .valid,
           "a ready event without a protocol is refused");
    check(!protocol::parseReadyEvent(QJsonDocument()).valid,
           "an undecodable first line is refused");
}

void testRequestLine() {
    const QByteArray line = protocol::requestLine(7, QStringLiteral("images.list"),
                                                  QJsonObject{{QLatin1String("limit"), 50}});
    const QJsonObject request = QJsonDocument::fromJson(line).object();
    check(request.value(QLatin1String("id")).toDouble() == 7.0, "requests carry the id");
    check(request.value(QLatin1String("method")).toString() == QLatin1String("images.list"),
          "requests carry the method");
    check(request.value(QLatin1String("params")).isObject(),
          "params is always present, even when empty");
    check(!line.contains('\n'), "a request line carries no newline of its own");
    check(protocol::requestLine(1, QStringLiteral("sdk.version")).size() > 0,
          "the empty-params form builds");
}

void testReplyRouting() {
    // result envelope
    const auto ok = protocol::parseReply(QJsonDocument::fromJson(QByteArray(
                                             R"json({"id":7,"result":{"pong":true}})json")),
                                         7);
    check(ok.valid && ok.routed && ok.isResult, "a result envelope routes to its request");
    check(ok.result.value(QLatin1String("pong")).toBool(), "the result body survives");

    // error envelope with the spec's codes
    const auto error = protocol::parseReply(QJsonDocument::fromJson(QByteArray(
                                                R"json({"id":8,"error":{"code":"read_only","message":"Capture One's albums are read-only","data":null}})json")),
                                            8);
    check(error.valid && error.routed && !error.isResult, "an error envelope routes to its request");
    check(error.errorCode == QLatin1String("read_only"), "the error code survives");
    check(protocol::isKnownErrorCode(error.errorCode), "read_only is in the spec's code set");
    check(!error.errorMessage.isEmpty(), "the error message survives");

    // id routing: a reply answering another request parses but does not route
    const auto stale = protocol::parseReply(QJsonDocument::fromJson(QByteArray(
                                                R"json({"id":9,"result":{}})json")),
                                            7);
    check(stale.valid && !stale.routed, "another request's reply parses but does not route");

    // shapes that must never route
    check(!protocol::parseReply(QJsonDocument::fromJson(QByteArray(R"json({"result":{}})json")), 1)
               .valid,
           "an envelope without an id is invalid");
    check(!protocol::parseReply(QJsonDocument(), 1).valid, "garbage on the wire is invalid");
    check(!protocol::parseReply(QJsonDocument::fromJson(QByteArray(R"("string")")), 1)
               .valid,
           "a non-object envelope is invalid");
}

void testErrorCodes() {
    for (const char* code : {"not_found", "invalid_params", "read_only", "conflict",
                             "source_busy", "unsupported", "exists", "in_use",
                             "ambiguous", "cross_volume", "io", "internal",
                             "unknown_method", "last_variant"}) {
        check(protocol::isKnownErrorCode(QLatin1String(code)),
              (std::string("the spec's code is known: ") + code).c_str());
    }
    // A newer helper may invent codes; they must survive verbatim, not
    // collapse into "internal" — the app decides what to do with them.
    check(!protocol::isKnownErrorCode(QLatin1String("quota_exceeded")),
          "an invented code is not claimed as known");
}

void testAbiGate() {
    const auto good = protocol::parseSdkVersion(QJsonObject{
        {QLatin1String("version"), QLatin1String("0.4.0")},
        {QLatin1String("abi"), 1},
        {QLatin1String("schema"), 3},
        {QLatin1String("capture_one"),
         QJsonObject{{QLatin1String("write_back"), false}, {QLatin1String("previews"), false}}}});
    check(good.valid && good.abi == protocol::kSupportedAbi && good.schema == 3,
          "sdk.version 0.4.0 parses and is supported");
    const auto future = protocol::parseSdkVersion(
        QJsonObject{{QLatin1String("abi"), 2}, {QLatin1String("schema"), 4}});
    check(future.valid && future.abi != protocol::kSupportedAbi,
          "a future abi parses but does not match the supported one");
    check(!protocol::parseSdkVersion(QJsonObject{{QLatin1String("abi"), QStringLiteral("one")}})
               .valid,
          "a non-numeric abi is invalid");
    check(!protocol::parseSdkVersion(QJsonObject{}).valid, "a missing abi is invalid");
}

// --- the host tier: the smoke gate -------------------------------------------

/// The one handshake phase 4 is built on: spawn, ready, abi, a real catalog
/// round-trip, shutdown.
void testHostSmoke(const QString& helperPath) {
    QTemporaryDir store;
    check(store.isValid(), "temporary store unavailable");
    host::LibraryHost helper;
    const auto failure = helper.start({helperPath, store.path()});
    check(!failure.has_value(), (std::string("helper start: ") + failure.value_or("?").toStdString()).c_str());
    check(helper.isRunning(), "the helper keeps running after the handshake");
    check(helper.sdkVersion().abi == protocol::kSupportedAbi, "the abi gate passed");

    // create a real temp catalog, then look at it
    const QString catalog = store.filePath(QStringLiteral("冒烟测试.spektralab"));
    std::optional<protocol::ParsedReply> reply = helper.waitForReply(
        2, QStringLiteral("library.create"),
        QJsonObject{{QLatin1String("path"), catalog},
                    {QLatin1String("kind"), QLatin1String("catalog")},
                    {QLatin1String("name"), QLatin1String("Smoke")}},
        std::chrono::milliseconds(30000));
    check(reply.has_value(), "library.create got a reply");
    check(reply->isResult && reply->result.value(QLatin1String("library")).toString() == QLatin1String("L1"),
          "library.create returns the first handle");
    const QString library = reply->result.value(QLatin1String("library")).toString();

    reply = helper.waitForReply(3, QStringLiteral("collections.list"),
                                QJsonObject{{QLatin1String("library"), library}},
                                std::chrono::milliseconds(30000));
    check(reply.has_value() && reply->isResult, "collections.list got a result");
    check(reply->result.value(QLatin1String("collections")).toArray().size() >= 5,
          "a fresh catalog carries the five fixed rows");

    reply = helper.waitForReply(4, QStringLiteral("no.such.method"), {},
                                std::chrono::milliseconds(10000));
    check(reply.has_value() && !reply->isResult, "an unknown method gets an error envelope");
    check(reply->errorCode == QLatin1String("unknown_method"),
          "the error envelope names unknown_method");

    reply = helper.waitForReply(5, QStringLiteral("library.close"),
                                QJsonObject{{QLatin1String("library"), library}},
                                std::chrono::milliseconds(10000));
    check(reply.has_value() && reply->isResult &&
              reply->result.value(QLatin1String("closed")).toBool(),
          "library.close closes the handle");

    helper.shutdown();
    check(!helper.isRunning(), "host.shutdown ends the helper");
    std::printf("host smoke: sdk=%s collections+create+close+shutdown all round-tripped\n",
                helper.sdkVersion().version.toUtf8().constData());
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testReadyEvent();
        testRequestLine();
        testReplyRouting();
        testErrorCodes();
        testAbiGate();
        if (argc > 1 && QFileInfo(argv[1]).isFile()) {
            testHostSmoke(QString::fromUtf8(argv[1]));
        } else {
            // Compile-level gate: no helper binary was handed to this run.
            std::printf("host smoke skipped: no spklib-host given (protocol tier still verified)\n");
        }
        std::printf("library_host: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("library_host: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
