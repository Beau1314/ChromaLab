// The library helper's wire, at three tiers. The protocol half is pure JSON
// in and JSON out — request lines, reply envelopes, the ready event, the abi
// gate, id routing, chunk-to-line reassembly — with no QProcess anywhere.
// The fake-peer half drives a real pipe against this test binary itself in a
// scripted mode (silent, or a reply split across two chunks), so the timeout
// and reassembly paths run ungated: no spklib-host needed. The smoke gate is
// last: it runs only when a built spklib-host is passed as argv[1] (the
// build points it at the local spklib checkout's build/spklib-host.exe);
// without one everything above still compiles and runs, so the gate can
// never masquerade as broken when it merely has no helper to talk to.
#include "LibraryHost.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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

void testLineSplitter() {
    // A reply split mid-JSON across pipe reads: nothing completes until the
    // chunk carrying the newline, then exactly one whole line that parses
    // and routes. Without the carried remainder this reply is lost.
    protocol::LineSplitter splitter;
    const QByteArray head = QByteArray(R"json({"id":2,"res)json");
    check(splitter.feed(head).empty(), "a chunk ending mid-line completes no line");
    check(splitter.remainder() == head, "the fragment is carried, not dropped");
    const auto completed = splitter.feed(QByteArray(R"json(ult":{"split":true}})json") + '\n');
    check(completed.size() == 1, "the completing chunk yields exactly one line");
    const auto reply = protocol::parseReply(QJsonDocument::fromJson(completed.front()), 2);
    check(reply.valid && reply.routed && reply.isResult &&
              reply.result.value(QLatin1String("split")).toBool(),
          "the reassembled reply parses and routes to its id");

    // One chunk may also hold several lines (plus an empty one, which
    // handleLine ignores); the splitter yields each and carries nothing.
    const auto batch = splitter.feed(QByteArray(R"json({"id":3,"result":{}})json") + "\n\n" +
                                     R"json({"id":4,"error":{"code":"io"}})json" "\n");
    check(batch.size() == 3, "a chunk holding several lines yields each");
    check(QJsonDocument::fromJson(batch[0]).object().value(QLatin1String("id")).toDouble() == 3.0,
          "the first line of a multi-line chunk is whole");
    check(QJsonDocument::fromJson(batch[2]).object()
              .value(QLatin1String("error"))
              .toObject()
              .value(QLatin1String("code"))
              .toString() == QLatin1String("io"),
          "the last line of a multi-line chunk is whole");
    check(splitter.remainder().isEmpty(),
          "nothing is carried past a newline-terminated chunk");

    splitter.reset();
    check(splitter.remainder().isEmpty(), "reset drops a stale child's tail");
}

// --- the host tier: fake peers over a real pipe (ungated) --------------------

/// A helper that stays alive but never speaks must fail start()'s ready gate,
/// and waitForReply must return nullopt at its deadline — driven for real:
/// the peer process is running, the pipe just never answers.
void testTimeoutGate() {
    host::LibraryHost helper;
    host::LibraryHost::Options options;
    options.helperPath = QCoreApplication::applicationFilePath();
    options.helperArguments = {QStringLiteral("--spk-silent-peer")};
    options.startTimeout = std::chrono::milliseconds(1200);
    const auto failure = helper.start(options);
    check(failure.has_value(), "a silent helper must fail start()'s ready gate");
    check(failure->contains(QLatin1String("no ready event")),
          "the failure names the ready timeout");
    check(helper.isRunning(), "the silent peer is still alive past the ready timeout");
    // nextId_ is still 1: the failed start never sent its sdk.version, so id 1
    // matches and the loop runs to its deadline instead of the id-mismatch exit.
    const auto reply = helper.waitForReply(1, QStringLiteral("sdk.version"), {},
                                           std::chrono::milliseconds(800));
    check(!reply.has_value(), "waitForReply returns nullopt at its deadline");
    // The sleeping peer is left to the destructor's kill(); shutdown()'s
    // terminate ladder is for a well-behaved helper, not a fake peer.
}

/// The partial-line reassembly end to end: the peer writes one reply in two
/// pipe chunks ~0.6 s apart, split mid-JSON. Without the reassembly fix this
/// reply reaches handleLine as fragments, is dropped, and the call times out.
void testSplitReplyAcrossChunks() {
    host::LibraryHost helper;
    host::LibraryHost::Options options;
    options.helperPath = QCoreApplication::applicationFilePath();
    options.helperArguments = {QStringLiteral("--spk-split-peer")};
    const auto failure = helper.start(options);
    check(!failure.has_value(),
          (std::string("split peer start: ") + failure.value_or("?").toStdString()).c_str());
    const auto reply = helper.waitForReply(2, QStringLiteral("split.test"), {},
                                           std::chrono::milliseconds(10000));
    check(reply.has_value(), "a reply split across pipe reads must still arrive");
    check(reply->valid && reply->routed && reply->isResult,
          "the reassembled reply routes to its request id");
    check(reply->result.value(QLatin1String("split")).toBool(),
          "the reassembled reply body survives");
}

// --- the typed layer: wire params, built pure ---------------------------------

using host::MetaValue;

