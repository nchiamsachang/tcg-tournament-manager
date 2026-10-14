# Logs and diagnostic reports

What the app records about itself and how to share it when reporting a problem. For
the short version see the [README](../README.md#reporting-a-problem).

The app writes a diagnostic log in `logs\app.log` inside the data folder
(`%LOCALAPPDATA%\TcgTournamentManager\logs`). A file is closed at 2 MB and
five are kept. Each line has the local time with its UTC offset, a severity,
the app version, an action and the ids involved:

```
2026-10-09T14:03:22.123-05:00 INFO  0.2.3 result.report status=attempt match=41 match_result=PLAYER1 op=4F2A91C7
2026-10-09T14:03:22.140-05:00 INFO  0.2.3 result.report status=ok match=41 match_result=PLAYER1 tournament=7 op=4F2A91C7
```

`status` is how the operation went: `attempt` when it starts, then `ok`,
`failed`, `pending` (a step inside a larger operation that has not been saved
yet) or a named outcome such as `no-change`. What was being saved has its own
field, `match_result` (`PLAYER1`, `PLAYER2`, `DRAW`, `CLEARED`) or `pod_result`
for Commander, so no line has two fields with the same name. Up to
0.2.1-preview.2 both were called `result`; older lines in an existing log keep
that form. Nothing in the app reads the log back by field name.

**What is recorded.** The start of a session (with the build identifier),
database updates and backups, every error shown in a dialog, pages that fail
to open, errors caught by the safety net in `main.cpp`, and the key actions
of both one-on-one and Commander events. Timer ticks and repaints are never
logged. The Commander audit history in the database is unchanged; the log is
in addition to it.

**Attempts and outcomes.** An action is written as `attempt` when it starts
and as `ok` or `failed` when it ends; `ok` is written only after the change
has been committed. Both entries carry the same operation id (`op=`), unique
to that attempt, so they can be matched when other entries lie between them
or when the same tournament is acted on from two places at once. A failed
action also gets an error reference (`ref=R-7K3QF2`); the dialog that reports
the error shows that reference, and its own log entry carries both ids.

**The exact build.** The start entry, Settings and every report name the
build as well as the version: the Git commit, with `-modified-<date>-<time>`
when files had been changed since that commit, or `no-git` when the app was
built without Git. The version itself (`project(VERSION)` in `CMakeLists.txt`)
names the package.

**Privacy.** Actions name players, tournaments and rounds by id, never by
name. Error messages are different: they are worded for the organizer and may
contain personal information. Before such a message is written, and again
when a report is made, the player and tournament names the app recognises
are replaced by ids and user-folder paths are shortened. That is best effort.
Names of one or two characters, names the app does not recognise, and
anything else that was typed in can remain. Read a report before sharing it.

**Reports.** Settings > Diagnostics has "Open log folder" and "Export
diagnostic report". The report is shown before it is saved, exactly as it
will be saved: version, build and system details, whether logging was
working, and a recent log excerpt. It never contains the database, and
nothing is uploaded.

**When the log cannot be written.** The app carries on normally. There is no
dialog and no error per entry; Settings > Diagnostics shows "Logging
unavailable" with the reason, a report says that recent entries may be
missing (and includes the newest unwritten ones, which are kept in memory),
and the file is tried again every 30 seconds. When it works again, a
`log.resumed` entry says how many entries were missed.

**What a log cannot tell you.** Each entry is handed to the operating system
as soon as it is written, which makes it likely, not certain, that it
survives if the app stops unexpectedly; a crash or a power failure can still
lose the most recent entries. Nothing is written by a crash itself and there
are no stack traces. If a session did not close normally, the next start
notes that, without knowing why: the app may have been ended from outside,
the computer may have shut down, or the app may have stopped unexpectedly.

The code is `applog` in `src/core`. A function that changes data declares an
`applog::Action` as its first statement.
