#include "applog.h"

#include "db.h"
#include "prefs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSysInfo>
#include <algorithm>
#include <exception>

namespace applog {

namespace {

QMutex g_lock;
QString g_dir;
QFile g_file;
qint64 g_maxBytes = 2 * 1024 * 1024;
int g_maxFiles = 5;
QStringList g_pending;              // entries from before the directory was known
const int MAX_PENDING = 400;
const int MAX_DETAIL = 2000;        // one entry never grows without bound

// When the file cannot be written: why, since when, how many entries were not written, the
// newest of those (for a report), and when it was last tried.
bool g_broken = false;
QString g_reason, g_since;
int g_lost = 0;
QStringList g_unwritten;
const int MAX_UNWRITTEN = 100;
QElapsedTimer g_lastTry;
int g_retryMs = 30000;

QString fileName(int n)
{
    return QDir(g_dir).filePath(n == 0 ? QStringLiteral("app.log") : QStringLiteral("app.%1.log").arg(n));
}

void fail(const QString &reason)
{
    if (g_file.isOpen())
        g_file.close();
    if (!g_broken) {
        g_broken = true;
        g_since = QDateTime::currentDateTime().toString(Qt::ISODate);
        g_lost = 0;
    }
    g_reason = reason;
    g_lastTry.start();
}

// Called with the lock held.  Never throws.
bool openFile()
{
    if (g_dir.isEmpty())
        return false;
    if (g_file.isOpen())
        return true;
    if (!QDir().mkpath(g_dir)) {
        fail(QStringLiteral("the log folder could not be created"));
        return false;
    }
    g_file.setFileName(fileName(0));
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        fail(QStringLiteral("the log file could not be opened (%1)").arg(g_file.errorString()));
        return false;
    }
    return true;
}

void rotate()
{
    g_file.close();
    QFile::remove(fileName(g_maxFiles - 1));
    for (int n = g_maxFiles - 2; n >= 0; --n)
        if (QFile::exists(fileName(n)))
            QFile::rename(fileName(n), fileName(n + 1));
}

bool put(const QString &line)
{
    if (!openFile())
        return false;
    const QByteArray bytes = line.toUtf8() + '\n';
    if (g_file.size() > 0 && g_file.size() + bytes.size() > g_maxBytes && g_maxFiles > 1) {
        rotate();
        if (!openFile())
            return false;
    }
    if (g_file.write(bytes) != bytes.size() || !g_file.flush()) {
        fail(QStringLiteral("the log file could not be written (%1)").arg(g_file.errorString()));
        return false;
    }
    return true;
}

QString stamp()
{
    // 2026-10-09T14:03:22.123-05:00: local time with its offset from UTC
    const QDateTime now = QDateTime::currentDateTime();
    QString text = now.toString(Qt::ISODateWithMs);
    if (now.timeSpec() == Qt::LocalTime) {
        const int offset = now.offsetFromUtc();
        text += QStringLiteral("%1%2:%3").arg(offset < 0 ? '-' : '+').arg(qAbs(offset) / 3600, 2, 10, QChar('0'))
                    .arg(qAbs(offset) % 3600 / 60, 2, 10, QChar('0'));
    }
    return text;
}

void keepUnwritten(const QString &line)
{
    ++g_lost;
    g_unwritten << line;
    if (g_unwritten.size() > MAX_UNWRITTEN)
        g_unwritten.removeFirst();
}

void append(const QString &line)
{
    if (g_dir.isEmpty()) {
        if (g_pending.size() < MAX_PENDING)
            g_pending << line;
        return;
    }
    if (g_broken) {
        // not on every entry: the file is tried again only now and then
        if (!g_lastTry.isValid() || g_lastTry.elapsed() < g_retryMs) {
            keepUnwritten(line);
            return;
        }
        const int lost = g_lost;
        const QString since = g_since, reason = g_reason;
        const QStringList kept = g_unwritten;
        g_broken = false;
        if (!put(stamp() + QStringLiteral(" WARN  ") + QString::fromLatin1(prefs::VERSION)
                 + QStringLiteral(" log.resumed lost=%1 since=%2").arg(lost).arg(since))) {
            g_since = since;            // still not possible: keep counting from the first failure
            g_lost = lost;
            g_unwritten = kept;
            keepUnwritten(line);
            return;
        }
        g_lost = 0;
        g_unwritten.clear();
        g_reason.clear();
        g_since.clear();
    }
    if (!put(line))
        keepUnwritten(line);
}

QString levelText(Level level)
{
    return level == Level::Error ? QStringLiteral("ERROR") : level == Level::Warning ? QStringLiteral("WARN ") : QStringLiteral("INFO ");
}

QString oneLine(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral(" / ")).replace('\n', QStringLiteral(" / ")).replace('\r', ' ');
    if (text.size() > MAX_DETAIL)
        text = text.left(MAX_DETAIL) + QStringLiteral(" …");
    return text;
}

