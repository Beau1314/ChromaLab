// The localization core: the Mac catalog's four languages behind L() — the
// English literal is the key and the fallback, a language switch is one
// signal, the first launch derives once from the host languages and then
// persists, and the shipped catalog's scripts hold (spot-checked telltale
// characters, the same families tools/localization_check_windows.py polices).
// Everything else runs over a synthetic catalog in a QTemporaryDir — this
// test never touches the user's real language setting.
#include "Localization.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace l10n = spk::desktop;

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

// A minimal signal counter: one connection, the number of emissions, the
// language codes each emission carried.
class SwitchSpy : public QObject {
public:
    explicit SwitchSpy(l10n::Localization& localization) {
        QObject::connect(&localization, &l10n::Localization::languageChanged, this, [this, &localization] {
            ++count;
            codes << localization.language();
        });
    }
    int count = 0;
    QStringList codes;
};

void writeSyntheticCatalog(const QString& path) {
    const char* json =
        "{\n"
        " \"version\": 1,\n"
        " \"languages\": [\"en\", \"zh-Hans\", \"zh-Hant\", \"ja\"],\n"
        " \"strings\": {\n"
        "  \"Settings\": {\"zh-Hans\": \"设置\", \"zh-Hant\": \"設定\", \"ja\": \"設定\"},\n"
        // A key whose ja value never arrived: the runtime falls back to the key.
        "  \"Only Hans\": {\"zh-Hans\": \"简中\", \"zh-Hant\": \"繁中\"},\n"
        // Positional re-ordering: the translation names the arguments by position.
        "  \"%@ of %@ frames\": {\"zh-Hans\": \"第 %1$@ 张，共 %2$@ 张\",\n"
        "                         \"zh-Hant\": \"第 %1$@ 張，共 %2$@ 張\",\n"
        "                         \"ja\": \"%2$@枚中%1$@枚\"},\n"
        // Sequential %@ arguments, numbered in the value's own order.
        "  \"%@ frames\": {\"zh-Hans\": \"%@ 张\", \"zh-Hant\": \"%@ 張\", \"ja\": \"%@枚\"}\n"
        " }\n}\n";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot write synthetic catalog");
    check(file.write(json) == static_cast<qint64>(std::strlen(json)), "incomplete synthetic catalog");
}

void testCatalogLoads(const QString& shipped) {
    l10n::Localization localization;
    check(!localization.isLoaded(), "fresh instance is not loaded");
    check(localization.language() == QStringLiteral("en"), "fresh instance is English");
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    check(localization.isLoaded(), "loaded after loadCatalog");
    check(localization.languages() == QStringList{"en", "zh-Hans", "zh-Hant", "ja"},
          "the four languages, in the Mac's order");
    check(localization.keyCount() >= 1200, "the Mac catalog's 1203 keys survived the conversion");
    QTemporaryDir dir;
    check(!localization.loadCatalog(dir.filePath("absent.json")), "a missing catalog fails, keeping the old one");
    check(localization.isLoaded() && localization.keyCount() >= 1200, "the shipped catalog stays loaded");
}

void testEnglishIsKeyAndFallback() {
    l10n::Localization localization;
    localization.setLanguage(QStringLiteral("en"));
    check(localization.L(QStringLiteral("Settings")) == QStringLiteral("Settings"),
          "English never reads a table: the literal comes back byte for byte");
    check(localization.L(QStringLiteral("Not in any catalog")) == QStringLiteral("Not in any catalog"),
          "a key the catalog lacks falls back to the English literal");
    check(localization.L(QStringLiteral("%1 frames")).isEmpty() == false,
          "an unknown key with markers still falls back to itself");
    // Before any catalog: the same fallback holds (English is the degraded mode too).
    l10n::Localization unloaded;
    check(unloaded.L(QStringLiteral("Settings")) == QStringLiteral("Settings"),
          "with no catalog at all, English is still the literal");
}

void testLookupPerLanguage(const QString& shipped) {
    l10n::Localization localization;
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    localization.setLanguage(QStringLiteral("zh-Hans"));
    check(localization.L(QStringLiteral("Settings")) == QStringLiteral("设置"), "zh-Hans lookup");
    check(localization.L(QStringLiteral("Library")) == QStringLiteral("图库"), "zh-Hans lookup (Library)");
    check(localization.L(QStringLiteral("Done")) == QStringLiteral("完成"), "zh-Hans lookup (Done)");
    localization.setLanguage(QStringLiteral("zh-Hant"));
    check(localization.L(QStringLiteral("Library")) == QStringLiteral("圖庫"), "zh-Hant lookup");
    check(localization.L(QStringLiteral("Grain")) == QStringLiteral("顆粒"), "zh-Hant lookup (Grain)");
    localization.setLanguage(QStringLiteral("ja"));
    check(localization.L(QStringLiteral("Library")) == QStringLiteral("ライブラリ"), "ja lookup");
    check(localization.L(QStringLiteral("Pre-flash")) == QStringLiteral("予備露光"), "ja lookup (Pre-flash)");
}

