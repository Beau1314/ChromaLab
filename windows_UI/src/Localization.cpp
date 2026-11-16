#include "Localization.hpp"

#include <QFile>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QQmlEngine>
#include <QSettings>
#include <QtDebug>

namespace spk::desktop {

const QStringList& Localization::kLanguages = *new QStringList{
    QStringLiteral("en"), QStringLiteral("zh-Hans"), QStringLiteral("zh-Hant"), QStringLiteral("ja")};

namespace {
// The embedded catalog (qt_add_resources in windows_UI/CMakeLists.txt,
// generated from the Mac's Localizable.xcstrings). The resource keeps the
// file's path under the source tree: PREFIX "/" + assets/localization.json.
constexpr const char* kDefaultCatalog = ":/assets/localization.json";
constexpr const char* kLanguageKey = "language";

// The catalog's markers are the Mac's: %@ sequential, %N$@ positional, %%
// a per-cent sign, and numeric formats (%.1f) untouched. This rewrites the
// first two as Qt's %N so QString::arg fills them; a %N already written
// plainly passes through.
QString normalizeMarkers(const QString& text) {
    QString out;
    out.reserve(text.size() + 4);
    int sequential = 1;
    for (int i = 0; i < text.size();) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('%')) {
            out.append(ch);
            ++i;
            continue;
        }
        if (i + 1 < text.size()) {
            const QChar next = text.at(i + 1);
            if (next == QLatin1Char('%')) {
                out.append(QLatin1String("%%"));
                i += 2;
                continue;
            }
            if (next == QLatin1Char('@')) {
                out.append(QLatin1Char('%') + QString::number(sequential++));
                i += 2;
                continue;
            }
            if (next.isDigit() && i + 3 < text.size() && text.at(i + 2) == QLatin1Char('$')
                && text.at(i + 3) == QLatin1Char('@')) {
                out.append(QLatin1Char('%'));
                out.append(next);
                i += 4;
                continue;
            }
        }
        out.append(ch);
        ++i;
    }
    return out;
}
} // namespace

Localization::Localization(QObject* parent) : QObject(parent) {}

Localization& Localization::instance() {
    static Localization theInstance(nullptr);
    return theInstance;
}

Localization* Localization::create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
    // The application and every QML engine share the one instance; the
    // engine must never delete it.
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    QQmlEngine::setObjectOwnership(&instance(), QQmlEngine::CppOwnership);
    return &instance();
}

// Returns whether this call loaded a catalog; on failure the previously
// loaded one (if any) stays standing and lookups keep working.
bool Localization::loadCatalog(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // The default catalog is embedded, so a failure there is a build
        // defect worth one startup line — the smoke gate reads stderr.
        qWarning("Localization: the catalog %s cannot be read", qUtf8Printable(path));
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (document.isNull() || !document.isObject()) {
        qWarning("Localization: the catalog %s is not JSON: %s", qUtf8Printable(path),
                 qUtf8Printable(error.errorString()));
        return false;
    }
    const QJsonObject strings = document.object().value(QLatin1String("strings")).toObject();
    if (strings.isEmpty()) {
        qWarning("Localization: the catalog %s holds no strings", qUtf8Printable(path));
        return false;
    }
    QHash<QString, Entry> parsed;
    parsed.reserve(strings.size());
    int incomplete = 0;
    for (auto it = strings.begin(); it != strings.end(); ++it) {
        const QJsonObject values = it.value().toObject();
        Entry entry;
        entry.hans = values.value(QLatin1String("zh-Hans")).toString();
        entry.hant = values.value(QLatin1String("zh-Hant")).toString();
        entry.ja = values.value(QLatin1String("ja")).toString();
        if (entry.hans.isEmpty() || entry.hant.isEmpty() || entry.ja.isEmpty()) {
            // The check gate (tools/localization_check_windows.py) holds the
            // catalog to four complete languages; the runtime degrades by
            // falling back to the key for whatever is missing.
            ++incomplete;
        }
        parsed.insert(it.key(), entry);
    }
    m_strings = parsed;
    if (incomplete)
        qWarning("Localization: %d of %d catalog keys hold an incomplete language set",
                 incomplete, int(parsed.size()));
    return isLoaded();
}