void testParamBuilders() {
    // snake_case keys verbatim — the helper's vocabulary, never renamed.
    const QJsonObject list = protocol::imagesListParams(QVariantMap{
        {QLatin1String("collection"), QLatin1String("A")},
        {QLatin1String("sort"), QLatin1String("captured")},
        {QLatin1String("descending"), true},
        {QLatin1String("offset"), 100},
        {QLatin1String("limit"), 200},
        {QLatin1String("check_files"), true},
        {QLatin1String("filter"), QVariantMap{{QLatin1String("min_rating"), 3}}}});
    check(list.value(QLatin1String("collection")).toString() == QLatin1String("A") &&
              list.value(QLatin1String("sort")).toString() == QLatin1String("captured") &&
              list.value(QLatin1String("descending")).toBool() &&
              list.value(QLatin1String("offset")).toInt() == 100 &&
              list.value(QLatin1String("limit")).toInt() == 200 &&
              list.value(QLatin1String("check_files")).toBool() &&
              list.value(QLatin1String("filter")).toObject().value(QLatin1String("min_rating")).toInt() == 3,
          "images.list params ride the query verbatim, filter included");
    check(protocol::imagesListParams({}).isEmpty(), "an empty query builds empty params");

    const QJsonObject meta = protocol::metaSetParams(
        QStringList() << QStringLiteral("V1"), {}, MetaValue::of(5), MetaValue::clear());
    check(meta.value(QLatin1String("variants")).toArray().size() == 1, "meta.set carries variants");
    check(!meta.contains(QLatin1String("images")), "variants-only meta.set writes no images key");
    check(meta.value(QLatin1String("rating")).toInt() == 5, "a rating value rides as a number");
    check(meta.value(QLatin1String("color_tag")).type() == QJsonValue::Null,
          "a cleared colour tag rides as explicit null");
    const QJsonObject metaImages = protocol::metaSetParams(
        {}, QStringList() << QStringLiteral("I1"), MetaValue::omit(), MetaValue::of(2));
    check(metaImages.contains(QLatin1String("images")) &&
              !metaImages.contains(QLatin1String("variants")),
          "images-only meta.set targets the first variant through images");
    check(!metaImages.contains(QLatin1String("rating")),
          "an omitted value leaves the rating key off the wire");
    check(metaImages.value(QLatin1String("color_tag")).toInt() == 2,
          "a colour-tag number rides as a number");

    const QJsonObject trash = protocol::imagesTrashParams(QStringList() << QStringLiteral("I1"),
                                                          MetaValue::clear());
    check(trash.value(QLatin1String("images")).toArray().size() == 1 &&
              trash.value(QLatin1String("trashed")).type() == QJsonValue::Null,
          "a null trashed flag rides as explicit null");

    const QJsonObject put =
        protocol::editPutParams(QStringLiteral("V1"), QJsonObject{{QLatin1String("film"), 1}}, 2);
    check(put.value(QLatin1String("variant")).toString() == QLatin1String("V1") &&
              put.value(QLatin1String("base_revision")).toInt() == 2 &&
              put.value(QLatin1String("edit")).toObject().contains(QLatin1String("film")),
          "edit.put params carry variant, edit and base_revision");

    const QJsonObject add = protocol::imagesAddParams(
        QVariantList() << QStringLiteral("/p/a.RAW")
                       << QVariantMap{{QLatin1String("path"), QLatin1String("/p/b.RAW")},
                                      {QLatin1String("metadata"),
                                       QVariantMap{{QLatin1String("camera_model"), QLatin1String("Canon R5")}}}},
        QStringLiteral("reference"), QStringLiteral("AL1"), QString());
    const QJsonArray items = add.value(QLatin1String("items")).toArray();
    check(items.size() == 2 && items.at(0).toString() == QLatin1String("/p/a.RAW") &&
              items.at(1).toObject().value(QLatin1String("path")).toString() == QLatin1String("/p/b.RAW") &&
              items.at(1).toObject()
                  .value(QLatin1String("metadata"))
                  .toObject()
                  .value(QLatin1String("camera_model"))
                  .toString() == QLatin1String("Canon R5"),
          "images.add items ride as a path or a path+metadata object");
    check(add.value(QLatin1String("mode")).toString() == QLatin1String("reference") &&
              add.value(QLatin1String("collection")).toString() == QLatin1String("AL1") &&
              !add.contains(QLatin1String("recent_import_at")),
          "images.add carries mode and collection, and omits an absent import time");
    check(protocol::imagesAddParams({}, QStringLiteral("copy"), {}, QStringLiteral("2026-11-05T04:39:00Z"))
                  .value(QLatin1String("recent_import_at"))
                  .toString() == QLatin1String("2026-11-05T04:39:00Z"),
          "an explicit import time rides as recent_import_at");

    const QJsonObject album = protocol::collectionCreateParams(
        QStringLiteral("album"), QStringLiteral("New Album"), QStringLiteral("P1"),
        QJsonObject{{QLatin1String("min_rating"), 4}});
    check(album.value(QLatin1String("kind")).toString() == QLatin1String("album") &&
              album.value(QLatin1String("name")).toString() == QLatin1String("New Album") &&
              album.value(QLatin1String("parent")).toString() == QLatin1String("P1") &&
              album.value(QLatin1String("criteria")).toObject().value(QLatin1String("min_rating")).toInt() == 4,
          "collection.create params carry kind, name, parent and criteria");
    check(!protocol::collectionCreateParams(QStringLiteral("album"), QStringLiteral("N"), {}, {})
                   .contains(QLatin1String("parent")),
          "an absent parent is omitted, not null");

    const QJsonObject move = protocol::sessionMoveParams(
        QStringList() << QStringLiteral("I1"), QStringLiteral("trash"), QStringLiteral("suffix"));
    check(move.value(QLatin1String("images")).toArray().size() == 1 &&
              move.value(QLatin1String("to")).toString() == QLatin1String("trash") &&
              move.value(QLatin1String("on_conflict")).toString() == QLatin1String("suffix"),
          "session.move params carry images, to and on_conflict");

    const QJsonObject folder =
        protocol::foldersAddParams(QStringLiteral("D:/S"), true, QStringList() << QStringLiteral("@eaDir"));
    check(folder.value(QLatin1String("path")).toString() == QLatin1String("D:/S") &&
              folder.value(QLatin1String("recursive")).toBool() &&
              folder.value(QLatin1String("skip")).toArray().at(0).toString() == QLatin1String("@eaDir"),
          "folders.add params carry path, recursive and skip");
}

// --- the typed layer: one fake peer drives method → wire → reply → struct ------

