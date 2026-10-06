// Tests that drive the real screens: workflows, settings, printing, the round
// clock, and a layout sweep over window sizes, themes and text sizes.
#include "commander_db.h"
#include "dialogs.h"
#include "main_window.h"
#include "printing.h"
#include "screens.h"
#include "store.h"
#include "theme.h"

#include <QCryptographicHash>
#include <QListView>
#include <QScrollBar>
#include <QWheelEvent>
#include <QScreen>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {

const QString LONG_NAME = "Alexandra Montgomery-Wellington of the Northern Isles";
const QString UNBROKEN = "Supercalifragilisticexpialidocious_Player_With_No_Spaces_At_All";

void settle()
{
    for (int i = 0; i < 6; ++i)
        QCoreApplication::processEvents();
}

QHash<QString, QPushButton *> buttons(QWidget *w)
{
    settle();
    QHash<QString, QPushButton *> out;
    for (QPushButton *b : w->findChildren<QPushButton *>())
        if (b->isVisible())
            out.insert(b->text(), b);
    return out;
}

QString labelText(QWidget *w)
{
    settle();
    QStringList out;
    for (QLabel *l : w->findChildren<QLabel *>())
        if (l->isVisible())
            out << l->text().remove(QChar(0x200B));
    return out.join(" | ");
}

QByteArray databaseFingerprint()
{
    QFile f(db::path());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

} // namespace

class UiTests : public QObject {
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> tmp_;
    std::unique_ptr<MainWindow> w_;

    QList<qint64> players(const QStringList &names)
    {
        QList<qint64> out;
        for (const QString &n : names)
            out << pdb::addPlayer(n);
        return out;
    }

    qint64 modern(const QStringList &names = {"Ana", "Bo", "Cy", "Di", "Ed"}, int minutes = 45, int rounds = 2,
                  bool start = true, const QString &name = "Modern Monday")
    {
        const qint64 tid = tdb::createTournament(name, "MTG", rounds, "Modern", {}, minutes);
        for (qint64 p : players(names))
            tdb::enrollPlayer(tid, p);
        if (start)
            swiss::startTournament(tid);
        return tid;
    }

    qint64 commander(const QStringList &names, int rounds = 1, bool start = true, int minutes = 80,
                     const QString &name = "Friday Commander")
    {
        const qint64 tid = cdb::createCommanderTournament(name, int(names.size()), rounds, {}, 5, {}, minutes);
        for (qint64 p : players(names))
            tdb::enrollPlayer(tid, p);
        if (start)
            cdb::startEvent(tid);
        return tid;
    }

    template <typename S> S *go(const QString &name, const QVariantMap &args = {})
    {
        w_->navigateTo(name, args);
        settle();
        return qobject_cast<S *>(w_->currentScreen());
    }

    // onScreen: a real, active window.  Keyboard focus only exists in one, and the page jump
    // this guards against was driven by focus moving, so that test needs it.
    void openWindow(bool onScreen = false)
    {
        w_ = std::make_unique<MainWindow>();
        if (!onScreen)
            w_->setAttribute(Qt::WA_DontShowOnScreen);
        w_->show();
        if (onScreen) {
            w_->activateWindow();
            w_->raise();
            QVERIFY(QTest::qWaitForWindowActive(w_.get(), 3000));
        }
        settle();
    }

    static bool inSidewaysScroll(QWidget *w)
    {
        for (QWidget *p = w->parentWidget(); p; p = p->parentWidget()) {
            // sideways-scrolling regions by design: standings tables and the live-tournament tab strip
            if (qobject_cast<T::HScroll *>(p))
                return true;
            if (auto *sa = qobject_cast<QScrollArea *>(p); sa && sa->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff)
                return true;
        }
        return false;
    }

    // Nothing visible may be clipped, pushed off the window or smaller than the minimum target size.
    QStringList layoutProblems()
    {
        QStringList problems;
        QWidget *win = w_.get();
        for (QWidget *wd : win->findChildren<QWidget *>()) {
            auto *button = qobject_cast<QPushButton *>(wd);
            auto *label = qobject_cast<QLabel *>(wd);
            auto *edit = qobject_cast<QLineEdit *>(wd);
            const bool input = edit || qobject_cast<QComboBox *>(wd) || qobject_cast<QSpinBox *>(wd)
                               || qobject_cast<QCheckBox *>(wd) || qobject_cast<QRadioButton *>(wd);
            if (!button && !label && !input)
                continue;
            if (!wd->isVisible() || wd->visibleRegion().isEmpty())
                continue;
            const QString text = (button ? button->text() : label ? label->text() : QString(wd->metaObject()->className())).left(40);
            const bool sideways = inSidewaysScroll(wd);
            const int right = wd->mapTo(win, QPoint(wd->width(), 0)).x();
            if (right > win->width() + 1 && !sideways)
                problems << QStringLiteral("off the right edge by %1px: '%2'").arg(right - win->width()).arg(text);
            QWidget *p = wd->parentWidget();
            while (p && !qobject_cast<QScrollArea *>(p))
                p = p->parentWidget();
            if (auto *sa = qobject_cast<QScrollArea *>(p); sa && !sideways && sa->viewport()->isAncestorOf(wd)) {
                const int over = wd->mapTo(sa->viewport(), QPoint(wd->width(), 0)).x() - sa->viewport()->width();
                if (over > 1)
                    problems << QStringLiteral("cut off by its scroll area by %1px: '%2'").arg(over).arg(text);
            }
            if (button && !qobject_cast<T::WrapButton *>(wd) && button->sizeHint().width() > button->width() + 1)
                problems << QStringLiteral("button label clipped (%1<%2): '%3'").arg(button->width()).arg(button->sizeHint().width()).arg(text);
            if (label && !label->text().isEmpty() && label->pixmap().isNull()) {
                if (label->wordWrap()) {
                    if (label->heightForWidth(label->width()) > label->height() + 2)
                        problems << QStringLiteral("wrapped label cut off vertically: '%1'").arg(text);
                } else if (label->sizeHint().width() > label->width() + 1 && !sideways) {
                    problems << QStringLiteral("label clipped (%1<%2): '%3'").arg(label->width()).arg(label->sizeHint().width()).arg(text);
                }
            }
            const bool innerEdit = edit && (qobject_cast<QSpinBox *>(wd->parentWidget()) || qobject_cast<QComboBox *>(wd->parentWidget()));
            if ((button || input) && !innerEdit && wd->height() < T::MIN_HIT - 1)
                problems << QStringLiteral("target too small (%1): '%2'").arg(wd->height()).arg(text);
        }
        return problems;
    }

private slots:
    void init()
    {
        tmp_ = std::make_unique<QTemporaryDir>();
        db::closeThreadConnection();
        db::setPath(tmp_->filePath("test.db"));
        db::initialize();
        setAutoConfirm(true);
        prefs::setPref("theme", "dark");
        openWindow();
    }

    void cleanup()
    {
        w_.reset();
        settle();
        db::closeThreadConnection();
        tmp_.reset();
    }

