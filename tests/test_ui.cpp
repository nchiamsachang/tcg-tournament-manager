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
#include <QMessageBox>
#include <QRegularExpression>
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

// Answers the dialog that the next click opens: presses the button with that label, or Escape
// for "<esc>", or Enter for "<enter>".  `seen` receives the dialog's title and text.
void answerNextDialog(const QString &answer, QString *seen = nullptr)
{
    QTimer::singleShot(200, qApp, [answer, seen] {
        QWidget *dlg = QApplication::activeModalWidget();
        if (!dlg)
            return;
        if (seen) {
            QStringList text{dlg->windowTitle()};
            for (QLabel *l : dlg->findChildren<QLabel *>())
                text << l->text().remove(QChar(0x200B));
            *seen = text.join(" | ");
        }
        if (answer == "<esc>") {
            QTest::keyClick(dlg, Qt::Key_Escape);
        } else if (answer == "<enter>") {
            QTest::keyClick(dlg, Qt::Key_Return);
        } else {
            for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
                if (b->text() == answer) {
                    b->click();
                    return;
                }
            }
            dlg->close();
        }
    });
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
        QVERIFY2(w_->gearBtn->text().isEmpty() && !w_->gearBtn->icon().isNull(), "an icon, not a text symbol");
        QCOMPARE(w_->gearBtn->toolTip(), QString("Settings"));
        QVERIFY(w_->gearBtn->styleSheet().contains("border-radius:22px"));
        QVERIFY(w_->gearBtn->styleSheet().contains("QPushButton:focus"));
        QVERIFY(w_->gearBtn->focusPolicy() & Qt::TabFocus);
        int rightmost = 0;
        for (QPushButton *b : w_->findChildren<QPushButton *>())        // (icon-only buttons share an empty label)
            if (b->isVisible())
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

    void everyIconIsBuiltInAndTakesItsColour()
    {
        const QStringList names = QDir(":/assets/icons").entryList({"*.svg"});
        QVERIFY(names.size() >= 20);
        for (const QString &file : names) {
            const QString name = QFileInfo(file).completeBaseName();
            const QImage red = T::iconPixmap(name, "#ff0000", 18).toImage();
            QVERIFY2(!red.isNull(), qPrintable(name));
            bool drawn = false;
            for (int y = 0; y < red.height() && !drawn; ++y)
                for (int x = 0; x < red.width() && !drawn; ++x) {
                    const QColor c = red.pixelColor(x, y);
                    drawn = c.alpha() == 255 && c.red() == 255 && c.green() == 0 && c.blue() == 0;
                }
            QVERIFY2(drawn, qPrintable(name + " is drawn in the colour asked for"));
        }
        QVERIFY(T::iconPixmap("no-such-icon", "#ff0000").isNull());
        // icon-only controls: a name and a tooltip, no text symbol, at least 44 by 44
        go<HomeScreen>("home");
        for (QPushButton *b : {w_->themeBtn, w_->gearBtn, w_->backBtn}) {
            QVERIFY(b->text().isEmpty() && !b->icon().isNull());
            QVERIFY(!b->toolTip().isEmpty() && !b->accessibleName().isEmpty());
            QVERIFY2(b->width() >= 44 && b->height() >= 44,
                     qPrintable(QStringLiteral("%1 is %2x%3").arg(b->accessibleName()).arg(b->width()).arg(b->height())));
        }
        // the theme icon shows the action: a different picture in each theme
        const QImage before = w_->themeBtn->icon().pixmap(22).toImage();
        w_->toggleTheme();
        QVERIFY(w_->themeBtn->icon().pixmap(22).toImage() != before);
        w_->toggleTheme();
    }

    void aQuietFooterEndsEveryMainPage()
    {
        HomeScreen *home = go<HomeScreen>("home");
        QVERIFY(labelText(home).contains("Organize trading card tournaments, manage players, and keep every round running smoothly. "
                                         "Track pairings, standings, and results for formats including MTG Modern and Commander."));
        const qint64 mod = modern();
        const qint64 cmd = commander({"Ed", "Flo", "Gus", "Hal"}, 2);
        const qint64 pending = modern({"Ida", "Jo"}, 45, 2, false, "Pending Modern");
        const qint64 pid = pdb::allPlayers().first()["player_id"].toLongLong();
        const QList<QPair<QString, QVariantMap>> pages{
            {"home", {}}, {"mtg_hub", {}}, {"players", {}}, {"player_profile", {{"player_id", pid}}},
            {"tournament_setup", {{"game", "MTG"}}}, {"registration", {{"tournament_id", pending}}},
            {"round", {{"tournament_id", mod}}}, {"standings", {{"tournament_id", mod}}},
            {"round", {{"tournament_id", cmd}}}, {"standings", {{"tournament_id", cmd}}},
        };
        for (const QString &mode : {QString("dark"), QString("light")}) {
            prefs::setPref("theme", mode);
            w_->restyle();
            for (const QSize &size : {QSize(1180, 780), QSize(360, 480)}) {         // roomy, and narrow and short
                w_->resize(size);
                for (const auto &page : pages) {
                    w_->navigateTo(page.first, page.second);
                    settle();
                    const QByteArray where = QStringLiteral("%1 %2 %3x%4").arg(page.first, mode).arg(size.width()).arg(size.height()).toUtf8();
                    const QList<QWidget *> footers = w_->currentScreen()->findChildren<QWidget *>("pageFooter");
                    QVERIFY2(footers.size() == 1, where);
                    QWidget *footer = footers.first();
                    QPushButton *bug = footer->findChild<QPushButton *>();
                    QVERIFY2(bug && bug->text() == "Report a bug", where);
                    QVERIFY2(bug->toolTip() == "https://github.com/nchiamsachang/tcg-tournament-manager/issues", where);
                    QVERIFY2(bug->toolTip() == prefs::issuesUrl(), where);         // the address the app is configured with
                    QVERIFY2(bug->focusPolicy() & Qt::TabFocus, where);
                    QVERIFY2(bug->height() >= T::MIN_HIT - 1, where);
                    QVERIFY2(bug->font().pixelSize() <= T::px(12), where);         // small text
                    QVERIFY2(labelText(footer).contains("Produced by Nathan Chiamsachang"), where);
                    // small: one line when there is room, never more than two short ones
                    QVERIFY2(footer->height() <= (size.width() > 700 ? 56 : 104), where + " " + QByteArray::number(footer->height()));
                    // it is the last thing in the page's scrolling content, below everything else there
                    auto *area = qobject_cast<QScrollArea *>(footer->parentWidget()->parentWidget()->parentWidget());
                    QVERIFY2(area, where);
                    QWidget *content = area->widget();
                    QVERIFY2(footer->mapTo(content, QPoint(0, footer->height())).y() == content->height(), where);
                    for (QWidget *other : content->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
                        if (other != footer && other->isVisible())
                            QVERIFY2(other->geometry().bottom() < footer->geometry().top() + 1, where + " nothing overlaps it");
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                }
            }
        }
        // none of the old footer wording, and nothing of the kind in a dialog
        const QString homeText = labelText(go<HomeScreen>("home"));
        for (const char *banned : {"Design notes", "Copy prompt", "Claude", "Demo data", "Founded by", "Report an issue"})
            QVERIFY2(!homeText.contains(banned), banned);
        SettingsDialog settings(w_.get());
        QVERIFY(settings.findChildren<QWidget *>("pageFooter").isEmpty());
    }

    void brandingIsLargerAndStillFits()
    {
        go<HomeScreen>("home");
        QCOMPARE(w_->logo->size(), QSize(36, 36));                      // was 30: 20% larger
        QCOMPARE(w_->logo->pixmap().deviceIndependentSize().toSize(), QSize(T::px(19), T::px(19)));     // was 16
        QCOMPARE(w_->appLabel->font().pixelSize(), T::px(18));          // was 15
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            prefs::setPref("text_size", textSize);
            w_->restyle();
            for (int width : {1180, 800, 640, 600, 480, 420, 360}) {
                w_->resize(width, 600);
                settle();
                const QByteArray where = (textSize + " " + QString::number(width)).toUtf8();
                // the name beside the logo is centred on it, and neither is cut off or runs into the tabs
                if (w_->appLabel->isVisible()) {
                    QVERIFY2(w_->appLabel->width() >= w_->appLabel->sizeHint().width(), where);
                    QVERIFY2(qAbs(w_->appLabel->geometry().center().y() - w_->logo->geometry().center().y()) <= 1, where);
                    QVERIFY2(w_->appLabel->geometry().right() < w_->navButtons["home"]->geometry().left(), where);
                }
                if (w_->logo->isVisible())
                    QVERIFY2(w_->logo->parentWidget()->height() >= w_->logo->height() + 8, where);
                for (QPushButton *b : {w_->navButtons["home"], w_->navButtons["players"], w_->themeBtn, w_->gearBtn}) {
                    QVERIFY2(b->isVisible() && b->width() >= b->sizeHint().width(), where);
                    QVERIFY2(b->mapTo(w_.get(), QPoint(b->width(), 0)).x() <= w_->width(), where);
                }
                QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
            }
        }
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
        QVERIFY(buttons(reg).contains(QStringLiteral("Start tournament")));
        reg->confirmStart();
        settle();

        auto *round = qobject_cast<RoundScreen *>(w_->currentScreen());
        QVERIFY(round);
        QVERIFY2(buttons(round).contains(QStringLiteral("Print")), "Print is offered inside the round");
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
        QVERIFY(buttons(ev).contains(QStringLiteral("Print")));
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
            const QString closing = last ? QString("Finish tournament") : QStringLiteral("Finalize Round %1").arg(number);
            QVERIFY2(!buttons(ev).value(closing)->isEnabled(), "not until every pod has a result");
            for (const QVariant &pv : rnd["pods"].toList()) {
                const QVariantMap pod = pv.toMap();
                ev->report(pod, "WIN", pod["seats"].toList()[0].toMap()["player_id"].toLongLong(), {});
            }
            if (number < 3) {
                QVERIFY(buttons(ev).value(closing)->isEnabled());
                ev->finalizeRound(ev->state["rounds"].toList().last().toMap());
                QVERIFY2(ev->state["rounds"].toList().size() == number, "finalizing does not create a round by itself");
                buttons(ev).value(QStringLiteral("Start Round %1").arg(number + 1))->click();
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
        QVERIFY(names.contains(QStringLiteral("View final standings")));
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
        QVERIFY(!shown.contains("Wins") && !shown.contains(QStringLiteral("Start Round 3")));
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
                    QSet<int> nameColumns, tagColumns;
                    for (QFrame *row : dir->findChildren<QFrame *>("pr")) {
                        if (!row->isVisible())
                            continue;           // rows from before the last re-layout, waiting to be deleted
                        auto *nameStrip = row->findChild<T::NameScroll *>();
                        auto *tagStrip = row->findChild<T::TagStrip *>();
                        QVERIFY2(nameStrip && tagStrip, where);
                        const QString name = nameStrip->label->text();
                        QVERIFY2(nameStrip->toolTip() == name, where);                  // the full name is always available
                        // [name]  16-24px  [tags]: the name is on the left, the tag area to its right
                        const int nameRight = nameStrip->mapTo(row, QPoint(nameStrip->width(), 0)).x();
                        const int nameX = nameStrip->mapTo(row, QPoint(0, 0)).x();
                        const int tagsX = tagStrip->mapTo(row, QPoint(0, 0)).x();
                        QVERIFY2(tagsX - nameRight >= 16 && tagsX - nameRight <= 24,
                                 where + QStringLiteral(" gap %1").arg(tagsX - nameRight).toUtf8());
                        QVERIFY2(nameX < tagsX && nameX <= 16, where + " the name starts at the row's left edge");
                        nameColumns.insert(nameX);
                        tagColumns.insert(tagsX);
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
                    QVERIFY2(tagColumns.size() == 1, where + " and so do the tags, to their right");
                    // one tag per game; two Modern events are still one Magic tag
                    QVERIFY2(tagsOf[LONG_NAME] == (QStringList{"One Piece", QStringLiteral("Pokémon"), "Magic"}), where);
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
                // (the very top of a short window shows only the search form, and the very bottom the
                // event settings and the page footer)
                for (double fraction : {0.12, 0.5, 0.8}) {
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
                QVERIFY2(buttons(results).contains(QStringLiteral("View final standings")), where);
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

    // The game filter on the Players page.

    void playersCanBeFilteredByTheGamesTheyHavePlayed()
    {
        // Ana: One Piece and Pokémon.  Bo: Pokémon and Magic (Modern).  Cy: Magic (Commander).
        // Di, Ed, Flo, Gus: in the Commander pod only.  Newcomer: registered for something, played nothing.
        const QList<qint64> ids = players({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Newcomer"});
        const auto played = [&](const QString &game, const QString &format, const QList<int> &who) {
            const qint64 t = tdb::createTournament(game + " " + format, game, 1, format);
            for (int i : who)
                tdb::enrollPlayer(t, ids[i]);
            swiss::startTournament(t);
        };
        played("ONEPIECE", "Standard", {0, 3});
        played("POKEMON", "Standard", {0, 1});
        played("MTG", "Modern", {1, 4});
        const qint64 cmd = cdb::createCommanderTournament("Commander", 4, 1, {}, 5);
        for (int i : {2, 4, 5, 6})
            tdb::enrollPlayer(cmd, ids[i]);
        cdb::startEvent(cmd);
        tdb::enrollPlayer(tdb::createTournament("Not started", "POKEMON", 1, "Standard"), ids[7]);

        auto *dir = go<PlayersScreen>("players");
        const auto shown = [&] {
            settle();
            QStringList names;
            for (QFrame *row : dir->findChildren<QFrame *>("pr"))
                if (row->isVisible())
                    names << row->findChild<T::NameScroll *>()->label->text();
            names.sort();
            return names;
        };
        const QStringList everyone{"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Newcomer"};
        // All games is the default, and includes players with no history
        QVERIFY(dir->gameFilter().isEmpty());
        QCOMPARE(shown(), everyone);
        QCOMPARE(dir->filterBtn->text(), QString("Filter"));
        QVERIFY(!dir->filterBtn->icon().isNull());
        QCOMPARE(dir->countLabel->text(), QString("8 players"));
        QVERIFY(dir->filterBtn->geometry().left() > dir->searchBox->geometry().left());      // beside the search field
        QVERIFY(qAbs(dir->filterBtn->geometry().center().y() - dir->searchBox->geometry().center().y()) <= 2);

        // one game
        dir->setGameChecked("ONEPIECE", true);
        QCOMPARE(shown(), (QStringList{"Ana", "Di"}));
        QCOMPARE(dir->filterBtn->text(), QStringLiteral("Filter · 1"));
        QVERIFY(dir->filterBtn->styleSheet().contains("border:3px solid " + T::PURPLE_LT));  // marked as on
        QVERIFY(dir->filterBtn->accessibleName().contains("One Piece"));
        QCOMPARE(dir->countLabel->text(), QString("2 of 8 players"));
        // several games: anyone who has played ANY of them.  Modern and Commander are both Magic.
        dir->setGameChecked("MTG", true);
        QCOMPARE(shown(), (QStringList{"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus"}));
        QCOMPARE(dir->filterBtn->text(), QStringLiteral("Filter · 2"));
        dir->setGameChecked("ONEPIECE", false);
        QCOMPARE(shown(), (QStringList{"Bo", "Cy", "Ed", "Flo", "Gus"}));
        // with the name search
        dir->searchBox->setText("o");
        QTest::qWait(400);
        QCOMPARE(shown(), (QStringList{"Bo", "Flo"}));
        QCOMPARE(dir->countLabel->text(), QString("2 of 3 players"));       // of the three names with an "o"
        dir->searchBox->setText("Newc");
        QTest::qWait(400);
        QVERIFY(shown().isEmpty());
        QLabel *empty = dir->findChild<QLabel *>("noPlayers");
        QVERIFY(empty && empty->isVisible());
        const QString emptyText = empty->text().remove(QChar(0x200B));
        QVERIFY2(emptyText.contains("Newc") && emptyText.contains("Magic: The Gathering") && emptyText.contains("All games"),
                 qPrintable(emptyText));
        dir->searchBox->clear();
        QTest::qWait(400);
        // unticking the last game goes back to All games
        dir->setGameChecked("MTG", false);
        QVERIFY(dir->gameFilter().isEmpty());
        QCOMPARE(shown(), everyone);
        QCOMPARE(dir->filterBtn->text(), QString("Filter"));
        QVERIFY(!dir->filterBtn->styleSheet().contains("border:3px solid " + T::PURPLE_LT));
        // ticking All games clears the games
        dir->setGameChecked("POKEMON", true);
        dir->setGameChecked("MTG", true);
        dir->setGameChecked(QString(), true);
        QVERIFY(dir->gameFilter().isEmpty());
        QCOMPARE(shown(), everyone);

        // the menu: one box for All games and one per supported game, opaque and themed, staying open while ticking
        for (const QString &mode : {QString("dark"), QString("light")}) {
            prefs::setPref("theme", mode);
            w_->restyle();
            dir = qobject_cast<PlayersScreen *>(w_->currentScreen());
            QVERIFY(dir);
            QStringList labels;
            dir->filterBtn->click();
            settle();
            QFrame *menu = dir->filterMenu();
            QVERIFY(menu && menu->isVisible());
            const QList<QCheckBox *> boxes = menu->findChildren<QCheckBox *>();
            QCOMPARE(boxes.size(), 1 + int(tdb::supportedGames().size()));      // All games, then one per supported game
            const auto read = [&] {
                for (QCheckBox *b : boxes)
                    labels << b->text() + (b->isChecked() ? " [x]" : " [ ]");
            };
            read();
            // opaque, in the theme's surface colour, under the button and inside the window
            const bool checked = !menu->testAttribute(Qt::WA_TranslucentBackground)
                                 && menu->styleSheet().contains("background:" + T::SURFACE2);
            QVERIFY(menu->mapToGlobal(QPoint(0, 0)).y() >= dir->filterBtn->mapToGlobal(QPoint(0, dir->filterBtn->height())).y());
            QVERIFY(menu->mapToGlobal(QPoint(menu->width(), 0)).x() <= w_->mapToGlobal(QPoint(w_->width(), 0)).x());
            for (QCheckBox *b : boxes) {                    // real, labelled, keyboard-reachable checkboxes
                QVERIFY(b->focusPolicy() & Qt::TabFocus);
                QVERIFY(!b->text().isEmpty() && b->height() >= T::MIN_HIT - 1);
            }
            boxes[2]->click();              // Pokémon
            boxes[3]->click();              // Magic
            labels << "then";
            read();
            labels << (menu->isVisible() ? "open" : "closed");
            QTest::keyClick(boxes[0], Qt::Key_Space);       // All games again, by keyboard
            labels << "then";
            read();
            QTest::keyClick(menu, Qt::Key_Escape);
            settle();
            QVERIFY(!dir->filterMenu() || !dir->filterMenu()->isVisible());
            const QStringList expected{
                "All games [x]", "One Piece [ ]", QStringLiteral("Pokémon [ ]"), "Magic: The Gathering [ ]", "then",
                "All games [ ]", "One Piece [ ]", QStringLiteral("Pokémon [x]"), "Magic: The Gathering [x]", "open", "then",
                "All games [x]", "One Piece [ ]", QStringLiteral("Pokémon [ ]"), "Magic: The Gathering [ ]"};
            QCOMPARE(labels, expected);
            QVERIFY2(checked, "the menu is opaque and uses the theme's surface colour");
            QVERIFY(dir->gameFilter().isEmpty());
        }
        // one entry per supported game, in that list's order
        QCOMPARE(tdb::supportedGames().size(), 3);

        // filtering a long list does not move the page
        QStringList many;
        for (int i = 0; i < 40; ++i)
            many << QStringLiteral("Extra %1").arg(i, 2, 10, QChar('0'));
        const QList<qint64> extra = players(many);
        const qint64 big = tdb::createTournament("Big Pokemon", "POKEMON", 1, "Standard");
        for (qint64 p : extra)
            tdb::enrollPlayer(big, p);
        swiss::startTournament(big);
        w_->resize(900, 500);
        dir = go<PlayersScreen>("players");
        QScrollBar *bar = dir->findChild<QScrollArea *>()->verticalScrollBar();
        settle();
        QVERIFY(bar->maximum() > 400);
        bar->setValue(300);
        dir->setGameChecked("POKEMON", true);           // 42 of the 48 remain: the list is still longer than the window
        settle();
        QCOMPARE(bar->value(), 300);
        dir->setGameChecked(QString(), true);
        settle();
        QCOMPARE(bar->value(), 300);
    }

    // Ending a tournament early.

    void endingATournamentEarlyAsksFirstAndKeepsItAsATerminatedRecord()
    {
        const qint64 tid = modern({"Ana", "Bo", "Cy", "Di", "Ed"}, 45, 3, true, "Cut Short Cup");
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        for (const db::Row &m : tdb::roundPairings(rid)) {
            if (!m["player2_id"].isNull()) {
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
                break;
            }
        }
        const qint64 playerId = tdb::enrolledPlayers(tid).first()["player_id"].toLongLong();
        auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
        QVERIFY(round && round->endEarlyBtn && round->endEarlyBtn->isVisible());
        QCOMPARE(round->endEarlyBtn->text(), QString("End tournament early"));
        QVERIFY(!round->endEarlyBtn->icon().isNull());
        QVERIFY(round->endEarlyBtn->styleSheet().contains(T::RED));                         // the destructive style
        // apart from the routine round actions, which are in the bar at the bottom
        QVERIFY(round->endEarlyBtn->parentWidget() != round->nextRoundBtn->parentWidget());
        QVERIFY(w_->tabButton(tid));
        const db::Rows matches = tdb::allMatches(tid);

        // keeping it: the button, Escape, Enter (the default) and closing the window all change nothing
        QString seen;
        for (const QString &answer : {QString("Keep tournament"), QString("<esc>"), QString("<enter>"), QString("no such button")}) {
            seen.clear();
            answerNextDialog(answer, &seen);
            round->endEarlyBtn->click();
            settle();
            QVERIFY2(!seen.isEmpty(), qPrintable(answer));
            QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);
            QVERIFY(tdb::tournamentById(tid)["terminated_at"].isNull());
            QCOMPARE(tdb::allMatches(tid), matches);
            QVERIFY(w_->tabButton(tid));
            QCOMPARE(w_->currentScreen(), round);
        }
        QVERIFY2(seen.startsWith("End this tournament early?"), qPrintable(seen));
        QVERIFY2(seen.contains(QStringLiteral("This will end Cut Short Cup and remove it from active tournaments. Its saved rounds and "
                                              "results will remain in History, marked \u2018Terminated.\u2019 Unreported matches "
                                              "will remain unfinished.")), qPrintable(seen));
        // the safe button is the default one
        QTimer::singleShot(200, qApp, [] {
            QWidget *dlg = QApplication::activeModalWidget();
            QStringList defaults;
            for (QPushButton *b : dlg->findChildren<QPushButton *>())
                if (b->isDefault())
                    defaults << b->text();
            dlg->setProperty("defaults", defaults);
            QTest::keyClick(dlg, Qt::Key_Escape);
        });
        setAutoConfirm(true);                           // "answer yes to everything" does not apply to this question
        round->endEarlyBtn->click();
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);

        // ending it
        timerdb::start(timerdb::Kind::OneOnOne, rid);
        answerNextDialog("End tournament");
        round->endEarlyBtn->click();
        settle();
        const db::Row ended = tdb::tournamentById(tid);
        QCOMPARE(ended["status"].toString(), tdb::TERMINATED);
        QVERIFY(!ended["terminated_at"].toString().isEmpty());
        QCOMPARE(tdb::allMatches(tid), matches);                // results and the unreported matches exactly as they were
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, rid).closed);
        QVERIFY(!w_->tabButton(tid));                           // no longer among the active tournaments
        QCOMPARE(w_->liveTabCount(), 0);
        QVERIFY(w_->toast->isVisible() && w_->toastLabel->text().remove(QChar(0x200B)).contains("ended early"));
        auto *hub = qobject_cast<HubScreen *>(w_->currentScreen());
        QVERIFY(hub);
        const auto historyRow = [&](HubScreen *h) -> T::ClickFrame * {
            for (T::ClickFrame *r : h->findChildren<T::ClickFrame *>())
                if (r->isVisible() && r->accessibleName() == "Cut Short Cup, terminated")
                    return r;
            return nullptr;
        };
        QVERIFY(historyRow(hub));
        QString rowText = labelText(historyRow(hub));
        QVERIFY2(rowText.contains("Terminated") && rowText.contains("Ended early in round 1 of 3"), qPrintable(rowText));
        QVERIFY2(!rowText.contains("remaining") && !rowText.contains("Round in progress"), qPrintable(rowText));
        QVERIFY(labelText(hub).contains("No active tournaments"));

        // after a restart it is still a terminated record, and nothing counts down
        w_.reset();
        db::closeThreadConnection();
        openWindow();
        QVERIFY(!w_->tabButton(tid));
        QCOMPARE(w_->liveTabCount(), 0);
        hub = go<HubScreen>("mtg_hub");
        QVERIFY(historyRow(hub));
        QTest::mouseClick(historyRow(hub), Qt::LeftButton);
        settle();
        auto *select = qobject_cast<RoundSelectScreen *>(w_->currentScreen());
        QVERIFY(select);
        QVERIFY(labelText(select).contains("Terminated"));
        QVERIFY(!labelText(select).contains("Tournament complete"));
        QVERIFY(buttons(select).contains("View standings") && buttons(select).contains("Round 1"));
        // the round is a read-only record
        round = go<RoundScreen>("round", {{"tournament_id", tid}, {"round_number", 1}});
        QVERIFY(round);
        QVERIFY(!round->endEarlyBtn);
        QVERIFY(!round->findChild<TimerWidget *>());
        QVERIFY(!round->nextRoundBtn->isVisible() && !round->finalizeBtn->isVisible());
        QVERIFY(!buttons(round).contains("Undo"));
        for (T::WrapButton *b : round->findChildren<T::WrapButton *>())
            QVERIFY(!b->isEnabled());
        const QString roundText = labelText(round);
        QVERIFY2(roundText.contains("Terminated. This tournament was ended early"), qPrintable(roundText));
        QVERIFY(roundText.contains("Awaiting result"));         // unreported matches are shown as they are
        QVERIFY(labelText(go<StandingsScreen>("standings", {{"tournament_id", tid}})).contains("Terminated"));
        auto *profile = go<PlayerProfileScreen>("player_profile", {{"player_id", playerId}});
        QVERIFY(labelText(profile).contains("Terminated"));
        QCOMPARE(tdb::allMatches(tid), matches);
    }

    void endingACommanderEventEarlyLeavesAReadOnlyRecord()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"}, 3, true, 80, "Cut Short Commander");
        const QVariantMap rnd = cdb::getRounds(tid).last().toMap();
        const QVariantMap pod = rnd["pods"].toList()[0].toMap();
        cdb::reportPodResult(pod["pod_id"].toLongLong(), "WIN", pod["seats"].toList()[0].toMap()["player_id"].toLongLong());
        timerdb::start(timerdb::Kind::Commander, rnd["round_id"].toLongLong());
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(ev && ev->endEarlyBtn && ev->endEarlyBtn->isVisible());
        QVERIFY(ev->findChild<TimerWidget *>());
        QVERIFY(w_->tabButton(tid));
        // reporting a result rebuilds the page; the action is still there afterwards
        QVERIFY(buttons(ev).contains("Change") && buttons(ev).contains("Wins"));

        answerNextDialog("<esc>");
        ev->endEarlyBtn->click();
        settle();
        QVERIFY(!tdb::isTerminated(tid));
        QCOMPARE(w_->currentScreen(), ev);

        QString seen;
        answerNextDialog("End tournament", &seen);
        ev->endEarlyBtn->click();
        settle();
        QVERIFY(seen.contains("Cut Short Commander"));
        QVERIFY(tdb::isTerminated(tid));
        QVERIFY(!w_->tabButton(tid));
        QVERIFY(qobject_cast<HubScreen *>(w_->currentScreen()));
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("SWISS"));       // not completed, no winner
        QVERIFY(cdb::getEvent(tid)["champion_player_id"].isNull());

        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(ev);
        QVERIFY(!ev->endEarlyBtn);
        QVERIFY(!ev->findChild<TimerWidget *>());
        QVERIFY(!w_->tabButton(tid));                           // opening the record does not make it active again
        const QStringList names = buttons(ev).keys();
        for (const QString &label : names)
            QVERIFY2(label != "Wins" && label != "Change" && label != "Clear" && !label.startsWith("Draw")
                     && !label.startsWith("Correct") && !label.startsWith("Finalize") && !label.startsWith("Start Round")
                     && !label.startsWith("Finish"), qPrintable(label));
        const QString text = labelText(ev);
        QVERIFY2(text.contains("Terminated. This tournament was ended early"), qPrintable(text));
        QVERIFY2(text.contains("round not finished"), qPrintable(text));
        QVERIFY(!text.contains("Tournament complete"));
        QVERIFY(labelText(go<CommanderStandingsScreen>("standings", {{"tournament_id", tid}})).contains("Terminated"));
    }

    // Renaming a tournament.

    void tournamentsCanBeRenamedFromTheirPagesInEveryState()
    {
        // Drives the rename dialog the next click opens: optionally types a name, then finishes with a
        // button label, "<enter>" or "<esc>".  If the dialog refuses the name it reports the message and cancels.
        const auto answer = [](const QString &typed, const QString &finish, QString *prefill = nullptr, QString *refusal = nullptr) {
            QTimer::singleShot(200, qApp, [=] {
                QWidget *dlg = QApplication::activeModalWidget();
                if (!dlg)
                    return;
                auto *edit = dlg->findChild<QLineEdit *>();
                if (prefill)
                    *prefill = edit->text() + "|" + edit->selectedText() + "|" + dlg->windowTitle();
                if (!typed.isNull())
                    edit->setText(typed);
                if (finish == "<enter>") {
                    QTest::keyClick(edit, Qt::Key_Return);
                } else if (finish == "<esc>") {
                    QTest::keyClick(dlg, Qt::Key_Escape);
                } else {
                    for (QPushButton *b : dlg->findChildren<QPushButton *>())
                        if (b->text() == finish)
                            b->click();
                }
                if (dlg->isVisible()) {             // the name was refused and the dialog stayed open
                    QLabel *message = dlg->findChild<QLabel *>("renameError");
                    if (refusal)
                        *refusal = message && message->isVisible() ? message->text().remove(QChar(0x200B)) : QString("(no message)");
                    QTest::keyClick(dlg, Qt::Key_Escape);
                }
            });
        };
        const auto shownLabels = [&] { return labelText(w_->currentScreen()); };

        // 1. while players are being registered
        QStringList many;
        for (int i = 0; i < 30; ++i)
            many << QStringLiteral("Player %1").arg(i, 2, 10, QChar('0'));
        const qint64 pending = modern(many, 45, 3, false, "Old Pending Name");
        w_->resize(900, 520);
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", pending}});
        QVERIFY(reg && reg->renameBtn && reg->renameBtn->isVisible());
        QCOMPARE(reg->renameBtn->toolTip(), QString("Edit tournament name"));
        QCOMPARE(reg->renameBtn->accessibleName(), QString("Edit tournament name"));
        QVERIFY(reg->renameBtn->text().isEmpty() && !reg->renameBtn->icon().isNull());
        QVERIFY(reg->renameBtn->width() >= T::MIN_HIT && reg->renameBtn->height() >= T::MIN_HIT);
        // beside the title: on its line, just to its right
        QVERIFY(reg->renameBtn->geometry().left() >= reg->titleLabel->geometry().right());
        QVERIFY2(reg->renameBtn->geometry().left() - reg->titleLabel->geometry().right() <= 12, "right beside the words");
        QVERIFY(qAbs(reg->renameBtn->geometry().top() - reg->titleLabel->geometry().top()) <= 12);
        QScrollBar *bar = reg->findChild<QScrollArea *>()->verticalScrollBar();
        QVERIFY(bar->maximum() > 200);
        bar->setValue(120);
        const QByteArray untouched = databaseFingerprint();

        // cancelling, however it is done, changes nothing
        QString prefill, refusal;
        for (const QString &way : {QString("Cancel"), QString("<esc>")}) {
            answer("Typed But Abandoned", way, &prefill);
            reg->renameBtn->click();
            settle();
            QCOMPARE(prefill, QString("Old Pending Name|Old Pending Name|Edit tournament name"));    // filled in and selected
            QCOMPARE(databaseFingerprint(), untouched);
            QVERIFY(reg->titleLabel->text().remove(QChar(0x200B)).endsWith("Old Pending Name"));
        }
        // a blank name is refused, with a message, and nothing is saved
        for (const QString &blank : {QString(""), QString("     ")}) {
            refusal.clear();
            answer(blank, "Save", nullptr, &refusal);
            reg->renameBtn->click();
            settle();
            QCOMPARE(refusal, QString("Enter a name for the tournament."));
            QCOMPARE(databaseFingerprint(), untouched);
        }
        // Enter saves; spaces around the name are removed
        answer("   Spring Open 2026  ", "<enter>");
        reg->renameBtn->click();
        settle();
        QCOMPARE(tdb::tournamentById(pending)["name"].toString(), QString("Spring Open 2026"));
        QCOMPARE(w_->currentScreen(), reg);                             // the same page, not a reloaded one
        QCOMPARE(reg->titleLabel->text().remove(QChar(0x200B)), QStringLiteral("Player Registration — Spring Open 2026"));
        QCOMPARE(bar->value(), 120);                                    // and it did not move
        QCOMPARE(tdb::enrolledPlayers(pending).size(), 30);
        QCOMPARE(db::value("SELECT COUNT(*) FROM tournaments").toInt(), 1);
        // the name is used from here on: starting it asks about, and opens, "Spring Open 2026"
        QCOMPARE(reg->tName, QString("Spring Open 2026"));

        // 2. while it is running: the heading and its pill in the top bar
        swiss::startTournament(pending);
        const qint64 rid = tdb::currentRound(pending)["round_id"].toLongLong();
        timerdb::start(timerdb::Kind::OneOnOne, rid);
        const db::Rows matches = tdb::allMatches(pending);
        auto *round = go<RoundScreen>("round", {{"tournament_id", pending}});
        QVERIFY(round && round->renameBtn);
        QVERIFY(w_->tabText(pending).startsWith("Spring Open 2026"));
        prefill.clear();
        answer("Summer Open", "Save", &prefill);
        round->renameBtn->click();
        settle();
        QVERIFY(prefill.startsWith("Spring Open 2026|Spring Open 2026|"));
        QCOMPARE(w_->currentScreen(), round);
        QCOMPARE(round->roundLabel->text().remove(QChar(0x200B)), QStringLiteral("Round 1 — Summer Open"));
        QVERIFY2(w_->tabText(pending).startsWith("Summer Open"), qPrintable(w_->tabText(pending)));
        QCOMPARE(w_->tabButton(pending)->text(), w_->tabText(pending));
        QVERIFY(w_->tabButton(pending)->toolTip().startsWith("Summer Open"));
        QVERIFY(w_->tabButton(pending)->accessibleName().startsWith("Summer Open"));
        QCOMPARE(w_->liveTabCount(), 1);
        QCOMPARE(tdb::allMatches(pending), matches);
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, rid).state == timerdb::State::Running);      // the clock was not touched
        QVERIFY(round->findChild<TimerWidget *>());
        // the next printed sheet carries the new name
        QCOMPARE(printing::modernRoundData(tdb::tournamentById(pending), 1, tdb::roundPairings(rid)).event, QString("Summer Open"));
        // and so do the overview and the standings
        QVERIFY(labelText(go<HubScreen>("mtg_hub")).contains("Summer Open"));
        QVERIFY(!shownLabels().contains("Spring Open") && !shownLabels().contains("Old Pending Name"));
        QVERIFY(labelText(go<StandingsScreen>("standings", {{"tournament_id", pending}})).contains("Summer Open"));

        // 3. an older tournament that was ended early: still a read-only record afterwards
        tdb::terminateTournament(pending);
        const QString endedAt = tdb::tournamentById(pending)["terminated_at"].toString();
        go<HubScreen>("mtg_hub");
        round = go<RoundScreen>("round", {{"tournament_id", pending}, {"round_number", 1}});
        QVERIFY(round->renameBtn && round->renameBtn->isVisible() && round->renameBtn->isEnabled());
        answer("Demo Night (ended early)", "<enter>");
        round->renameBtn->click();
        settle();
        QCOMPARE(tdb::tournamentById(pending)["name"].toString(), QString("Demo Night (ended early)"));
        QCOMPARE(tdb::tournamentById(pending)["status"].toString(), tdb::TERMINATED);
        QCOMPARE(tdb::tournamentById(pending)["terminated_at"].toString(), endedAt);
        QVERIFY(round->roundLabel->text().remove(QChar(0x200B)).endsWith("Demo Night (ended early)"));
        QVERIFY(!w_->tabButton(pending));                               // renaming did not make it active again
        QVERIFY(!round->endEarlyBtn && !round->findChild<TimerWidget *>());
        for (T::WrapButton *b : round->findChildren<T::WrapButton *>())
            QVERIFY(!b->isEnabled());                                   // and nothing else became editable
        QCOMPARE(tdb::allMatches(pending), matches);
        // its history page has the pencil too
        auto *select = go<RoundSelectScreen>("round_select", {{"tournament_id", pending}});
        QVERIFY(select && select->renameBtn && select->renameBtn->toolTip() == "Edit tournament name");
        QVERIFY(select->renameBtn->geometry().left() >= select->nameLabel->geometry().right());
        QVERIFY(select->renameBtn->geometry().left() - select->nameLabel->geometry().right() <= 12);
        // the name itself is still centred in the panel
        QVERIFY(qAbs(select->nameLabel->geometry().center().x() - select->panel->rect().center().x()) <= 3);

        // 4. an older completed tournament, from its history page
        const qint64 done = modern({"Ana", "Bo"}, 45, 1, true, "Old Finished Name");
        tdb::reportMatchResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(done, "MTG");
        w_->removeTournamentTab(done);
        const db::Rows finalStandings = swiss::standings(done, "MTG");
        select = go<RoundSelectScreen>("round_select", {{"tournament_id", done}});
        QVERIFY(select && select->renameBtn);
        prefill.clear();
        answer("Demo Final", "Save", &prefill);
        select->renameBtn->click();
        settle();
        QVERIFY(prefill.startsWith("Old Finished Name|Old Finished Name|"));
        QCOMPARE(w_->currentScreen(), select);
        QCOMPARE(select->nameLabel->text().remove(QChar(0x200B)), QString("Demo Final"));
        QCOMPARE(tdb::tournamentById(done)["name"].toString(), QString("Demo Final"));
        QCOMPARE(tdb::tournamentById(done)["status"].toString(), tdb::COMPLETED);
        QCOMPARE(swiss::standings(done, "MTG"), finalStandings);        // placings and results as they were
        QVERIFY(!w_->tabButton(done));
        // pages opened from here use the new name
        buttons(select).value("View final standings")->click();
        settle();
        QVERIFY(shownLabels().contains("Demo Final") && !shownLabels().contains("Old Finished Name"));

        // 5. a Commander event, running and then completed
        const qint64 cmd = commander({"Ed", "Flo", "Gus", "Hal"}, 1, true, 80, "Old Commander Name");
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", cmd}});
        QVERIFY(ev && ev->renameBtn);
        answer("Demo Commander", "<enter>");
        ev->renameBtn->click();
        settle();
        QCOMPARE(w_->currentScreen(), ev);
        QVERIFY(ev->title->text().remove(QChar(0x200B)).endsWith("Demo Commander"));
        QVERIFY(w_->tabText(cmd).startsWith("Demo Commander"));
        QCOMPARE(cdb::getEvent(cmd)["name"].toString(), QString("Demo Commander"));
        QCOMPARE(printing::commanderRoundData("Demo Commander", "Round 1", cdb::getRounds(cmd).last().toMap()).event,
                 QString("Demo Commander"));
        const QVariantMap pod = cdb::getRounds(cmd).last().toMap()["pods"].toList()[0].toMap();
        cdb::reportPodResult(pod["pod_id"].toLongLong(), "WIN", pod["seats"].toList()[0].toMap()["player_id"].toLongLong());
        cdb::finishTournament(cmd);
        ev = go<CommanderEventScreen>("round", {{"tournament_id", cmd}});
        const QString champion = cdb::getState(cmd)["event"].toMap()["champion_name"].toString();
        answer("Demo Commander Final", "Save");
        ev->renameBtn->click();
        settle();
        QVERIFY(ev->title->text().remove(QChar(0x200B)).endsWith("Demo Commander Final"));
        QCOMPARE(cdb::getState(cmd)["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(cdb::getState(cmd)["event"].toMap()["champion_name"].toString(), champion);
        QVERIFY(!w_->tabButton(cmd));

        // long names, both themes, large text and a small window: everything still fits
        const QString longName = "The Extraordinarily Long Annual Invitational Championship Of Absolutely Everything, Part Two";
        QVERIFY(longName.size() <= tdb::MAX_NAME_LENGTH);
        tdb::renameTournament(done, longName);
        for (const QString &mode : {QString("dark"), QString("light")}) {
            for (const QString &textSize : {QString("standard"), QString("large")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(360, 640)}) {
                    w_->resize(size);
                    for (const auto &page : QList<QPair<QString, QVariantMap>>{
                             {"round_select", {{"tournament_id", done}}}, {"round", {{"tournament_id", done}, {"round_number", 1}}},
                             {"round", {{"tournament_id", cmd}}}, {"round", {{"tournament_id", pending}, {"round_number", 1}}}}) {
                        w_->navigateTo(page.first, page.second);
                        settle();
                        const QByteArray where = QStringLiteral("%1 %2 %3 %4x%5").arg(page.first, mode, textSize)
                                                     .arg(size.width()).arg(size.height()).toUtf8();
                        QPushButton *pencil = nullptr;
                        for (QPushButton *b : w_->currentScreen()->findChildren<QPushButton *>())
                            if (b->toolTip() == "Edit tournament name")
                                pencil = b;
                        QVERIFY2(pencil && pencil->isVisible(), where);
                        QVERIFY2(pencil->mapTo(w_.get(), QPoint(pencil->width(), 0)).x() <= w_->width(), where);
                        QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                    }
                }
            }
        }
        // the dialog itself fits a small window
        prefill.clear();
        answer(QString(), "<esc>", &prefill);
        qobject_cast<RoundScreen *>(w_->currentScreen())->renameBtn->click();
        settle();
        QVERIFY(prefill.startsWith("Demo Night (ended early)|"));

        // after a restart every name is the one that was saved
        w_.reset();
        db::closeThreadConnection();
        openWindow();
        QCOMPARE(tdb::tournamentById(pending)["name"].toString(), QString("Demo Night (ended early)"));
        QCOMPARE(tdb::tournamentById(done)["name"].toString(), longName);
        QCOMPARE(tdb::tournamentById(cmd)["name"].toString(), QString("Demo Commander Final"));
        const QString hubText = labelText(go<HubScreen>("mtg_hub"));
        for (const QString &name : {QString("Demo Night (ended early)"), longName, QString("Demo Commander Final")})
            QVERIFY2(hubText.contains(name), qPrintable(name));
        for (const char *old : {"Old Pending Name", "Spring Open", "Summer Open", "Old Finished Name", "Old Commander Name"})
            QVERIFY2(!hubText.contains(old), old);
        QCOMPARE(db::value("SELECT COUNT(*) FROM tournaments").toInt(), 3);      // none was created along the way
    }

    // Player ids, same names, and removing a player.

    void playersHaveIdsThatOnlyShowWhenNamesClash()
    {
        const qint64 a = pdb::addPlayer("Alex Smith"), b = pdb::addPlayer("alex  SMITH");
        const qint64 jones = pdb::addPlayer("Alex Jones"), jordan = pdb::addPlayer("Jordan Smith");
        const QString la = QStringLiteral("Alex Smith · ") + pdb::formatId(a), lb = QStringLiteral("alex  SMITH · ") + pdb::formatId(b);
        Q_UNUSED(jones);
        Q_UNUSED(jordan);
        const auto listed = [&](PlayersScreen *dir) {
            settle();
            QStringList names;
            for (QFrame *row : dir->findChildren<QFrame *>("pr"))
                if (row->isVisible())
                    names << row->findChild<T::NameScroll *>()->label->text();
            names.sort();
            return names;
        };
        // the directory: ids only on the two records whose whole name is the same
        auto *dir = go<PlayersScreen>("players");
        QCOMPARE(listed(dir), (QStringList{"Alex Jones", la, "Jordan Smith", lb}));
        // searching down to one of them keeps its id: it was decided from the whole directory
        dir->searchBox->setText("alex  S");
        QTest::qWait(400);
        QCOMPARE(listed(dir), QStringList{lb});
        dir->searchBox->setText("Jones");
        QTest::qWait(400);
        QCOMPARE(listed(dir), QStringList{"Alex Jones"});
        dir->searchBox->clear();
        QTest::qWait(400);
        // so does the game filter
        const qint64 cup = tdb::createTournament("Cup", "POKEMON", 1, "Standard");
        tdb::enrollPlayer(cup, a);
        tdb::enrollPlayer(cup, jones);
        swiss::startTournament(cup);
        dir->setGameChecked("POKEMON", true);
        QCOMPARE(listed(dir), (QStringList{"Alex Jones", la}));
        dir->setGameChecked(QString(), true);

        // a profile always shows the id, small and muted, beside the name
        for (qint64 id : {a, jones}) {
            auto *profile = go<PlayerProfileScreen>("player_profile", {{"player_id", id}});
            QVERIFY(profile);
            QLabel *idLabel = profile->findChild<QLabel *>("playerId");
            QVERIFY(idLabel && idLabel->isVisible());
            QCOMPARE(idLabel->text(), pdb::formatId(id));
            QVERIFY(idLabel->styleSheet().contains(T::MUTED));
            QLabel *name = nullptr;
            for (QLabel *l : profile->findChildren<QLabel *>())
                if (l->text().remove(QChar(0x200B)) == pdb::playerById(id)["display_name"].toString())
                    name = l;
            QVERIFY(name);
            QVERIFY(idLabel->font().pixelSize() < name->font().pixelSize() / 1.5);
            QVERIFY(idLabel->geometry().left() >= name->geometry().right());
            QVERIFY(idLabel->geometry().left() - name->geometry().right() <= 16);
        }

        // renaming one of the pair: the id stays, and the labels go as soon as the names differ
        auto *profile = go<PlayerProfileScreen>("player_profile", {{"player_id", b}});
        QTimer::singleShot(200, qApp, [] {
            QWidget *dlg = QApplication::activeModalWidget();
            dlg->findChild<QLineEdit *>()->setText("Alexandra Smith");
            for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                if (btn->text() == "Save")
                    btn->click();
        });
        buttons(profile).value("Edit name")->click();
        settle();
        QCOMPARE(pdb::playerById(b)["display_name"].toString(), QString("Alexandra Smith"));
        QCOMPARE(profile->findChild<QLabel *>("playerId")->text(), pdb::formatId(b));
        QCOMPARE(listed(go<PlayersScreen>("players")), (QStringList{"Alex Jones", "Alex Smith", "Alexandra Smith", "Jordan Smith"}));
        pdb::renamePlayer(b, "alex  SMITH");

        // inside a tournament: pairings, standings and the printed sheet name the two apart; others stay plain
        const qint64 tid = modern({"Sam Lee"}, 45, 1, false, "Twins Cup");
        tdb::enrollPlayer(tid, a);
        tdb::enrollPlayer(tid, b);
        tdb::enrollPlayer(tid, jordan);
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", tid}});
        QString regText = labelText(reg);
        QVERIFY2(regText.contains(la) && regText.contains(lb) && !regText.contains("Jordan Smith ·") && !regText.contains("Sam Lee ·"),
                 qPrintable(regText));
        QVERIFY(buttons(reg).keys().contains("Remove"));
        swiss::startTournament(tid);
        auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
        QStringList onButtons;
        for (T::WrapButton *wb : round->findChildren<T::WrapButton *>())
            onButtons << wb->accessibleName();
        QVERIFY2(onButtons.contains(la) && onButtons.contains(lb) && onButtons.contains("Sam Lee") && onButtons.contains("Jordan Smith"),
                 qPrintable(onButtons.join(", ")));
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        QStringList printed;
        for (const printing::Group &g : printing::modernRoundData(tdb::tournamentById(tid), 1, tdb::roundPairings(rid)).groups)
            for (const printing::Player &pl : g.players)
                printed << pl.name;
        QVERIFY2(printed.contains(la) && printed.contains(lb) && printed.contains("Sam Lee"), qPrintable(printed.join(", ")));
        const QString standingsText = labelText(go<StandingsScreen>("standings", {{"tournament_id", tid}}));
        QVERIFY2(standingsText.contains(la) && standingsText.contains(lb) && !standingsText.contains("Sam Lee ·"), qPrintable(standingsText));
        for (const QSize &size : {QSize(1180, 780), QSize(360, 640)}) {
            w_->resize(size);
            for (const QString &screen : {QString("round"), QString("standings")}) {
                w_->navigateTo(screen, {{"tournament_id", tid}});
                settle();
                QVERIFY2(layoutProblems().isEmpty(), qPrintable(screen + " " + layoutProblems().join("; ")));
            }
        }
    }

    void addingANameThatExistsAsksWhichPlayerIsMeant()
    {
        const qint64 a = pdb::addPlayer("Alex Smith"), b = pdb::addPlayer("Alex Smith");
        pdb::addPlayer("Alex Jones");
        const qint64 gone = pdb::addPlayer("Robin Fox");
        pdb::removePlayer(gone);
        // Presses a button of the next dialog (by label, or the first whose label starts with it) and notes what it offered.
        const auto pick = [](const QString &label, QStringList *offered = nullptr) {
            QTimer::singleShot(250, qApp, [=] {
                QWidget *dlg = QApplication::activeModalWidget();
                if (!dlg)
                    return;
                if (offered) {
                    *offered << dlg->windowTitle();
                    for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                        if (btn->isVisible())
                            *offered << (btn->text().isEmpty() ? btn->accessibleName() : btn->text());
                }
                for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                    if (const QString name = btn->text().isEmpty() ? btn->accessibleName() : btn->text();
                        name == label || (!label.isEmpty() && name.startsWith(label))) {
                        btn->click();
                        return;
                    }
                QTest::keyClick(dlg, Qt::Key_Escape);
            });
        };
        // typing the name into the "New player" prompt, then answering the question that follows
        const auto addInDirectory = [&](PlayersScreen *dir, const QString &typed, const QString &answer, QStringList *offered) {
            QTimer::singleShot(150, qApp, [typed] {
                QWidget *dlg = QApplication::activeModalWidget();
                dlg->findChild<QLineEdit *>()->setText(typed);
                for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                    if (btn->text() == "Add player")
                        btn->click();
            });
            QTimer::singleShot(600, qApp, [answer, offered] {
                QWidget *dlg = QApplication::activeModalWidget();
                if (!dlg)
                    return;         // no question was asked
                *offered << dlg->windowTitle();
                for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                    if (btn->isVisible())
                        *offered << (btn->text().isEmpty() ? btn->accessibleName() : btn->text());
                for (QPushButton *btn : dlg->findChildren<QPushButton *>())
                    if (const QString name = btn->text().isEmpty() ? btn->accessibleName() : btn->text();
                        name == answer || (!answer.isEmpty() && name.startsWith(answer))) {
                        btn->click();
                        return;
                    }
                QTest::keyClick(dlg, Qt::Key_Escape);
            });
            buttons(dir).value("New player")->click();
            QTest::qWait(700);
            settle();
        };

        // in the directory
        auto *dir = go<PlayersScreen>("players");
        QStringList offered;
        addInDirectory(dir, "  alex   smith ", "Cancel", &offered);
        QCOMPARE(offered.value(0), QString("A player with this name already exists"));
        QVERIFY2(offered.contains(QStringLiteral("Open Alex Smith · ") + pdb::formatId(a))
                 && offered.contains(QStringLiteral("Open Alex Smith · ") + pdb::formatId(b))
                 && offered.contains("Create a different player with this name") && offered.contains("Cancel"),
                 qPrintable(offered.join(" | ")));
        QCOMPARE(pdb::allPlayers().size(), 3);                  // cancelled: nobody was added
        offered.clear();
        addInDirectory(qobject_cast<PlayersScreen *>(w_->currentScreen()), "Alex Smith", "Create a different", &offered);
        QCOMPARE(pdb::allPlayers().size(), 4);                  // a third, separate Alex Smith
        QCOMPARE(pdb::playersNamed("Alex Smith").size(), 3);
        offered.clear();
        addInDirectory(qobject_cast<PlayersScreen *>(w_->currentScreen()), "Alex Smith",
                       QStringLiteral("Open Alex Smith · ") + pdb::formatId(b), &offered);
        QCOMPARE(pdb::allPlayers().size(), 4);                  // picking an existing player adds nobody
        auto *profile = qobject_cast<PlayerProfileScreen *>(w_->currentScreen());
        QVERIFY(profile && profile->findChild<QLabel *>("playerId")->text() == pdb::formatId(b));
        // a name nobody has, a shared first name only, and the name of a removed player: no question
        dir = go<PlayersScreen>("players");
        for (const QString &fresh : {QString("Casey Park"), QString("Alex Brown"), QString("Robin Fox")}) {
            offered.clear();
            addInDirectory(qobject_cast<PlayersScreen *>(w_->currentScreen()), fresh, "Cancel", &offered);
            QVERIFY2(offered.isEmpty(), qPrintable(fresh + ": " + offered.join(" | ")));
        }
        QCOMPARE(pdb::allPlayers().size(), 7);
        const qint64 newRobin = pdb::playersNamed("Robin Fox").first()["player_id"].toLongLong();
        QVERIFY(newRobin != gone && pdb::isRemoved(gone));      // the removed record was not brought back or reused

        // on a registration page
        const qint64 tid = modern({}, 45, 1, false, "Sign-up");
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", tid}});
        const auto addAndEnroll = [&](const QString &typed, const QString &answer, QStringList *seen) {
            reg->newNameBox->setText(typed);
            pick(answer, seen);
            reg->addBtn->click();
            QTest::qWait(350);
            settle();
        };
        offered.clear();
        const int before = int(pdb::allPlayers().size());
        addAndEnroll("Alex Smith", "Cancel", &offered);
        QVERIFY2(offered.contains(QStringLiteral("Enroll Alex Smith · ") + pdb::formatId(a)), qPrintable(offered.join(" | ")));
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 0);
        addAndEnroll("Alex Smith", QStringLiteral("Enroll Alex Smith · ") + pdb::formatId(a), nullptr);
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        QCOMPARE(tdb::enrolledPlayers(tid).first()["player_id"].toLongLong(), a);
        QCOMPARE(int(pdb::allPlayers().size()), before);        // the existing player was enrolled; nobody was created
        // the same player cannot be enrolled twice
        addAndEnroll("Alex Smith", QStringLiteral("Enroll Alex Smith · ") + pdb::formatId(a), nullptr);
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        // "a different player" makes and enrolls a new one
        addAndEnroll("Alex Smith", "Create a different", nullptr);
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 2);
        QCOMPARE(int(pdb::allPlayers().size()), before + 1);
        // searching offers both namesakes with their ids, and never the removed player
        reg->searchBox->setText("Alex Smith");
        QTest::qWait(500);
        const QString found = labelText(reg);
        QVERIFY2(found.contains(QStringLiteral("Alex Smith · ") + pdb::formatId(a))
                 && found.contains(QStringLiteral("Alex Smith · ") + pdb::formatId(b)), qPrintable(found));
        pdb::removePlayer(pdb::addPlayer("Vanished Vera"));
        reg->searchBox->setText("Vanished");
        QTest::qWait(500);
        QVERIFY(!labelText(reg).contains("Vanished Vera"));
    }

    void removingAPlayerHidesTheirProfileAndLeavesHistoryAlone()
    {
        // a finished tournament the player took part in, and a running one they are still in
        const qint64 done = modern({"Dana Cruz", "Eli Ross", "Finn Vale"}, 45, 1, true, "Finished Cup");
        const qint64 dana = pdb::playersNamed("Dana Cruz").first()["player_id"].toLongLong();
        const qint64 rid = tdb::currentRound(done)["round_id"].toLongLong();
        for (const db::Row &m : tdb::roundPairings(rid))
            if (!m["player2_id"].isNull())
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(done, "MTG");
        w_->removeTournamentTab(done);
        const qint64 live = tdb::createTournament("Still Running", "MTG", 2, "Modern");
        tdb::enrollPlayer(live, dana);
        tdb::enrollPlayer(live, pdb::playersNamed("Eli Ross").first()["player_id"].toLongLong());
        swiss::startTournament(live);
        const auto standingsOf = [&] {
            QStringList rows;
            for (const db::Row &r : swiss::viewStandings(done, "MTG"))
                rows << QStringLiteral("%1 %2 %3 %4").arg(r["standing"].toInt()).arg(r["display_name"].toString())
                            .arg(r["match_points"].toInt()).arg(r["omw_pct"].toDouble());
            return rows;
        };
        const QStringList standings = standingsOf();
        const db::Rows matches = tdb::allMatches(done);
        const QByteArray untouched = databaseFingerprint();

        auto *profile = go<PlayerProfileScreen>("player_profile", {{"player_id", dana}});
        QVERIFY(profile);
        QVERIFY(buttons(profile).contains("Remove player") && !buttons(profile).contains("Delete player"));
        // cancelling changes nothing, whichever way
        QString seen;
        for (const QString &way : {QString("Cancel"), QString("<esc>"), QString("<enter>")}) {
            seen.clear();
            answerNextDialog(way, &seen);
            buttons(profile).value("Remove player")->click();
            settle();
            QCOMPARE(databaseFingerprint(), untouched);
            QCOMPARE(w_->currentScreen(), profile);
        }
        QVERIFY2(seen.contains("Remove Dana Cruz from the player directory?")
                 && seen.contains("They will no longer appear in player searches or be available for new enrollment. Their name and "
                                  "existing tournament results will remain in history, but their profile will no longer be accessible."),
                 qPrintable(seen));
        QVERIFY(!seen.contains("#"));                           // nobody shares the name, so no id is needed

        // still in a tournament being played: explained, and nothing changes there or here
        setAutoConfirm(false);
        QString refusal;
        QTimer::singleShot(200, qApp, [] {
            for (QPushButton *btn : QApplication::activeModalWidget()->findChildren<QPushButton *>())
                if (btn->text() == "Remove player")
                    btn->click();
        });
        QTimer::singleShot(700, qApp, [&refusal] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                refusal = box->windowTitle() + " | " + box->text();
                box->accept();
            }
        });
        buttons(profile).value("Remove player")->click();
        QTest::qWait(800);
        setAutoConfirm(true);
        QVERIFY2(refusal.contains("Player not removed") && refusal.contains("Still Running") && refusal.contains("not reported yet"),
                 qPrintable(refusal));
        QCOMPARE(databaseFingerprint(), untouched);
        QVERIFY(!pdb::isRemoved(dana));
        QCOMPARE(tdb::enrolledPlayers(live).size(), 2);         // not quietly taken out of the event

        // once that tournament is over, the removal goes through
        tdb::terminateTournament(live);
        w_->removeTournamentTab(live);
        profile = go<PlayerProfileScreen>("player_profile", {{"player_id", dana}});
        answerNextDialog("Remove player");
        buttons(profile).value("Remove player")->click();
        settle();
        QVERIFY(pdb::isRemoved(dana));
        auto *dir = qobject_cast<PlayersScreen *>(w_->currentScreen());
        QVERIFY(dir);
        QVERIFY(!labelText(dir).contains("Dana Cruz"));
        QVERIFY(w_->toast->isVisible() && w_->toastLabel->text().remove(QChar(0x200B)).contains("removed from the player directory"));
        // their profile cannot be reached any more, directly or with Back
        QVERIFY(!go<PlayerProfileScreen>("player_profile", {{"player_id", dana}}));
        QVERIFY(qobject_cast<PlayersScreen *>(w_->currentScreen()));
        w_->goBack();
        settle();
        QVERIFY(!qobject_cast<PlayerProfileScreen *>(w_->currentScreen()));

        // history is exactly as it was: same matches, same standings, the name still shown, as plain text
        QCOMPARE(tdb::allMatches(done), matches);
        QCOMPARE(standingsOf(), standings);
        auto *table = go<StandingsScreen>("standings", {{"tournament_id", done}});
        QVERIFY(labelText(table).contains("Dana Cruz"));
        QCOMPARE(standingsOf(), standings);                     // looking at it changed nothing
        auto *round = go<RoundScreen>("round", {{"tournament_id", done}, {"round_number", 1}});
        QStringList onButtons;
        for (T::WrapButton *wb : round->findChildren<T::WrapButton *>())
            onButtons << wb->accessibleName();
        QVERIFY2(onButtons.contains("Dana Cruz") || labelText(round).contains("Dana Cruz"), qPrintable(onButtons.join(", ")));
        for (QWidget *screen : {static_cast<QWidget *>(table), static_cast<QWidget *>(round)})
            for (QPushButton *btn : screen->findChildren<QPushButton *>())
                QVERIFY2(!btn->accessibleName().contains("profile", Qt::CaseInsensitive) && !btn->text().contains("profile", Qt::CaseInsensitive),
                         qPrintable(btn->text()));                // no link to a profile anywhere in a tournament view
        // an opponent's own history still has the match against them
        const qint64 eli = pdb::playersNamed("Eli Ross").first()["player_id"].toLongLong();
        bool mentioned = false;
        for (qint64 other : {eli, pdb::playersNamed("Finn Vale").first()["player_id"].toLongLong()})
            for (const db::Row &m : pdb::matchHistory(other, done))
                mentioned = mentioned || m["opponent_name"].toString() == "Dana Cruz";
        bool hadBye = false;
        for (const db::Row &m : pdb::matchHistory(dana, done))
            hadBye = hadBye || m["result"].toString() == "BYE";
        QVERIFY(mentioned || hadBye);

        // they cannot be found or enrolled for something new
        const qint64 fresh = modern({}, 45, 1, false, "Next Cup");
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", fresh}});
        reg->searchBox->setText("Dana");
        QTest::qWait(500);
        QVERIFY(!labelText(reg).contains("Dana Cruz"));
        // a new Dana Cruz is a new person: no question about the removed one, a new id, no history
        reg->newNameBox->setText("Dana Cruz");
        reg->addBtn->click();
        QTest::qWait(300);
        settle();
        QCOMPARE(tdb::enrolledPlayers(fresh).size(), 1);
        const qint64 newDana = tdb::enrolledPlayers(fresh).first()["player_id"].toLongLong();
        QVERIFY(newDana > dana);
        QVERIFY(pdb::tournamentHistory(newDana).size() == 1 && pdb::lifetimeStats(newDana).isEmpty());
        QCOMPARE(standingsOf(), standings);

        // after a restart: still removed, still in history
        w_.reset();
        db::closeThreadConnection();
        openWindow();
        QVERIFY(pdb::isRemoved(dana));
        dir = go<PlayersScreen>("players");
        int danas = 0;
        for (QFrame *row : dir->findChildren<QFrame *>("pr"))
            if (row->isVisible() && row->findChild<T::NameScroll *>()->label->text().startsWith("Dana Cruz"))
                ++danas;
        QCOMPARE(danas, 1);                                     // only the new one
        QVERIFY(labelText(go<StandingsScreen>("standings", {{"tournament_id", done}})).contains("Dana Cruz"));
        QCOMPARE(standingsOf(), standings);

        // the dialog names the id when another player has the same name
        const qint64 twin = pdb::addPlayer("Dana Cruz");
        profile = go<PlayerProfileScreen>("player_profile", {{"player_id", twin}});
        seen.clear();
        answerNextDialog("Cancel", &seen);
        buttons(profile).value("Remove player")->click();
        settle();
        QVERIFY2(seen.contains(QStringLiteral("Remove Dana Cruz · %1 from the player directory?").arg(pdb::formatId(twin))), qPrintable(seen));
        // everything fits in both themes, at Large text and in a narrow window
        for (const QString &mode : {QString("dark"), QString("light")}) {
            prefs::setPref("theme", mode);
            prefs::setPref("text_size", mode == "light" ? "large" : "standard");
            w_->restyle();
            for (const QSize &size : {QSize(1180, 780), QSize(360, 640)}) {
                w_->resize(size);
                for (const auto &page : QList<QPair<QString, QVariantMap>>{{"players", {}}, {"player_profile", {{"player_id", twin}}},
                                                                            {"standings", {{"tournament_id", done}}}}) {
                    w_->navigateTo(page.first, page.second);
                    settle();
                    QVERIFY2(layoutProblems().isEmpty(), qPrintable(page.first + " " + mode + " " + layoutProblems().join("; ")));
                }
            }
        }
    }

    // Clock colours.

    void clocksAreGreenWhileCountingDownRedAtZeroAndNeutralOtherwise()
    {
        const qint64 tid = modern({"Ana", "Bo", "Cy", "Di"}, 45, 3, true, "Colour Cup");
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        const timerdb::Kind kind = timerdb::Kind::OneOnOne;
        // the clock on the hub's tournament row: the time, its caption and its icon
        const auto hubClock = [&](QString *time, QString *caption) {
            HubScreen *hub = go<HubScreen>("mtg_hub");
            static const QRegularExpression digits("^\\d\\d:\\d\\d$");
            QLabel *value = nullptr, *text = nullptr;
            for (QLabel *l : hub->findChildren<QLabel *>()) {
                if (digits.match(l->text()).hasMatch())
                    value = l;
                if (l->text() == "remaining" || l->text() == "Time expired" || l->text().startsWith("clock "))
                    text = l;
            }
            *time = value ? value->text() + " " + value->styleSheet() : QString();
            *caption = text ? text->text() + " " + text->styleSheet() : QString();
        };
        for (const QString &mode : {QString("dark"), QString("light")}) {
            prefs::setPref("theme", mode);
            w_->restyle();
            const QByteArray where = mode.toUtf8();
            QVERIFY2(QColor(T::GREEN) != QColor(T::RED) && QColor(T::GREEN).green() > QColor(T::GREEN).red(), where);
            QVERIFY2(QColor(T::RED).red() > QColor(T::RED).green(), where);
            timerdb::reset(kind, rid);
            QString time, caption;

            // not started: neutral everywhere
            auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
            TimerWidget *clock = round->findChild<TimerWidget *>();
            QVERIFY2(clock, where);
            QVERIFY2(!clock->display->styleSheet().contains(T::GREEN) && !clock->display->styleSheet().contains(T::RED), where);
            QVERIFY2(w_->tabClockColor(tid).isEmpty() && w_->tabText(tid).endsWith("Not started"), where);
            hubClock(&time, &caption);
            QVERIFY2(!time.contains(T::GREEN) && !time.contains(T::RED) && caption.startsWith("clock not started"), qPrintable(time + caption));

            // counting down: green, with the words that say so
            timerdb::start(kind, rid);
            round = go<RoundScreen>("round", {{"tournament_id", tid}});
            clock = round->findChild<TimerWidget *>();
            QVERIFY2(clock->display->styleSheet().contains("color:" + T::GREEN), where);
            QVERIFY2(clock->caption->text().contains("RUNNING"), where);
            QVERIFY2(!clock->clockIcon->pixmap().isNull(), where);
            QCOMPARE(w_->tabClockColor(tid), T::GREEN);
            QVERIFY2(w_->tabText(tid).endsWith("remaining"), where);
            // the tournament's name and round on the pill keep the pill's usual colours
            QVERIFY2(!w_->tabButton(tid)->styleSheet().contains(T::GREEN) && !w_->tabButton(tid)->styleSheet().contains(T::RED), where);
            hubClock(&time, &caption);
            QVERIFY2(time.contains("color:" + T::GREEN) && caption.startsWith("remaining") && caption.contains(T::GREEN), qPrintable(time + caption));

            // paused: neutral again, and it says "paused"
            timerdb::pause(kind, rid);
            round = go<RoundScreen>("round", {{"tournament_id", tid}});
            clock = round->findChild<TimerWidget *>();
            QVERIFY2(!clock->display->styleSheet().contains(T::GREEN) && !clock->display->styleSheet().contains(T::RED), where);
            QVERIFY2(clock->caption->text().contains("PAUSED"), where);
            QVERIFY2(w_->tabClockColor(tid).isEmpty() && w_->tabText(tid).contains("Paused at"), where);
            hubClock(&time, &caption);
            QVERIFY2(!time.contains(T::GREEN) && !time.contains(T::RED) && caption.startsWith("clock paused"), qPrintable(time + caption));

            // time up: red everywhere, 00:00 where a number is shown, and nothing else changes
            timerdb::start(kind, rid);
            db::exec("UPDATE rounds SET timer_started_at = timer_started_at - 99999 WHERE round_id = ?", {rid});
            round = go<RoundScreen>("round", {{"tournament_id", tid}});
            clock = round->findChild<TimerWidget *>();
            QCOMPARE(clock->display->text(), QString("Time expired"));
            QVERIFY2(clock->display->styleSheet().contains("color:" + T::RED), where);
            QCOMPARE(w_->tabClockColor(tid), T::RED);
            QVERIFY2(w_->tabText(tid).endsWith("Time expired"), where);
            QVERIFY2(w_->tabText(tid).startsWith("Colour Cup") && w_->tabText(tid).contains("Round 1 of 3"), where);
            hubClock(&time, &caption);
            QVERIFY2(time.startsWith("00:00") && time.contains("color:" + T::RED), qPrintable(time));
            QVERIFY2(caption.startsWith("Time expired") && caption.contains(T::RED), qPrintable(caption));
            QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);     // expiry ends nothing
            QCOMPARE(tdb::rounds(tid).size(), 1);
            QCOMPARE(tdb::pendingMatchCount(rid), 2);
        }
        // a saved, running clock is still running (and green) after a restart
        timerdb::reset(kind, rid);
        timerdb::start(kind, rid);
        w_.reset();
        db::closeThreadConnection();
        openWindow();
        QCOMPARE(w_->tabClockColor(tid), T::GREEN);
        // once the tournament is ended, nothing counts down anywhere
        tdb::terminateTournament(tid);
        QString time, caption;
        hubClock(&time, &caption);
        QVERIFY(time.isEmpty() && caption.isEmpty());
        QVERIFY(!w_->tabButton(tid));
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