/// A scripted stand-in helper (the 4.0 self-spawn pattern): answers every
/// library method with a spec-shaped canned reply, echoing the request's
/// params under "echo" so the parent can assert what actually crossed the
/// wire. The error paths are keyed off the request's own values.
void runTypedPeer() {
    const auto write = [](const QByteArray& bytes) {
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        std::fflush(stdout);
    };
    write(QByteArray(R"json({"event":"ready","version":"spklib 0.4.0-fake (abi 1, schema 3)","protocol":1})json") +
          '\n');
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const QJsonObject request =
            QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const qint64 id = static_cast<qint64>(request.value(QLatin1String("id")).toDouble(-1));
        const QString method = request.value(QLatin1String("method")).toString();
        const QJsonObject params = request.value(QLatin1String("params")).toObject();
        if (method == QLatin1String("host.shutdown")) return;

        QJsonObject reply;
        reply.insert(QLatin1String("id"), static_cast<double>(id));
        QJsonObject result;
        QJsonObject error;
        if (method == QLatin1String("sdk.version")) {
            result = QJsonObject{{QLatin1String("version"), QLatin1String("0.4.0-fake")},
                                 {QLatin1String("abi"), 1},
                                 {QLatin1String("schema"), 3},
                                 {QLatin1String("capture_one"),
                                  QJsonObject{{QLatin1String("write_back"), false},
                                              {QLatin1String("previews"), false}}}};
        } else if (method == QLatin1String("library.create") || method == QLatin1String("library.open")) {
            QJsonObject info{{QLatin1String("uuid"), QLatin1String("C1F5EA7E-1111-2222-3333-444455556666")},
                             {QLatin1String("kind"), QLatin1String("catalog")},
                             {QLatin1String("name"), QLatin1String("Nature")},
                             {QLatin1String("path"), params.value(QLatin1String("path"))},
                             {QLatin1String("counts"),
                              QJsonObject{{QLatin1String("images"), 2},
                                          {QLatin1String("variants"), 2},
                                          {QLatin1String("edits"), 1}}}};
            if (method == QLatin1String("library.create"))
                info.insert(QLatin1String("copied_from"),
                            QJsonObject{{QLatin1String("kind"), QLatin1String("capture_one.catalog")},
                                        {QLatin1String("path"), QLatin1String("/Volumes/C1/nature.cocatalog")},
                                        {QLatin1String("document_uuid"), QLatin1String("D-1")},
                                        {QLatin1String("format"), QLatin1String("16.8.6.25 Pro Mac")},
                                        {QLatin1String("copied_at"), QLatin1String("2026-11-10T09:00:00Z")},
                                        {QLatin1String("updated_at"), QJsonValue::Null}});
            result = QJsonObject{{QLatin1String("library"), QLatin1String("L1")},
                                 {QLatin1String("info"), info},
                                 {QLatin1String("echo"), params}};
        } else if (method == QLatin1String("library.info")) {
            result = QJsonObject{{QLatin1String("uuid"), QLatin1String("C1F5EA7E-1111-2222-3333-444455556666")},
                                 {QLatin1String("kind"), QLatin1String("catalog")},
                                 {QLatin1String("name"), QLatin1String("Nature")},
                                 {QLatin1String("path"), QLatin1String("D:/Photos/Nature.spektralab")},
                                 {QLatin1String("counts"),
                                  QJsonObject{{QLatin1String("images"), 2},
                                              {QLatin1String("variants"), 2},
                                              {QLatin1String("edits"), 1}}}};
        } else if (method == QLatin1String("library.close")) {
            result = QJsonObject{{QLatin1String("closed"), true}};
        } else if (method == QLatin1String("library.sync")) {
            result = QJsonObject{
                {QLatin1String("images"),
                 QJsonObject{{QLatin1String("added"), 1}, {QLatin1String("updated"), 0},
                             {QLatin1String("unchanged"), 2}, {QLatin1String("removed"), 0}}},
                {QLatin1String("warnings"), QJsonArray{}}, {QLatin1String("ms"), 1}};
        } else if (method == QLatin1String("collections.list")) {
            const QJsonArray collections = {
                QJsonObject{{QLatin1String("uuid"), QLatin1String("R")}, {QLatin1String("parent"), QJsonValue::Null},
                            {QLatin1String("kind"), QLatin1String("root")}, {QLatin1String("name"), QLatin1String("root")},
                            {QLatin1String("title"), QLatin1String("root")}, {QLatin1String("position"), 0},
                            {QLatin1String("editable"), false}, {QLatin1String("image_count"), 2}},
                QJsonObject{{QLatin1String("uuid"), QLatin1String("P1")}, {QLatin1String("parent"), QLatin1String("R")},
                            {QLatin1String("kind"), QLatin1String("project")}, {QLatin1String("name"), QLatin1String("Weddings")},
                            {QLatin1String("title"), QLatin1String("Weddings")}, {QLatin1String("position"), 1},
                            {QLatin1String("editable"), true}, {QLatin1String("image_count"), 0}},
                QJsonObject{{QLatin1String("uuid"), QLatin1String("AL1")}, {QLatin1String("parent"), QLatin1String("P1")},
                            {QLatin1String("kind"), QLatin1String("album")}, {QLatin1String("name"), QLatin1String("Best of")},
                            {QLatin1String("title"), QLatin1String("Best of")}, {QLatin1String("position"), 2},
                            {QLatin1String("editable"), true}, {QLatin1String("image_count"), 1},
                            {QLatin1String("created_at"), QLatin1String("2026-11-14T20:00:00Z")}}};
            result = QJsonObject{{QLatin1String("collections"), collections}};
        } else if (method == QLatin1String("images.list")) {
            const QJsonArray images = {
                QJsonObject{{QLatin1String("uuid"), QLatin1String("I1")},
                            {QLatin1String("path"), QLatin1String("/x/a.RAW")},
                            {QLatin1String("file_name"), QLatin1String("a.RAW")},
                            {QLatin1String("kind"), QLatin1String("raw")}, {QLatin1String("rating"), 4},
                            {QLatin1String("color_tag"), QJsonValue::Null}, {QLatin1String("trashed"), false},
                            {QLatin1String("captured_local"), QLatin1String("2024-01-08T23:39:04")},
                            {QLatin1String("variants"),
                             QJsonArray{QJsonObject{{QLatin1String("uuid"), QLatin1String("V1")},
                                                    {QLatin1String("position"), 1},
                                                    {QLatin1String("rating"), 4},
                                                    {QLatin1String("color_tag"), 2},
                                                    {QLatin1String("edit_revision"), 3}}}},
                            {QLatin1String("exists"), true}},
                QJsonObject{{QLatin1String("uuid"), QLatin1String("I2")},
                            {QLatin1String("path"), QLatin1String("/y/b.RAW")},
                            {QLatin1String("file_name"), QLatin1String("b.RAW")},
                            {QLatin1String("kind"), QLatin1String("raster")}, {QLatin1String("rating"), 0},
                            {QLatin1String("color_tag"), QJsonValue::Null}, {QLatin1String("trashed"), true},
                            {QLatin1String("captured_local"), QLatin1String("2024-02-01T10:00:00")},
                            {QLatin1String("variants"),
                             QJsonArray{QJsonObject{{QLatin1String("uuid"), QLatin1String("V2")},
                                                    {QLatin1String("position"), 1},
                                                    {QLatin1String("rating"), 0},
                                                    {QLatin1String("color_tag"), QJsonValue::Null},
                                                    {QLatin1String("edit_revision"), 0}}}},
                            {QLatin1String("exists"), false}}};
            result = QJsonObject{{QLatin1String("total"), 2}, {QLatin1String("offset"), 0},
                                 {QLatin1String("images"), images}};
        } else if (method == QLatin1String("image.get")) {
            if (params.value(QLatin1String("image")).toString() == QLatin1String("nope")) {
                error = QJsonObject{{QLatin1String("code"), QLatin1String("not_found")},
                                    {QLatin1String("message"), QLatin1String("no such image")},
                                    {QLatin1String("data"), QJsonValue::Null}};
            } else {
                result = QJsonObject{{QLatin1String("uuid"), QLatin1String("I1")},
                                     {QLatin1String("path"), QLatin1String("/x/a.RAW")},
                                     {QLatin1String("camera"),
                                      QJsonObject{{QLatin1String("make"), QLatin1String("Canon")},
                                                  {QLatin1String("model"), QLatin1String("R5")}}},
                                     {QLatin1String("variants"),
                                      QJsonArray{QJsonObject{{QLatin1String("uuid"), QLatin1String("V1")},
                                                             {QLatin1String("edit_revision"), 3}}}},
                                     {QLatin1String("albums"),
                                      QJsonArray{QJsonObject{{QLatin1String("uuid"), QLatin1String("AL1")},
                                                             {QLatin1String("name"), QLatin1String("Best of")}}}},
                                     {QLatin1String("exists"), true}};
            }
        } else if (method == QLatin1String("images.lookup")) {
            const QJsonArray paths = params.value(QLatin1String("paths")).toArray();
            QJsonArray results;
            if (paths.size() > 0)
                results.append(QJsonObject{{QLatin1String("path"), paths.at(0)},
                                           {QLatin1String("found"), true},
                                           {QLatin1String("match"),
                                            QJsonObject{{QLatin1String("image"), QLatin1String("I1")},
                                                        {QLatin1String("variant"), QLatin1String("V1")}}}});
            if (paths.size() > 1)
                results.append(QJsonObject{{QLatin1String("path"), paths.at(1)},
                                           {QLatin1String("found"), false}});
            result = QJsonObject{{QLatin1String("results"), results}};
        } else if (method == QLatin1String("images.facets")) {
            result = QJsonObject{{QLatin1String("total"), 9},
                                 {QLatin1String("rating"),
                                  QJsonArray{QJsonObject{{QLatin1String("value"), 0}, {QLatin1String("count"), 2}},
                                             QJsonObject{{QLatin1String("value"), 5}, {QLatin1String("count"), 1}}}},
                                 {QLatin1String("edited"), QJsonObject{{QLatin1String("yes"), 1}, {QLatin1String("no"), 8}}},
                                 {QLatin1String("undescribed"), 0}};
        } else if (method == QLatin1String("meta.set")) {
            bool gone = false;
            for (const QJsonValue& v : params.value(QLatin1String("variants")).toArray())
                if (v.toString() == QLatin1String("gone-variant")) gone = true;
            if (gone) {
                error = QJsonObject{{QLatin1String("code"), QLatin1String("not_found")},
                                    {QLatin1String("message"), QLatin1String("no such variant")},
                                    {QLatin1String("data"), QJsonValue::Null}};
            } else {
                result = QJsonObject{{QLatin1String("updated"), true}, {QLatin1String("echo"), params}};
            }
        } else if (method == QLatin1String("images.add")) {
            result = QJsonObject{
                {QLatin1String("added"), 2},
                {QLatin1String("results"),
                 QJsonArray{QJsonObject{{QLatin1String("path"), QLatin1String("/p/a.RAW")},
                                        {QLatin1String("image"), QLatin1String("I1")},
                                        {QLatin1String("variant"), QLatin1String("V1")},
                                        {QLatin1String("existing"), false}},
                            QJsonObject{{QLatin1String("path"), QLatin1String("/p/b.RAW")},
                                        {QLatin1String("image"), QLatin1String("I2")},
                                        {QLatin1String("variant"), QLatin1String("V2")},
                                        {QLatin1String("existing"), true},
                                        {QLatin1String("stored_at"), QLatin1String("store/Originals/2026/11/14/b.RAW")}}}},
                {QLatin1String("recent_import"), QLatin1String("ALB1")}, {QLatin1String("echo"), params}};
        } else if (method == QLatin1String("images.trash")) {
            result = QJsonObject{{QLatin1String("updated"), true}, {QLatin1String("files_moved"), false}};
        } else if (method == QLatin1String("images.remove")) {
            result = QJsonObject{{QLatin1String("removed"), 1},
                                 {QLatin1String("refused"),
                                  QJsonArray{QJsonObject{{QLatin1String("image"), QLatin1String("I9")},
                                                         {QLatin1String("code"), QLatin1String("read_only")}}}},
                                 {QLatin1String("files_moved"), false}};
        } else if (method == QLatin1String("collection.create")) {
            result = QJsonObject{{QLatin1String("collection"), QLatin1String("NEWC")}, {QLatin1String("echo"), params}};
        } else if (method == QLatin1String("collection.update")) {
            result = QJsonObject{};
        } else if (method == QLatin1String("collection.delete")) {
            result = QJsonObject{{QLatin1String("deleted"), true}, {QLatin1String("images_deleted"), 0}};
        } else if (method == QLatin1String("collection.add")) {
            result = QJsonObject{{QLatin1String("added"), 2}};
        } else if (QLatin1String("collection.remove") == method) {
            result = QJsonObject{{QLatin1String("removed"), 1}};
        } else if (method == QLatin1String("folders.add")) {
            result = QJsonObject{{QLatin1String("folder"), QLatin1String("F1")},
                                 {QLatin1String("collection"), QLatin1String("FC1")},
                                 {QLatin1String("path"), params.value(QLatin1String("path"))},
                                 {QLatin1String("sync"),
                                  QJsonObject{{QLatin1String("path"), params.value(QLatin1String("path"))},
                                              {QLatin1String("offline"), false}, {QLatin1String("seen"), 3},
                                              {QLatin1String("added"), 3}, {QLatin1String("moved"), 0},
                                              {QLatin1String("removed"), 0}, {QLatin1String("restored"), 0}}},
                                 {QLatin1String("echo"), params}};
        } else if (method == QLatin1String("folders.list")) {
            result = QJsonObject{{QLatin1String("folders"),
                                  QJsonArray{QJsonObject{{QLatin1String("folder"), QLatin1String("F1")},
                                                         {QLatin1String("path"), QLatin1String("D:/Shooting")},
                                                         {QLatin1String("recursive"), true},
                                                         {QLatin1String("collection"), QLatin1String("FC1")},
                                                         {QLatin1String("online"), true},
                                                         {QLatin1String("image_count"), 3}}}}};
        } else if (method == QLatin1String("folders.sync")) {
            result = QJsonObject{{QLatin1String("folders"),
                                  QJsonArray{QJsonObject{{QLatin1String("path"), QLatin1String("D:/Shooting")},
                                                         {QLatin1String("offline"), false},
                                                         {QLatin1String("added"), 1}}}},
                                 {QLatin1String("added"), 1}, {QLatin1String("moved"), 0},
                                 {QLatin1String("removed"), 0}, {QLatin1String("restored"), 0}};
        } else if (method == QLatin1String("folders.remove")) {
            result = QJsonObject{{QLatin1String("removed"), true}, {QLatin1String("images_removed"), 0}};
        } else if (method == QLatin1String("session.folders")) {
            result = QJsonObject{{QLatin1String("root"), QLatin1String("D:/Session")},
                                 {QLatin1String("folders"),
                                  QJsonArray{QJsonObject{{QLatin1String("path"), QLatin1String("D:/Session/Capture")},
                                                         {QLatin1String("relative_path"), QLatin1String("Capture")},
                                                         {QLatin1String("role"), QLatin1String("capture")},
                                                         {QLatin1String("outside_session"), false},
                                                         {QLatin1String("exists"), true},
                                                         {QLatin1String("image_count"), 3}}}}};
        } else if (method == QLatin1String("session.scan")) {
            result = QJsonObject{{QLatin1String("seen"), 5}, {QLatin1String("added"), 2}, {QLatin1String("missing"), 1}};
        } else if (method == QLatin1String("session.move") || method == QLatin1String("session.restore")) {
            result = QJsonObject{{QLatin1String("moved"),
                                  QJsonArray{QJsonObject{{QLatin1String("image"), QLatin1String("I1")},
                                                         {QLatin1String("from"), QLatin1String("Capture/a.RAW")},
                                                         {QLatin1String("to"), QLatin1String("Selects/a.RAW")}}}},
                                 {QLatin1String("conflicts"), QJsonArray{}}, {QLatin1String("echo"), params}};
        } else if (method == QLatin1String("session.trash.plan")) {
            result = QJsonObject{{QLatin1String("files"),
                                  QJsonArray{QJsonObject{{QLatin1String("image"), QLatin1String("I2")},
                                                         {QLatin1String("path"), QLatin1String("Trash/b.RAW")}}}}};
        } else if (method == QLatin1String("session.trash.commit")) {
            result = QJsonObject{{QLatin1String("dropped"), 1}, {QLatin1String("refused"), QJsonArray{}}};
        } else if (method == QLatin1String("edit.get")) {
            result = QJsonObject{{QLatin1String("revision"), 3},
                                 {QLatin1String("edit"),
                                  QJsonObject{{QLatin1String("film"), QLatin1String("Portra 400")},
                                              {QLatin1String("paper"), QLatin1String("Baryta")}}},
                                 {QLatin1String("modified_at"), QLatin1String("2026-11-14T21:00:00Z")}};
        } else if (method == QLatin1String("edit.put")) {
            if (params.value(QLatin1String("base_revision")).toInt() == 99) {
                error = QJsonObject{{QLatin1String("code"), QLatin1String("conflict")},
                                    {QLatin1String("message"), QLatin1String("saved elsewhere first")},
                                    {QLatin1String("data"), QJsonObject{{QLatin1String("revision"), 7}}}};
            } else {
                result = QJsonObject{{QLatin1String("revision"), 4},
                                     {QLatin1String("modified_at"), QLatin1String("2026-11-14T21:05:00Z")},
                                     {QLatin1String("echo"), params}};
            }
        } else if (method == QLatin1String("edit.delete")) {
            result = QJsonObject{{QLatin1String("deleted"), true}};
        } else {
            error = QJsonObject{{QLatin1String("code"), QLatin1String("unknown_method")},
                                {QLatin1String("message"), QLatin1String("no such method")},
                                {QLatin1String("data"), QJsonValue::Null}};
        }
        if (!error.isEmpty())
            reply.insert(QLatin1String("error"), error);
        else
            reply.insert(QLatin1String("result"), result);
        write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    }
}