QString fieldText(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool)
        return value.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    QString text = oneLine(value.toString());
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_.:+#-]*$"));
    if (!plain.match(text).hasMatch() || text.isEmpty())
        text = '"' + text.replace('"', '\'') + '"';
    return text;
}

// The operations this thread is inside, outermost first, and the one that failed last.
thread_local QStringList t_operations;
thread_local QString t_failedOperation, t_failedReference;
thread_local QElapsedTimer t_failedAt;
const int FAILURE_FRESH_MS = 15000;     // an error dialog follows a failed action at once

QString newOperation()
{
    return QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')).toUpper();
}

bool hasField(const Fields &fields, const QString &key)
{
    for (const auto &f : fields)
        if (f.first == key)
            return true;
    return false;
}

} // namespace

void setDirectory(const QString &dir)
{
    try {
        QMutexLocker lock(&g_lock);
        if (g_file.isOpen())
            g_file.close();
        g_dir = dir;
        g_broken = false;
        g_reason.clear();
        g_since.clear();
        g_lost = 0;
        g_unwritten.clear();
        const QStringList pending = g_pending;
        g_pending.clear();
        if (!dir.isEmpty())             // no folder: what was held is dropped, and entries are held again
            for (const QString &line : pending)
                append(line);
    } catch (...) {
    }
}

QString directory()
{
    QMutexLocker lock(&g_lock);
    return g_dir;
}

void setLimits(qint64 maxBytes, int files)
{
    QMutexLocker lock(&g_lock);
    g_maxBytes = qMax<qint64>(1024, maxBytes);
    g_maxFiles = qMax(1, files);
}

void setRetryInterval(int milliseconds)
{
    QMutexLocker lock(&g_lock);
    g_retryMs = qMax(0, milliseconds);
}

Status status()
{
    QMutexLocker lock(&g_lock);
    Status s;
    s.available = !g_broken;
    s.reason = g_reason;
    s.since = g_since;
    s.lost = g_lost;
    return s;
}

void write(Level level, const QString &action, const Fields &fields, const QString &detail)
{
    try {
        QString line = stamp() + ' ' + levelText(level) + ' ' + QString::fromLatin1(prefs::VERSION) + ' ' + action;
        for (const auto &f : fields)
            line += ' ' + f.first + '=' + fieldText(f.second);
        // anything logged while an operation runs on this thread belongs to it
        if (!t_operations.isEmpty() && !hasField(fields, QStringLiteral("op")))
            line += QStringLiteral(" op=") + t_operations.last();
        if (!detail.isEmpty())
            line += QStringLiteral(" | ") + redact(oneLine(detail));
        QMutexLocker lock(&g_lock);
        append(line);
    } catch (...) {
        // a log entry is never worth an error of its own
    }
}

QString newReference()
{
    // no vowels (no accidental words) and no look-alike characters
    static const char alphabet[] = "23456789BCDFGHJKMNPQRSTVWXZ";
    QString ref = QStringLiteral("R-");
    for (int i = 0; i < 6; ++i)
        ref += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(int(sizeof alphabet) - 1)]);
    return ref;
}

Action::Action(const QString &name, const Fields &fields)
    : name_(name), fields_(fields), exceptions_(std::uncaught_exceptions())
{
    op_ = t_operations.isEmpty() ? newOperation() : t_operations.last();    // inside another operation: the same one
    t_failedOperation.clear();          // a new action: an earlier failure is no longer "the one that just failed"
    t_failedReference.clear();
    Fields f{{"status", "attempt"}};
    write(Level::Info, name_, f + fields_ + Fields{{"op", op_}});
    t_operations.append(op_);
}

Action::~Action()
{
    if (!t_operations.isEmpty())
        t_operations.removeLast();
    const bool failed = std::uncaught_exceptions() > exceptions_;
    // Still inside a caller's transaction: this step is done but nothing is saved yet, and it
    // may still be rolled back.  Only the action that owns the transaction can say "ok".
    const QString done = db::inTransaction() ? QStringLiteral("pending") : QStringLiteral("ok");
    Fields f{{"status", failed ? QStringLiteral("failed") : (outcome_.isEmpty() ? done : outcome_)}};
    f = f + fields_ + Fields{{"op", op_}};
    if (failed) {
        // one reference for the whole failed operation, also when it was nested
        if (t_failedOperation != op_ || t_failedReference.isEmpty()) {
            t_failedOperation = op_;
            t_failedReference = newReference();
        }
        t_failedAt.start();
        f.append({"ref", t_failedReference});
    }
    write(failed ? Level::Warning : Level::Info, name_, f);
}

