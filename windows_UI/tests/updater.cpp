// The self-update channel's pure core (no network): the version parse and
// the order it induces. The compare is the gate that decides whether a
// release on the channel looks newer than the running copy — a wrong order
// either nags forever or never updates, so the edge cases are spelled out.
#include "Updater.hpp"

#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void checkParse(const QString& text, std::initializer_list<long long> expected) {
    const auto parts = spk::desktop::Updater::parseVersion(text);
    check(parts == QList<long long>(expected),
          (std::string("parse: ") + text.toStdString()).c_str());
}
void checkOrder(const QString& a, const QString& b, int expected, const char* message) {
    const int got = spk::desktop::Updater::compareVersions(a, b);
    check(got == expected, message);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        checkParse("1.2.3", {1, 2, 3});
        checkParse("v0.6.0", {0, 6, 0});
        checkParse("1.2", {1, 2});
        checkParse("2.0.14", {2, 0, 14});
        // A pre-release tail is cut, not parsed: 1.2.3-rc1 == 1.2.3.
        checkParse("1.2.3-rc1", {1, 2, 3});
        checkParse("", {});
        checkParse("vNext", {});

        // Segment-wise order, never lexicographic: 1.10.0 > 1.9.0.
        checkOrder("1.10.0", "1.9.0", 1, "1.10.0 is newer than 1.9.0");
        checkOrder("v0.6.0", "0.6.0", 0, "the v prefix changes nothing");
        checkOrder("0.5.9", "0.6.0", -1, "0.5.9 is older than 0.6.0");
        checkOrder("1.0", "1.0.0", 0, "a missing tail segment reads as zero");
        checkOrder("1.2.3", "1.2.3-rc1", 0, "a pre-release tag reads as its release");
        // An unreadable side never looks newer, either way around: a channel
        // glitch (an HTML error page parsed as a tag) must not trigger an
        // update, and a broken local build must not suppress one.
        checkOrder("garbage", "1.0", 0, "an unreadable remote never looks newer");
        checkOrder("1.0", "garbage", 0, "an unreadable local never looks older");

        std::printf("updater: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("updater: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