/// The typed methods against the scripted peer: build, send, parse, and the
/// error envelopes keyed off the request values.
void testTypedMethods() {
    host::LibraryHost helper;
    host::LibraryHost::Options options;
    options.helperPath = QCoreApplication::applicationFilePath();
    options.helperArguments = {QStringLiteral("--spk-typed-peer")};
    const auto failure = helper.start(options);
    check(!failure.has_value(),
          (std::string("typed peer start: ") + failure.value_or("?").toStdString()).c_str());

    // library.create → SourceInfo, and the wire params proven by the echo.
    const auto created = helper.libraryCreate(QStringLiteral("D:/Photos/Nature.spektralab"),
                                              QStringLiteral("catalog"), QStringLiteral("Nature"));
    check(created.ok, (std::string("library.create: ") + created.errorMessage.toStdString()).c_str());
    check(created.value.library == QLatin1String("L1"), "create returns the handle");
    check(created.value.valid && created.value.uuid == QLatin1String("C1F5EA7E-1111-2222-3333-444455556666"),
          "create's info carries the library uuid");
    check(created.value.kind == QLatin1String("catalog") && created.value.name == QLatin1String("Nature") &&
              created.value.path == QLatin1String("D:/Photos/Nature.spektralab"),
          "create's info carries kind, name and path");
    check(created.value.images == 2 && created.value.variants == 2 && created.value.edits == 1,
          "create's counts are read from info");
    check(created.value.copiedFrom.value(QLatin1String("kind")).toString() ==
                  QLatin1String("capture_one.catalog") &&
              created.value.copiedFrom.value(QLatin1String("format")).toString() ==
                  QLatin1String("16.8.6.25 Pro Mac"),
          "create's copied_from rides through as a map");
    const QVariantMap createEcho = created.value.raw.value(QLatin1String("echo")).toMap();
    check(createEcho.value(QLatin1String("path")).toString() == QLatin1String("D:/Photos/Nature.spektralab") &&
              createEcho.value(QLatin1String("kind")).toString() == QLatin1String("catalog") &&
              createEcho.value(QLatin1String("name")).toString() == QLatin1String("Nature"),
          "the wire params reached the helper verbatim (echo)");

    // library.open: same handle, no copied_from, path-only params.
    const auto opened = helper.libraryOpen(QStringLiteral("D:/Photos/Nature.spektralab"));
    check(opened.ok && opened.value.library == QLatin1String("L1"), "open returns the same handle");
    check(opened.value.copiedFrom.isEmpty(), "open's info carries no copied_from");
    const QVariantMap openEcho = opened.value.raw.value(QLatin1String("echo")).toMap();
    check(openEcho.size() == 1 && openEcho.contains(QLatin1String("path")),
          "open's wire params are path only");

    const auto info = helper.libraryInfo(QLatin1String("L1"));
    check(info.ok && info.value.value(QLatin1String("uuid")).toString() ==
                           QLatin1String("C1F5EA7E-1111-2222-3333-444455556666") &&
              info.value.value(QLatin1String("counts")).toMap().value(QLatin1String("images")).toInt() == 2,
          "library.info rides through as a map");

    const auto synced = helper.librarySync(QLatin1String("L1"), true);
    check(synced.ok && synced.value.value(QLatin1String("images")).toMap().value(QLatin1String("added")).toInt() == 1,
          "library.sync's report rides through as a map");

    // collections.list → rows as maps.
    const auto collections = helper.collectionsList(QLatin1String("L1"));
    check(collections.ok && collections.value.size() == 3, "collections.list returns three rows");
    check(collections.value.front().value(QLatin1String("kind")).toString() == QLatin1String("root") &&
              !collections.value.front().value(QLatin1String("editable")).toBool(),
          "the root row reads as fixed and not editable");
    const QVariantMap album = collections.value.at(2);
    check(album.value(QLatin1String("kind")).toString() == QLatin1String("album") &&
              album.value(QLatin1String("editable")).toBool() &&
              album.value(QLatin1String("image_count")).toInt() == 1 &&
              album.value(QLatin1String("parent")).toString() == QLatin1String("P1"),
          "an album row reads editable with its count and parent");

    // images.list → ImagePage with essentials.
    const auto page = helper.imagesList(
        QLatin1String("L1"),
        QVariantMap{{QLatin1String("collection"), QLatin1String("A")},
                    {QLatin1String("sort"), QLatin1String("captured")},
                    {QLatin1String("descending"), true},
                    {QLatin1String("limit"), 200},
                    {QLatin1String("filter"), QVariantMap{{QLatin1String("min_rating"), 3}}}});
    check(page.ok && page.value.total == 2 && page.value.offset == 0, "the page carries total and offset");
    check(page.value.images.size() == 2, "the page carries both rows");
    const host::ImageRow& row = page.value.images.at(0);
    check(row.valid && row.uuid() == QLatin1String("I1") && row.path() == QLatin1String("/x/a.RAW") &&
              row.fileName() == QLatin1String("a.RAW") && row.kind() == QLatin1String("raw"),
          "the row's essentials read from the map");
    check(row.rating() == 4 && row.trashed() == false &&
              row.capturedLocal() == QLatin1String("2024-01-08T23:39:04"),
          "rating, trash flag and capture time read from the row");
    check(row.exists().has_value() && *row.exists(), "exists reads when present");
    check(row.variantCount() == 1 && row.primaryVariant() == QLatin1String("V1") &&
              row.primaryEditRevision() == 3,
          "the primary variant and its edit revision read from the row");
    const host::ImageRow& trashedRow = page.value.images.at(1);
    check(trashedRow.trashed() && trashedRow.exists().has_value() && !*trashedRow.exists(),
          "a trashed, offline row reads its flags");

    // image.get → full map; error path for a missing image.
    const auto detail = helper.imageGet(QLatin1String("L1"), QStringLiteral("I1"));
    check(detail.ok && detail.value.value(QLatin1String("camera")).toMap().value(QLatin1String("model")).toString() ==
                           QLatin1String("R5") &&
              detail.value.value(QLatin1String("albums")).toList().size() == 1,
          "image.get rides through as the full map");
    const auto missing = helper.imageGet(QLatin1String("L1"), QStringLiteral("nope"));
    check(!missing.ok && missing.errorCode == QLatin1String("not_found") &&
              missing.errorMessage == QLatin1String("no such image"),
          "a not_found error envelope surfaces with code and message");

    const auto lookup = helper.imagesLookup(QLatin1String("L1"),
                                            QStringList() << QStringLiteral("/x/a.RAW")
                                                          << QStringLiteral("/gone.RAW"));
    check(lookup.ok && lookup.value.size() == 2, "lookup returns one entry per path");
    check(lookup.value.front().value(QLatin1String("found")).toBool() &&
              lookup.value.front()
                  .value(QLatin1String("match"))
                  .toMap()
                  .value(QLatin1String("image"))
                  .toString() == QLatin1String("I1"),
          "a found lookup carries the match");
    check(!lookup.value.back().value(QLatin1String("found")).toBool(), "a missing path reads not found");

    const auto facets = helper.imagesFacets(QLatin1String("L1"));
    check(facets.ok && facets.value.value(QLatin1String("total")).toInt() == 9 &&
              facets.value.value(QLatin1String("rating")).toList().size() == 2 &&
              facets.value.value(QLatin1String("edited")).toMap().value(QLatin1String("yes")).toInt() == 1,
          "facets ride through as a map");

    // meta.set: the ack path, the local-argument path and the error path.
    const auto meta = helper.metaSet(QLatin1String("L1"), QStringList() << QStringLiteral("V1"),
                                     MetaValue::of(5), MetaValue::clear());
    check(meta.ok && meta.value, "meta.set acks");
    const auto metaBoth = helper.metaSet(QLatin1String("L1"), QStringList() << QStringLiteral("V1"),
                                         MetaValue::omit(), MetaValue::omit(),
                                         QStringList() << QStringLiteral("I1"));
    check(!metaBoth.ok && metaBoth.errorCode == QLatin1String("invalid_params"),
          "variants and images together are refused before the wire");
    const auto metaNeither =
        helper.metaSet(QLatin1String("L1"), {}, MetaValue::of(3), MetaValue::omit());
    check(!metaNeither.ok && metaNeither.errorCode == QLatin1String("invalid_params"),
          "an empty target list is refused before the wire");
    const auto metaGone = helper.metaSet(QLatin1String("L1"), QStringList() << QStringLiteral("gone-variant"),
                                         MetaValue::of(1), MetaValue::omit());
    check(!metaGone.ok && metaGone.errorCode == QLatin1String("not_found"),
          "the helper's not_found surfaces through the typed call");

    const auto added = helper.imagesAdd(
        QLatin1String("L1"),
        QVariantList() << QStringLiteral("/p/a.RAW")
                       << QVariantMap{{QLatin1String("path"), QLatin1String("/p/b.RAW")},
                                      {QLatin1String("metadata"),
                                       QVariantMap{{QLatin1String("camera_model"), QLatin1String("Canon R5")}}}},
        QStringLiteral("reference"), QStringLiteral("AL1"));
    check(added.ok && added.value.added == 2 && added.value.rows.size() == 2, "images.add acks with rows");
    check(!added.value.rows.front().existing && added.value.rows.front().image == QLatin1String("I1"),
          "a new add reads image and not-existing");
    check(added.value.rows.back().existing && !added.value.rows.back().storedAt.isEmpty(),
          "an existing add carries its stored_at");
    check(added.value.recentImport == QLatin1String("ALB1"), "the Recent Imports album surfaces");

    const auto trashed = helper.imagesTrash(QLatin1String("L1"), QStringList() << QStringLiteral("I1"),
                                            MetaValue::of(true));
    check(trashed.ok && trashed.value, "images.trash acks");

    const auto removed = helper.imagesRemove(QLatin1String("L1"),
                                             QStringList() << QStringLiteral("I1") << QStringLiteral("I9"));
    check(removed.ok && removed.value.removed == 1, "images.remove counts the forgotten rows");
    check(removed.value.refused.size() == 1 && removed.value.refused.front().image == QLatin1String("I9") &&
              removed.value.refused.front().code == QLatin1String("read_only"),
          "a refused row surfaces with its image and code");

    // collection.*
    const auto album2 = helper.collectionCreate(QLatin1String("L1"), QStringLiteral("album"),
                                                QStringLiteral("New Album"), QStringLiteral("P1"));
    check(album2.ok && album2.value == QLatin1String("NEWC"), "collection.create returns the new uuid");
    check(helper.collectionUpdate(QLatin1String("L1"), QStringLiteral("NEWC"),
                                  QStringLiteral("Renamed"))
              .ok,
          "collection.update acks");
    check(helper.collectionDelete(QLatin1String("L1"), QStringLiteral("NEWC"), false).ok,
          "collection.delete acks");
    const auto addedTo = helper.collectionAdd(QLatin1String("L1"), QStringLiteral("AL1"),
                                              QStringList() << QStringLiteral("I1")
                                                            << QStringLiteral("I2"));
    check(addedTo.ok && addedTo.value == 2, "collection.add counts what joined");
    const auto removedFrom = helper.collectionRemove(QLatin1String("L1"), QStringLiteral("AL1"),
                                                     QStringList() << QStringLiteral("I2"));
    check(removedFrom.ok && removedFrom.value == 1, "collection.remove counts what left");

    // folders.*
    const auto folderAdded =
        helper.foldersAdd(QLatin1String("L1"), QStringLiteral("D:/Shooting"), true,
                          QStringList() << QStringLiteral("@eaDir"));
    check(folderAdded.ok && folderAdded.value.value(QLatin1String("folder")).toString() == QLatin1String("F1") &&
              folderAdded.value.value(QLatin1String("collection")).toString() == QLatin1String("FC1"),
          "folders.add returns the folder and its row");
    const auto watched = helper.foldersList(QLatin1String("L1"));
    check(watched.ok && watched.value.size() == 1 &&
              watched.value.front().value(QLatin1String("online")).toBool(),
          "folders.list rides through as rows");
    const auto folderSync = helper.foldersSync(QLatin1String("L1"));
    check(folderSync.ok && folderSync.value.value(QLatin1String("added")).toInt() == 1,
          "folders.sync's totals ride through");
    check(helper.foldersRemove(QLatin1String("L1"), QStringLiteral("F1")).ok, "folders.remove acks");

    // session.*
    const auto sessionFolders = helper.sessionFolders(QLatin1String("L1"));
    check(sessionFolders.ok && sessionFolders.value.value(QLatin1String("root")).toString() == QLatin1String("D:/Session") &&
              sessionFolders.value.value(QLatin1String("folders")).toList().size() == 1,
          "session.folders rides through as root plus rows");
    const auto scan = helper.sessionScan(QLatin1String("L1"));
    check(scan.ok && scan.value.value(QLatin1String("seen")).toInt() == 5 &&
              scan.value.value(QLatin1String("added")).toInt() == 2 &&
              scan.value.value(QLatin1String("missing")).toInt() == 1,
          "session.scan's three counts ride through");
    const auto moved = helper.sessionMove(QLatin1String("L1"), QStringList() << QStringLiteral("I1"),
                                          QStringLiteral("selects"), QStringLiteral("suffix"));
    check(moved.ok && moved.value.value(QLatin1String("moved")).toList().size() == 1 &&
              moved.value.value(QLatin1String("conflicts")).toList().isEmpty(),
          "session.move returns the moved rows");
    const auto restored = helper.sessionRestore(QLatin1String("L1"), QStringList() << QStringLiteral("I1"));
    check(restored.ok && restored.value.value(QLatin1String("moved")).toList().size() == 1,
          "session.restore returns the moved rows");
    const auto plan = helper.sessionTrashPlan(QLatin1String("L1"));
    check(plan.ok && plan.value.size() == 1 &&
              plan.value.front().value(QLatin1String("path")).toString() == QLatin1String("Trash/b.RAW"),
          "session.trash.plan lists the files");
    const auto commit = helper.sessionTrashCommit(QLatin1String("L1"),
                                                  QStringList() << QStringLiteral("Trash/b.RAW"));
    check(commit.ok && commit.value.value(QLatin1String("dropped")).toInt() == 1,
          "session.trash.commit acks with dropped");

    // edit.*: get, put, the conflict envelope and delete.
    const auto edit = helper.editGet(QLatin1String("L1"), QStringLiteral("V1"));
    check(edit.ok && edit.value.valid && edit.value.revision == 3 && edit.value.hasEdit &&
              edit.value.edit.value(QLatin1String("film")).toString() == QLatin1String("Portra 400") &&
              edit.value.modifiedAt == QLatin1String("2026-11-14T21:00:00Z"),
          "edit.get reads revision, edit and modified_at");
    const auto put = helper.editPut(QLatin1String("L1"), QStringLiteral("V1"),
                                    QJsonObject{{QLatin1String("film"), QLatin1String("Portra 160")}}, 3);
    check(put.ok && put.value.revision == 4 && put.value.modifiedAt == QLatin1String("2026-11-14T21:05:00Z"),
          "edit.put returns the new revision");
    const auto conflict = helper.editPut(QLatin1String("L1"), QStringLiteral("V1"),
                                         QJsonObject{{QLatin1String("film"), QLatin1String("Portra 160")}}, 99);
    check(!conflict.ok && conflict.errorCode == QLatin1String("conflict") &&
              conflict.errorData.toObject().value(QLatin1String("revision")).toInt() == 7,
          "a conflict surfaces with the server's revision in data");
    check(helper.editDelete(QLatin1String("L1"), QStringLiteral("V1")).ok, "edit.delete acks");

    check(helper.libraryClose(QLatin1String("L1")).ok, "library.close acks");
    helper.shutdown();
    check(!helper.isRunning(), "the typed peer shuts down cleanly");
}