void Action::set(const QString &key, const QVariant &value)
{
    for (auto &f : fields_) {
        if (f.first == key) {
            f.second = value;
            return;
        }
    }
    fields_.append({key, value});
}

void Action::outcome(const QString &result)
{
    outcome_ = result;
}

QString currentOperation()
{
    return t_operations.isEmpty() ? QString() : t_operations.last();
}

OperationScope::OperationScope(const QString &operation)
{
    if (!operation.isEmpty()) {
        t_operations.append(operation);
        pushed_ = true;
    }
}

OperationScope::~OperationScope()
{
    if (pushed_ && !t_operations.isEmpty())
        t_operations.removeLast();
}

QString errorReference(QString *operation)
{
    const bool fresh = !t_failedReference.isEmpty() && t_failedAt.isValid() && t_failedAt.elapsed() < FAILURE_FRESH_MS;
    const QString ref = fresh ? t_failedReference : newReference();
    if (operation)
        *operation = fresh ? t_failedOperation : currentOperation();
    t_failedReference.clear();          // used once: the next error is a different one
    t_failedOperation.clear();
    return ref;
}

// Qt's own messages

static QtMessageHandler g_previous = nullptr;
static thread_local bool t_inHandler = false;

static void qtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (!t_inHandler && (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)) {
        t_inHandler = true;         // anything logging itself triggers is not logged again
        static QMutex seenLock;
        static QString last;
        static int count = 0;
        bool wanted = false;
        {
            QMutexLocker lock(&seenLock);
            // the same message over and over is written once, and a session is capped
            if (message != last && count < 300) {
                last = message;
                ++count;
                wanted = true;
            }
        }
        if (wanted)
            write(type == QtWarningMsg ? Level::Warning : Level::Error, QStringLiteral("qt.message"),
                  {{"category", QString::fromLatin1(context.category ? context.category : "default")}}, message);
        t_inHandler = false;
    }
    if (g_previous)
        g_previous(type, context, message);
}

void captureQtMessages()
{
    g_previous = qInstallMessageHandler(qtMessage);
}

static QString sessionMarker()
{
    const QString dir = directory();
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("session.open"));
}

void sessionStarted()
{
    try {
        const QString marker = sessionMarker();
        const bool unclean = !marker.isEmpty() && QFile::exists(marker);
        info(QStringLiteral("app.start"),
             {{"build", QString::fromLatin1(prefs::BUILD)}, {"qt", QString::fromLatin1(qVersion())},
              {"os", QSysInfo::prettyProductName()}, {"arch", QSysInfo::currentCpuArchitecture()},
              {"pid", QCoreApplication::applicationPid()}},
             QStringLiteral("data folder ") + QDir::toNativeSeparators(db::dataDir()));
        if (unclean)
            warning(QStringLiteral("app.previous_session"), {{"status", "unclean"}},
                    QStringLiteral("The previous session did not close normally. The reason is not known: the app may have been "
                                   "ended from outside, the computer may have shut down or lost power, or the app may have stopped "
                                   "unexpectedly. Its entries are above; the last ones may be missing."));
        if (!marker.isEmpty()) {
            QFile f(marker);
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray::number(QCoreApplication::applicationPid()));
        }
    } catch (...) {
    }
}

void sessionEnded()
{
    try {
        info(QStringLiteral("app.exit"));
        const QString marker = sessionMarker();
        if (!marker.isEmpty())
            QFile::remove(marker);
    } catch (...) {
    }
}

QString redact(const QString &text)
{
    QString out = text;
    const QString home = QDir::homePath();              // C:/Users/name
    if (home.size() > 3) {
        out.replace(home, QStringLiteral("~"), Qt::CaseInsensitive);
        out.replace(QDir::toNativeSeparators(home), QStringLiteral("~"), Qt::CaseInsensitive);
    }
    // any other user's folder that turns up in a path (a file dialog, a message from the system)
    static const QRegularExpression users(QStringLiteral("([A-Za-z]:[\\\\/]Users[\\\\/])[^\\\\/\\s\"']+"),
                                          QRegularExpression::CaseInsensitiveOption);
    out.replace(users, QStringLiteral("\\1…"));
    return out;
}

QStringList recentLines(int maxLines, qint64 maxBytes)
{
    QStringList lines;
    try {
        QString dir;
        int files;
        {
            QMutexLocker lock(&g_lock);
            if (g_file.isOpen())
                g_file.flush();
            dir = g_dir;
            files = g_maxFiles;
        }
        if (dir.isEmpty())
            return lines;
        qint64 bytes = 0;
        // newest file first, reading each from its end, until enough has been collected
        for (int n = 0; n < files && lines.size() < maxLines && bytes < maxBytes; ++n) {
            QFile f(QDir(dir).filePath(n == 0 ? QStringLiteral("app.log") : QStringLiteral("app.%1.log").arg(n)));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const qint64 want = qMin(f.size(), maxBytes - bytes);
            f.seek(f.size() - want);
            QStringList part = QString::fromUtf8(f.read(want)).split('\n', Qt::SkipEmptyParts);
            if (want < f.size() && !part.isEmpty())
                part.removeFirst();         // a line cut in half
            bytes += want;
            while (!part.isEmpty() && lines.size() < maxLines)
                lines.prepend(part.takeLast());
        }
    } catch (...) {
    }
    return lines;
}

