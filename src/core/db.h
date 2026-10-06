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
    Error(const QString &message, bool constraint);
    bool constraint;
    QString message;
};

struct Result {
    qint64 lastId = 0;
    int affected = 0;
};

// The .db file.  By default it sits beside the program; a database found in a
// parent folder (the project root during development) is used instead.
QString path();
void setPath(const QString &file);
QString dataDir();

Rows query(const QString &sql, const QVariantList &args = {});
Row one(const QString &sql, const QVariantList &args = {});          // empty map when there is no row
QVariant value(const QString &sql, const QVariantList &args = {});   // first column of the first row
Result exec(const QString &sql, const QVariantList &args = {});

// Creates all tables if they don't exist and applies pending migrations.
// Safe to call every time the app starts.
void initialize();

// Closes this thread's connection (call before a worker thread ends).
void closeThreadConnection();

// BEGIN IMMEDIATE ... COMMIT.  Rolls back if commit() was never reached.
//
// A Tx opened while another is already open on this thread joins it: only the outermost one
// begins and commits.  That lets a function that needs "all or nothing" call other functions
// that also do, without either knowing about the other.  An error anywhere throws, which
// unwinds to the outermost Tx and rolls everything back.
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
};

// round(x, digits), correctly rounded in decimal.  Stored percentages use it so equal values
// always compare equal.
double roundTo(double x, int digits);

struct Migration {
    int version;
    const char *name;
    void (*apply)();
};
const QList<Migration> &migrations();

} // namespace db
