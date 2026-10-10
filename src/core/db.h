// Database access: one SQLite file, one connection per thread.
// Rows come back as QVariantMap keyed by column name, the way the rest of
// the app reads them.
#pragma once

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <stdexcept>

namespace db {

using Row = QVariantMap;
using Rows = QList<QVariantMap>;

// Any failed statement.  `constraint` is true when SQLite refused the write
// because of a UNIQUE / CHECK / foreign key / trigger rule.
struct Error : std::runtime_error {
    Error(const QString &message, bool constraint, int code = 0);
    bool constraint;
    QString message;
    int code;           // SQLite's extended result code, 0 when the error did not come from SQLite
    // The write would have duplicated a row that must be unique (UNIQUE or PRIMARY KEY).
    bool unique() const { return code == 2067 || code == 1555; }
    // The write referred to a row that does not exist (FOREIGN KEY).
    bool foreignKey() const { return code == 787; }
    // Another connection held the database (BUSY or LOCKED) for longer than the wait allowed.
    bool busy() const { return (code & 0xff) == 5 || (code & 0xff) == 6; }
};

struct Result {
    qint64 lastId = 0;
    int affected = 0;
};

// The .db file.  By default it is in the user's own application-data folder
// (%LOCALAPPDATA%\TcgTournamentManager on Windows), so replacing the program never touches
// it.  Setting the TCG_DATA_DIR environment variable chooses another folder.
QString path();
void setPath(const QString &file);
QString dataDir();

// The default rule behind path().  When `userDir` has no database yet and an earlier build
// left one beside the program in `programDir` (or in a parent folder of it), that database
// and its settings.json are copied into `userDir`; the originals are left as they were and
// nothing already in `userDir` is overwritten.  If the copy cannot be made, the earlier file
// keeps being used where it is.
QString resolveDataFile(const QString &userDir, const QString &programDir);

// Saves a copy of the database under backups/ in the data folder and returns its path.
// initialize() calls it before applying a schema update to a database that already has data.
// Throws Error when the copy cannot be made.
QString backupBeforeUpdate();

Rows query(const QString &sql, const QVariantList &args = {});
Row one(const QString &sql, const QVariantList &args = {});          // empty map when there is no row
QVariant value(const QString &sql, const QVariantList &args = {});   // first column of the first row
Result exec(const QString &sql, const QVariantList &args = {});

// Creates all tables if they don't exist and applies pending migrations, backing up an
// existing database first.  Safe to call every time the app starts.
void initialize();

// Closes this thread's connection (call before a worker thread ends).
void closeThreadConnection();

// True while a db::Tx is open on this thread: what is written now is not saved until the
// outermost one commits.
bool inTransaction();

// BEGIN IMMEDIATE ... COMMIT.  Rolls back if commit() was never reached.
//
// A Tx opened while another is already open on this thread joins it: only the outermost one
// begins and commits.  That lets a function that needs "all or nothing" call other functions
// that also do, without either knowing about the other.  An error anywhere throws, which
// unwinds to the outermost Tx and rolls everything back.
//
// If a joined step fails and its exception is caught before it reaches the owner, the
// transaction is still spoiled: the owner's commit() rolls everything back and throws,
// so half a change is never saved.  (There are no savepoints: a step cannot be undone on
// its own.)  A step that simply returns without committing, having changed nothing, does
// not spoil anything, and the next transaction always starts clean.
class Tx {
public:
    Tx();
    ~Tx();
    void commit();
    Tx(const Tx &) = delete;
    Tx &operator=(const Tx &) = delete;

private:
    bool outermost_ = false;
    bool done_ = false;
    int exceptions_ = 0;
};

// round(x, digits), correctly rounded in decimal.  Stored percentages use it so equal values
// always compare equal.
double roundTo(double x, int digits);

struct Migration {
    int version;
    const char *name;
    void (*apply)();
    bool rebuildsTable = false;     // drops and recreates a table: runs with foreign keys off, then checks them
};
const QList<Migration> &migrations();

} // namespace db
