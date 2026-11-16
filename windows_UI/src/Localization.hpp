// The localization core (task 6.0): the Mac catalog's four languages behind
// L(), ported from app/SpektraLab/LOCALIZATION.md and Sources/Model/
// Localization.swift. The English literal is the key AND the fallback; the
// catalog (windows_UI/assets/localization.json, converted from the Mac's
// Localizable.xcstrings by windows_UI/tools/xcstrings_to_json.py) holds
// zh-Hans, zh-Hant and ja. There is no follow-the-system: the first launch
// derives once from the host languages (AppLanguage.derived's region rules)
// and persists; from then on the stored choice stands.
//
// The QML seam is this object registered as the QML singleton
// `Localization` (same module, so pages need no import — Theme.qml set that
// convention). One C++ instance backs every page: a switch emits
// languageChanged and bumps `epoch`, and a QML binding that shows a looked-up
// sentence reads the epoch so it re-evaluates:
//
//     text: Localization.epoch && Localization.L("Reset colour")
//     text: Localization.epoch && Localization.L("%@ frames", [frames])
//
// The catalog's Mac-style markers (%@ sequential, %N$@ positional) are
// normalized to Qt's %N at lookup, so QString::arg fills them and a
// reordered translation keeps its meaning. Numeric formats (%.1f) pass
// through untouched, like the Mac's String(format:) keys.
#ifndef SPK_DESKTOP_LOCALIZATION_HPP
#define SPK_DESKTOP_LOCALIZATION_HPP

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QVariantList>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace spk::desktop {

class Localization : public QObject {
    Q_OBJECT
    // The QML singleton: the engine hands back the one instance() below.
    QML_SINGLETON
    QML_NAMED_ELEMENT(Localization)
    // The current language code: "en", "zh-Hans", "zh-Hant" or "ja".
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged FINAL)
    // Starts at 1 and bumps with every language change. QML bindings read it
    // ahead of L() so a switch re-renders every page at once.
    Q_PROPERTY(int epoch READ epoch NOTIFY languageChanged FINAL)

public:
    // Requires an argument on purpose: a default-constructible QML_SINGLETON
    // is engine-constructed with `new T` and the static create() factory is
    // ignored (Qt's singletonConstructionMode prefers Constructor), which
    // would give QML its own English instance beside the bootstrapped one.
    // Tests construct with nullptr.
    explicit Localization(QObject* parent);

    // The one instance the QML engine and the application share.
    static Localization& instance();
    static Localization* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // Parses the converted catalog ({"version":1, "languages":[en, zh-Hans,
    // zh-Hant, ja], "strings":{key:{...}}}). A key missing a language is kept
    // (its lookups fall back to the key); an unreadable file fails and leaves
    // the previous catalog standing. Returns whether this call loaded one.
    bool loadCatalog(const QString& path);
    bool isLoaded() const { return !m_strings.isEmpty(); }
    int keyCount() const { return int(m_strings.size()); }
    QStringList keys() const { return QStringList(m_strings.keyBegin(), m_strings.keyEnd()); }
    // One language of one key, raw (markers untouched); empty when absent.
    Q_INVOKABLE QString valueFor(const QString& key, const QString& language) const;

    QStringList languages() const { return kLanguages; }

    QString language() const { return m_language; }
    void setLanguage(const QString& code);
    int epoch() const { return m_epoch; }

    // The lookup: the English literal is the key and the fallback. The
    // value's markers come back normalized to %N; with arguments they are
    // filled in order, so a reordered translation keeps its meaning.
    Q_INVOKABLE QString L(const QString& key) const;
    Q_INVOKABLE QString L(const QString& key, const QVariantList& arguments) const;

    // AppLanguage.derived: the first host language the app speaks decides —
    // en→en, ja→ja, zh→zh-Hans unless the script is Hant or the region is
    // TW/HK/MO (or the language is Cantonese), anything else falls through,
    // and nothing spoken derives English. Underscored subtags parse the same.
    static QString deriveLanguage(const QStringList& hostLanguages);

    // The language store. Production uses QSettings() (the application's
    // organization/name); a test hands over its own before bootstrap().
    void useSettings(QSettings* store);

    // main.cpp calls this once, before the QML engine loads: load the
    // embedded catalog and take the stored language, or — on the first
    // launch, when none is stored — derive once from the host languages and
    // persist. After this the host languages are never consulted again.
    void bootstrap(const QStringList& hostLanguages);

signals:
    void languageChanged();

private:
    // The catalog value for the current language, the key itself when absent,
    // with markers normalized to %N.
    QString resolve(const QString& key) const;
    QSettings* settingsStore();
    QString storedLanguage() const;

    struct Entry {
        QString hans;
        QString hant;
        QString ja;
    };
    QHash<QString, Entry> m_strings;
    QString m_language = QStringLiteral("en");
    int m_epoch = 1;
    QPointer<QSettings> m_userSettings;
    std::unique_ptr<QSettings> m_ownedSettings;

    static const QStringList& kLanguages;
};

} // namespace spk::desktop

#endif // SPK_DESKTOP_LOCALIZATION_HPP