void testSwitchEmitsAndBumpsEpoch() {
    l10n::Localization localization;
    SwitchSpy spy(localization);
    check(localization.epoch() == 1, "the epoch starts at 1 (the QML re-render idiom reads it)");
    localization.setLanguage(QStringLiteral("ja"));
    check(spy.count == 1, "one switch emits languageChanged once");
    check(localization.epoch() == 2, "the epoch bumps on the switch");
    localization.setLanguage(QStringLiteral("ja"));
    check(spy.count == 1, "setting the same language again is silent");
    localization.setLanguage(QStringLiteral("ko"));
    check(spy.count == 1 && localization.language() == QStringLiteral("ja"),
          "an unknown code is ignored, language and signal untouched");
    localization.setLanguage(QStringLiteral("zh-Hans"));
    check(localization.epoch() == 3, "a second real switch bumps again");
    check(spy.codes == QStringList{"ja", "zh-Hans"}, "each emission carries the new language");
}

void testMissingLanguageFallsBackToKey(const QString& synthetic) {
    l10n::Localization localization;
    check(localization.loadCatalog(synthetic), "synthetic catalog loads");
    localization.setLanguage(QStringLiteral("ja"));
    check(localization.L(QStringLiteral("Only Hans")) == QStringLiteral("Only Hans"),
          "a key with no ja value falls back to the English literal, never to another language");
    localization.setLanguage(QStringLiteral("zh-Hans"));
    check(localization.L(QStringLiteral("Only Hans")) == QStringLiteral("简中"),
          "the languages the key does have still look up");
}

void testInterpolation(const QString& shipped, const QString& synthetic) {
    l10n::Localization localization;
    check(localization.loadCatalog(synthetic), "synthetic catalog loads");
    // Positional %N$@ in a value becomes %N, so QString::arg fills it.
    localization.setLanguage(QStringLiteral("zh-Hans"));
    check(localization.L(QStringLiteral("%@ of %@ frames")) == QStringLiteral("第 %1 张，共 %2 张"),
          "positional arguments normalize to Qt's %N markers");
    check(localization.L(QStringLiteral("%@ of %@ frames"),
                         {QVariant::fromValue(3), QVariant::fromValue(9)})
              == QStringLiteral("第 3 张，共 9 张"),
          "arguments fill by number, so a reordered translation stays truthful");
    localization.setLanguage(QStringLiteral("ja"));
    check(localization.L(QStringLiteral("%@ of %@ frames"), {QVariant(QStringLiteral("x")), QVariant(QStringLiteral("y"))})
              == QStringLiteral("y枚中x枚"),
          "Japanese reorders the same two arguments");
    // Sequential %@ in a value numbers itself in order.
    check(localization.L(QStringLiteral("%@ frames"), {QVariant::fromValue(12)}) == QStringLiteral("12枚"),
          "sequential %@ arguments number in the value's own order");
    // The shipped Mac catalog's own mixed-style entry.
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    localization.setLanguage(QStringLiteral("zh-Hans"));
    check(localization.L(QStringLiteral("  ·  %@ of %@")) == QStringLiteral("  ·  %1 / %2"),
          "the shipped catalog normalizes the same way");
    check(localization.L(QStringLiteral(" Last sent %@."), {QVariant(QStringLiteral("12:00"))})
              == QStringLiteral("上次发送：12:00。"),
          "a trailing-period sentence interpolates");
    // English: the key IS the value, markers and all; args fill it the same way.
    localization.setLanguage(QStringLiteral("en"));
    check(localization.L(QStringLiteral("%@ frames"), {QVariant::fromValue(4)}) == QStringLiteral("4 frames"),
          "English interpolation works without the catalog");
}