    void topBarHasThemeToggleAndSettingsButNoPrinter()
    {
        go<HomeScreen>("home");
        QCOMPARE(w_->gearBtn->accessibleName(), QString("Settings"));
        QVERIFY(w_->themeBtn->accessibleName().contains("mode"));
        // a circular gear in the top-right corner, with a tooltip, a name for screen readers and keyboard focus
        QCOMPARE(w_->gearBtn->size(), QSize(44, 44));
        QCOMPARE(w_->gearBtn->text(), QStringLiteral("⚙"));
        QCOMPARE(w_->gearBtn->toolTip(), QString("Settings"));
        QVERIFY(w_->gearBtn->styleSheet().contains("border-radius:22px"));
        QVERIFY(w_->gearBtn->styleSheet().contains("QPushButton:focus"));
        QVERIFY(w_->gearBtn->focusPolicy() & Qt::TabFocus);
        int rightmost = 0;
        for (QPushButton *b : buttons(w_.get()))
            rightmost = qMax(rightmost, b->mapTo(w_.get(), QPoint(b->width(), 0)).x());
        QCOMPARE(w_->gearBtn->mapTo(w_.get(), QPoint(w_->gearBtn->width(), 0)).x(), rightmost);
        QCOMPARE(w_->themeBtn->size(), QSize(44, 44));
        const QStringList texts = buttons(w_.get()).keys();
        for (const QString &t : texts)
            QVERIFY2(!t.contains("Print"), "printing lives inside tournaments, not on the home page");
        QCOMPARE(w_->navButtons.size(), 2);                    // Home and Players; Settings is the gear
        // the open page's pill has a 3px outline all the way round, and says so to screen readers
        QVERIFY(w_->navButtons["home"]->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
        QVERIFY(!w_->navButtons["home"]->styleSheet().contains("border-left"));
        QVERIFY(w_->navButtons["home"]->accessibleName().contains("current page"));
        QVERIFY(!w_->navButtons["players"]->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
        QVERIFY(w_->backBtn && w_->backBtn->isVisible());
    }

    void themeToggleAndSettingsStayInSync()
    {
        QCOMPARE(w_->effectiveMode(), QString("dark"));
        w_->toggleTheme();
        QCOMPARE(prefs::load()["theme"].toString(), QString("light"));
        QCOMPARE(T::MODE, QString("light"));
        QVERIFY(w_->themeBtn->accessibleName().contains("dark"));
        {
            SettingsDialog dialog(w_.get());            // the panel shows what the toggle chose
            QCOMPARE(dialog.theme->checkedButton()->property("value").toString(), QString("light"));
            // pill selections that show the current choice with a tick as well as the outline
            QCOMPARE(dialog.theme->buttons().size(), 2);
            QCOMPARE(dialog.text->buttons().size(), 2);
            QVERIFY(dialog.theme->checkedButton()->text().startsWith(QStringLiteral("✓")));
            QVERIFY(dialog.theme->checkedButton()->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
            dialog.set("theme", "dark");
            QCOMPARE(T::MODE, QString("dark"));
            dialog.text->buttons()[1]->click();                 // "Large" applies at once
            QCOMPARE(prefs::load()["text_size"].toString(), QString("large"));
            dialog.set("text_size", "large");
            QCOMPARE(T::SCALE, 1.4);
            QCOMPARE(T::font(14).pixelSize(), 20);              // body text: 16px Standard, 20px Large
            dialog.set("alert_sound", true);
            dialog.set("alert_notify", false);
        }
        w_.reset();
        openWindow();                                   // a restart keeps every choice
        QCOMPARE(T::MODE, QString("dark"));
        QCOMPARE(T::SCALE, 1.4);
        QJsonObject p = prefs::load();
        QVERIFY(p["alert_sound"].toBool() && !p["alert_notify"].toBool());
        {
            SettingsDialog dialog(w_.get());
            QCOMPARE(dialog.text->checkedButton()->property("value").toString(), QString("large"));
            QVERIFY(dialog.sound->isChecked());
            dialog.restoreDefaults();
        }
        p = prefs::load();
        QCOMPARE(p["theme"].toString(), QString("system"));
        QCOMPARE(p["text_size"].toString(), QString("standard"));
        QCOMPARE(T::SCALE, 1.15);
        QCOMPARE(T::font(14).pixelSize(), 16);
        QCOMPARE(T::font(12).pixelSize(), 14);                  // secondary text
        QCOMPARE(T::MODE, MainWindow::systemMode());    // "Follow system"
    }

    void roundAlertsFollowThePreferences()
    {
        w_->roundAlert("Time expired — test");
        QVERIFY(w_->toast->isVisible());
        QVERIFY(w_->toastLabel->text().contains("Time expired"));
        w_->toast->hide();
        prefs::setPref("alert_notify", false);
        w_->roundAlert("Time expired — test");
        QVERIFY(!w_->toast->isVisible());
    }

    void footerTextAndIssueLink()
    {
        HomeScreen *home = go<HomeScreen>("home");
        const QString text = labelText(home);
        QVERIFY(text.contains("Organize trading card tournaments, manage players, and keep every round running smoothly. "
                              "Track pairings, standings, and results for formats including MTG Modern and Commander."));
        QPushButton *link = buttons(home).value(QStringLiteral("Report an issue ↗"));
        QVERIFY(link);
        QCOMPARE(link->toolTip(), QString("https://github.com/nchiamsachang/tcg_tournament_manager/issues"));
        for (const char *banned : {"Design notes", "Copy prompt", "Claude", "Demo data", "Founded by"})
            QVERIFY2(!text.contains(banned), banned);
    }

    void everyControlIsKeyboardReachable()
    {
        const qint64 tid = modern();
        const QList<QPair<QString, QVariantMap>> screens{{"home", {}}, {"players", {}},
                                                         {"round", {{"tournament_id", tid}}}};
        for (const auto &s : screens) {
            w_->navigateTo(s.first, s.second);
            settle();
            for (QWidget *wd : w_->currentScreen()->findChildren<QWidget *>()) {
                const bool control = qobject_cast<QPushButton *>(wd) || qobject_cast<QComboBox *>(wd)
                                     || qobject_cast<QSpinBox *>(wd) || qobject_cast<T::ClickFrame *>(wd)
                                     || (qobject_cast<QLineEdit *>(wd) && !qobject_cast<QSpinBox *>(wd->parentWidget()));
                if (control && wd->isVisible() && wd->isEnabled())
                    QVERIFY2(wd->focusPolicy() & Qt::TabFocus, qPrintable(s.first + ": " + wd->metaObject()->className()));
            }
        }
    }

    void tournamentRowsHaveAnAccentEdgeAStatusLabelAndALiveCountdown()
    {
        const qint64 live = modern();
        modern({"Fa", "Gi"}, 45, 2, false, "Pending Modern");
        timerdb::start(timerdb::Kind::OneOnOne, tdb::currentRound(live)["round_id"].toLongLong());
        HubScreen *hub = go<HubScreen>("mtg_hub");
        QVERIFY(hub);
        T::ClickFrame *liveRow = nullptr, *idleRow = nullptr;
        for (T::ClickFrame *r : hub->findChildren<T::ClickFrame *>()) {
            if (r->accessibleName().contains("in progress"))
                liveRow = r;
            if (r->accessibleName().contains("pending"))
                idleRow = r;
        }
        QVERIFY(liveRow && idleRow);
        // the outline runs round the whole panel: green while a round is in progress
        QVERIFY(liveRow->styleSheet().contains("border:3px solid " + T::LIVE));
        QVERIFY(idleRow->styleSheet().contains("border:3px solid " + T::BORDER2));
        QVERIFY(!liveRow->styleSheet().contains("border-left"));
        const QString liveText = labelText(liveRow);
        QVERIFY2(liveText.contains("Round in progress"), "status is written out, not only shown by colour");
        QVERIFY(labelText(idleRow).contains("Not started"));
        QVERIFY2(liveText.contains("44:5") || liveText.contains("45:00"), qPrintable(liveText));   // from the saved clock
        QVERIFY(liveText.contains("remaining"));
    }

    // The clock in the top tournament area.

    void topAreaShowsEachTournamentsRoundAndSavedClock()
    {
        const qint64 a = modern({"Ana", "Bo", "Cy", "Di"}, 45, 3, true, "Modern Monday");
        const qint64 b = commander({"Ed", "Flo", "Gus", "Hal"}, 2, true, 80, "Commander Night");
        const qint64 ra = tdb::currentRound(a)["round_id"].toLongLong();
        const qint64 rb = cdb::getRounds(b).last().toMap()["round_id"].toLongLong();
        w_.reset();
        openWindow();                                           // a restart finds both tournaments
        QVERIFY(w_->tabButton(a) && w_->tabButton(b));
        QVERIFY(w_->tabText(a).contains("Modern Monday") && w_->tabText(a).contains("Round 1 of 3"));
        QVERIFY(w_->tabText(a).endsWith("Not started"));
        QVERIFY(w_->tabText(b).contains("Round 1 of 2") && w_->tabText(b).endsWith("Not started"));

        timerdb::start(timerdb::Kind::OneOnOne, ra);
        db::exec("UPDATE rounds SET timer_started_at = timer_started_at - 1242 WHERE round_id = ?", {ra});   // 20:42 gone
        go<HomeScreen>("home");                                 // navigating re-reads the saved clocks
        QVERIFY2(w_->tabText(a).contains("24:18 remaining") || w_->tabText(a).contains("24:17 remaining"), qPrintable(w_->tabText(a)));
        QCOMPARE(w_->tabButton(a)->text(), w_->tabText(a));     // what is on screen is that text
        QVERIFY2(w_->tabText(b).endsWith("Not started"), "each tournament has its own clock");

        // navigating, resizing and restarting do not reset it
        go<PlayersScreen>("players");
        w_->resize(700, 500);
        settle();
        w_.reset();
        openWindow();
        QVERIFY2(w_->tabText(a).contains("24:1"), qPrintable(w_->tabText(a)));

        timerdb::pause(timerdb::Kind::OneOnOne, ra);
        timerdb::start(timerdb::Kind::Commander, rb);
        go<HomeScreen>("home");
        QVERIFY2(w_->tabText(a).contains("Paused at 24:1"), qPrintable(w_->tabText(a)));
        QVERIFY2(w_->tabText(b).contains("80:00 remaining") || w_->tabText(b).contains("79:5"), qPrintable(w_->tabText(b)));

        // at zero it says so, stays at zero, and changes nothing else
        db::exec("UPDATE commander_rounds SET timer_started_at = timer_started_at - 99999 WHERE round_id = ?", {rb});
        go<HomeScreen>("home");
        QVERIFY(w_->tabText(b).endsWith("Time expired"));
        QVERIFY(!w_->tabText(b).contains("-"));
        QCOMPARE(cdb::getRounds(b).size(), 1);
        QCOMPARE(cdb::getRounds(b).last().toMap()["status"].toString(), QString("ACTIVE"));

        // the open tournament's pill is outlined all the way round, and the bar wraps when narrow
        auto *round = go<RoundScreen>("round", {{"tournament_id", a}});
        QVERIFY(round);
        QVERIFY(w_->tabButton(a)->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
        QVERIFY(!w_->tabButton(b)->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
        w_->resize(480, 640);
        settle();
        QVERIFY2(w_->tabButton(b)->y() > w_->tabButton(a)->y(), "the second pill wraps onto its own line");
        for (qint64 t : {a, b})
            QVERIFY(w_->tabButton(t)->width() >= w_->tabButton(t)->sizeHint().width());
    }

    void modernEventRunsThroughTheScreens()
    {
        auto *setup = go<TournamentSetupScreen>("tournament_setup", {{"game", "MTG"}});
        QVERIFY(setup);
        setup->nameInput->setText("UI Modern");
        setup->playerCount->setValue(4);
        setup->roundsSpin->setValue(2);
        // round length: only the number is in the field, "min" is a label beside it
        QVERIFY(!setup->minutesEdit->text().contains("min"));
        QCOMPARE(setup->minutesUnit->text(), QString("min"));
        QVERIFY(setup->minutesUnit->mapTo(setup, QPoint(0, 0)).x()
                >= setup->minutesEdit->mapTo(setup, QPoint(setup->minutesEdit->width(), 0)).x());
        for (const QString &bad : {QString(""), QString("abc"), QString("0"), QString("999"), QString("12.5")}) {
            setup->minutesEdit->setText(bad);                   // typing anything is allowed; saving is not
            setup->createTournament();
            QVERIFY2(setup->lastError.contains("1 to 240"), qPrintable(bad));
            QVERIFY(qobject_cast<TournamentSetupScreen *>(w_->currentScreen()));
        }
        QCOMPARE(db::value("SELECT COUNT(*) FROM tournaments").toInt(), 0);
        setup->minutesEdit->setText("35");
        setup->createTournament();
        QVERIFY(setup->lastError.isEmpty());
        settle();
        auto *reg = qobject_cast<RegistrationScreen *>(w_->currentScreen());
        QVERIFY(reg);
        const qint64 tid = reg->tournamentId;
        QVERIFY2(tdb::tournamentById(tid)["round_time_mins"].toInt() == 35, "round length belongs to the tournament");
        for (const QString &name : {LONG_NAME, UNBROKEN, QString("Ana"), QString("Bo")}) {
            reg->newNameBox->setText(name);
            reg->addAndEnroll();
        }
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 4);
        QVERIFY(buttons(reg).contains(QStringLiteral("Start tournament  →")));
        reg->confirmStart();
        settle();

        auto *round = qobject_cast<RoundScreen *>(w_->currentScreen());
        QVERIFY(round);
        QVERIFY2(buttons(round).contains(QStringLiteral("🖨  Print")), "Print is offered inside the round");
        QCOMPARE(timerdb::get(timerdb::Kind::OneOnOne, round->currentRoundId).limit, 35 * 60);
        QVERIFY(!round->nextRoundBtn->isEnabled());
        for (const db::Row &m : tdb::roundPairings(round->currentRoundId))
            round->recordResult(m["match_id"].toLongLong(), "PLAYER1");
        settle();
        QVERIFY(round->nextRoundBtn->isEnabled());
        QVERIFY(!round->finalizeBtn->isVisible());
        round->endRound();
        settle();
        QCOMPARE(round->currentRoundNum, 2);
        QVERIFY(round->finalizeBtn->isVisible() && !round->nextRoundBtn->isVisible());
        round->endRound();                                      // there is no round three
        QCOMPARE(tdb::currentRound(tid)["round_number"].toInt(), 2);
        for (const db::Row &m : tdb::roundPairings(round->currentRoundId))
            round->recordResult(m["match_id"].toLongLong(), "PLAYER2");
        round->finalizeTournament();
        settle();

        auto *standings = qobject_cast<StandingsScreen *>(w_->currentScreen());
        QVERIFY(standings);
        QVERIFY(labelText(standings).contains("Final Standings"));
        QCOMPARE(standings->standings.size(), 4);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));
        QCOMPARE(w_->liveTabCount(), 0);

        QVERIFY(go<RoundSelectScreen>("round_select", {{"tournament_id", tid}}));
        auto *review = go<RoundScreen>("round", {{"tournament_id", tid}, {"round_number", 1}});
        QCOMPARE(review->currentRoundNum, 1);
        for (T::WrapButton *b : review->findChildren<T::WrapButton *>())
            QVERIFY2(!b->isEnabled(), "a finished round is read-only");

        // the PDF report is built from the saved rounds
        int pages = 0;
        const QByteArray pdf = printing::pdfBytes(printing::tournamentReport(tid), "Letter", &pages);
        QVERIFY(pdf.startsWith("%PDF"));
        QCOMPARE(pages, 3);                                     // two rounds and the standings
    }

    void commanderThreeRoundEventRunsThroughTheScreens()
    {
        const qint64 tid = commander({LONG_NAME, UNBROKEN, "Ana", "Bo", "Cy", "Di", "Ed", "Flo"}, 3, false);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        QVERIFY(reg);
        QVERIFY(reg->enrolledCountLabel->text().contains("8 checked in"));
        QVERIFY2(!reg->findChild<QComboBox *>("cut"), "there is no playoff setting");
        QVERIFY(!labelText(reg).contains("layoff"));
        reg->roundsSpin->setValue(3);
        reg->confirmStart();
        settle();
        auto *ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QVERIFY(ev);
        QVERIFY(buttons(ev).contains(QStringLiteral("🖨  Print")));
        QCOMPARE(timerdb::get(timerdb::Kind::Commander, ev->state["rounds"].toList().last().toMap()["round_id"].toLongLong()).limit, 80 * 60);

        for (int number = 1; number <= 3; ++number) {
            const QVariantMap rnd = ev->state["rounds"].toList().last().toMap();
            QCOMPARE(rnd["round_number"].toInt(), number);
            QCOMPARE(rnd["stage"].toString(), QString("SWISS"));
            const QString shown = buttons(ev).keys().join(" | ") + " | " + labelText(ev);
            for (const char *word : {"review", "Playoff", "Bracket", "Semifinal", "Final pod", "Top 4"})
                QVERIFY2(!shown.contains(word), word);
            const bool last = buttons(ev).contains("Finish tournament");
            QVERIFY2(last == (number == 3), "Finish tournament is offered on the last round only");
            const QString closing = last ? QString("Finish tournament") : QStringLiteral("Finalize Round %1  →").arg(number);
            QVERIFY2(!buttons(ev).value(closing)->isEnabled(), "not until every pod has a result");
            for (const QVariant &pv : rnd["pods"].toList()) {
                const QVariantMap pod = pv.toMap();
                ev->report(pod, "WIN", pod["seats"].toList()[0].toMap()["player_id"].toLongLong(), {});
            }
            if (number < 3) {
                QVERIFY(buttons(ev).value(closing)->isEnabled());
                ev->finalizeRound(ev->state["rounds"].toList().last().toMap());
                QVERIFY2(ev->state["rounds"].toList().size() == number, "finalizing does not create a round by itself");
                buttons(ev).value(QStringLiteral("Start Round %1  →").arg(number + 1))->click();
                settle();
                QCOMPARE(ev->state["rounds"].toList().size(), number + 1);
            }
        }
        settle();
        int chips = 0;
        for (QPushButton *b : ev->findChildren<QPushButton *>())
            if (b->isVisible() && b->accessibleName().startsWith("Round "))
                ++chips;
        QVERIFY2(chips == 3, "three round chips and nothing after them");
        QPushButton *finish = buttons(ev).value("Finish tournament");
        QVERIFY(finish && finish->isEnabled());
        finish->click();
        settle();

        auto *standings = qobject_cast<CommanderStandingsScreen *>(w_->currentScreen());
        QVERIFY2(standings, "finishing shows the results");
        const QString shown = labelText(standings);
        QVERIFY(shown.contains("Final Standings"));
        QVERIFY(!shown.contains("layoff"));
        const QVariantMap state = cdb::getState(tid);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(state["rounds"].toList().size(), 3);
        QCOMPARE(standings->rows.size(), 8);
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(standings->rows[i].toMap()["place"].toInt(), i + 1);
            QCOMPARE(standings->rows[i].toMap()["player_id"], state["final_standings"].toList()[i].toMap()["player_id"]);
        }
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));

