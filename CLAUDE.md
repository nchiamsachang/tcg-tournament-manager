# TCG Tournament Manager: project instructions

C++17 / Qt 6 Widgets desktop app with a local SQLite database. See README.md for the
architecture, build scripts and data location.

## Code comments

This section is the one place the comment rules for this project are written down. The
general rules in the user-level `~/.claude/CLAUDE.md` (short, lower case, plain text) still
apply; this adds what is specific here. If the two ever disagree, ask rather than guess.

- No decorative banners: no rows of dashes, equals signs or asterisks, no Unicode box or
  divider characters, no ASCII art. Separate sections with a blank line and a plain comment.
- No comment that only repeats what the code already says.
- Keep a concise explanation where the code alone does not show why: a tournament rule, a
  design decision, a compatibility requirement for existing databases, a workaround.
- Keep license notices and anything a tool needs (for example `#include "x.moc"`, pragma and
  lint markers) exactly as they are.
- Existing comments written in sentence case are left alone unless the code around them is
  being changed for another reason. Do not restyle comments as a change of its own.

## Working rules

- Never run the app or tests against the real database in
  `%LOCALAPPDATA%\TcgTournamentManager`. Use a temporary folder or `TCG_DATA_DIR`
  (`qa_sandbox.bat` does this).
- Schema changes go in a new numbered migration in `src/core/db.cpp`; never edit one that
  has shipped.
- Every rule is enforced in `src/core`, not only by a disabled button or a field limit in
  `src/ui`.
- Run `run_tests.bat` (core, rules and UI suites) before calling a change done.
  `build\bench_core.exe` prints timings on generated data and is run by hand.