void testDeriveRules() {
    const auto derive = [](QStringList hosts) { return l10n::Localization::deriveLanguage(hosts); };
    check(derive({QStringLiteral("zh")}) == QStringLiteral("zh-Hans"), "bare zh derives Simplified");
    check(derive({QStringLiteral("zh-CN")}) == QStringLiteral("zh-Hans"), "zh-CN derives Simplified");
    check(derive({QStringLiteral("zh-Hans")}) == QStringLiteral("zh-Hans"), "the script tag decides Hans");
    check(derive({QStringLiteral("zh-Hans-CN")}) == QStringLiteral("zh-Hans"), "Hans holds with a region");
    check(derive({QStringLiteral("zh-HK")}) == QStringLiteral("zh-Hant"), "Hong Kong derives Traditional");
    check(derive({QStringLiteral("zh_TW")}) == QStringLiteral("zh-Hant"), "an underscored subtag derives Traditional");
    check(derive({QStringLiteral("zh-Hant-TW")}) == QStringLiteral("zh-Hant"), "the script tag decides Hant");
    check(derive({QStringLiteral("zh-MO")}) == QStringLiteral("zh-Hant"), "Macau derives Traditional");
    check(derive({QStringLiteral("yue")}) == QStringLiteral("zh-Hant"), "Cantonese derives Traditional");
    check(derive({QStringLiteral("ja")}) == QStringLiteral("ja"), "ja derives Japanese");
    check(derive({QStringLiteral("ja-JP")}) == QStringLiteral("ja"), "ja-JP derives Japanese");
    check(derive({QStringLiteral("en-US")}) == QStringLiteral("en"), "en-US derives English");
    check(derive({QStringLiteral("en")}) == QStringLiteral("en"), "bare en derives English");
    check(derive({QStringLiteral("fr-FR"), QStringLiteral("en-GB")}) == QStringLiteral("en"),
          "the first host language the app speaks decides");
    check(derive({QStringLiteral("fr-FR"), QStringLiteral("zh-CN")}) == QStringLiteral("zh-Hans"),
          "a Chinese after an unspoken French still decides");
    check(derive({QStringLiteral("fr-FR"), QStringLiteral("de-DE")}) == QStringLiteral("en"),
          "nothing the app speaks derives English");
    check(derive({QStringLiteral("es-mx")}) == QStringLiteral("en"),
          "unspoken languages fall through to English");
    check(derive({}) == QStringLiteral("en"), "no host languages derive English");
}

void testFirstLaunchDerivesOnceAndPersists(const QString& shipped) {
    QTemporaryDir dir;
    const QString store = dir.filePath("settings.ini");
    {
        l10n::Localization localization;
        QSettings settings(store, QSettings::IniFormat);
        localization.useSettings(&settings);
        check(localization.loadCatalog(shipped), "shipped catalog loads");
        check(!settings.contains(QStringLiteral("language")), "a fresh store holds no language yet");
        localization.bootstrap({QStringLiteral("fr-FR"), QStringLiteral("zh-CN")});
        check(localization.language() == QStringLiteral("zh-Hans"),
              "the first launch derives from the host languages");
        check(settings.value(QStringLiteral("language")).toString() == QStringLiteral("zh-Hans"),
              "the derived language is persisted at once");
    }
    {
        // The second launch: the stored choice stands even as the host says Japanese.
        l10n::Localization localization;
        QSettings settings(store, QSettings::IniFormat);
        localization.useSettings(&settings);
        check(localization.loadCatalog(shipped), "shipped catalog loads");
        localization.bootstrap({QStringLiteral("ja-JP")});
        check(localization.language() == QStringLiteral("zh-Hans"),
              "the derived choice persists; the host languages are never asked again");
    }
}

void testUserChoiceSurvives(const QString& shipped) {
    QTemporaryDir dir;
    const QString store = dir.filePath("settings.ini");
    {
        l10n::Localization localization;
        QSettings settings(store, QSettings::IniFormat);
        localization.useSettings(&settings);
        check(localization.loadCatalog(shipped), "shipped catalog loads");
        localization.bootstrap({QStringLiteral("en-US")});
        localization.setLanguage(QStringLiteral("zh-Hant"));
        check(settings.value(QStringLiteral("language")).toString() == QStringLiteral("zh-Hant"),
              "an explicit choice persists too");
    }
    l10n::Localization localization;
    QSettings settings(store, QSettings::IniFormat);
    localization.useSettings(&settings);
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    localization.bootstrap({QStringLiteral("en-US")});
    check(localization.language() == QStringLiteral("zh-Hant"), "the user's choice survives a restart");
    check(localization.L(QStringLiteral("Library")) == QStringLiteral("圖庫"),
          "the restart's lookups use the persisted language");
    // A stored code the app no longer knows degrades to English, not to a crash.
    settings.setValue(QStringLiteral("language"), QStringLiteral("ko"));
    l10n::Localization second;
    QSettings settings2(store, QSettings::IniFormat);
    second.useSettings(&settings2);
    check(second.loadCatalog(shipped), "shipped catalog loads");
    second.bootstrap({QStringLiteral("en-US")});
    check(second.language() == QStringLiteral("en"), "a corrupted stored code derives afresh");
}

