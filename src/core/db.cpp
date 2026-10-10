#include "db.h"

#include "applog.h"

#include <QCoreApplication>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>

namespace db {

Error::Error(const QString &m, bool c, int errorCode)
    : std::runtime_error(m.toStdString()), constraint(c), message(m), code(errorCode)
{
}

static QMutex g_pathLock;
static QString g_path;

static const QString kFileName = QStringLiteral("tcg_tournament.db");

// A consistent copy of a database file made by SQLite itself, so it is complete even when
// the source has unsaved pages in a write-ahead log or is open elsewhere.  Never overwrites.
static bool snapshot(const QString &source, const QString &dest)
{
    if (QFileInfo::exists(dest))
        return false;
    const QString name = QStringLiteral("tcg_snapshot_%1").arg(quintptr(QThread::currentThreadId()));
    bool ok = false;
    {
        QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        d.setDatabaseName(source);
        d.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (d.open()) {
            QSqlQuery q(d);
            ok = q.exec(QStringLiteral("VACUUM INTO '%1'").arg(QString(dest).replace('\'', QLatin1String("''"))));
            q.clear();
            d.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    if (!ok)
        QFile::remove(dest);
    return ok && QFileInfo(dest).size() > 0;
}

// Where builds before the per-user folder kept the database: beside the program, or in a
// parent folder of it.  Empty when there is none.
static QString earlierDatabase(const QString &programDir)
{
    if (programDir.isEmpty())
        return QString();
    QDir up(programDir);
    for (int i = 0; i < 5; ++i) {
        if (QFileInfo::exists(up.filePath(kFileName)))
            return up.filePath(kFileName);
        if (!up.cdUp())
            break;
    }
    return QString();
}

QString resolveDataFile(const QString &userDir, const QString &programDir)
{
    const QString target = QDir(userDir).filePath(kFileName);
    if (QFileInfo::exists(target))
        return target;
    QDir().mkpath(userDir);
    const QString earlier = earlierDatabase(programDir);
    if (earlier.isEmpty())
        return target;                      // first launch: initialize() creates it here

    // Copied, not moved: the earlier file stays where it was.  The copy is made under another
    // name and renamed, so an interrupted copy is never mistaken for the database.
    const QString partial = target + QStringLiteral(".importing");
    QFile::remove(partial);
    if (!snapshot(earlier, partial) && !QFile::copy(earlier, partial)) {
        applog::error("db.import", {{"status", "failed"}}, "An earlier database could not be copied to the user folder; it is used where it is.");
        return earlier;                     // keep working on the data where it is; retried next launch
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        applog::error("db.import", {{"status", "failed"}}, "The copied database could not be put in place.");
        return QFileInfo::exists(target) ? target : earlier;
    }
    applog::info("db.import", {{"status", "ok"}}, "An earlier database was copied to the user folder from " + QDir::toNativeSeparators(earlier));
    const QString settings = QStringLiteral("settings.json");
    QFile::copy(QFileInfo(earlier).dir().filePath(settings), QDir(userDir).filePath(settings));
    return target;
}

static QString defaultPath()
{
    const QByteArray env = qgetenv("TCG_DATA_DIR");
    if (!env.isEmpty()) {
        const QString dir = QString::fromLocal8Bit(env);
        QDir().mkpath(dir);
        return QDir(dir).filePath(kFileName);
    }
    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                            + QStringLiteral("/TcgTournamentManager");
    return resolveDataFile(userDir, QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString());
}

QString path()
{
    QMutexLocker lock(&g_pathLock);
    if (g_path.isEmpty())
        g_path = defaultPath();
    return g_path;
}

void setPath(const QString &file)
{
    QMutexLocker lock(&g_pathLock);
    g_path = file;
}

QString dataDir()
{
    return QFileInfo(path()).absolutePath();
}

// One connection per thread, reopened if the path changes.

static thread_local QString t_openPath;
static thread_local int t_txDepth = 0;      // open db::Tx objects on this thread
static thread_local bool t_txFailed = false;    // a joined step failed: the open transaction can only roll back

static QString connectionName()
{
    return QStringLiteral("tcg_%1").arg(quintptr(QThread::currentThreadId()));
}

void closeThreadConnection()
{
    const QString name = connectionName();
    if (QSqlDatabase::contains(name)) {
        {
            QSqlDatabase d = QSqlDatabase::database(name, false);
            if (d.isOpen())
                d.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    t_openPath.clear();
    t_txDepth = 0;
}

static QSqlDatabase connection()
{
    const QString file = path();
    const QString name = connectionName();
    if (t_openPath == file && QSqlDatabase::contains(name))
        return QSqlDatabase::database(name, false);
    closeThreadConnection();
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    d.setDatabaseName(file);
    d.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!d.open())
        throw Error(QStringLiteral("Cannot open database %1: %2").arg(file, d.lastError().text()), false);
    QSqlQuery q(d);
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    t_openPath = file;
    return d;
}

static void run(QSqlQuery &q, const QString &sql, const QVariantList &args)
{
    q.setForwardOnly(true);
    bool ok;
    if (args.isEmpty()) {
        ok = q.exec(sql);
    } else {
        ok = q.prepare(sql);
        if (ok) {
            for (const QVariant &a : args)
                q.addBindValue(a.typeId() == QMetaType::Bool ? QVariant(a.toBool() ? 1 : 0) : a);
            ok = q.exec();
        }
    }
    if (!ok) {
        const QSqlError e = q.lastError();
        bool isNumber = false;
        const int code = e.nativeErrorCode().toInt(&isNumber);
        const bool constraint = isNumber && (code & 0xff) == 19;   // SQLITE_CONSTRAINT and its extended codes
        QString text = e.databaseText().isEmpty() ? e.text() : e.databaseText();
        throw Error(text, constraint, isNumber ? code : 0);
    }
}

Rows query(const QString &sql, const QVariantList &args)
{
    QSqlQuery q(connection());
    run(q, sql, args);
    Rows out;
    const QSqlRecord rec = q.record();
    QStringList names;
    for (int i = 0; i < rec.count(); ++i)
        names << rec.fieldName(i);
    while (q.next()) {
        Row row;
        for (int i = 0; i < names.size(); ++i)
            row.insert(names[i], q.value(i));
        out.append(row);
    }
    return out;
}

Row one(const QString &sql, const QVariantList &args)
{
    const Rows rows = query(sql, args);
    return rows.isEmpty() ? Row() : rows.first();
}

QVariant value(const QString &sql, const QVariantList &args)
{
    QSqlQuery q(connection());
    run(q, sql, args);
    return q.next() ? q.value(0) : QVariant();
}

Result exec(const QString &sql, const QVariantList &args)
{
    QSqlQuery q(connection());
    run(q, sql, args);
    Result r;
    r.lastId = q.lastInsertId().toLongLong();
    r.affected = q.numRowsAffected();
    return r;
}

bool inTransaction()
{
    return t_txDepth > 0;
}

Tx::Tx() : outermost_(t_txDepth == 0), exceptions_(std::uncaught_exceptions())
{
    if (outermost_) {
        exec(QStringLiteral("BEGIN IMMEDIATE"));
        t_txFailed = false;         // a new transaction starts clean, whatever happened before
    }
    ++t_txDepth;
}

void Tx::commit()
{
    if (outermost_) {
        if (t_txFailed) {
            // A step that had joined this transaction failed and its error was caught on the
            // way here.  Part of the change is missing, so none of it is saved.
            t_txFailed = false;
            done_ = true;
            try {
                exec(QStringLiteral("ROLLBACK"));
            } catch (const Error &) {
            }
            throw Error(QStringLiteral("Part of this change failed, so none of it was saved."), false);
        }
        exec(QStringLiteral("COMMIT"));
    }
    done_ = true;
}

Tx::~Tx()
{
    --t_txDepth;
    // A joined step that is left by an exception without committing has failed.  The owner
    // must not commit what is left, even if the exception is caught before it gets there.
    if (!outermost_ && !done_ && std::uncaught_exceptions() > exceptions_)
        t_txFailed = true;
    if (outermost_)
        t_txFailed = false;
    if (done_ || !outermost_)
        return;
    try {
        exec(QStringLiteral("ROLLBACK"));
    } catch (const Error &) {
        // nothing was open to roll back
    }
}

double roundTo(double x, int digits)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, x);
    return std::strtod(buf, nullptr);
}

static QSet<QString> columns(const QString &table)
{
    QSet<QString> out;
    for (const Row &r : query(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        out.insert(r["name"].toString());
    return out;
}

// Migrations are numbered, additive, applied once each and recorded in schema_migrations.

// Commander multiplayer events: check-ins, pods, seats, byes, audit.
static void migration001Commander()
{
    if (!columns("enrollments").contains("checked_in"))
        exec("ALTER TABLE enrollments ADD COLUMN checked_in INTEGER NOT NULL DEFAULT 1");

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_events (
            tournament_id        INTEGER PRIMARY KEY REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            rules_version        TEXT    NOT NULL,
            settings_json        TEXT    NOT NULL,
            recommended_rounds   INTEGER,
            swiss_rounds         INTEGER NOT NULL CHECK (swiss_rounds >= 1),
            recommended_cut      INTEGER,
            playoff_cut          INTEGER NOT NULL DEFAULT 0 CHECK (playoff_cut IN (0, 4, 10, 16)),
            base_seed            INTEGER NOT NULL,
            stage                TEXT    NOT NULL DEFAULT 'REGISTRATION'
                                 CHECK (stage IN ('REGISTRATION', 'SWISS', 'PLAYOFF', 'COMPLETE')),
            started_at           TEXT,
            started_player_count INTEGER,
            playoff_seeds_json   TEXT,
            champion_player_id   INTEGER REFERENCES players(player_id)
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_rounds (
            round_id            INTEGER PRIMARY KEY AUTOINCREMENT,
            tournament_id       INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            stage               TEXT    NOT NULL CHECK (stage IN ('SWISS', 'SEMIFINAL', 'FINAL')),
            round_number        INTEGER NOT NULL,
            stage_round         INTEGER NOT NULL,
            status              TEXT    NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE', 'FINALIZED')),
            pairing_algorithm   TEXT,
            pairing_seed        INTEGER,
            pairing_inputs_json TEXT,
            pairing_cost_json   TEXT,
            created_at          TEXT    DEFAULT (DATETIME('now')),
            finalized_at        TEXT,
            UNIQUE (tournament_id, round_number),
            UNIQUE (tournament_id, stage, stage_round)
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_pods (
            pod_id              INTEGER PRIMARY KEY AUTOINCREMENT,
            round_id            INTEGER NOT NULL REFERENCES commander_rounds(round_id) ON DELETE CASCADE,
            tournament_id       INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            pod_number          INTEGER NOT NULL,
            status              TEXT    NOT NULL DEFAULT 'PENDING' CHECK (status IN ('PENDING', 'REPORTED')),
            outcome             TEXT    CHECK (outcome IN ('WIN', 'DRAW')),
            advancing_player_id INTEGER REFERENCES players(player_id),
            result_version      INTEGER NOT NULL DEFAULT 0,
            reported_at         TEXT,
            confirmed_at        TEXT,
            UNIQUE (round_id, pod_number),
            CHECK ((status = 'PENDING' AND outcome IS NULL) OR (status = 'REPORTED' AND outcome IS NOT NULL))
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_seats (
            seat_id       INTEGER PRIMARY KEY AUTOINCREMENT,
            pod_id        INTEGER NOT NULL REFERENCES commander_pods(pod_id) ON DELETE CASCADE,
            round_id      INTEGER NOT NULL REFERENCES commander_rounds(round_id) ON DELETE CASCADE,
            tournament_id INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            player_id     INTEGER NOT NULL REFERENCES players(player_id),
            seat_number   INTEGER NOT NULL CHECK (seat_number BETWEEN 1 AND 4),
            result        TEXT    CHECK (result IN ('WIN', 'LOSS', 'DRAW')),
            eliminated    INTEGER NOT NULL DEFAULT 0,
            playoff_seed  INTEGER,
            UNIQUE (pod_id, seat_number),
            UNIQUE (round_id, player_id)
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_byes (
            bye_id        INTEGER PRIMARY KEY AUTOINCREMENT,
            round_id      INTEGER NOT NULL REFERENCES commander_rounds(round_id) ON DELETE CASCADE,
            tournament_id INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            player_id     INTEGER NOT NULL REFERENCES players(player_id),
            UNIQUE (round_id, player_id)
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_advancements (
            advancement_id INTEGER PRIMARY KEY AUTOINCREMENT,
            tournament_id  INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            player_id      INTEGER NOT NULL REFERENCES players(player_id),
            to_stage       TEXT    NOT NULL CHECK (to_stage IN ('SEMIFINAL', 'FINAL')),
            reason         TEXT    NOT NULL CHECK (reason IN ('SEED', 'POD_WIN')),
            seed           INTEGER NOT NULL,
            from_pod_id    INTEGER REFERENCES commander_pods(pod_id) ON DELETE CASCADE,
            created_at     TEXT    DEFAULT (DATETIME('now')),
            UNIQUE (tournament_id, to_stage, player_id),
            UNIQUE (from_pod_id)
        )
    )sql");
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS commander_audit (
            audit_id      INTEGER PRIMARY KEY AUTOINCREMENT,
            tournament_id INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            round_id      INTEGER,
            pod_id        INTEGER,
            action        TEXT    NOT NULL,
            actor         TEXT,
            detail_json   TEXT,
            created_at    TEXT    DEFAULT (DATETIME('now'))
        )
    )sql");
    // a player is in exactly one place per round: a seat or a bye, never both
    exec(R"sql(
        CREATE TRIGGER IF NOT EXISTS commander_bye_not_seated
        BEFORE INSERT ON commander_byes
        WHEN EXISTS (SELECT 1 FROM commander_seats WHERE round_id = NEW.round_id AND player_id = NEW.player_id)
        BEGIN SELECT RAISE(ABORT, 'player already seated in this round'); END
    )sql");
    exec(R"sql(
        CREATE TRIGGER IF NOT EXISTS commander_seat_not_bye
        BEFORE INSERT ON commander_seats
        WHEN EXISTS (SELECT 1 FROM commander_byes WHERE round_id = NEW.round_id AND player_id = NEW.player_id)
        BEGIN SELECT RAISE(ABORT, 'player already has a bye this round'); END
    )sql");
    exec("CREATE INDEX IF NOT EXISTS idx_cmd_rounds_tournament ON commander_rounds(tournament_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_cmd_pods_round        ON commander_pods(round_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_cmd_seats_tournament  ON commander_seats(tournament_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_cmd_seats_pod         ON commander_seats(pod_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_cmd_audit_tournament  ON commander_audit(tournament_id)");
}

// Saved round countdown for Commander rounds (Modern rounds already have these columns).
static void migration002CommanderRoundTimer()
{
    const QSet<QString> cols = columns("commander_rounds");
    if (!cols.contains("timer_state"))
        exec("ALTER TABLE commander_rounds ADD COLUMN timer_state TEXT NOT NULL DEFAULT 'STOPPED'");
    if (!cols.contains("timer_elapsed_secs"))
        exec("ALTER TABLE commander_rounds ADD COLUMN timer_elapsed_secs INTEGER NOT NULL DEFAULT 0");
    if (!cols.contains("timer_started_at"))
        exec("ALTER TABLE commander_rounds ADD COLUMN timer_started_at REAL");
}

// Commander events are the configured number of rounds and nothing else: no
// playoff, semifinal or final stage.  Nothing already played is deleted.
//   - events that already reached a playoff (seeded, or with playoff rounds)
//     are flagged legacy_playoff and keep those rounds as a read-only record;
//   - events that had a playoff configured but had not reached it lose only
//     that setting;
//   - triggers stop any round beyond the configured total from being saved.
static void migration003CommanderFixedRounds()
{
    const QSet<QString> cols = columns("commander_events");
    if (!cols.contains("legacy_playoff"))
        exec("ALTER TABLE commander_events ADD COLUMN legacy_playoff INTEGER NOT NULL DEFAULT 0");
    if (!cols.contains("final_standings_json"))
        exec("ALTER TABLE commander_events ADD COLUMN final_standings_json TEXT");
    if (!cols.contains("completed_at"))
        exec("ALTER TABLE commander_events ADD COLUMN completed_at TEXT");

    const QString reached = QStringLiteral(
        "stage = 'PLAYOFF' OR playoff_seeds_json IS NOT NULL OR EXISTS ("
        "SELECT 1 FROM commander_rounds r WHERE r.tournament_id = commander_events.tournament_id AND r.stage <> 'SWISS')");
    exec(QStringLiteral(
             "INSERT INTO commander_audit (tournament_id, action, actor, detail_json) "
             "SELECT tournament_id, 'LEGACY_PLAYOFF_FLAGGED', 'migration-3', "
             "'{\"playoff_cut\": ' || playoff_cut || ', \"stage\": \"' || stage || '\"}' "
             "FROM commander_events WHERE legacy_playoff = 0 AND (%1)").arg(reached));
    exec(QStringLiteral("UPDATE commander_events SET legacy_playoff = 1 WHERE %1").arg(reached));
    exec("INSERT INTO commander_audit (tournament_id, action, actor, detail_json) "
         "SELECT tournament_id, 'PLAYOFF_SETTING_REMOVED', 'migration-3', '{\"previous_cut\": ' || playoff_cut || '}' "
         "FROM commander_events WHERE legacy_playoff = 0 AND playoff_cut <> 0");
    exec("UPDATE tournaments SET top_cut = 0 WHERE tournament_id IN ("
         "SELECT tournament_id FROM commander_events WHERE legacy_playoff = 0 AND playoff_cut <> 0)");
    exec("UPDATE commander_events SET playoff_cut = 0, recommended_cut = 0 WHERE legacy_playoff = 0");

    // the configured round count is the whole event: nothing can be saved past it
    exec(R"sql(
        CREATE TRIGGER IF NOT EXISTS commander_round_limit
        BEFORE INSERT ON commander_rounds
        WHEN NEW.stage <> 'SWISS'
          OR NEW.round_number > (SELECT swiss_rounds FROM commander_events WHERE tournament_id = NEW.tournament_id)
          OR NEW.stage_round  > (SELECT swiss_rounds FROM commander_events WHERE tournament_id = NEW.tournament_id)
          OR (SELECT stage FROM commander_events WHERE tournament_id = NEW.tournament_id) <> 'SWISS'
        BEGIN SELECT RAISE(ABORT, 'no round can be created beyond the configured number of rounds'); END
    )sql");
    exec(R"sql(
        CREATE TRIGGER IF NOT EXISTS commander_round_count_frozen
        BEFORE UPDATE OF swiss_rounds ON commander_events
        WHEN OLD.stage <> 'REGISTRATION' AND NEW.swiss_rounds <> OLD.swiss_rounds
        BEGIN SELECT RAISE(ABORT, 'the number of rounds is fixed once the event has started'); END
    )sql");
    exec(R"sql(
        CREATE TRIGGER IF NOT EXISTS commander_no_new_playoff
        BEFORE INSERT ON commander_events
        WHEN NEW.playoff_cut <> 0
        BEGIN SELECT RAISE(ABORT, 'Commander events no longer have a playoff stage'); END
    )sql");
}

// A tournament can be ended early: status 'TERMINATED', with the moment it happened.
// SQLite cannot change a CHECK rule in place, so the tournaments table is rebuilt the way
// its documentation prescribes: a new table with the wider rule, every row copied across
// unchanged, the old table dropped and the new one given its name.  Foreign keys are off
// while this runs (see runMigrations), so no other table's rows are touched.
static void migration004TournamentTerminated()
{
    const QString sql = value("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'tournaments'").toString();
    if (!sql.contains("'TERMINATED'")) {
        static const QRegularExpression head(QStringLiteral("^CREATE TABLE\\s+\"?tournaments\"?"));
        QString wider = sql;
        wider.replace(head, QStringLiteral("CREATE TABLE tournaments_rebuild"));
        wider.replace(QStringLiteral("'CANCELLED'"), QStringLiteral("'CANCELLED', 'TERMINATED'"));
        if (sql.count("'CANCELLED'") != 1 || !wider.startsWith("CREATE TABLE tournaments_rebuild"))
            throw Error(QStringLiteral("The tournaments table is not in the expected form, so it was left unchanged."), false);

        // indexes (and any triggers) on the table go with it when it is dropped: put them back afterwards
        const Rows attached = query("SELECT sql FROM sqlite_master WHERE tbl_name = 'tournaments' "
                                    "AND type IN ('index', 'trigger') AND sql IS NOT NULL");
        const QVariant sequence = value("SELECT seq FROM sqlite_sequence WHERE name = 'tournaments'");
        const int before = value("SELECT COUNT(*) FROM tournaments").toInt();

        exec("DROP TABLE IF EXISTS tournaments_rebuild");
        exec(wider);
        exec("INSERT INTO tournaments_rebuild SELECT * FROM tournaments");
        if (value("SELECT COUNT(*) FROM tournaments_rebuild").toInt() != before)
            throw Error(QStringLiteral("Not every tournament could be copied, so the table was left unchanged."), false);
        exec("DROP TABLE tournaments");
        exec("ALTER TABLE tournaments_rebuild RENAME TO tournaments");
        for (const Row &r : attached)
            exec(r["sql"].toString());
        // ids are never reused, even when the newest tournament had been deleted
        if (sequence.isValid())
            exec("UPDATE sqlite_sequence SET seq = MAX(seq, ?) WHERE name = 'tournaments'", {sequence});
    }
    if (!columns("tournaments").contains("terminated_at"))
        exec("ALTER TABLE tournaments ADD COLUMN terminated_at TEXT");
}

// A player can be removed from the directory without being erased: deleted_at records when.
// Their id, name and every tournament record stay, so history is unchanged.
static void migration005PlayerRemoved()
{
    if (!columns("players").contains("deleted_at"))
        exec("ALTER TABLE players ADD COLUMN deleted_at TEXT");
}

const QList<Migration> &migrations()
{
    static const QList<Migration> list = {
        {1, "commander_multiplayer", migration001Commander},
        {2, "commander_round_timer", migration002CommanderRoundTimer},
        {3, "commander_fixed_rounds", migration003CommanderFixedRounds},
        {4, "tournament_terminated", migration004TournamentTerminated, true},
        {5, "player_removed", migration005PlayerRemoved},
    };
    return list;
}

// Applies one migration and records it, all or nothing.
static void applyMigration(const Migration &m)
{
    applog::Action log("db.update", {{"version", m.version}, {"name", QString::fromLatin1(m.name)}});
    Tx tx;
    // another connection may have applied it while we waited for the lock
    if (value("SELECT 1 FROM schema_migrations WHERE version = ?", {m.version}).isValid())
        return;
    m.apply();
    if (m.rebuildsTable) {
        // every record that refers to a tournament must still find it
        for (const Row &r : query("PRAGMA foreign_key_check"))
            if (r["parent"].toString() == "tournaments")
                throw Error(QStringLiteral("Update %1 would leave records without their tournament, so it was not applied.")
                                .arg(m.version), false);
    }
    exec("INSERT INTO schema_migrations (version, name) VALUES (?, ?)", {m.version, QString::fromLatin1(m.name)});
    tx.commit();
}

QString backupBeforeUpdate()
{
    const QString dir = dataDir() + QStringLiteral("/backups");
    QDir().mkpath(dir);
    const QString stem = dir + QStringLiteral("/tcg_tournament.before-update-")
                         + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString dest = stem + QStringLiteral(".db");
    for (int n = 2; QFileInfo::exists(dest); ++n)
        dest = QStringLiteral("%1-%2.db").arg(stem).arg(n);
    if (!snapshot(path(), dest)) {
        applog::error("db.backup", {{"status", "failed"}, {"file", QFileInfo(dest).fileName()}});
        throw Error(QStringLiteral("The database needs updating for this version, but a backup could not be "
                                   "saved to %1. Nothing was changed.").arg(QDir::toNativeSeparators(dest)), false);
    }
    applog::info("db.backup", {{"status", "ok"}, {"file", QFileInfo(dest).fileName()}});
    return dest;
}

// `hadData` is false for a database this launch has just created: there is nothing to back up.
static void runMigrations(bool hadData)
{
    exec(R"sql(
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version    INTEGER PRIMARY KEY,
            name       TEXT NOT NULL,
            applied_at TEXT DEFAULT (DATETIME('now'))
        )
    )sql");
    QSet<int> applied;
    for (const Row &r : query("SELECT version FROM schema_migrations"))
        applied.insert(r["version"].toInt());
    bool backedUp = !hadData;
    for (const Migration &m : migrations()) {
        if (applied.contains(m.version))
            continue;
        if (!backedUp) {
            backupBeforeUpdate();
            backedUp = true;
        }
        if (!m.rebuildsTable) {
            applyMigration(m);
            continue;
        }
        // Dropping a table deletes the rows that refer to it unless foreign keys are off, and
        // that setting can only change outside a transaction.  If it did not take, stop.
        exec("PRAGMA foreign_keys = OFF");
        try {
            if (t_txDepth > 0 || value("PRAGMA foreign_keys").toInt() != 0)
                throw Error(QStringLiteral("Update %1 cannot run inside another change.").arg(m.version), false);
            applyMigration(m);
        } catch (...) {
            exec("PRAGMA foreign_keys = ON");
            throw;
        }
        exec("PRAGMA foreign_keys = ON");
    }
}

void initialize()
{
    const bool hadData = value("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'players'").isValid();

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS players (
            player_id       INTEGER PRIMARY KEY AUTOINCREMENT,
            display_name    TEXT    NOT NULL,
            email           TEXT    UNIQUE,
            phone           TEXT,
            date_joined     TEXT    DEFAULT (DATE('now')),
            plays_mtg       INTEGER DEFAULT 0,
            plays_onepiece  INTEGER DEFAULT 0,
            plays_pokemon   INTEGER DEFAULT 0,
            notes           TEXT
        )
    )sql");

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS tournaments (
            tournament_id   INTEGER PRIMARY KEY AUTOINCREMENT,
            name            TEXT    NOT NULL,
            game            TEXT    NOT NULL CHECK (game IN ('MTG', 'ONEPIECE', 'POKEMON')),
            format          TEXT,
            tournament_date TEXT    NOT NULL,
            location        TEXT,
            total_rounds    INTEGER NOT NULL,
            top_cut         INTEGER DEFAULT 0,
            round_time_mins INTEGER DEFAULT 50,
            status          TEXT    DEFAULT 'PENDING'
                            CHECK (status IN ('PENDING', 'IN_PROGRESS', 'COMPLETED', 'CANCELLED', 'TERMINATED')),
            notes           TEXT
        )
    )sql");

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS enrollments (
            enrollment_id   INTEGER PRIMARY KEY AUTOINCREMENT,
            tournament_id   INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            player_id       INTEGER NOT NULL REFERENCES players(player_id),
            deck_name       TEXT,
            decklist        TEXT,
            seed            INTEGER,
            match_points    INTEGER DEFAULT 0,
            match_wins      INTEGER DEFAULT 0,
            match_losses    INTEGER DEFAULT 0,
            match_draws     INTEGER DEFAULT 0,
            game_wins       INTEGER DEFAULT 0,
            game_losses     INTEGER DEFAULT 0,
            omw_pct         REAL    DEFAULT 0.0,
            gw_pct          REAL    DEFAULT 0.0,
            ogw_pct         REAL    DEFAULT 0.0,
            final_placement INTEGER,
            dropped         INTEGER DEFAULT 0,
            drop_round      INTEGER,
            UNIQUE (tournament_id, player_id)
        )
    )sql");

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS rounds (
            round_id        INTEGER PRIMARY KEY AUTOINCREMENT,
            tournament_id   INTEGER NOT NULL REFERENCES tournaments(tournament_id) ON DELETE CASCADE,
            round_number    INTEGER NOT NULL,
            round_type      TEXT    DEFAULT 'SWISS'
                            CHECK (round_type IN ('SWISS', 'TOP8', 'TOP4', 'FINALS')),
            timer_started_at TEXT,
            timer_paused_at  TEXT,
            timer_elapsed_secs INTEGER DEFAULT 0,
            timer_state      TEXT DEFAULT 'STOPPED'
                            CHECK (timer_state IN ('STOPPED', 'RUNNING', 'PAUSED', 'ENDED')),
            started_at      TEXT,
            ended_at        TEXT,
            UNIQUE (tournament_id, round_number)
        )
    )sql");

    exec(R"sql(
        CREATE TABLE IF NOT EXISTS matches (
            match_id            INTEGER PRIMARY KEY AUTOINCREMENT,
            round_id            INTEGER NOT NULL REFERENCES rounds(round_id) ON DELETE CASCADE,
            tournament_id       INTEGER NOT NULL REFERENCES tournaments(tournament_id),
            table_number        INTEGER,
            player1_id          INTEGER NOT NULL REFERENCES players(player_id),
            player2_id          INTEGER REFERENCES players(player_id),
            winner              TEXT    DEFAULT 'PENDING'
                                CHECK (winner IN ('PLAYER1', 'PLAYER2', 'DRAW', 'BYE', 'PENDING')),
            player1_game_wins   INTEGER DEFAULT 0,
            player2_game_wins   INTEGER DEFAULT 0,
            games_drawn         INTEGER DEFAULT 0,
            result_reported_at  TEXT,
            reported_by         TEXT,
            notes               TEXT
        )
    )sql");

    exec("CREATE INDEX IF NOT EXISTS idx_matches_player1    ON matches(player1_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_matches_player2    ON matches(player2_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_matches_tournament ON matches(tournament_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_enrollments_player ON enrollments(player_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_enrollments_tourney ON enrollments(tournament_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_rounds_tournament  ON rounds(tournament_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_tournaments_game   ON tournaments(game)");
    exec("CREATE INDEX IF NOT EXISTS idx_tournaments_date   ON tournaments(tournament_date)");

    runMigrations(hadData);
}

} // namespace db