        // reopening restores the finished state: read-only rounds, no way to add one
        auto *done = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        const QStringList names = buttons(done).keys();
        QVERIFY(names.contains(QStringLiteral("View final standings  →")));
        for (const QString &label : names)
            QVERIFY2(!label.startsWith("Start Round") && !label.startsWith("Finalize") && !label.startsWith("Finish")
                     && label != "Wins", qPrintable(label));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid, 4));
        HubScreen *hub = go<HubScreen>("mtg_hub");
        for (T::ClickFrame *r : hub->findChildren<T::ClickFrame *>())
            if (r->accessibleName().contains("Friday Commander"))
                QVERIFY(labelText(r).contains("Completed"));
    }

    void earlierVersionPlayoffShowsANoticeAndFinishesOnItsRounds()
    {
        // what the earlier version left behind: two rounds done, Top 4 seeded, an unplayed final pod
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"}, 2);
        for (int n = 1; n <= 2; ++n) {
            const QVariantMap rnd = cdb::getRounds(tid).last().toMap();
            for (const QVariant &pv : rnd["pods"].toList())
                cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN",
                                     pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong());
            cdb::finalizeRound(rnd["round_id"].toLongLong());
            if (n == 1)
                cdb::publishNextRound(tid);
        }
        const QVariantList table = cdb::getStandings(tid);
        for (const char *trigger : {"commander_round_limit", "commander_round_count_frozen", "commander_no_new_playoff"})
            db::exec(QStringLiteral("DROP TRIGGER %1").arg(trigger));
        db::exec("DELETE FROM schema_migrations WHERE version = 3");
        db::exec("UPDATE commander_events SET stage = 'PLAYOFF', playoff_cut = 4, playoff_seeds_json = '[]', "
                 "champion_player_id = NULL, final_standings_json = NULL, completed_at = NULL, legacy_playoff = 0 "
                 "WHERE tournament_id = ?", {tid});
        db::exec("UPDATE tournaments SET status = 'IN_PROGRESS', top_cut = 4 WHERE tournament_id = ?", {tid});
        db::exec("UPDATE enrollments SET final_placement = NULL WHERE tournament_id = ?", {tid});
        const qint64 rid = db::exec("INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round) "
                                    "VALUES (?, 'FINAL', 3, 1)", {tid}).lastId;
        const qint64 pod = db::exec("INSERT INTO commander_pods (round_id, tournament_id, pod_number) VALUES (?, ?, 1)", {rid, tid}).lastId;
        for (int seat = 1; seat <= 4; ++seat)
            db::exec("INSERT INTO commander_seats (pod_id, round_id, tournament_id, player_id, seat_number, playoff_seed) "
                     "VALUES (?, ?, ?, ?, ?, ?)", {pod, rid, tid, table[seat - 1].toMap()["player_id"], seat, seat});
        db::initialize();                                       // the upgrade flags it

        HubScreen *hub = go<HubScreen>("mtg_hub");
        QVERIFY(labelText(hub).contains("Needs finishing"));
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(labelText(ev).contains("Earlier-version playoff"));
        QVERIFY(ev->title->text().startsWith("Round 2"));       // the last scheduled round, not the old final
        QHash<QString, QPushButton *> shown = buttons(ev);
        QVERIFY(shown.contains(QStringLiteral("Final pod · earlier version")));
        QVERIFY(!shown.contains("Wins") && !shown.contains(QStringLiteral("Start Round 3  →")));
        shown.value(QStringLiteral("Final pod · earlier version"))->click();
        shown = buttons(ev);
        QVERIFY(labelText(ev).contains(QStringLiteral("Read-only record · not counted")));
        QVERIFY2(!shown.contains("Wins") && !shown.contains(QStringLiteral("Correct result…")), "the old final cannot be played");
        shown.value("Finish tournament")->click();
        settle();
        auto *standings = qobject_cast<CommanderStandingsScreen *>(w_->currentScreen());
        QVERIFY(standings);
        const QVariantMap state = cdb::getState(tid);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(state["rounds"].toList().size(), 3);           // the old final pod is still on record
        for (int i = 0; i < table.size(); ++i)                  // placings come from the two rounds
            QCOMPARE(standings->rows[i].toMap()["player_id"], table[i].toMap()["player_id"]);
    }

    void longNamesWrapAndActionsFitInANarrowWindow()
    {
        const qint64 tid = commander({LONG_NAME, UNBROKEN, "Ana"}, 1, false);
        w_->resize(320, 640);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        QVERIFY2(reg->addBtn->width() >= reg->addBtn->sizeHint().width(), "the Add & enroll label is never cut off");
        QVERIFY(reg->addBtn->height() >= T::MIN_HIT);
        QVERIFY(reg->narrow && reg->tiny);
        int wrapped = 0;
        for (QLabel *l : reg->findChildren<QLabel *>()) {
            if (l->wordWrap() && l->text().contains(QChar(0x200B))) {
                ++wrapped;
                QVERIFY(l->heightForWidth(l->width()) <= l->height() + 2);
            }
        }
        QVERIFY2(wrapped > 0, "long unbroken names get break points");
        reg->newNameBox->setText("typed but not saved");
        w_->resize(1280, 800);
        settle();
        QVERIFY2(w_->currentScreen() == reg, "resizing keeps the same screen");
        QVERIFY2(reg->newNameBox->text() == "typed but not saved", "and what was typed into it");
        QVERIFY(!reg->narrow);
    }

    void openedDropdownsAreOpaqueAndThemed()
    {
        for (const QString &mode : {QString("dark"), QString("light")}) {
            prefs::setPref("theme", mode);
            w_->restyle();
            for (const QSize &size : {QSize(1180, 780), QSize(800, 460), QSize(360, 640)}) {
                w_->resize(size);
                auto *setup = go<TournamentSetupScreen>("tournament_setup", {{"game", "MTG"}});
                QVERIFY(setup);
                QComboBox *format = setup->formatCombo;
                QCOMPARE(format->count(), 2);
                const QByteArray where = QStringLiteral("%1 %2x%3").arg(mode).arg(size.width()).arg(size.height()).toUtf8();
                format->showPopup();
                QTest::qWait(150);
                QAbstractItemView *view = format->view();
                QWidget *popup = view->window();
                QVERIFY2(popup->isVisible() && popup != w_.get(), where);
                QVERIFY2(!popup->testAttribute(Qt::WA_TranslucentBackground), where);
                // every pixel of the open list is solid: sample the unselected row away from its text,
                // the list padding and the popup frame
                const QImage shot = popup->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QRect other = view->visualRect(format->model()->index(format->currentIndex() == 0 ? 1 : 0, 0));
                const QPoint inRow = view->viewport()->mapTo(popup, QPoint(other.right() - 6, other.center().y()));
                const qreal dpr = shot.devicePixelRatio();
                const QColor rowColor = shot.pixelColor(int(inRow.x() * dpr), int(inRow.y() * dpr));
                QVERIFY2(rowColor.alpha() == 255, where);
                // (the hover colour if the real mouse pointer happens to be resting on that row)
                QVERIFY2(rowColor.name() == QColor(T::SURFACE2).name() || rowColor.name() == QColor(T::SELECTED).name(),
                         where + " row " + rowColor.name().toUtf8());
                for (int y = 0; y < shot.height(); y += 3)
                    for (int x = 0; x < shot.width(); x += 3)
                        QVERIFY2(shot.pixelColor(x, y).alpha() == 255, where + " a transparent pixel in the popup");
                // the chosen option is clearly marked, and its text is readable on the mark
                const QRect chosen = view->visualRect(format->model()->index(format->currentIndex(), 0));
                const QPoint inChosen = view->viewport()->mapTo(popup, QPoint(chosen.right() - 6, chosen.center().y()));
                const QString chosenColor = shot.pixelColor(int(inChosen.x() * dpr), int(inChosen.y() * dpr)).name();
                QVERIFY2(chosenColor == QColor(T::PURPLE).name() || chosenColor == QColor(T::PURPLE_HOVER).name(),
                         where + " chosen row " + chosenColor.toUtf8());
                // rows are comfortable to hit and the list fits on the screen
                QVERIFY2(other.height() >= 34, where);
                // (only meaningful when the test window itself fits on this display)
                if (popup->screen()->availableGeometry().contains(w_->frameGeometry()))
                    QVERIFY2(popup->screen()->availableGeometry().contains(popup->frameGeometry()), where);
                if (!qEnvironmentVariable("TCG_UI_SHOTS").isEmpty())
                    popup->grab().save(qEnvironmentVariable("TCG_UI_SHOTS") + "/dropdown_" + QString::fromUtf8(where).replace(' ', '_') + ".png");
                // choosing with the keyboard works and closes the list
                QTest::keyClick(view, Qt::Key_Down);
                QTest::keyClick(view, Qt::Key_Return);
                QTest::qWait(50);
                QVERIFY2(!popup->isVisible(), where);
                QCOMPARE(format->currentText(), QString("Commander"));
            }
        }
        // every other dropdown in the app is built the same way
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di"}, 1, false);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        for (QComboBox *box : reg->findChildren<QComboBox *>()) {
            QVERIFY(qobject_cast<QListView *>(box->view()));
            QVERIFY(box->styleSheet().contains("QAbstractItemView::item"));
        }
        QCOMPARE(reg->findChildren<QComboBox *>().size(), 2);
        cdb::startEvent(tid);
        const QVariantMap pod = cdb::getState(tid)["rounds"].toList()[0].toMap()["pods"].toList()[0].toMap();
        ResultDialog dialog(w_.get(), pod, "Pod 1 result", "ELIMINATED_LOSE");
        QVERIFY(qobject_cast<QListView *>(dialog.outcomeCombo->view()));
    }

    void playerDirectoryShowsOneTagPerGameActuallyPlayed()
    {
        const QList<qint64> ids = players({LONG_NAME, "Bo", "Cy", "Di", "Newcomer"});
        const qint64 poke = tdb::createTournament("Poke", "POKEMON", 1, "Standard");
        const qint64 op = tdb::createTournament("OP", "ONEPIECE", 1, "Standard");
        const qint64 modernA = tdb::createTournament("Modern A", "MTG", 1, "Modern");
        const qint64 modernB = tdb::createTournament("Modern B", "MTG", 1, "Modern");
        for (qint64 t : {poke, op, modernA, modernB}) {
            tdb::enrollPlayer(t, ids[0]);
            tdb::enrollPlayer(t, ids[1]);
            swiss::startTournament(t);
        }
        const qint64 pending = tdb::createTournament("Pending", "POKEMON", 1, "Standard");
        tdb::enrollPlayer(pending, ids[2]);             // registered only
        tdb::enrollPlayer(pending, ids[3]);

        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1440, 900), QSize(1024, 768), QSize(800, 600), QSize(640, 480), QSize(360, 640)}) {
                    w_->resize(size);
                    auto *dir = go<PlayersScreen>("players");
                    QVERIFY(dir);
                    const QByteArray where = QStringLiteral("%1 %2 %3x%4").arg(mode, textSize).arg(size.width()).arg(size.height()).toUtf8();
                    if (!qEnvironmentVariable("TCG_UI_SHOTS").isEmpty())
                        w_->grab().save(qEnvironmentVariable("TCG_UI_SHOTS") + "/players_" + QString::fromUtf8(where).replace(' ', '_') + ".png");
                    QHash<QString, QStringList> tagsOf;
                    QHash<QString, QFrame *> rowOf;
                    QSet<int> nameColumns;
                    for (QFrame *row : dir->findChildren<QFrame *>("pr")) {
                        if (!row->isVisible())
                            continue;           // rows from before the last re-layout, waiting to be deleted
                        auto *nameStrip = row->findChild<T::NameScroll *>();
                        auto *tagStrip = row->findChild<T::TagStrip *>();
                        QVERIFY2(nameStrip && tagStrip, where);
                        const QString name = nameStrip->label->text();
                        QVERIFY2(nameStrip->toolTip() == name, where);                  // the full name is always available
                        // [tags]  16-24px  [name]: the tag area is on the left, the name to its right
                        const int tagsRight = tagStrip->mapTo(row, QPoint(tagStrip->width(), 0)).x();
                        const int nameX = nameStrip->mapTo(row, QPoint(0, 0)).x();
                        QVERIFY2(nameX - tagsRight >= 16 && nameX - tagsRight <= 24,
                                 where + QStringLiteral(" gap %1").arg(nameX - tagsRight).toUtf8());
                        nameColumns.insert(nameX);
                        QStringList tags;
                        int tagY = -1;
                        for (QLabel *l : tagStrip->tags) {
                            tags << l->text();
                            QVERIFY2(l->font().pixelSize() == T::px(12), where);
                            QVERIFY2(l->width() >= l->sizeHint().width(), where);      // never clipped
                            // side by side on ONE line, never stacked
                            if (tagY < 0)
                                tagY = l->y();
                            QVERIFY2(l->y() == tagY, where + " tags share one line");
                        }
                        // when the tags do not fit, only their own strip scrolls; the name keeps its room
                        const bool overflow = tagStrip->contentWidth() > tagStrip->width();
                        QVERIFY2((tagStrip->horizontalScrollBar()->maximum() > 0) == overflow, where);
                        QVERIFY2(tagStrip->horizontalScrollBar()->isVisible() == overflow, where + " a bar only when needed");
                        QVERIFY2(nameStrip->width() >= 90, where);
                        tagsOf.insert(name, tags);
                        rowOf.insert(name, row);
                    }
                    QVERIFY2(tagsOf.size() == 5, where);
                    // with room to spare, every tag is in view without scrolling
                    if (size.width() >= 1024)
                        QVERIFY2(rowOf[LONG_NAME]->findChild<T::TagStrip *>()->horizontalScrollBar()->maximum() == 0, where);
                    QVERIFY2(nameColumns.size() == 1, where + " names start at the same place in every row");
                    // one tag per game; two Modern events are still one Magic tag
                    QVERIFY2(tagsOf[LONG_NAME] == (QStringList{QStringLiteral("Pokémon"), "One Piece", "Magic"}), where);
                    QVERIFY2(tagsOf["Bo"] == tagsOf[LONG_NAME], where);
                    for (const QString &none : {QString("Cy"), QString("Di"), QString("Newcomer")}) {
                        QVERIFY2(tagsOf[none].isEmpty(), where);
                        QVERIFY2(labelText(rowOf[none]).contains("No games yet"), where);
                    }
                    // the action stays visible and clickable, and rows are no taller than their content
                    for (QFrame *row : rowOf) {
                        QPushButton *view = row->findChild<QPushButton *>();
                        QVERIFY2(view && view->isVisible() && view->height() >= T::MIN_HIT, where);
                        QVERIFY2(view->width() >= view->sizeHint().width(), where);
                        QVERIFY2(view->visibleRegion().boundingRect().width() == view->width()
                                 || view->visibleRegion().isEmpty(), where);            // (empty = scrolled out of view)
                        QVERIFY2(row->height() <= row->sizeHint().height() + 2, where);
                    }
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());

                    // a name too long for its strip scrolls sideways by keyboard; it never moves by itself
                    auto *strip = rowOf[LONG_NAME]->findChild<T::NameScroll *>();
                    QScrollBar *bar = strip->horizontalScrollBar();
                    if (strip->label->width() > strip->viewport()->width()) {
                        QVERIFY2(bar->maximum() > 0 && bar->isVisible(), where);
                        QCOMPARE(bar->value(), 0);
                        QTest::keyClick(strip, Qt::Key_Right);
                        QVERIFY2(bar->value() > 0, where + " arrow key scrolls the name");
                        bar->setValue(bar->maximum());      // the end of the name can be reached
                        QCOMPARE(bar->value(), bar->maximum());
                    }
                    // an overflowing tag strip scrolls the same way, and an ordinary wheel turn over
                    // either strip is left for the page
                    auto *tagStrip = rowOf[LONG_NAME]->findChild<T::TagStrip *>();
                    if (tagStrip->horizontalScrollBar()->maximum() > 0) {
                        QVERIFY2(tagStrip->focusPolicy() & Qt::TabFocus, where);
                        QTest::keyClick(tagStrip, Qt::Key_Right);
                        QVERIFY2(tagStrip->horizontalScrollBar()->value() > 0, where + " arrow key scrolls the tags");
                        tagStrip->horizontalScrollBar()->setValue(tagStrip->horizontalScrollBar()->maximum());
                        const QLabel *lastTag = tagStrip->tags.last();
                        QVERIFY2(!lastTag->visibleRegion().isEmpty(), where + " the last tag can be reached");
                    }
                    for (QScrollArea *area : {static_cast<QScrollArea *>(strip), static_cast<QScrollArea *>(tagStrip)}) {
                        const int before = area->horizontalScrollBar()->value();
                        QWheelEvent wheel(QPointF(5, 5), area->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, -120),
                                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                        wheel.setAccepted(true);
                        QCoreApplication::sendEvent(area->viewport(), &wheel);
                        QVERIFY2(area->horizontalScrollBar()->value() == before, where + " the wheel is not captured");
                    }
                }
            }
        }
        // tags follow the saved history: a newly played round adds one, and a restart shows the same
        swiss::startTournament(pending);
        w_.reset();
        db::closeThreadConnection();
        openWindow();
        auto *dir = go<PlayersScreen>("players");
        int found = 0;
        for (QFrame *row : dir->findChildren<QFrame *>("pr")) {
            if (row->isVisible() && row->findChild<T::NameScroll *>()->label->text() == "Cy") {
                QCOMPARE(row->findChild<T::TagStrip *>()->tags.size(), 1);
                QCOMPARE(row->findChild<T::TagStrip *>()->tags[0]->text(), QStringLiteral("Pokémon"));
                ++found;
            }
        }
        QCOMPARE(found, 1);
        dir->searchBox->setText("Newc");
        QTest::qWait(400);
        int visibleRows = 0;
        for (QFrame *row : dir->findChildren<QFrame *>("pr"))
            visibleRows += row->isVisible() ? 1 : 0;
        QCOMPARE(visibleRows, 1);                       // search still works
    }

    // Clicking does not move the page.

    void ordinaryActionsLeaveThePageWhereItIs()
    {
        QStringList names;
        for (int i = 1; i <= 30; ++i)
            names << QStringLiteral("Player %1").arg(i, 2, 10, QChar('0'));
        const qint64 tid = commander(names, 2, false);
        w_.reset();
        openWindow(true);               // a real window, so focus behaves as it does for a user
        for (const QSize &size : {QSize(640, 480), QSize(1180, 780), QSize(420, 600)}) {
            for (const QString &textSize : {QString("standard"), QString("large")}) {
                prefs::setPref("text_size", textSize);
                w_->restyle();
                w_->resize(size);
                auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
                QVERIFY(reg);
                QScrollArea *page = reg->findChild<QScrollArea *>();
                QScrollBar *bar = page->verticalScrollBar();
                QVERIFY2(bar->maximum() > 300, "the list is long enough to scroll");
                const QByteArray where = QStringLiteral("%1 %2x%3").arg(textSize).arg(size.width()).arg(size.height()).toUtf8();

                // check players in and out again from the top, the middle and the bottom of the list
                for (double fraction : {0.12, 0.5, 0.86}) {      // (the very top of a short window shows only the search form)
                    const int target = int(bar->maximum() * fraction);
                    bar->setValue(target);
                    settle();
                    QList<QPushButton *> toggles;
                    for (QPushButton *b : reg->findChildren<QPushButton *>())
                        if (b->isVisible() && (b->text().contains("Check in") || b->text().contains("Checked in"))
                            && b->visibleRegion().contains(b->rect().center()))
                            toggles << b;
                    QVERIFY2(!toggles.isEmpty(), where);
                    QPushButton *toggle = toggles[toggles.size() / 2];
                    QPushButton *neighbour = toggles.first();
                    const QPoint placeBefore = toggle->mapTo(reg, QPoint(0, 0));
                    const int rowsBefore = int(reg->findChildren<QPushButton *>().size());
                    for (int click = 0; click < 4; ++click) {
                        const bool wasIn = toggle->property("checkedIn").toBool();
                        toggle->setFocus();
                        QTest::mouseClick(toggle, Qt::LeftButton);
                        settle();
                        QVERIFY2(bar->value() == target, where + QStringLiteral(" jumped from %1 to %2").arg(target).arg(bar->value()).toUtf8());
                        QVERIFY2(toggle->mapTo(reg, QPoint(0, 0)) == placeBefore, where + " the row stayed under the pointer");
                        QVERIFY2(toggle->property("checkedIn").toBool() != wasIn, where);       // and it really toggled
                        QVERIFY2(toggle->text().contains(wasIn ? "Check in" : "Checked in"), where);
                        QVERIFY2(QApplication::focusWidget() == toggle, where + " focus stays on the button");
                    }
                    // nothing else on the page was rebuilt
                    QVERIFY2(neighbour->isVisible(), where);
                    QCOMPARE(int(reg->findChildren<QPushButton *>().size()), rowsBefore);
                }
                QCOMPARE(cdb::getEvent(tid)["checked_in_count"].toInt(), 30);

                // changing a setup option at the bottom of the page does not move it either
                page->ensureWidgetVisible(reg->roundsSpin, 0, 60);
                settle();
                const int bottom = bar->value();
                const QPoint spinAt = reg->roundsSpin->mapTo(reg, QPoint(0, 0));
                reg->shortCombo->setCurrentIndex(1);
                reg->drawCombo->setCurrentIndex(1);
                reg->roundsSpin->setValue(4);
                settle();
                QVERIFY2(bar->value() == bottom, where + " changing a setting");
                QVERIFY2(reg->roundsSpin->mapTo(reg, QPoint(0, 0)) == spinAt, where + " the control stayed put");
                reg->shortCombo->setCurrentIndex(0);
                reg->drawCombo->setCurrentIndex(0);
                reg->roundsSpin->setValue(2);

                // removing a player (which does rebuild the list) keeps the page where it was
                bar->setValue(bar->maximum() / 2);
                settle();
                const int middle = bar->value();
                QPushButton *remove = nullptr;
                for (QPushButton *b : reg->findChildren<QPushButton *>())
                    if (b->isVisible() && b->text() == "Remove" && b->visibleRegion().contains(b->rect().center()))
                        remove = b;
                QVERIFY2(remove, where);
                const QString removedName = remove->accessibleName().mid(7);
                remove->setFocus();
                QTest::mouseClick(remove, Qt::LeftButton);
                settle();
                QVERIFY2(qAbs(bar->value() - middle) <= 2, where + QStringLiteral(" remove: %1 -> %2").arg(middle).arg(bar->value()).toUtf8());
                // put the player back for the next pass
                for (const db::Row &p : pdb::searchPlayers(removedName))
                    tdb::enrollPlayer(tid, p["player_id"].toLongLong());
            }
        }

        // the same holds when reporting results in a round
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(640, 480);
        cdb::startEvent(tid);
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QScrollBar *bar = ev->findChild<QScrollArea *>()->verticalScrollBar();
        for (double fraction : {0.3, 0.6, 0.95}) {
            bar->setValue(int(bar->maximum() * fraction));
            settle();
            const int target = bar->value();
            QPushButton *wins = nullptr;
            for (QPushButton *b : ev->findChildren<QPushButton *>())
                if (b->isVisible() && b->text() == "Wins" && b->visibleRegion().contains(b->rect().center()))
                    wins = b;
            QVERIFY(wins);
            wins->setFocus();
            QTest::mouseClick(wins, Qt::LeftButton);
            settle();
            QVERIFY2(qAbs(bar->value() - target) <= 2, qPrintable(QStringLiteral("round: %1 -> %2").arg(target).arg(bar->value())));
        }
        const qint64 mod = modern(names, 45, 2);
        auto *round = go<RoundScreen>("round", {{"tournament_id", mod}});
        bar = round->findChild<QScrollArea *>()->verticalScrollBar();
        for (double fraction : {0.4, 0.9}) {
            bar->setValue(int(bar->maximum() * fraction));
            settle();
            const int target = bar->value();
            T::WrapButton *pick = nullptr;
            for (T::WrapButton *b : round->findChildren<T::WrapButton *>())
                if (b->isVisible() && b->isEnabled() && b->visibleRegion().contains(b->rect().center()))
                    pick = b;
            QVERIFY(pick);
            pick->setFocus();
            QTest::mouseClick(pick, Qt::LeftButton);
            settle();
            QVERIFY2(qAbs(bar->value() - target) <= 2, qPrintable(QStringLiteral("modern round: %1 -> %2").arg(target).arg(bar->value())));
        }
    }

    // Commander setup explains each option.

    void commanderSetupOptionsHaveShortAccurateExplanations()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus"}, 3, false);
        w_->resize(1180, 780);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        auto words = [](const QLabel *l) { return int(QString(l->text()).remove(QChar(0x200B)).split(' ', Qt::SkipEmptyParts).size()); };

        // three-player pods: a standings-based seating rule, described as one
        QCOMPARE(reg->shortCombo->currentData().toString(), QString("LOW"));
        QCOMPARE(reg->shortHint->text(), QString("Lowest-ranked players fill the three-player pods."));
        reg->shortCombo->setCurrentIndex(reg->shortCombo->findData("HIGH"));
        QCOMPARE(reg->shortHint->text(), QString("Highest-ranked players fill the three-player pods."));
        QCOMPARE(cdb::getEvent(tid)["settings"].toMap()["three_pod_placement"].toString(), QString("HIGH"));
        reg->shortCombo->setCurrentIndex(reg->shortCombo->findData("BEST_MATCH"));
        QCOMPARE(reg->shortHint->text(), QString("Three-player pods go wherever point totals match best."));
        for (const QString &text : {reg->shortHint->text()})
            QVERIFY(!text.contains("bracket", Qt::CaseInsensitive) && !text.contains("eliminat", Qt::CaseInsensitive));

        // draws
        QCOMPARE(reg->drawHint->text(), QString("Knocked-out players take a loss; survivors share the draw."));
        reg->drawCombo->setCurrentIndex(1);
        QCOMPARE(reg->drawHint->text(), QString("Everyone seated in the pod gets the draw point."));
        // rounds: the total, not "plus a final"
        reg->roundsSpin->setValue(4);
        QCOMPARE(reg->roundsHint->text(), QString("Total rounds. The event ends after Round 4."));
        for (QLabel *hint : {reg->roundsHint, reg->drawHint, reg->shortHint}) {
            QVERIFY2(words(hint) >= 5 && words(hint) <= 10, qPrintable(hint->text()));
            QVERIFY(hint->isVisible());
            QCOMPARE(hint->font().pixelSize(), T::px(12));          // secondary text
        }

        // beside the control when there is room, directly below it when the window is narrow
        auto place = [reg](QWidget *w) { return w->mapTo(reg, QPoint(0, 0)); };
        QVERIFY(place(reg->shortHint).x() > place(reg->shortCombo).x() + reg->shortCombo->width() - 1);
        QVERIFY(qAbs(place(reg->shortHint).y() + reg->shortHint->height() - place(reg->shortCombo).y() - reg->shortCombo->height()) < 30);
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("light"), QString("dark")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                w_->resize(480, 700);
                reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
                QScrollArea *page = reg->findChild<QScrollArea *>();
                page->verticalScrollBar()->setValue(page->verticalScrollBar()->maximum());
                settle();
                const QPoint hint = reg->shortHint->mapTo(reg, QPoint(0, 0)), control = reg->shortCombo->mapTo(reg, QPoint(0, 0));
                QVERIFY2(hint.y() >= control.y() + reg->shortCombo->height(), "the explanation moves below its control");
                QVERIFY(hint.y() - (control.y() + reg->shortCombo->height()) < 30);
                QVERIFY2(layoutProblems().isEmpty(), qPrintable(layoutProblems().join("; ")));
                if (!qEnvironmentVariable("TCG_UI_SHOTS").isEmpty())
                    w_->grab().save(qEnvironmentVariable("TCG_UI_SHOTS") + QStringLiteral("/setup_%1_%2.png").arg(mode, textSize));
            }
        }
    }

    // Results are centred in the content area.

    void resultsViewsAreCentredInTheContentArea()
    {
        QStringList names{LONG_NAME, UNBROKEN};
        for (int i = 1; i <= 22; ++i)
            names << QStringLiteral("Player %1").arg(i);
        const qint64 mod = modern(names, 45, 2);
        for (int r = 1; r <= 2; ++r) {
            const db::Row rnd = tdb::currentRound(mod);
            for (const db::Row &m : tdb::roundPairings(rnd["round_id"].toLongLong()))
                if (!m["player2_id"].isNull())
                    tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
            if (r == 1)
                swiss::advanceToNextRound(mod, 1);
        }
        swiss::finalizeTournament(mod, "MTG");
        const qint64 cmd = commander(names.mid(0, 11), 1);
        for (const QVariant &pv : cdb::getRounds(cmd).last().toMap()["pods"].toList())
            cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN", pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong());
        cdb::finishTournament(cmd);

        // gaps on the two sides of `inner`, measured inside the screen it belongs to
        auto sideGaps = [](QWidget *screen, QWidget *inner) {
            const int left = inner->mapTo(screen, QPoint(0, 0)).x();
            return qMakePair(left, screen->width() - left - inner->width());
        };
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            prefs::setPref("text_size", textSize);
            w_->restyle();
            for (const QSize &size : {QSize(1920, 1040), QSize(1440, 900), QSize(1180, 780), QSize(900, 700), QSize(360, 640),
                                      QSize(1024, 500), QSize(800, 420)}) {
                w_->resize(size);
                const QByteArray where = QStringLiteral("%1 %2x%3").arg(textSize).arg(size.width()).arg(size.height()).toUtf8();

                // "View results" on a finished one-on-one tournament
                auto *results = go<RoundSelectScreen>("round_select", {{"tournament_id", mod}});
                QVERIFY2(results && results->panel, where);
                if (!qEnvironmentVariable("TCG_UI_SHOTS").isEmpty())
                    w_->grab().save(qEnvironmentVariable("TCG_UI_SHOTS") + "/results_" + QString::fromUtf8(where).replace(' ', '_') + ".png");
                auto gaps = sideGaps(results, results->panel);
                QVERIFY2(qAbs(gaps.first - gaps.second) <= 1, where + QStringLiteral(" results panel %1/%2").arg(gaps.first).arg(gaps.second).toUtf8());
                QVERIFY2(gaps.first >= 10, where);
                QVERIFY2(results->panel->width() <= 760, where);
                QVERIFY2(buttons(results).contains(QStringLiteral("View final standings  →")), where);
                QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());

                // the standings tables (one-on-one and Commander), with a page long enough to scroll
                for (qint64 tid : {mod, cmd}) {
                    w_->navigateTo("standings", {{"tournament_id", tid}});
                    settle();
                    QWidget *screen = w_->currentScreen();
                    auto *table = screen->findChild<T::HScroll *>();
                    QVERIFY2(table, where);
                    gaps = sideGaps(screen, table);
                    QVERIFY2(qAbs(gaps.first - gaps.second) <= 1, where + QStringLiteral(" table %1/%2").arg(gaps.first).arg(gaps.second).toUtf8());
                    // figures sit in right-aligned columns under right-aligned headings; names stay left
                    int rightAligned = 0;
                    for (QLabel *l : table->findChildren<QLabel *>()) {
                        const QString text = l->text();
                        if (text == "PTS" || text == "W" || text == "L")
                            QVERIFY2(l->alignment() & Qt::AlignRight, where);
                        if (text == "PLAYER" || text.startsWith("Alexandra"))
                            QVERIFY2(l->alignment() & Qt::AlignLeft, where);
                        if (l->alignment() & Qt::AlignRight)
                            ++rightAligned;
                    }
                    QVERIFY2(rightAligned > 20, where);
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                }

                // "View results" on a finished Commander event: the pods fill the row evenly
                auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", cmd}});
                QList<QFrame *> pods = ev->findChildren<QFrame *>("pc");
                pods.erase(std::remove_if(pods.begin(), pods.end(), [](QFrame *f) { return !f->isVisible(); }), pods.end());
                QCOMPARE(pods.size(), 3);
                int minLeft = 99999, maxRight = 0;
                for (QFrame *pod : pods) {
                    const int left = pod->mapTo(ev, QPoint(0, 0)).x();
                    minLeft = qMin(minLeft, left);
                    maxRight = qMax(maxRight, left + pod->width());
                }
                QVERIFY2(qAbs(minLeft - (ev->width() - maxRight)) <= 1, where + QStringLiteral(" pods %1/%2").arg(minLeft).arg(ev->width() - maxRight).toUtf8());
                const int lastLeft = pods.last()->mapTo(ev, QPoint(0, 0)).x();
                QVERIFY2(qAbs(lastLeft - (ev->width() - lastLeft - pods.last()->width())) <= 1, where + " the odd pod is centred");
            }
        }
    }

    void pairingSheetsAndSignsComeFromTheSavedRound()
    {
        QStringList names;
        for (int i = 1; i <= 11; ++i)
            names << QStringLiteral("Player %1").arg(i);
        names[3] = LONG_NAME;
        const qint64 tid = commander(names);
        const QVariantMap state = cdb::getState(tid);
        const QVariantMap rnd = state["rounds"].toList().last().toMap();
        const QByteArray before = databaseFingerprint();
        const printing::RoundData data = printing::commanderRoundData(state["event"].toMap()["name"].toString(), cdb::stageLabel(rnd), rnd);
        QCOMPARE(data.unit, QString("Pod"));
        QCOMPARE(data.round, QString("Round 1"));
        QVERIFY(data.seated);

        const printing::Doc byPod = printing::pairingsDoc(data, "table");
        QCOMPARE(byPod.columns[0].first, QString("Pod"));
        QCOMPARE(byPod.columns[1].first, QStringLiteral("Seat 1 · first turn"));
        for (const QVariant &pv : rnd["pods"].toList()) {
            const QVariantMap pod = pv.toMap();
            QStringList row;
            for (const QStringList &r : byPod.rows)
                if (r[0] == QStringLiteral("Pod %1").arg(pod["pod_number"].toInt()))
                    row = r;
            const QVariantList seats = pod["seats"].toList();
            for (int i = 0; i < seats.size(); ++i)
                QCOMPARE(row[i + 1], seats[i].toMap()["display_name"].toString());   // full names, in seat order
        }
        const printing::Doc byName = printing::pairingsDoc(data, "name");
        QCOMPARE(byName.rows.size(), 11);
        for (int i = 1; i < byName.rows.size(); ++i)
            QVERIFY(byName.rows[i - 1][0].toCaseFolded() <= byName.rows[i][0].toCaseFolded());

        for (const QString &paper : {QString("Letter"), QString("A4")}) {
            int pages = 0;
            QVERIFY(printing::pdfBytes({byPod}, paper, &pages).startsWith("%PDF"));
            QCOMPARE(pages, 1);
            printing::pdfBytes({printing::signsDoc(data)}, paper, &pages);
            QCOMPARE(pages, 3);                                 // one sign per pod
        }
        QVERIFY2(databaseFingerprint() == before, "printing never writes to the database");

        // the dialog offers both sheets, the signs, both papers and a page count
        printing::PrintDialog dialog(w_.get(), data);
        QCOMPARE(dialog.whatGroup->buttons().size(), 3);
        QCOMPARE(dialog.paperGroup->buttons().size(), 2);
        QVERIFY(dialog.pagesLabel->text().contains("1 page on Letter"));
        dialog.whatGroup->buttons()[2]->click();
        dialog.paperGroup->buttons()[1]->click();
        QVERIFY(dialog.pagesLabel->text().contains("3 pages on A4"));
        QVERIFY(dialog.printBtn && dialog.pdfBtn && dialog.closeBtn);
    }

    void longSheetsRepeatOverSeveralPagesAndModernListsTheBye()
    {
        QStringList names;
        for (int i = 1; i <= 121; ++i)
            names << QStringLiteral("Tournament Player Number %1").arg(i);
        const qint64 tid = modern(names);
        const db::Row rnd = tdb::currentRound(tid);
        const printing::RoundData data = printing::modernRoundData(tdb::tournamentById(tid), 1, tdb::roundPairings(rnd["round_id"].toLongLong()));
        QCOMPARE(data.unit, QString("Table"));
        QCOMPARE(data.groups.size(), 60);
        QCOMPARE(data.byes.size(), 1);
        const printing::Doc byTable = printing::pairingsDoc(data, "table");
        QVERIFY(byTable.note.startsWith("Bye: "));
        QCOMPARE(byTable.rows.first()[0], QString("Table 1"));
        const printing::Doc byName = printing::pairingsDoc(data, "name");
        QCOMPARE(byName.rows.size(), 121);                      // every player listed once, including the bye
        int pages = 0;
        printing::pdfBytes({byName}, "Letter", &pages);
        QVERIFY2(pages >= 3, "a long sheet runs over several pages");
        QCOMPARE(printing::pageCount(printing::signsDoc(data)), 60);
    }

    void clockWidgetShowsTheSavedTimeAndOnlyAnnouncesExpiry()
    {
        const qint64 tid = modern();
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        int expired = 0;
        TimerWidget clock(timerdb::Kind::OneOnOne, rid, [&expired] { ++expired; });
        QCOMPARE(clock.display->text(), QString("45:00"));
        QCOMPARE(clock.toggleBtn->text(), QString("Start"));
        clock.toggleBtn->click();
        QVERIFY(clock.caption->text().contains("RUNNING"));
        QCOMPARE(clock.toggleBtn->text(), QString("Pause"));
        db::exec("UPDATE rounds SET timer_started_at = timer_started_at - 1500 WHERE round_id = ?", {rid});
        clock.sync();
        QVERIFY2(clock.display->text().startsWith("20:0") || clock.display->text() == "19:59", qPrintable(clock.display->text()));
        clock.toggleBtn->click();
        QCOMPARE(clock.toggleBtn->text(), QString("Resume"));
        clock.toggleBtn->click();
        db::exec("UPDATE rounds SET timer_started_at = timer_started_at - 5000 WHERE round_id = ?", {rid});
        clock.sync();
        QCOMPARE(clock.display->text(), QString("Time expired"));
        QCOMPARE(expired, 1);
        clock.sync();
        QCOMPARE(expired, 1);                                   // announced once
        QVERIFY(!clock.toggleBtn->isEnabled());
        QCOMPARE(tdb::pendingMatchCount(rid), 2);               // nothing was finalized
        QCOMPARE(tdb::currentRound(tid)["round_number"].toInt(), 1);
        clock.resetBtn->click();
        QCOMPARE(clock.display->text(), QString("45:00"));
    }

    void everyScreenFitsEveryWindowSizeThemeAndTextSize()
    {
        const QList<qint64> pids = players({LONG_NAME, UNBROKEN, "Ana", "Bo", "Cy", "Di", "Ed"});
        const QList<qint64> extra = players({"Priya", "Marcus", "Nami", "Dom"});
        const qint64 mod = tdb::createTournament("Modern Monday With A Really Long Tournament Name", "MTG", 3, "Modern", {}, 45);
        const qint64 pendingMod = tdb::createTournament("Pending Modern", "MTG", 3, "Modern");
        for (qint64 p : pids) {
            tdb::enrollPlayer(mod, p);
            tdb::enrollPlayer(pendingMod, p);
        }
        swiss::startTournament(mod);
        for (const db::Row &m : tdb::roundPairings(tdb::currentRound(mod)["round_id"].toLongLong())) {
            if (!m["player2_id"].isNull()) {
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
                break;
            }
        }
        const qint64 cmd = cdb::createCommanderTournament("Friday Commander Night", 11, 2, {}, 7, {}, 80);
        const qint64 pendingCmd = cdb::createCommanderTournament("Pending Commander", 8);
        const qint64 doneCmd = cdb::createCommanderTournament("Finished Commander With A Long Name", 8, 3, {}, 9);
        for (qint64 p : pids + extra)
            tdb::enrollPlayer(cmd, p);
        for (qint64 p : pids)
            tdb::enrollPlayer(pendingCmd, p);
        for (qint64 p : pids + extra.mid(0, 3))
            tdb::enrollPlayer(doneCmd, p);
        cdb::startEvent(cmd);
        const QVariantMap r1 = cdb::getRounds(cmd).last().toMap();
        const QVariantMap pod0 = r1["pods"].toList()[0].toMap(), pod1 = r1["pods"].toList()[1].toMap();
        cdb::reportPodResult(pod0["pod_id"].toLongLong(), "WIN", pod0["seats"].toList()[0].toMap()["player_id"].toLongLong());
        cdb::reportPodResult(pod1["pod_id"].toLongLong(), "DRAW", 0, {pod1["seats"].toList()[1].toMap()["player_id"].toLongLong()});
        cdb::startEvent(doneCmd);
        for (int n = 1; n <= 3; ++n) {
            const QVariantMap rnd = cdb::getRounds(doneCmd).last().toMap();
            for (const QVariant &pv : rnd["pods"].toList())
                cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN",
                                     pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong());
            if (n < 3) {
                cdb::finalizeRound(rnd["round_id"].toLongLong());
                cdb::publishNextRound(doneCmd, n + 1);
            }
        }
        cdb::finishTournament(doneCmd);

        const QList<QPair<QString, QVariantMap>> screens{
            {"home", {}}, {"mtg_hub", {}}, {"poke_hub", {}}, {"players", {}},
            {"player_profile", {{"player_id", pids[0]}}},
            {"tournament_setup", {{"game", "MTG"}}},
            {"registration", {{"tournament_id", pendingMod}}},
            {"registration", {{"tournament_id", pendingCmd}}},
            {"round", {{"tournament_id", mod}}},
            {"standings", {{"tournament_id", mod}}},
            {"round", {{"tournament_id", cmd}}},
            {"standings", {{"tournament_id", cmd}}},
            {"round", {{"tournament_id", doneCmd}}},
            {"standings", {{"tournament_id", doneCmd}}},
        };
        QList<QSize> sizes{{360, 640}, {480, 800}, {640, 480}, {800, 600}, {768, 900}, {1024, 768}, {1280, 800},
                           {1920, 1040}, {800, 500}, {1024, 600}};
        // TCG_UI_SIZES=640x400,720x450 checks other sizes (used with QT_SCALE_FACTOR=2 for 200% scaling)
        const QString custom = qEnvironmentVariable("TCG_UI_SIZES");
        if (!custom.isEmpty()) {
            sizes.clear();
            for (const QString &item : custom.split(','))
                sizes << QSize(item.section('x', 0, 0).toInt(), item.section('x', 1, 1).toInt());
        }
        const QString shots = qEnvironmentVariable("TCG_UI_SHOTS");     // optional folder for screenshots
        QStringList report;
        int combinations = 0;
        for (const auto &look : {qMakePair(QString("dark"), QString("standard")), qMakePair(QString("light"), QString("standard")),
                                 qMakePair(QString("light"), QString("large")), qMakePair(QString("dark"), QString("large"))}) {
            prefs::setPref("theme", look.first);
            prefs::setPref("text_size", look.second);
            w_->restyle();
            for (const QSize &size : sizes) {
                w_->resize(size);
                settle();
                QCOMPARE(w_->size(), size);
                for (const auto &s : screens) {
                    w_->navigateTo(s.first, s.second);
                    settle();
                    QVERIFY2(!w_->history.isEmpty() && w_->history.last().first == s.first, qPrintable(s.first));
                    if (auto *setup = qobject_cast<TournamentSetupScreen *>(w_->currentScreen())) {
                        setup->formatCombo->setCurrentText("Commander");
                        settle();
                    }
                    ++combinations;
                    const qint64 tid = s.second.value("tournament_id").toLongLong();
                    const QString tag = QStringLiteral("%1_%2_%3x%4_%5%6").arg(look.first, look.second).arg(size.width())
                                            .arg(size.height()).arg(s.first, tid ? QStringLiteral("_%1").arg(tid) : QString());
                    for (const QString &problem : layoutProblems())
                        report << tag + ": " + problem;
                    const bool wanted = (size == QSize(320, 640) || size == QSize(1280, 800) || size == QSize(800, 500))
                                        && look.second != look.first.left(0) && (look == qMakePair(QString("dark"), QString("standard"))
                                                                               || look == qMakePair(QString("light"), QString("large")));
                    if (!shots.isEmpty() && wanted)
                        w_->grab().save(shots + "/" + tag + ".png");
                }
            }
        }
        if (!report.isEmpty())
            qWarning().noquote() << report.mid(0, 40).join("\n");
        QVERIFY2(report.isEmpty(), qPrintable(QStringLiteral("%1 layout problems").arg(report.size())));
        QCOMPARE(combinations, int(sizes.size() * screens.size() * 4));
    }
};

int main(int argc, char *argv[])
{
    QApplication::setStyle("Fusion");       // as main.cpp does
    QApplication app(argc, argv);
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_ui.moc"