QString Localization::valueFor(const QString& key, const QString& language) const {
    const auto found = m_strings.constFind(key);
    if (found == m_strings.constEnd())
        return {};
    if (language == QLatin1String("zh-Hans"))
        return found->hans;
    if (language == QLatin1String("zh-Hant"))
        return found->hant;
    if (language == QLatin1String("ja"))
        return found->ja;
    return {};
}

QString Localization::resolve(const QString& key) const {
    if (m_language == QLatin1String("en"))
        return normalizeMarkers(key);
    const QString value = valueFor(key, m_language);
    return normalizeMarkers(value.isEmpty() ? key : value);
}

QString Localization::L(const QString& key) const {
    return resolve(key);
}

QString Localization::L(const QString& key, const QVariantList& arguments) const {
    QString text = resolve(key);
    for (const QVariant& argument : arguments)
        text = text.arg(argument.toString());
    return text;
}

void Localization::setLanguage(const QString& code) {
    if (!kLanguages.contains(code)) {
        qWarning("Localization: %s is not one of the four languages; keeping %s",
                 qUtf8Printable(code), qUtf8Printable(m_language));
        return;
    }
    if (code == m_language)
        return;
    m_language = code;
    ++m_epoch;
    if (QSettings* store = settingsStore()) {
        store->setValue(QLatin1String(kLanguageKey), code);
        store->sync();
    }
    emit languageChanged();
}

void Localization::useSettings(QSettings* store) {
    m_userSettings = store;
}

QSettings* Localization::settingsStore() {
    if (m_userSettings)
        return m_userSettings.data();
    if (!m_ownedSettings)
        m_ownedSettings = std::make_unique<QSettings>();
    return m_ownedSettings.get();
}

QString Localization::storedLanguage() const {
    const QSettings* store = m_userSettings ? m_userSettings.data() : m_ownedSettings.get();
    if (!store)
        return {};
    return store->value(QLatin1String(kLanguageKey)).toString();
}

void Localization::bootstrap(const QStringList& hostLanguages) {
    if (!isLoaded())
        loadCatalog(QLatin1String(kDefaultCatalog));
    // The production store exists before the first read: without this the
    // default-constructed QSettings never comes into being and every launch
    // would re-derive (the tests inject their INI with useSettings, so only
    // the production path was blind to the stored choice — a task 6.0 gap
    // the 6.1 smoke caught: the picker's choice must survive restarts).
    settingsStore();
    const QString stored = storedLanguage();
    if (kLanguages.contains(stored)) {
        if (stored != m_language)
            setLanguage(stored);
        return;
    }
    if (!stored.isEmpty())
        qWarning("Localization: the stored language %s is not one of the four; deriving afresh",
                 qUtf8Printable(stored));
    setLanguage(deriveLanguage(hostLanguages));
}

QString Localization::deriveLanguage(const QStringList& hostLanguages) {
    for (const QString& raw : hostLanguages) {
        const QStringList tags = QString(raw).toLower().replace(QLatin1Char('_'), QLatin1Char('-'))
                                     .split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (tags.isEmpty())
            continue;
        const QString& language = tags.first();
        if (language == QLatin1String("en"))
            return QStringLiteral("en");
        if (language == QLatin1String("ja"))
            return QStringLiteral("ja");
        if (language == QLatin1String("yue"))
            return QStringLiteral("zh-Hant");
        if (language == QLatin1String("zh")) {
            for (int i = 1; i < tags.size(); ++i) {
                const QString& tag = tags.at(i);
                if (tag == QLatin1String("hant"))
                    return QStringLiteral("zh-Hant");
                if (tag == QLatin1String("hans"))
                    return QStringLiteral("zh-Hans");
                if (tag == QLatin1String("tw") || tag == QLatin1String("hk")
                    || tag == QLatin1String("mo"))
                    return QStringLiteral("zh-Hant");
            }
            return QStringLiteral("zh-Hans");
        }
    }
    return QStringLiteral("en");
}

} // namespace spk::desktop
