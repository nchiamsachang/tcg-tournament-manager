// Interface preferences, kept in settings.json beside the database.
#pragma once

#include <QJsonObject>
#include <QStringList>

namespace prefs {

extern const char *const VERSION;
extern const char *const REPO_URL;
extern const char *const PRODUCER;      // the "Produced by" credit in the page footer
QString issuesUrl();

QString filePath();
QJsonObject load();                 // always complete: missing or invalid entries come back as defaults
// Both return the preferences as they now are on disk; if the file could not be written
// that is the previous contents, so the caller always shows what was really saved.
QJsonObject setPref(const QString &key, const QJsonValue &value);
QJsonObject restoreDefaults();      // interface preferences only
int defaultRoundMinutes(const QString &game);

} // namespace prefs