/// A typed call on a host that never started: transport failure, not a
/// helper error — ok=false with an empty code.
void testTransportFailure() {
    host::LibraryHost helper;
    const auto reply = helper.libraryOpen(QStringLiteral("D:/nowhere.spektralab"));
    check(!reply.ok && reply.errorCode.isEmpty() && reply.errorMessage.isEmpty(),
          "an un-started host reports transport failure, not a helper error");
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
    // The fake peers: this binary spawned by the tests above as a stand-in
    // helper. They return before any Qt or test machinery so the child stays
    // scriptable and DLL-light; the parent's destructor kills them.
    if (argc > 1 && std::strcmp(argv[1], "--spk-silent-peer") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(60)); // alive, never speaks
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--spk-typed-peer") == 0) {
        // The scripted helper behind the typed-method tests: one canned,
        // spec-shaped reply per library method, each echoing the request's
        // params so the parent can assert what crossed the wire. Qt JSON is
        // used without an app object, as the static imports already load it.
        runTypedPeer();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--spk-split-peer") == 0) {
        // Scripted stdout with gaps, so the parent's pipe reads see real chunk
        // boundaries: the ready event, the abi-gate reply, then a reply for
        // id 2 written in two pieces 0.6 s apart, split mid-JSON. Pipe stdout
        // is block-buffered, so every write flushes explicitly.
        const auto write = [](const char* bytes) {
            std::fwrite(bytes, 1, std::strlen(bytes), stdout);
            std::fflush(stdout);
        };
        write(R"json({"event":"ready","version":"spklib 0.4.0 (abi 1, schema 3)","protocol":1})json" "\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        write(R"json({"id":1,"result":{"version":"0.4.0-fake","abi":1,"schema":3}})json" "\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        write(R"json({"id":2,"res)json"); // ends mid-line on purpose
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        write(R"json(ult":{"split":true}})json" "\n");
        std::this_thread::sleep_for(std::chrono::seconds(60)); // until killed
        return 0;
    }
    QCoreApplication application(argc, argv);
    try {
        testReadyEvent();
        testRequestLine();
        testReplyRouting();
        testErrorCodes();
        testAbiGate();
        testLineSplitter();
        testParamBuilders();
        testTransportFailure();
        testTypedMethods();
        testTimeoutGate();
        testSplitReplyAcrossChunks();
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
