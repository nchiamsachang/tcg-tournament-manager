#include "prefs.h"

#include "db.h"
#include "tournaments.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace prefs {

const char *const VERSION = TCG_VERSION;       // set in the top-level CMakeLists.txt
const char *const BUILD = TCG_BUILD;           // worked out there too, from Git
// verified from this project's Git remote (origin)
const char *const REPO_URL = "https://github.com/nchiamsachang/tcg-tournament-manager";

const char *const PRODUCER = "Nathan Chiamsachang";

QString issuesUrl()
{
    return QString::fromLatin1(REPO_URL) + "/issues";
}

static QJsonObject interfaceDefaults()
{
    return {
        {"theme", "system"},            // light | dark | system
        {"text_size", "standard"},      // standard | large
        {"alert_sound", false},         // play a sound when a round timer reaches zero
        {"alert_notify", true},         // show an in-app notice when a round timer reaches zero
    };
}

QString filePath()
{
    return db::dataDir() + "/settings.json";
}

QJsonObject load()
{
    QJsonObject data;
    QFile f(filePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject())
            data = doc.object();
    }
    // kept from earlier versions: the suggested round length when creating a tournament
    for (const char *game : {"ONEPIECE", "POKEMON", "MTG"})
        if (!data.contains(game))
            data[game] = tdb::DEFAULT_ROUND_MINUTES;
    const QJsonObject defaults = interfaceDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!data.contains(it.key()))
            data[it.key()] = it.value();
    if (!QStringList{"light", "dark", "system"}.contains(data["theme"].toString()))
        data["theme"] = "system";
    if (!QStringList{"standard", "large"}.contains(data["text_size"].toString()))
        data["text_size"] = "standard";
    for (const char *key : {"alert_sound", "alert_notify"}) {
        const QJsonValue v = data[key];
        data[key] = v.isBool() ? v.toBool() : (v.isDouble() ? v.toDouble() != 0 : !v.toString().isEmpty());
    }
    return data;
}

static bool save(const QJsonObject &data)
{
    QSaveFile f(filePath());       // written to a temporary file and swapped in, so a failure leaves the old file
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    return f.commit();
}

QJsonObject setPref(const QString &key, const QJsonValue &value)
{
    QJsonObject data = load();
    data[key] = value;
    return save(data) ? data : load();
}

QJsonObject restoreDefaults()
{
    QJsonObject data = load();
    const QJsonObject defaults = interfaceDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        data[it.key()] = it.value();
    return save(data) ? data : load();
}

int defaultRoundMinutes(const QString &game)
{
    const QJsonValue v = load().value(game);
    const int mins = v.isDouble() ? int(v.toDouble()) : v.toString().toInt();
    return mins ? qBound(tdb::MIN_ROUND_MINUTES, mins, tdb::MAX_ROUND_MINUTES) : tdb::DEFAULT_ROUND_MINUTES;
}

} // namespace prefs