QString scrub(const QString &text)
{
    QString out = text;
    try {
        QList<QPair<QString, QString>> names;
        for (const db::Row &r : db::query("SELECT player_id, display_name FROM players"))
            names.append({r["display_name"].toString().simplified(),
                          QStringLiteral("[player #%1]").arg(r["player_id"].toLongLong(), 4, 10, QChar('0'))});
        for (const db::Row &r : db::query("SELECT tournament_id, name FROM tournaments"))
            names.append({r["name"].toString().simplified(), QStringLiteral("[tournament %1]").arg(r["tournament_id"].toLongLong())});
        std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
        for (const auto &n : names) {
            if (n.first.size() < 3)
                continue;           // too short to tell from ordinary text
            const QRegularExpression whole(QStringLiteral("(?<![\\w\\[])") + QRegularExpression::escape(n.first) + QStringLiteral("(?![\\w\\]])"),
                                           QRegularExpression::CaseInsensitiveOption);
            out.replace(whole, n.second);
        }
    } catch (...) {
        return text;                // the names could not be looked up: the text is left as it is
    }
    return out;
}

QString diagnosticReport()
{
    const Status state = status();
    QStringList unwritten;
    {
        QMutexLocker lock(&g_lock);
        unwritten = g_unwritten;
    }
    QStringList out;
    out << QStringLiteral("TCG Tournament Manager diagnostic report")
        << QStringLiteral("Made %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
        << QString()
        << QStringLiteral("PLEASE READ THIS THROUGH BEFORE SHARING IT.")
        << QStringLiteral("It holds version details and a recent part of the app's log. It never includes the database.")
        << QStringLiteral("Actions are recorded by id number, not by name. Error messages are different: they are kept as")
        << QStringLiteral("they were worded, and may contain personal information. Player and tournament names that the")
        << QStringLiteral("app recognises have been replaced by ids, and user-folder paths shortened, but this is best")
        << QStringLiteral("effort: very short names, names it does not recognise and other typed-in text can remain.")
        << QString()
        << QStringLiteral("App version:  %1").arg(QString::fromLatin1(prefs::VERSION))
        << QStringLiteral("Build:        %1").arg(QString::fromLatin1(prefs::BUILD))
        << QStringLiteral("Qt version:   %1").arg(QString::fromLatin1(qVersion()))
        << QStringLiteral("System:       %1 (%2), kernel %3").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                                                                 QSysInfo::kernelVersion())
        << QStringLiteral("Data folder:  %1").arg(redact(QDir::toNativeSeparators(db::dataDir())));
    try {
        QStringList versions;
        for (const db::Row &r : db::query("SELECT version FROM schema_migrations ORDER BY version"))
            versions << r["version"].toString();
        out << QStringLiteral("Database:     updates applied: %1").arg(versions.isEmpty() ? QStringLiteral("none") : versions.join(", "))
            << QStringLiteral("              %1 tournaments, %2 players, %3 matches, %4 Commander pods")
                   .arg(db::value("SELECT COUNT(*) FROM tournaments").toInt()).arg(db::value("SELECT COUNT(*) FROM players").toInt())
                   .arg(db::value("SELECT COUNT(*) FROM matches").toInt()).arg(db::value("SELECT COUNT(*) FROM commander_pods").toInt());
    } catch (...) {
        out << QStringLiteral("Database:     could not be read");
    }
    if (state.available) {
        out << QStringLiteral("Logging:      working");
    } else {
        out << QStringLiteral("Logging:      UNAVAILABLE since %1: %2.").arg(state.since, state.reason)
            << QStringLiteral("              %1 entries could not be written to the log file since then, so recent")
                   .arg(state.lost)
            << QStringLiteral("              entries may be missing from this report.");
    }
    const QStringList lines = recentLines();
    out << QString() << QStringLiteral("Recent log entries from the log file (%1, oldest first):").arg(lines.size()) << QString();
    out << (lines.isEmpty() ? QStringLiteral("(none)") : scrub(redact(lines.join('\n'))));
    if (!unwritten.isEmpty()) {
        out << QString()
            << QStringLiteral("Entries that could NOT be written to the log file (the newest %1 of %2, kept in memory only):")
                   .arg(unwritten.size()).arg(state.lost)
            << QString() << scrub(redact(unwritten.join('\n')));
    }
    return out.join('\n') + '\n';
}

} // namespace applog
