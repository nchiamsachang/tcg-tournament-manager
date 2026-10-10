// The diagnostic log: a plain text file of what the app did and what went wrong.
//
// It supplements the Commander audit history in the database and never replaces it.  One
// line per entry:
//
//     2026-10-09T14:03:22.123-05:00 INFO  0.2.1-preview.3 tournament.start status=attempt tournament=12 op=4F2A91C7
//     2026-10-09T14:03:22.140-05:00 INFO  0.2.1-preview.3 tournament.start status=ok tournament=12 op=4F2A91C7
//     2026-10-09T14:03:40.511-05:00 WARN  0.2.1-preview.3 dialog.error ref=R-7K3QF2 title="Player not removed" op=9D01B6E3 | ...
//
// Field names are not reused within a line.  "status" is how the operation went (attempt,
// then ok, pending, failed, or a named outcome such as no-change).  What was being saved has
// its own field: match_result (PLAYER1, PLAYER2, DRAW, CLEARED) or pod_result for Commander.
//
// What is and is not in it:
//   - Actions name players, tournaments and rounds by id, never by name.
//   - The text of an error is written with the player and tournament names it can recognise
//     replaced by ids.  That is best effort: a name it does not recognise, a very short one,
//     or anything else that was typed in can remain.  A diagnostic report applies the same
//     replacement again, and is shown before it is saved so it can be read first.
//
// Logging never gets in the app's way: it is thread-safe, never throws, and if the file
// cannot be written it says so through status() and carries on quietly (no dialog, no error
// per entry), trying again now and then.
//
// Every entry is flushed to the operating system as it is written.  That makes it likely,
// not certain, that entries survive when the app stops unexpectedly; a crash or power loss
// can still lose the last ones.  Nothing is written by a crash itself and there are no
// stack traces.  A session that did not close normally is noted at the next start, whatever
// the reason was.
#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace applog {

enum class Level { Info, Warning, Error };
using Fields = QList<QPair<QString, QVariant>>;     // in the order they are written

// Where the files go (app.log, then app.1.log … as they rotate).  Entries written before
// this is called are held in memory, a bounded number of them, and written first.  An empty
// name stops writing: entries are held again, and those already held are dropped.
void setDirectory(const QString &dir);
QString directory();
// Rotation: a file is closed at `maxBytes` and at most `files` are kept (default 2 MB, 5).
void setLimits(qint64 maxBytes, int files);

// Whether the log file can be written.  While it cannot, entries are counted (and the newest
// kept in memory for a report) and the file is tried again every `setRetryInterval`
// milliseconds (default 30 seconds), not on every entry.
struct Status {
    bool available = true;
    QString reason;             // why not, in a few words
    QString since;              // when it stopped, local time
    int lost = 0;               // entries that could not be written since then
};
Status status();
void setRetryInterval(int milliseconds);

void write(Level level, const QString &action, const Fields &fields = {}, const QString &detail = {});
inline void info(const QString &action, const Fields &fields = {}, const QString &detail = {})
{
    write(Level::Info, action, fields, detail);
}
inline void warning(const QString &action, const Fields &fields = {}, const QString &detail = {})
{
    write(Level::Warning, action, fields, detail);
}
inline void error(const QString &action, const Fields &fields = {}, const QString &detail = {})
{
    write(Level::Error, action, fields, detail);
}

// A short id such as "R-7K3QF2", shown in an error dialog and written with its log entry so
// the two can be matched.
QString newReference();

// One operation, logged as attempted when it starts and as ok or failed when it ends.  Both
// entries carry the same operation id (op=…), unique to this attempt, so they can be matched
// when other entries lie between them or when the same tournament or player is acted on
// from two places at once.  Everything else logged on the thread while it runs carries that
// id too, and an operation started inside another one shares its id.
//
// Declare it as the first thing in the function, before any db::Tx, so it ends after the
// transaction has committed or rolled back: "ok" is only ever written for a saved change.
// An action that runs as a step inside a caller's transaction ends as "pending" instead:
// its work is not saved until that caller commits, and the caller's own entry says how the
// whole operation ended.
// It ends as failed when the function is left by an exception; the failure entry also gets
// an error reference (ref=…), which the dialog that reports the error then shows.
class Action {
public:
    Action(const QString &name, const Fields &fields = {});
    ~Action();
    void set(const QString &key, const QVariant &value);    // an id only known later (a new row)
    void outcome(const QString &result);                    // instead of "ok", e.g. "no-change"
    QString operation() const { return op_; }
    Action(const Action &) = delete;
    Action &operator=(const Action &) = delete;

private:
    QString name_, outcome_, op_;
    Fields fields_;
    int exceptions_;
};

// The operation this thread is in the middle of (empty when none).  To continue it on
// another thread or in a later callback, pass the id along and hold an OperationScope there.
QString currentOperation();
class OperationScope {
public:
    explicit OperationScope(const QString &operation);
    ~OperationScope();
    OperationScope(const OperationScope &) = delete;
    OperationScope &operator=(const OperationScope &) = delete;

private:
    bool pushed_ = false;
};

// For the code that tells the organizer about an error: the reference (and operation id) of
// the action that has just failed on this thread, so the dialog and its log entry use the
// same ones.  Returns a new reference when no action has just failed.
QString errorReference(QString *operation = nullptr);

// Sends Qt's own warnings and errors to the log as well (not debug or info messages).
void captureQtMessages();
// Notes the start of a session, and whether the previous one did not close normally.
void sessionStarted();
void sessionEnded();

// Replaces the user's folder in a path or message with "~", so a user name is not written.
QString redact(const QString &text);
// Best effort: replaces the player and tournament names it recognises with their ids
// ("[player #0042]", "[tournament 12]").  Names of one or two characters, names that are not
// in the database and other typed-in text are left as they are.  Returns the text unchanged
// when the names cannot be looked up.
QString scrub(const QString &text);
// The newest entries, oldest first: at most `maxLines` lines and `maxBytes` bytes.
QStringList recentLines(int maxLines = 400, qint64 maxBytes = 96 * 1024);
// The text of a diagnostic report: app version and build, Qt and system versions, the
// database's update level and some counts, whether logging was working, and a recent log
// excerpt passed through redact() and scrub().  It never includes the database.
QString diagnosticReport();

} // namespace applog