void testScriptPolicingSpotChecks(const QString& shipped) {
    // The telltale characters, straight from the Mac's table families
    // (app/SpektraLab/tools/localization_check.py): a few of each kind, not a
    // dictionary. The full tables police the catalog in the check gate; this
    // is the shipped asset's own runtime guard.
    // Single characters only: a compound like 设置/設定 shares its second
    // character (置/定) with both scripts, and would fail a correct sentence.
    // 灯 is left out too: Japanese writes it (the Mac table's JA_WRITES
    // carve-out), so it would fail a correct ja value.
    const QString simplifiedOnly = QStringLiteral("设图过还张导档库帧胶电显测滤阅");
    const QString traditionalOnly = QStringLiteral("設圖過還張導檔庫幀膠電顯測濾燈閱");
    const QString notJapanese = QStringLiteral("设图冲黑會體國邊轉");
    l10n::Localization localization;
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    int zhHansChecked = 0, zhHantChecked = 0, jaChecked = 0;
    for (const QString& key : localization.keys()) {
        const QString hans = localization.valueFor(key, QStringLiteral("zh-Hans"));
        const QString hant = localization.valueFor(key, QStringLiteral("zh-Hant"));
        const QString ja = localization.valueFor(key, QStringLiteral("ja"));
        check(!hant.isEmpty() && !ja.isEmpty(), "every key of the shipped catalog has all three languages");
        for (const QChar& ch : simplifiedOnly) {
            check(!hant.contains(ch), "zh-Hant holds no Simplified-only character");
            check(!ja.contains(ch), "ja holds no Simplified character Japanese does not write");
        }
        for (const QChar& ch : traditionalOnly)
            check(!hans.contains(ch), "zh-Hans holds no Traditional-only character");
        for (const QChar& ch : notJapanese)
            check(!ja.contains(ch), "ja holds no old form (旧字体) modern Japanese writes otherwise");
        ++zhHansChecked; ++zhHantChecked; ++jaChecked;
    }
    check(zhHantChecked >= 1200, "the policing loop covered the whole catalog");
}

void testKeysAndValueFor(const QString& shipped) {
    l10n::Localization localization;
    check(localization.loadCatalog(shipped), "shipped catalog loads");
    check(localization.keys().contains(QStringLiteral("Settings")), "keys() exposes the catalog's keys");
    check(localization.keys().contains(QStringLiteral("Done")), "keys() exposes the catalog's keys (Done)");
    check(localization.valueFor(QStringLiteral("Settings"), QStringLiteral("zh-Hans")) == QStringLiteral("设置"),
          "valueFor reads one language of one key");
    check(localization.valueFor(QStringLiteral("Absent"), QStringLiteral("zh-Hans")).isEmpty(),
          "valueFor of an absent key is empty, not the key");
}

void testImportedQmlSingletonExists(const QString& shipped) {
    // The QML seam: the same C++ object backs the QML type. The instance()
    // accessor and the engine's create() hand back the one instance, so a
    // C++ switch is every page's at once.
    l10n::Localization& shared = l10n::Localization::instance();
    SwitchSpy spy(shared);
    check(shared.loadCatalog(shipped), "the shared instance loads the shipped catalog");
    shared.setLanguage(QStringLiteral("zh-Hans"));
    check(spy.count == 1, "the shared instance signals like any other");
    shared.setLanguage(QStringLiteral("en"));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("SpektraLab"));
    application.setApplicationName(QStringLiteral("SpektraLabQt"));
    if (argc < 2) {
        std::printf("localization: FAIL: argv[1] (the shipped localization.json) is required\n");
        return 2;
    }
    const QString shipped = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1]));
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("localization: FAIL: no temporary directory\n");
        return 2;
    }
    const QString synthetic = dir.filePath("synthetic.json");
    writeSyntheticCatalog(synthetic);
    try {
        testCatalogLoads(shipped);
        testEnglishIsKeyAndFallback();
        testLookupPerLanguage(shipped);
        testSwitchEmitsAndBumpsEpoch();
        testMissingLanguageFallsBackToKey(synthetic);
        testInterpolation(shipped, synthetic);
        testDeriveRules();
        testFirstLaunchDerivesOnceAndPersists(shipped);
        testUserChoiceSurvives(shipped);
        testScriptPolicingSpotChecks(shipped);
        testKeysAndValueFor(shipped);
        testImportedQmlSingletonExists(shipped);
        std::printf("localization: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("localization: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
