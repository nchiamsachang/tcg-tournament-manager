---
name: Bug report
about: Something the app did wrong during setup, a round, printing or afterwards
title: "[Bug] <screen or feature>: <what went wrong, in one line>"
labels: bug
assignees: ''
---

<!-- One problem per issue. Describe what you saw, not what you think the cause is. -->

## Version and environment

- App version (home screen or Settings, e.g. `0.2.1-preview`):
- Commit, if built from source (`git rev-parse --short HEAD`):
- Windows version:
- Display scaling and window size, for layout problems (e.g. 125 %, 1280 × 720):
- Theme and text size (Light / Dark, Standard / Large):
- Data used: real event / disposable test data (`qa_sandbox.bat`)

## Where it happened

- Game and format (One Piece / Pokémon / MTG Modern / MTG Commander):
- Players and rounds (e.g. 7 players, round 2 of 3):
- Screen (registration, round, standings, players, print preview, …):

## Steps to reproduce

1.
2.
3.

Reproduces: always / sometimes (n of m tries) / once

## Expected result

## Actual result

## Severity and impact

<!-- Pick one and say who was affected and whether there was a workaround. -->

- [ ] **S1 Critical** — wrong pairings, results or standings; data lost or changed; the app cannot be used
- [ ] **S2 Major** — a feature fails, but there is a workaround and saved data is correct
- [ ] **S3 Minor** — works, but confusing, slow or awkward
- [ ] **S4 Cosmetic** — layout, wording or appearance only

Impact:

Workaround:

## Evidence

<!-- Screenshot, photo of a printout, exported CSV/PDF, or the text of the error. Remove real
     player names you do not have permission to share. If none is available, say so. -->

## For the maintainer

- Status: New / Reproduced / Cannot reproduce / Fixed / Won't fix
- Regression test (test name, or the manual case id in `docs/qa/REGRESSION_CASES.md`):
- Fix (commit or pull request):
- Retest result (date, version, pass/fail, who):
