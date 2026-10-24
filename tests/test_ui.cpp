// Tests that drive the real screens: workflows, settings, printing, the round
// clock, and a layout sweep over window sizes, themes and text sizes.
#include "applog.h"
#include "commander.h"
#include "commander_db.h"
#include "dialogs.h"
#include "main_window.h"
#include "printing.h"
#include "round_status.h"
#include "screens.h"
#include "store.h"
#include "theme.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QListView>
#include <QListWidget>
#include <QStyleOptionSpinBox>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextDocumentFragment>
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

// the first visible button whose text starts with these words, or null
QPushButton *buttonStarting(QWidget *w, const QString &start)
{
    settle();
    for (QPushButton *b : w->findChildren<QPushButton *>())
        if (b->isVisible() && b->text().startsWith(start))
            return b;
    return nullptr;
}

// the visible button with this accessible name, or null
QPushButton *buttonNamed(QWidget *w, const QString &name)
{
    settle();
    for (QPushButton *b : w->findChildren<QPushButton *>())
        if (b->isVisible() && b->accessibleName() == name)
            return b;
    return nullptr;
}

// the final's editor on the page now; a rebuilt page may still hold a hidden one being deleted
FinalEditor *shownEditor(QWidget *w)
{
    for (FinalEditor *e : w->findChildren<FinalEditor *>())
        if (e->isVisible())
            return e;
    return nullptr;
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
                     const QString &name = "Friday Commander", bool topFourFinal = false)
    {
        const qint64 tid = cdb::createCommanderTournament(name, int(names.size()), rounds, {}, 5, {}, minutes, topFourFinal);
        for (qint64 p : players(names))
            tdb::enrollPlayer(tid, p);
        if (start)
            cdb::startEvent(tid);
        return tid;
    }

    // plays the only qualifying round (winner seat 1, no clear second) and creates the final; returns the seeds
    QList<qint64> playToFinal(qint64 tid)
    {
        for (const QVariant &pv : cdb::getRounds(tid).last().toMap()["pods"].toList()) {
            const QVariantList seats = pv.toMap()["seats"].toList();
            cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN", seats[0].toMap()["player_id"].toLongLong(), {},
                                 cdb::NONE, cdb::SecondPlace::noClearSecond());
        }
        cdb::finalizeRound(cdb::getRounds(tid).last().toMap()["round_id"].toLongLong());
        QList<qint64> seeds;
        for (const QVariant &c : cdb::finalCandidates(tid)["players"].toList())
            seeds << c.toMap()["player_id"].toLongLong();
        cdb::createFinal(tid, seeds);
        return seeds;
    }

    // rewrites a Commander event that has not started to the settings an earlier version saved
    void earlierRules(qint64 tid)
    {
        QVariantMap settings = cdb::getEvent(tid)["settings"].toMap();
        settings.remove("second_place_points");
        settings["rules_version"] = "commander-rules-3";
        db::exec("UPDATE commander_events SET rules_version = 'commander-rules-3', settings_json = ? WHERE tournament_id = ?",
                 {QString::fromUtf8(QJsonDocument::fromVariant(settings).toJson(QJsonDocument::Compact)), tid});
    }

    // types into a search box and waits until that search has run and its rows are shown
    template <typename S> void search(S *screen, const QString &text)
    {
        screen->searchBox->setText(text);
        QTRY_VERIFY_WITH_TIMEOUT(!screen->searchPending(), 10000);
        settle();
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
        applog::setDirectory(QString());
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

        // shown: the seconds on a pill's clock, -1 if it has none.  agrees: that reading fits a
        // clock that had `left` seconds to go and has run between beforeMs and nowMs since
        static const QRegularExpression clock(QStringLiteral("(\\d+):(\\d\\d)"));
        const auto shown = [&](qint64 tid) {
            const QRegularExpressionMatch m = clock.match(w_->tabText(tid));
            return m.hasMatch() ? m.captured(1).toInt() * 60 + m.captured(2).toInt() : -1;
        };
        const auto agrees = [](int seconds, int left, qint64 beforeMs, qint64 nowMs) {
            return seconds >= left - int(nowMs / 1000) - 1 && seconds <= left - int(beforeMs / 1000) + 1;
        };

        QElapsedTimer sinceA;
        sinceA.start();
        timerdb::start(timerdb::Kind::OneOnOne, ra);
        db::exec("UPDATE rounds SET timer_started_at = timer_started_at - 1242 WHERE round_id = ?", {ra});   // 20:42 gone
        const int leftA = 45 * 60 - 1242;                       // 24:18
        qint64 before = sinceA.elapsed();
        go<HomeScreen>("home");                                 // navigating re-reads the saved clocks
        QVERIFY2(w_->tabText(a).endsWith("remaining") && agrees(shown(a), leftA, before, sinceA.elapsed()), qPrintable(w_->tabText(a)));
        QCOMPARE(w_->tabButton(a)->text(), w_->tabText(a));     // what is on screen is that text
        QVERIFY2(w_->tabText(b).endsWith("Not started"), "each tournament has its own clock");

        // navigating, resizing and restarting do not reset it
        go<PlayersScreen>("players");
        w_->resize(700, 500);
        settle();
        before = sinceA.elapsed();
        w_.reset();
        openWindow();
        QVERIFY2(w_->tabText(a).endsWith("remaining") && agrees(shown(a), leftA, before, sinceA.elapsed()), qPrintable(w_->tabText(a)));

        const qint64 pausedFrom = sinceA.elapsed();
        timerdb::pause(timerdb::Kind::OneOnOne, ra);
        const qint64 pausedBy = sinceA.elapsed();
        QElapsedTimer sinceB;
        sinceB.start();
        timerdb::start(timerdb::Kind::Commander, rb);
        before = sinceB.elapsed();
        go<HomeScreen>("home");
        QVERIFY2(w_->tabText(a).contains("Paused at ") && agrees(shown(a), leftA, pausedFrom, pausedBy), qPrintable(w_->tabText(a)));
        QVERIFY2(w_->tabText(b).endsWith("remaining") && agrees(shown(b), 80 * 60, before, sinceB.elapsed()), qPrintable(w_->tabText(b)));
        // a paused clock stays where it was however long the page is open
        const int pausedAt = shown(a);
        QTest::qWait(1200);
        go<HomeScreen>("home");
        QCOMPARE(shown(a), pausedAt);

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

        // saved placings are shown as saved, also when a number is missing (older data: 1, 2, 4, 5)
        db::exec("UPDATE enrollments SET final_placement = final_placement + 1 WHERE tournament_id = ? AND final_placement >= 3", {tid});
        auto *saved = go<StandingsScreen>("standings", {{"tournament_id", tid}});
        QVERIFY(saved);
        QCOMPARE(saved->standings.last()["standing"].toInt(), 5);
        QVERIFY(labelText(saved).contains("5th") && !labelText(saved).contains("3rd"));
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
            // the winners alone leave second place to decide: the points are marked provisional and the round waits
            QVERIFY2(!buttons(ev).value(closing)->isEnabled(), "not until every second place is decided");
            QVERIFY(labelText(ev).contains("Points are provisional"));
            QVERIFY(labelText(ev).contains("2nd place not recorded yet"));
            const QVariantList reported = ev->state["rounds"].toList().last().toMap()["pods"].toList();
            for (int i = 0; i < reported.size(); ++i) {
                const QVariantMap pod = reported[i].toMap();
                const QVariantList seats = pod["seats"].toList();
                ev->report(pod, "WIN", seats[0].toMap()["player_id"].toLongLong(), {},
                           i % 2 ? cdb::SecondPlace::noClearSecond() : cdb::SecondPlace(seats[1].toMap()["player_id"].toLongLong()));
            }
            QVERIFY(!labelText(ev).contains("Points are provisional"));
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
                     && !label.startsWith("Winner"), qPrintable(label));
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
                                     pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong(), {}, cdb::NONE,
                                     cdb::SecondPlace::noClearSecond());
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
        QVERIFY(!buttonStarting(ev, "Winner") && !shown.contains(QStringLiteral("Start Round 3")));
        shown.value(QStringLiteral("Final pod · earlier version"))->click();
        shown = buttons(ev);
        QVERIFY(labelText(ev).contains(QStringLiteral("Read-only record · not counted")));
        QVERIFY2(!buttonStarting(ev, "Winner") && !shown.contains(QStringLiteral("Correct result…")), "the old final cannot be played");
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
                QAbstractItemView *view = format->view();
                QWidget *popup = view->window();
                QTRY_VERIFY2_WITH_TIMEOUT(popup->isVisible(), where, 10000);
                QVERIFY2(popup != w_.get(), where);
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
                QTRY_VERIFY2_WITH_TIMEOUT(!popup->isVisible(), where, 10000);
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
        search(dir, "Newc");
        QStringList visibleNames;
        for (QFrame *row : dir->findChildren<QFrame *>("pr"))
            if (row->isVisible())
                visibleNames << row->findChild<T::NameScroll *>()->label->text();
        QCOMPARE(visibleNames, QStringList{"Newcomer"});        // search still works
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
                if (b->isVisible() && b->text().startsWith("Winner") && b->visibleRegion().contains(b->rect().center()))
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

        // draws: a short label, one sentence, and the default for a new event; draw points are never split
        QCOMPARE(reg->drawCombo->currentData().toString(), QString("ELIMINATED_LOSE"));
        QCOMPARE(reg->drawCombo->currentText(), QString("Knocked-out players lose"));
        QCOMPARE(reg->drawHint->text(), QString("Players still in draw; knocked-out players lose."));
        QVERIFY(reg->drawCombo->toolTip().contains("never split"));
        reg->drawCombo->setCurrentIndex(1);
        QCOMPARE(reg->drawHint->text(), QString("Every seated player draws, even those knocked out."));
        // rounds: the suggestion beside them, with the number of players it is for
        reg->roundsSpin->setValue(4);
        QCOMPARE(reg->roundsHint->text(), QStringLiteral("Suggested for 7 checked in: %1 rounds.\nSet by you: check-ins leave it alone.")
                                              .arg(cdb::recommendation(7)["rounds"].toInt()));
        for (QLabel *hint : {reg->roundsHint, reg->drawHint, reg->shortHint}) {
            for (const QString &line : hint->text().remove(QChar(0x200B)).split('\n')) {
                const int n = int(line.split(' ', Qt::SkipEmptyParts).size());
                QVERIFY2(n >= 5 && n <= 10, qPrintable(line));
            }
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
                    swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
            if (r == 1)
                swiss::advanceToNextRound(mod, 1);
        }
        swiss::finalizeTournament(mod, "MTG");
        const qint64 cmd = commander(names.mid(0, 11), 1);
        for (const QVariant &pv : cdb::getRounds(cmd).last().toMap()["pods"].toList())
            cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN", pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong(), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
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
        search(dir, "o");
        QCOMPARE(shown(), (QStringList{"Bo", "Flo"}));
        QCOMPARE(dir->countLabel->text(), QString("2 of 3 players"));       // of the three names with an "o"
        search(dir, "Newc");
        QVERIFY(shown().isEmpty());
        QLabel *empty = dir->findChild<QLabel *>("noPlayers");
        QVERIFY(empty && empty->isVisible());
        const QString emptyText = empty->text().remove(QChar(0x200B));
        QVERIFY2(emptyText.contains("Newc") && emptyText.contains("Magic: The Gathering") && emptyText.contains("All games"),
                 qPrintable(emptyText));
        search(dir, QString());
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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

    // an event created with earlier rules (commander-rules-3): second place is recognition only
    void commanderPodsRecordAnOptionalSecondPlace()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di"}, 2, false);
        earlierRules(tid);
        cdb::startEvent(tid);
        const auto firstPod = [tid] { return cdb::getRounds(tid)[0].toMap()["pods"].toList()[0].toMap(); };
        const auto seatOf = [&](const QString &name) {
            for (const QVariant &s : firstPod()["seats"].toList())
                if (s.toMap()["display_name"].toString() == name)
                    return s.toMap()["player_id"].toLongLong();
            return qint64(0);
        };
        const auto items = [](QComboBox *box) {
            QStringList out;
            for (int i = 0; i < box->count(); ++i)
                out << box->itemText(i);
            return out;
        };

        // the picker: offered only with a winner, never the winner, "Not recorded" by default
        {
            ResultDialog d(w_.get(), firstPod(), "Pod 1 result", "ELIMINATED_LOSE", true);
            QVERIFY(!d.secondCombo->isVisibleTo(&d));
            QCOMPARE(d.secondPlace().playerId, qint64(0));
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->findData(seatOf("Ana")));
            QVERIFY(d.secondCombo->isVisibleTo(&d));
            QCOMPARE(d.secondCombo->accessibleName(), QString("Second place"));
            QCOMPARE(d.secondCombo->currentText(), QString("Not recorded"));
            QCOMPARE(items(d.secondCombo).size(), 4);
            QVERIFY(!items(d.secondCombo).contains("Ana"));
            QStringList hints;
            for (QLabel *l : d.findChildren<QLabel *>())
                if (l->isVisibleTo(&d))
                    hints << l->text();
            QVERIFY2(hints.contains("Optional recognition; no extra points."), qPrintable(hints.join(" | ")));
            d.secondCombo->setCurrentIndex(d.secondCombo->findData(seatOf("Bo")));
            QCOMPARE(d.secondPlace().playerId, seatOf("Bo"));
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->findData(seatOf("Cy")));
            QCOMPARE(d.secondPlace().playerId, seatOf("Bo"));                // still valid for the new winner
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->findData(seatOf("Bo")));
            QCOMPARE(d.secondPlace().playerId, qint64(0));                   // the new winner cannot also be second
            QVERIFY(!items(d.secondCombo).contains("Bo"));
            d.secondCombo->setCurrentIndex(d.secondCombo->findData(seatOf("Di")));
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->count() - 1);
            QCOMPARE(d.outcome(), QString("DRAW"));
            QCOMPARE(d.secondPlace().playerId, qint64(0));                   // a draw has none
            QVERIFY(!d.secondCombo->isVisibleTo(&d));
        }

        // fills in the result dialog the next click opens, once it is up
        const auto inNextResultDialog = [](const std::function<void(ResultDialog *)> &fill) {
            auto *poll = new QTimer(qApp);
            poll->setInterval(50);
            QObject::connect(poll, &QTimer::timeout, qApp, [poll, fill] {
                if (auto *d = qobject_cast<ResultDialog *>(QApplication::activeModalWidget())) {
                    poll->stop();
                    poll->deleteLater();
                    fill(d);
                    d->accept();
                }
            });
            poll->start();
        };

        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(ev);
        const auto seconds = [&] {
            QStringList out;
            for (QLabel *l : w_->currentScreen()->findChildren<QLabel *>())
                if (l->isVisible() && l->text() == "2nd")
                    out << l->accessibleName();
            return out;
        };
        QVERIFY(seconds().isEmpty());
        buttonStarting(ev, "Winner")->click();       // the quick button records no second place
        settle();
        QVERIFY(firstPod()["second_place_player_id"].isNull() || firstPod()["second_place_player_id"].toLongLong() == 0);
        QString winner;
        for (const QVariant &s : firstPod()["seats"].toList())
            if (s.toMap()["result"].toString() == "WIN")
                winner = s.toMap()["display_name"].toString();
        const QString second = winner == "Bo" ? "Cy" : "Bo";
        const QVariantList pointsBefore = cdb::getStandings(tid);

        // a correction: the saved winner stays, and second place is added with a reason
        inNextResultDialog([&](ResultDialog *d) {
            QCOMPARE(d->secondCombo->currentText(), QString("Not recorded"));
            d->secondCombo->setCurrentIndex(d->secondCombo->findText(second));
            d->findChild<QLineEdit *>()->setText("second was left out");
        });
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        buttons(ev)[QStringLiteral("Correct…")]->click();
        QTRY_COMPARE_WITH_TIMEOUT(firstPod()["second_place_player_id"].toLongLong(), seatOf(second), 10000);
        settle();
        QCOMPARE(cdb::getStandings(tid), pointsBefore);             // no points for it
        QCOMPARE(seconds(), QStringList{"Second place in pod 1"});

        // the badge fits in both themes, at both text sizes, in a narrow window
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(360, 480)}) {
                    w_->resize(size);
                    go<CommanderEventScreen>("round", {{"tournament_id", tid}});
                    const QByteArray where = QStringLiteral("%1 %2 %3").arg(textSize, mode).arg(size.width()).toUtf8();
                    QVERIFY2(seconds().size() == 1, where);
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                }
            }
        }
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(1180, 780);

        // the player's profile lists it beside that pod
        auto *player = go<PlayerProfileScreen>("player_profile", {{"player_id", seatOf(second)}});
        QVERIFY(player);
        for (T::ClickFrame *r : player->findChildren<T::ClickFrame *>())
            if (r->accessibleName() == "Friday Commander: show matches")
                QTest::keyClick(r, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(labelText(w_->currentScreen()).contains(QStringLiteral("Pod 1  ·  seat")), 10000);
        const QString profile = labelText(w_->currentScreen());
        QVERIFY2(profile.contains(QStringLiteral("Pod 1  ·  seat")) && profile.contains(QStringLiteral("  ·  2nd")), qPrintable(profile));

        // reopening shows the saved choice; a draw clears it
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        inNextResultDialog([&](ResultDialog *d) {
            QCOMPARE(d->secondCombo->currentText(), second);
            d->outcomeCombo->setCurrentIndex(d->outcomeCombo->count() - 1);
            d->findChild<QLineEdit *>()->setText("it was a draw");
        });
        buttons(ev)[QStringLiteral("Correct…")]->click();
        QTRY_COMPARE_WITH_TIMEOUT(firstPod()["outcome"].toString(), QString("DRAW"), 10000);
        settle();
        QCOMPARE(firstPod()["second_place_player_id"].toLongLong(), qint64(0));
        QVERIFY(seconds().isEmpty());
    }

private:
    // fills in the next modal dialog of the given type once it is up; the filler closes it
    template <typename D> void inNextDialog(const std::function<void(D *)> &fill)
    {
        auto *poll = new QTimer(qApp);
        poll->setInterval(50);
        QObject::connect(poll, &QTimer::timeout, qApp, [poll, fill] {
            if (auto *d = qobject_cast<D *>(QApplication::activeModalWidget())) {
                poll->stop();
                poll->deleteLater();
                fill(d);
                if (d->isVisible())
                    d->reject();        // a check failed inside: never leave the test waiting on it
            }
        });
        poll->start();
    }

    QLabel *labelWithText(QWidget *w, const QString &text)
    {
        for (QLabel *l : w->findChildren<QLabel *>())
            if (l->isVisible() && l->text().remove(QChar(0x200B)) == text)
                return l;
        return nullptr;
    }

private slots:
    void commanderSecondPlaceScoresAndIsMarkedWhilePending()
    {
        const qint64 tid = commander({"Ana", LONG_NAME, "Cy", "Di"}, 2);
        const auto firstPod = [tid] { return cdb::getRounds(tid)[0].toMap()["pods"].toList()[0].toMap(); };
        const auto seatOf = [&](const QString &name) {
            for (const QVariant &s : firstPod()["seats"].toList())
                if (s.toMap()["display_name"].toString() == name)
                    return s.toMap()["player_id"].toLongLong();
            return qint64(0);
        };
        const auto items = [](QComboBox *box) {
            QStringList out;
            for (int i = 0; i < box->count(); ++i)
                out << box->itemText(i);
            return out;
        };
        {
            // the choice of second place, with what it is worth
            ResultDialog d(w_.get(), firstPod(), "Pod 1 result", "ELIMINATED_LOSE", true, false, true);
            QVERIFY(!d.secondCombo->isVisibleTo(&d));
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->findData(seatOf("Ana")));
            QVERIFY(d.secondCombo->isVisibleTo(&d));
            QCOMPARE(items(d.secondCombo).mid(0, 2), (QStringList{"Not recorded yet", "No clear second (eliminated at the same moment)"}));
            QCOMPARE(items(d.secondCombo).size(), 5);
            QVERIFY(!items(d.secondCombo).contains("Ana"));
            QVERIFY(d.secondPlace().kind == cdb::SecondPlace::NotRecorded);
            QStringList hints;
            for (QLabel *l : d.findChildren<QLabel *>())
                if (l->isVisibleTo(&d))
                    hints << l->text();
            QVERIFY2(hints.join(' ').contains("earns 2.5 points and still counts as a loss"), qPrintable(hints.join(" | ")));
            d.secondCombo->setCurrentIndex(d.secondCombo->findData(ResultDialog::NO_CLEAR_SECOND));
            QVERIFY(d.secondPlace().kind == cdb::SecondPlace::NoClearSecond);
            d.secondCombo->setCurrentIndex(d.secondCombo->findData(seatOf(LONG_NAME)));
            QCOMPARE(d.secondPlace().playerId, seatOf(LONG_NAME));
            d.outcomeCombo->setCurrentIndex(d.outcomeCombo->count() - 1);
            QVERIFY(d.secondPlace().kind == cdb::SecondPlace::NotRecorded);              // a draw has none
            // in a finalized round it has to be decided
            ResultDialog fix(w_.get(), firstPod(), "Correct Pod 1", "ELIMINATED_LOSE", false, true, true, true);
            QVERIFY(!items(fix.secondCombo).contains("Not recorded yet"));
        }

        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QPushButton *wins = nullptr;
        for (QPushButton *b : ev->findChildren<QPushButton *>())
            if (b->accessibleName() == "Ana wins pod 1")
                wins = b;
        QVERIFY(wins);
        wins->click();                          // the quick button records the winner only
        settle();
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QCOMPARE(firstPod()["second_place_status"].toString(), QString("PENDING"));
        QVERIFY(labelText(ev).contains("2nd place not recorded yet. Choose 2nd above, or No clear second"));
        QVERIFY(labelText(ev).contains("2nd place still to choose, points provisional"));
        QVERIFY(labelText(ev).contains("Points are provisional. Second place is not recorded yet in pod 1."));
        QPushButton *finalize = buttons(ev).value("Finalize Round 1");
        QVERIFY(finalize && !finalize->isEnabled());
        QVERIFY(finalize->toolTip().contains("Second place is not recorded yet"));

        // recorded on the card itself: no window and no reason, and the keyboard is already there
        QVERIFY(w_->focusWidget() && w_->focusWidget()->accessibleName().endsWith("second in pod 1"));
        QVERIFY(!buttonNamed(ev, "Ana second in pod 1"));                       // never the winner
        QPushButton *pick = buttonNamed(ev, LONG_NAME + " second in pod 1");
        QVERIFY(pick);
        QCOMPARE(pick->text(), QString("2nd  +2.5"));
        pick->click();
        QTRY_COMPARE_WITH_TIMEOUT(firstPod()["second_place_player_id"].toLongLong(), seatOf(LONG_NAME), 10000);
        settle();
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("SECOND_PLACE_RECORDED"));
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QVERIFY(labelWithText(ev, QStringLiteral("2nd  +2.5")));
        QVERIFY(labelText(ev).contains("2.5 pts"));
        QVERIFY(!labelText(ev).contains("Points are provisional"));
        QVERIFY(buttons(ev).value("Finalize Round 1")->isEnabled());
        // saving the same choice again adds nothing
        const int corrections = db::value("SELECT COUNT(*) FROM commander_audit WHERE tournament_id = ?", {tid}).toInt();
        ev->report(firstPod(), "WIN", seatOf("Ana"), {}, seatOf(LONG_NAME));
        ev->report(firstPod(), "WIN", seatOf("Ana"), {}, seatOf(LONG_NAME));
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_audit WHERE tournament_id = ?", {tid}).toInt(), corrections);

        // the marker and the badge fit in both themes, at both text sizes, in a narrow window, with a long name
        const QVariantMap other = firstPod();
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(360, 480)}) {
                    w_->resize(size);
                    go<CommanderEventScreen>("round", {{"tournament_id", tid}});
                    const QByteArray where = QStringLiteral("%1 %2 %3").arg(textSize, mode).arg(size.width()).toUtf8();
                    QVERIFY2(labelWithText(w_->currentScreen(), QStringLiteral("2nd  +2.5")), where);
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                }
            }
        }
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(1180, 780);
        Q_UNUSED(other);
    }

    void topFourFinalRunsThroughTheScreens()
    {
        const QStringList names{"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", LONG_NAME};
        const qint64 tid = commander(names, 1, false, 80, "Final Night");
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        QVERIFY(reg && reg->finalCheck && reg->finalCheck->isVisible());
        QVERIFY(!reg->finalCheck->isChecked());                      // off unless chosen
        QCOMPARE(reg->finalCheck->text(), QString("Add a top-four final"));
        QVERIFY(labelText(reg).contains("clear second place 2.5"));
        reg->finalCheck->setChecked(true);
        settle();
        QVERIFY(cdb::getEvent(tid)["final_enabled"].toBool());
        QVERIFY(labelText(reg).contains("1 qualifying round + 1 final"));
        QVERIFY(labelText(reg).contains("This tournament will be 1 qualifying round, then the top-four final."));
        QVERIFY(layoutProblems().isEmpty());
        reg->confirmStart();
        settle();
        auto *ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QVERIFY(ev);
        QVERIFY(!buttons(ev).contains("Finish tournament"));
        QVERIFY(labelText(ev).contains("Round 1 is the last qualifying round. The top-four final follows."));
        for (const QVariant &pv : cdb::getRounds(tid).last().toMap()["pods"].toList()) {
            const QVariantList seats = pv.toMap()["seats"].toList();
            cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN", seats[0].toMap()["player_id"].toLongLong(), {},
                                 cdb::NONE, seats[1].toMap()["player_id"].toLongLong());
        }
        ev->refresh();
        buttons(ev).value("Finalize Round 1")->click();
        settle();
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("SWISS"));        // not finished: the final follows
        QVERIFY(labelText(ev).contains("TOP-FOUR FINAL"));
        const QVariantList candidates = cdb::finalCandidates(tid)["players"].toList();
        QCOMPARE(candidates.size(), 4);
        for (const QVariant &c : candidates)
            QVERIFY(labelText(ev).contains(c.toMap()["display_name"].toString().left(10)));
        QPushButton *create = buttons(ev).value(QStringLiteral("Create final…"));
        QVERIFY(create);
        create->click();
        settle();
        create = nullptr;
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QVariantMap fin = cdb::getFinal(tid);
        QCOMPARE(fin["status"].toString(), QString("PLAYING"));
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_finals WHERE tournament_id = ?", {tid}).toInt(), 1);
        ev->createFinal();                      // pressed again from a stale page: still one final
        settle();
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_finals WHERE tournament_id = ?", {tid}).toInt(), 1);
        ev = qobject_cast<CommanderEventScreen *>(w_->currentScreen());
        QVERIFY(ev->title->text().remove(QChar(0x200B)).startsWith("Top-four final"));
        QVERIFY(ev->findChild<TimerWidget *>());
        QVERIFY(live::roundStatus(tid).finalRound);
        QPushButton *chip = nullptr;
        for (QPushButton *b : ev->findChildren<QPushButton *>())
            if (b->accessibleName() == "Top-four final, in play")
                chip = b;
        QVERIFY(chip);
        QList<qint64> seeds;
        QHash<qint64, QString> nameOf;
        for (const QVariant &p : fin["players"].toList()) {
            seeds << p.toMap()["player_id"].toLongLong();
            nameOf.insert(seeds.last(), p.toMap()["display_name"].toString());
        }

        // knockouts are recorded on the page as they happen
        auto *editor = shownEditor(ev);
        QVERIFY(editor);
        QCOMPARE(editor->picks.size(), 4);
        QVERIFY(labelText(ev).contains("No knockouts yet."));
        QPushButton *confirmBtn = editor->confirmBtn;
        QVERIFY(confirmBtn && confirmBtn->isVisible() && !confirmBtn->isEnabled());     // no ending chosen yet
        QVERIFY(!editor->knockoutBtn->isEnabled());
        for (qint64 p : seeds)
            editor->picks.value(p)->setChecked(true);
        QVERIFY2(!editor->knockoutBtn->isEnabled() && editor->knockoutNote->isVisible()
                     && editor->knockoutNote->text().contains("Leave at least one player in"),
                 qPrintable(editor->knockoutNote->text()));
        for (qint64 p : seeds)
            editor->picks.value(p)->setChecked(p == seeds[3]);
        QVERIFY(editor->knockoutBtn->isEnabled());
        const QPointer<FinalEditor> same = editor;
        editor->knockoutBtn->click();
        editor->knockoutBtn->click();                                            // a second press records nothing more
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 1, 10000);
        settle();
        QCOMPARE(cdb::getFinal(tid)["groups"].toList()[0].toList(), (QVariantList{seeds[3]}));
        QVERIFY(same);                                                           // the editor was updated, not rebuilt
        QVERIFY(!editor->picks.contains(seeds[3]) && !editor->winners.contains(seeds[3]));   // out: cannot be picked or win
        QVERIFY(labelText(ev).contains("Knockout 1: " + nameOf[seeds[3]].left(10)));
        // leaving and coming back keeps it
        go<HomeScreen>("home");
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        editor = shownEditor(ev);
        QVERIFY(labelText(ev).contains("Knockout 1: " + nameOf[seeds[3]].left(10)));
        // taking it back is a correction: it asks for a reason (answered automatically here)
        editor->undoBtn->click();
        QTRY_VERIFY_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().isEmpty(), 10000);
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("FINAL_ELIMINATIONS_CORRECTED"));
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["reason"].toString(), QString("test correction"));
        editor->picks.value(seeds[3])->setChecked(true);
        editor->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 1, 10000);
        settle();

        // the placement rules are one press away, and announced as open or closed
        QVERIFY(editor->howText->isHidden());
        editor->howBtn->click();
        QVERIFY(editor->howText->isVisible() && editor->howBtn->accessibleDescription() == "expanded");
        editor->howBtn->click();
        QVERIFY(editor->howText->isHidden() && editor->howBtn->accessibleDescription() == "collapsed");

        // how it ended: one winner, the places shown before anything is saved
        editor->winners.value(seeds[1])->click();
        editor->winners.value(seeds[0])->click();
        QVERIFY(!editor->winners.value(seeds[1])->isChecked());
        QCOMPARE(editor->winner(), seeds[0]);
        const QStringList lines = editor->preview->text().remove(QChar(0x200B)).split('\n');
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines[0].startsWith("1st") && lines[0].contains("won the final"));
        QVERIFY(lines[1].startsWith("2nd") && lines[1].contains("(seed 2)"));
        QVERIFY(lines[2].startsWith("3rd") && lines[2].contains("(seed 3)"));
        QVERIFY(lines[3].startsWith("4th") && lines[3].contains("(seed 4)"));
        QCOMPARE(cdb::getFinal(tid)["status"].toString(), QString("PLAYING"));     // choosing saves nothing
        confirmBtn = editor->confirmBtn;
        QVERIFY(confirmBtn && confirmBtn->isEnabled());
        confirmBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"), 10000);
        settle();
        QVERIFY(qobject_cast<CommanderStandingsScreen *>(w_->currentScreen()));   // finishing shows the standings
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(labelText(ev).contains("(won the final)"));

        // standings: the finalists in the final's places, then the rest; the PDF says the same
        auto *standings = go<CommanderStandingsScreen>("standings", {{"tournament_id", tid}});
        const QVariantList saved = cdb::getState(tid)["final_standings"].toList();
        QCOMPARE(standings->rows.size(), 8);
        for (int i = 0; i < 4; ++i)
            QCOMPARE(standings->rows[i].toMap()["player_id"].toLongLong(), seeds[i]);
        QCOMPARE(standings->rows[0].toMap()["note"].toString(), QString("Final: 1st, won"));
        QCOMPARE(standings->rows[1].toMap()["note"].toString(), QString("Final: 2nd"));
        const printing::Doc pdf = standings->pdfDoc();
        QCOMPARE(pdf.rows.size(), 8);
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(pdf.rows[i][0], QString::number(i + 1));
            QCOMPARE(pdf.rows[i][1], saved[i].toMap()["display_name"].toString());
            QCOMPARE(pdf.rows[i][2], cmdr::pointsText(saved[i].toMap()["points"].toDouble()));
        }
        QVERIFY(pdf.rows[0].last().startsWith("Final: 1st"));
        QVERIFY(!printing::pdfBytes({pdf}).isEmpty());

        // corrected: the final had no winner after all, so seeds 1 to 3 are placed by seed
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        bool beside = false;
        inNextDialog<FinalCorrectionDialog>([&](FinalCorrectionDialog *d) {
            // the page is not replaced while its own question is open
            w_->navigateTo("home");
            QVERIFY(qobject_cast<CommanderEventScreen *>(w_->currentScreen()));
            QVERIFY(d->before->text().remove(QChar(0x200B)).startsWith("1st"));
            QVERIFY(!d->saveBtn->isEnabled());                                   // no reason yet
            d->editor->undoBtn->click();                                         // seeds 2 and 3 were still in at the end
            d->editor->noWinner->click();
            QVERIFY2(d->editor->preview->text().contains("placed by seed 1"),
                     qPrintable(d->editor->preview->text() + d->editor->problem->text()));
            d->reasonEdit->setText("   ");
            QVERIFY(!d->saveBtn->isEnabled());
            d->reasonEdit->setText("it was a draw");
            QVERIFY(d->saveBtn->isEnabled());
            beside = qAbs(d->reasonEdit->mapTo(d, QPoint(0, 0)).y() - d->saveBtn->mapTo(d, QPoint(0, 0)).y()) < d->saveBtn->height();
            d->saveBtn->click();
        });
        buttons(ev).value(QStringLiteral("Correct final…"))->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["outcome"].toString(), QString("NO_WINNER"), 10000);
        settle();
        QVERIFY2(beside, "the reason sits beside Save");
        QVERIFY(labelText(w_->currentScreen()).contains("placed by qualifying seed"));
        QCOMPARE(cdb::getAudit(tid)[1].toMap()["action"].toString(), QString("FINAL_CORRECTED"));

        // the final and standings fit in both themes, at both text sizes, in a narrow window
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(360, 480)}) {
                    w_->resize(size);
                    for (const QString &page : {QString("round"), QString("standings")}) {
                        w_->navigateTo(page, {{"tournament_id", tid}});
                        settle();
                        const QByteArray where = QStringLiteral("%1 %2 %3 %4").arg(page, textSize, mode).arg(size.width()).toUtf8();
                        QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                    }
                }
            }
        }
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(1180, 780);
        // the player profile lists the final
        auto *profile = go<PlayerProfileScreen>("player_profile", {{"player_id", seeds[0]}});
        for (T::ClickFrame *r : profile->findChildren<T::ClickFrame *>())
            if (r->accessibleName() == "Final Night: show matches")
                QTest::keyClick(r, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(labelText(w_->currentScreen()).contains("Top-four final  ·  seed 1"), 10000);
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
        QVERIFY(buttons(ev).contains(QStringLiteral("Correct…")) && buttonStarting(ev, "Winner"));

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
            QVERIFY2(!label.startsWith("Winner") && !label.startsWith("2nd") && label != "No clear second"
                     && !label.startsWith("Clear") && !label.startsWith("Draw") && !label.startsWith("Save draw")
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
            QVERIFY2(QRegularExpression("^Enter a name for the tournament\\. \\(Reference: R-[0-9A-Z]{6}\\)$").match(refusal).hasMatch(),
                     qPrintable(refusal));
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
        swiss::reportResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
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
        cdb::reportPodResult(pod["pod_id"].toLongLong(), "WIN", pod["seats"].toList()[0].toMap()["player_id"].toLongLong(), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
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
        search(dir, "alex  S");
        QCOMPARE(listed(dir), QStringList{lb});
        search(dir, "Jones");
        QCOMPARE(listed(dir), QStringList{"Alex Jones"});
        search(dir, QString());
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
        search(reg, "Alex Smith");
        const QString found = labelText(reg);
        QVERIFY2(found.contains(QStringLiteral("Alex Smith · ") + pdb::formatId(a))
                 && found.contains(QStringLiteral("Alex Smith · ") + pdb::formatId(b)), qPrintable(found));
        pdb::removePlayer(pdb::addPlayer("Vanished Vera"));
        search(reg, "Vanished");
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
        search(reg, "Dana");
        QVERIFY(!labelText(reg).contains("Dana Cruz"));
        // a new Dana Cruz is a new person: no question about the removed one, a new id, no history
        reg->newNameBox->setText("Dana Cruz");
        reg->addBtn->click();
        QTest::qWait(300);
        settle();
        QCOMPARE(tdb::enrolledPlayers(fresh).size(), 1);
        const qint64 newDana = tdb::enrolledPlayers(fresh).first()["player_id"].toLongLong();
        QVERIFY(newDana > dana);
        QVERIFY(pdb::tournamentHistory(newDana).size() == 1 && pdb::tournamentHistory(newDana).first()["final_placement"].isNull());
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

    // The diagnostic log, from the screens.

    void errorsShownAreLoggedWithAReferenceAndCanBeExported()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const auto logText = [&] {
            QFile f(tmp_->filePath("logs/app.log"));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        // an error dialog: the reference it shows is the one in the log
        setAutoConfirm(false);
        QString shown;
        QTimer::singleShot(200, qApp, [&shown] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                shown = box->windowTitle() + " | " + box->text();
                box->accept();
            }
        });
        warn(w_.get(), "Could not save", "The result could not be saved.");
        setAutoConfirm(true);
        static const QRegularExpression refIn("Reference: (R-[0-9A-Z]{6})");
        const QString ref = refIn.match(shown).captured(1);
        QVERIFY2(!ref.isEmpty() && shown.startsWith("Could not save | The result could not be saved."), qPrintable(shown));
        QString line;
        for (const QString &l : logText().split('\n'))
            if (l.contains("ref=" + ref))
                line = l;
        QVERIFY2(line.contains(" WARN ") && line.contains("dialog.error") && line.contains("title=\"Could not save\"")
                 && line.endsWith("| The result could not be saved."), qPrintable(line));

        // a failed action from a screen: the attempt, the failure and the message shown are all there
        const qint64 tid = modern({"Ana", "Bo", "Cy", "Di"}, 45, 2, true, "Log Cup");
        auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
        tdb::terminateTournament(tid);                          // ended elsewhere while this page is still open
        const qint64 match = tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()).first()["match_id"].toLongLong();
        round->recordResult(match, "PLAYER1");
        QString text = logText();
        QVERIFY(text.contains(QStringLiteral("result.report status=attempt match=%1").arg(match)));
        QVERIFY(text.contains(QStringLiteral("result.report status=failed match=%1").arg(match)));
        QVERIFY(!text.contains(QStringLiteral("result.report status=ok match=%1").arg(match)));
        QVERIFY(text.contains("dialog.error") && text.contains("This tournament was ended early"));
        // the failed action and the message about it share one operation id and one reference
        {
            static const QRegularExpression opIn(" op=([0-9A-F]{8})"), refOf(" ref=(R-[0-9A-Z]{6})");
            QString failed, dialog;
            for (const QString &l : text.split('\n')) {
                if (l.contains(QStringLiteral("result.report status=failed match=%1").arg(match)))
                    failed = l;
                if (l.contains("dialog.error") && l.contains("This tournament was ended early"))
                    dialog = l;
            }
            QVERIFY(!failed.isEmpty() && !dialog.isEmpty());
            QVERIFY(!opIn.match(failed).captured(1).isEmpty());
            QCOMPARE(opIn.match(dialog).captured(1), opIn.match(failed).captured(1));
            QCOMPARE(refOf.match(dialog).captured(1), refOf.match(failed).captured(1));
        }
        // a page that cannot be opened
        w_->navigateTo("no_such_page", {{"tournament_id", tid}});
        QVERIFY2(logText().contains(QStringLiteral("page.open status=failed page=no_such_page tournament=%1").arg(tid)), qPrintable(logText().right(400)));
        // and a successful one, done from the screen
        const qint64 other = modern({"Ed", "Flo"}, 45, 1, true, "Second Log Cup");
        round = go<RoundScreen>("round", {{"tournament_id", other}});
        const qint64 m2 = tdb::roundPairings(tdb::currentRound(other)["round_id"].toLongLong()).first()["match_id"].toLongLong();
        round->recordResult(m2, "PLAYER1");
        QVERIFY(logText().contains(QStringLiteral("result.report status=ok match=%1").arg(m2)));
        // none of the names involved were written by the actions (the dialog text above had none either)
        for (const char *name : {"Log Cup", "Ana", "Flo"})
            QVERIFY2(!logText().contains(name), name);

        // Settings: the two troubleshooting buttons
        SettingsDialog settings(w_.get());
        QVERIFY(buttons(&settings).isEmpty());                  // (not shown yet)
        QStringList labels;
        for (QPushButton *b : settings.findChildren<QPushButton *>())
            labels << b->text();
        QVERIFY2(labels.contains("Open log folder") && labels.contains(QStringLiteral("Export diagnostic report…")), qPrintable(labels.join(", ")));
        QCOMPARE(QDir(logFolder()).canonicalPath(), QDir(tmp_->filePath("logs")).canonicalPath());
        // the report is previewed exactly as it would be saved, and nothing is saved until asked
        QString preview;
        QStringList reportButtons;
        QTimer::singleShot(250, qApp, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            if (!dlg)
                return;
            if (auto *box = dlg->findChild<QPlainTextEdit *>("reportPreview"))
                preview = box->toPlainText();
            for (QPushButton *b : dlg->findChildren<QPushButton *>())
                reportButtons << b->text();
            QTest::keyClick(dlg, Qt::Key_Escape);
        });
        settings.exportReport();
        QVERIFY2(preview.startsWith("TCG Tournament Manager diagnostic report") && preview.contains("App version:")
                 && preview.contains("Recent log entries"), qPrintable(preview.left(300)));
        QVERIFY(preview.contains(ref));                         // the error above is in the excerpt
        QVERIFY(reportButtons.contains(QStringLiteral("Save report…")) && reportButtons.contains("Close"));
        QVERIFY(preview.contains(QStringLiteral("Build:        ") + prefs::BUILD));
        QVERIFY(preview.contains("may contain personal information"));
        QVERIFY(QDir(tmp_->path()).entryList({"*.txt"}, QDir::Files).isEmpty());
        const QString file = tmp_->filePath("report.txt");
        QVERIFY(saveDiagnosticReport(file, preview));
        QFile saved(file);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray bytes = saved.readAll();
        QCOMPARE(QString::fromUtf8(bytes), preview);
        QVERIFY(!bytes.contains("SQLite format") && bytes.size() < 120 * 1024);
        QVERIFY(!preview.contains(QDir::toNativeSeparators(QDir::homePath())));
        QVERIFY(!saveDiagnosticReport(tmp_->filePath("no/such/folder/report.txt"), preview));
        // Settings names the exact build, and says nothing about logging while it works
        QLabel *buildLabel = settings.findChild<QLabel *>("buildLabel");
        QVERIFY(buildLabel && buildLabel->text().remove(QChar(0x200B)).contains(QStringLiteral("v%1  ·  build %2").arg(prefs::VERSION, prefs::BUILD)));
        QVERIFY(!settings.findChild<QLabel *>("logStatus"));
        QStringList headings;
        for (QLabel *l : settings.findChildren<QLabel *>())
            headings << l->text();
        QVERIFY(headings.contains("DIAGNOSTICS"));

        // when the log cannot be written: a quiet status in Settings and in the report, no dialog, and the app works as before
        QFile blocker(tmp_->filePath("blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        applog::setDirectory(tmp_->filePath("blocked/logs"));
        const qint64 third = modern({"Gus", "Hal"}, 45, 1, true, "Unlogged Cup");
        round = go<RoundScreen>("round", {{"tournament_id", third}});
        const qint64 m3 = tdb::roundPairings(tdb::currentRound(third)["round_id"].toLongLong()).first()["match_id"].toLongLong();
        round->recordResult(m3, "PLAYER1");
        QCOMPARE(tdb::roundPairings(tdb::currentRound(third)["round_id"].toLongLong()).first()["result"].toString(), QString("PLAYER1"));
        QVERIFY(!QApplication::activeModalWidget());            // nothing popped up about it
        QVERIFY(!applog::status().available);
        SettingsDialog offline(w_.get());
        QLabel *status = offline.findChild<QLabel *>("logStatus");
        QVERIFY(status);
        const QString statusText = status->text().remove(QChar(0x200B));
        QVERIFY2(statusText.startsWith("Logging unavailable: ") && statusText.contains("The app works normally"), qPrintable(statusText));
        QVERIFY(!statusText.contains(QDir::toNativeSeparators(QDir::homePath())));
        QString offlinePreview, offlineNote;
        QTimer::singleShot(250, qApp, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            if (!dlg)
                return;
            if (auto *box = dlg->findChild<QPlainTextEdit *>("reportPreview"))
                offlinePreview = box->toPlainText();
            if (auto *note = dlg->findChild<QLabel *>("reportLogStatus"))
                offlineNote = note->text().remove(QChar(0x200B));
            QTest::keyClick(dlg, Qt::Key_Escape);
        });
        offline.exportReport();
        QVERIFY2(offlineNote.contains("recent entries may be missing"), qPrintable(offlineNote));
        QVERIFY2(offlinePreview.contains("Logging:      UNAVAILABLE since") && offlinePreview.contains("may be missing from this report"),
                 qPrintable(offlinePreview.left(1500)));
        QVERIFY(offlinePreview.contains(QStringLiteral("result.report status=ok match=%1").arg(m3)));     // kept in memory, and shown as such
        applog::setDirectory(tmp_->filePath("logs"));
        QVERIFY(applog::status().available);

        // the Settings panel still fits with the new section, at Large text in a small window
        prefs::setPref("text_size", "large");
        w_->restyle();
        w_->resize(360, 520);
        SettingsDialog small(w_.get());
        small.show();
        settle();
        for (QPushButton *b : small.findChildren<QPushButton *>())
            if (b->isVisible())
                QVERIFY2(b->width() >= b->sizeHint().width() && b->height() >= T::MIN_HIT - 1, qPrintable(b->text()));
    }

    // A save that fails is shown as a failure, and the page keeps showing what is really saved.

    void aFailedSaveLeavesThePageAndTheDataAsTheyWere()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const auto logText = [&] {
            QFile f(tmp_->filePath("logs/app.log"));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        // Add & enroll while the database refuses registrations
        const qint64 tid = modern({"Ana", "Bo"}, 45, 1, false, "Sign-up");
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", tid}});
        QVERIFY(labelText(reg).contains("2 players enrolled"));
        db::exec("CREATE TRIGGER refuse_enrollment BEFORE INSERT ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'enrollments are closed'); END");
        const int playersBefore = db::value("SELECT COUNT(*) FROM players").toInt();
        reg->newNameBox->setText("Cy Newcomer");
        reg->addBtn->click();
        settle();
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), playersBefore);     // no stray new player
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 2);
        QVERIFY(labelText(reg).contains("2 players enrolled") && !labelText(reg).contains("Cy Newcomer"));
        QCOMPARE(reg->newNameBox->text(), QString("Cy Newcomer"));      // still there to try again
        QString text = logText();
        QVERIFY(text.contains("player.add_and_enroll status=failed") && !text.contains("player.add_and_enroll status=ok"));
        QVERIFY2(text.contains("dialog.error") && text.contains("title=\"Player not added\"") && text.contains("enrollments are closed"),
                 qPrintable(text.right(600)));
        // enrolling someone from the search results fails the same way, without changing them
        const qint64 di = pdb::addPlayer("Di Existing");
        search(reg, "Di Exist");
        QPushButton *enroll = nullptr;
        for (QPushButton *b : reg->findChildren<QPushButton *>())
            if (b->isVisible() && b->accessibleName() == "Enroll Di Existing")
                enroll = b;
        QVERIFY(enroll);
        enroll->click();
        settle();
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 2);
        QVERIFY(!pdb::isRemoved(di) && pdb::playerById(di)["display_name"].toString() == "Di Existing");
        QVERIFY(logText().contains("title=\"Not enrolled\""));
        db::exec("DROP TRIGGER refuse_enrollment");
        reg->addBtn->click();
        settle();
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 3);                  // the same click works once the cause is gone
        QVERIFY(labelText(reg).contains("3 players enrolled") && reg->newNameBox->text().isEmpty());

        // a result while the standings cannot be saved
        const qint64 live = modern({"Ed", "Flo"}, 45, 1, true, "Running");
        auto *round = go<RoundScreen>("round", {{"tournament_id", live}});
        const qint64 match = tdb::roundPairings(tdb::currentRound(live)["round_id"].toLongLong()).first()["match_id"].toLongLong();
        db::exec("CREATE TRIGGER break_standings BEFORE UPDATE ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'standings cannot be saved'); END");
        round->recordResult(match, "PLAYER1");
        settle();
        QCOMPARE(tdb::roundPairings(tdb::currentRound(live)["round_id"].toLongLong()).first()["result"], QVariant());
        QVERIFY(labelText(round).contains("Awaiting result") && labelText(round).contains("0 / 1 reported"));
        text = logText();
        QVERIFY(text.contains(QStringLiteral("result.report status=failed match=%1").arg(match)));
        QVERIFY(!text.contains(QStringLiteral("result.report status=ok match=%1").arg(match)));
        QVERIFY(text.contains("title=\"Result not saved\""));
        db::exec("DROP TRIGGER break_standings");
        round->recordResult(match, "PLAYER1");
        settle();
        QVERIFY(labelText(round).contains("1 / 1 reported"));
        QCOMPARE(db::value("SELECT MAX(match_wins) FROM enrollments WHERE tournament_id = ?", {live}).toInt(), 1);
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

    void everyScreenFitsEveryWindowSizeThemeAndTextSize_data()
    {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("textSize");
        QTest::newRow("dark, standard text") << "dark" << "standard";
        QTest::newRow("light, standard text") << "light" << "standard";
        QTest::newRow("light, large text") << "light" << "large";
        QTest::newRow("dark, large text") << "dark" << "large";
    }

    void everyScreenFitsEveryWindowSizeThemeAndTextSize()
    {
        QFETCH(QString, theme);
        QFETCH(QString, textSize);
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
                                     pv.toMap()["seats"].toList()[0].toMap()["player_id"].toLongLong(), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
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
        {
            const auto look = qMakePair(theme, textSize);
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
        QCOMPARE(combinations, int(sizes.size() * screens.size()));
    }
    // the players page used to build every row twice on opening, and again on every resize
    void playersPageBuildsItsRowsOnceAndFollowsTheWindow()
    {
        const QList<qint64> ids = players({LONG_NAME, "Bo", "Cy", "Di", "Ed", "Flo"});
        const qint64 poke = tdb::createTournament("Poke", "POKEMON", 1, "Standard");
        tdb::enrollPlayer(poke, ids[0]);
        tdb::enrollPlayer(poke, ids[1]);
        swiss::startTournament(poke);
        const auto tagWidths = [](PlayersScreen *page) {
            QList<int> widths;
            for (QFrame *row : page->findChildren<QFrame *>("pr"))
                if (row->isVisible())
                    widths << row->findChild<T::TagStrip *>()->width();
            return widths;
        };

        w_->resize(1280, 800);
        settle();
        auto *page = go<PlayersScreen>("players");
        QVERIFY(page);
        QCOMPARE(page->listBuilds, 1);
        QCOMPARE(tagWidths(page).size(), 6);
        for (const QSize &size : {QSize(900, 700), QSize(640, 480), QSize(360, 640), QSize(1440, 900)}) {
            w_->resize(size);
            settle();
            const QByteArray where = QStringLiteral("%1x%2").arg(size.width()).arg(size.height()).toUtf8();
            QVERIFY2(page == w_->currentScreen() && page->listBuilds == 1, where);     // the same rows
            const QList<int> kept = tagWidths(page);
            // and they look as a page opened at this size does
            auto *fresh = go<PlayersScreen>("players");
            QVERIFY2(fresh->listBuilds == 1, where);
            QVERIFY2(kept == tagWidths(fresh) && kept.size() == 6, where);
            QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
            page = fresh;
        }
        // a search is still one rebuild, with the new result
        search(page, "Bo");
        QCOMPARE(page->listBuilds, 2);
        QCOMPARE(tagWidths(page).size(), 1);
    }

    // a typed name is text, never markup: as HTML it could make Qt load an image file
    void typedNamesAreNeverShownAsMarkup()
    {
        const QString markup = QStringLiteral("<b>Bo</b><img src=\"none.svg\">");
        const qint64 tid = modern({markup, "Cy"}, 45, 1, true, QStringLiteral("<i>Cup</i>"));
        // what a tooltip puts on screen: Qt renders it as HTML when it looks like HTML
        const auto displayed = [](const QString &tip) {
            return Qt::mightBeRichText(tip) ? QTextDocumentFragment::fromHtml(tip).toPlainText() : tip;
        };

        auto *dir = go<PlayersScreen>("players");
        int found = 0;
        for (T::NameScroll *strip : dir->findChildren<T::NameScroll *>()) {
            if (strip->isVisible() && strip->label->text() == markup) {
                QCOMPARE(strip->label->textFormat(), Qt::PlainText);
                QCOMPARE(displayed(strip->toolTip()), markup);
                ++found;
            }
        }
        QCOMPARE(found, 1);

        auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
        QVERIFY(round);
        int winTips = 0;
        for (T::WrapButton *b : round->findChildren<T::WrapButton *>()) {
            if (b->toolTip().isEmpty())
                continue;
            QVERIFY2(displayed(b->toolTip()).endsWith(" wins") && !displayed(b->toolTip()).isEmpty(), qPrintable(b->toolTip()));
            winTips += displayed(b->toolTip()) == markup + " wins";
        }
        QCOMPARE(winTips, 1);
        QVERIFY2(displayed(w_->tabButton(tid)->toolTip()).startsWith("<i>Cup</i>"), qPrintable(w_->tabButton(tid)->toolTip()));

        // and the question, notice and error boxes
        setAutoConfirm(false);
        QList<Qt::TextFormat> formats;
        QStringList texts;
        const auto answer = [&] {
            QTimer::singleShot(0, qApp, [&] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    formats << box->textFormat();
                    texts << box->text();
                    box->reject();
                }
            });
        };
        answer();
        confirm(w_.get(), "Remove Player", "Remove " + markup + " from this tournament?");
        answer();
        inform(w_.get(), "Saved", markup);
        answer();
        warn(w_.get(), "Player not removed", markup + " is still playing.");
        setAutoConfirm(true);
        QCOMPARE(formats, (QList<Qt::TextFormat>{Qt::PlainText, Qt::PlainText, Qt::PlainText}));
        for (const QString &text : texts)
            QVERIFY2(text.contains(markup), qPrintable(text));
    }

    // an accessibility or automation tool can press a button behind an open question, which a
    // mouse cannot.  replacing the page then crashed: its handler was still waiting for the answer
    void aPageIsNotReplacedWhileItsOwnQuestionIsOpen()
    {
        const qint64 tid = modern({"Ana", "Bo", "Cy", "Di"}, 45, 2);
        auto *round = go<RoundScreen>("round", {{"tournament_id", tid}});
        QVERIFY(round);
        qint64 match = 0;
        for (const db::Row &m : tdb::roundPairings(round->currentRoundId))
            if (!m["player2_id"].isNull())
                match = m["match_id"].toLongLong();
        round->recordResult(match, "PLAYER1");
        settle();
        QPushButton *undo = nullptr;
        for (QPushButton *b : round->findChildren<QPushButton *>())
            if (b->isVisible() && b->text() == "Undo")
                undo = b;
        QVERIFY(undo);

        setAutoConfirm(false);
        bool asked = false, stayedOnNavigate = false, stayedOnBack = false;
        const int pagesBefore = int(w_->history.size());
        QTimer::singleShot(0, qApp, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            asked = box != nullptr;
            w_->navigateTo("home");
            stayedOnNavigate = w_->currentScreen() == round;
            w_->goBack();
            stayedOnBack = w_->currentScreen() == round && int(w_->history.size()) == pagesBefore;
            if (box)
                box->reject();          // "No": the result stays
        });
        undo->click();
        setAutoConfirm(true);
        settle();
        QVERIFY(asked);
        QVERIFY2(stayedOnNavigate, "the page was replaced under its own open question");
        QVERIFY2(stayedOnBack, "Back replaced the page under its own open question");
        QCOMPARE(w_->currentScreen(), round);
        QCOMPARE(tdb::pendingMatchCount(round->currentRoundId), 1);         // the answer was No
        // with the question answered, navigation works as before
        QVERIFY(go<HomeScreen>("home"));

        // Settings is not a page's question: changing the look from it still rebuilds the page
        go<RoundScreen>("round", {{"tournament_id", tid}});
        bool rebuilt = false;
        QTimer::singleShot(0, qApp, [&] {
            QWidget *before = w_->currentScreen();
            w_->setTheme("light");
            rebuilt = w_->currentScreen() != before && qobject_cast<RoundScreen *>(w_->currentScreen());
            if (QWidget *dlg = QApplication::activeModalWidget())
                dlg->close();
        });
        w_->navigateTo("settings");
        settle();
        QVERIFY(rebuilt);
        w_->setTheme("dark");
    }

    // audit: what the screens must not let through

    void goingBackAfterTheStartDoesNotReopenRegistration()
    {
        const qint64 tid = modern({"Ana", "Bo", "Cy", "Di"}, 45, 2, false);
        go<HubScreen>("hub", {{"game", "MTG"}});
        auto *reg = go<RegistrationScreen>("registration", {{"tournament_id", tid}});
        QVERIFY(reg);
        reg->confirmStart();
        settle();
        QVERIFY(qobject_cast<RoundScreen *>(w_->currentScreen()));
        w_->goBack();
        settle();
        QVERIFY2(!qobject_cast<RegistrationScreen *>(w_->currentScreen()), "registration is over once round 1 is paired");
        QVERIFY(qobject_cast<HubScreen *>(w_->currentScreen()));
        QCOMPARE(tdb::enrolledPlayers(tid, true).size(), 4);

        // the same for a Commander event
        const qint64 cmd = commander({"Ka", "Le", "Mo", "Ni"}, 1, false);
        go<HubScreen>("hub", {{"game", "MTG"}});
        QVERIFY(go<CommanderRegistrationScreen>("registration", {{"tournament_id", cmd}}));
        cdb::startEvent(cmd);
        go<CommanderEventScreen>("round", {{"tournament_id", cmd}});
        w_->goBack();
        settle();
        QVERIFY(qobject_cast<HubScreen *>(w_->currentScreen()));
        // a tournament still being set up keeps its way back
        const qint64 open = modern({"Oz", "Pi"}, 45, 2, false, "Open");
        go<HubScreen>("hub", {{"game", "MTG"}});
        go<RegistrationScreen>("registration", {{"tournament_id", open}});
        go<PlayersScreen>("players");
        w_->goBack();
        settle();
        QVERIFY(qobject_cast<RegistrationScreen *>(w_->currentScreen()));
    }

    void exportedCellsCannotBecomeSpreadsheetFormulas()
    {
        QCOMPARE(printing::csvCell("Ana"), QString("Ana"));
        QCOMPARE(printing::csvCell("Smith, Bo"), QString("\"Smith, Bo\""));
        QCOMPARE(printing::csvCell("Say \"hi\""), QString("\"Say \"\"hi\"\"\""));
        for (const char *risky : {"=1+1", "+1", "-1", "@SUM(A1)"})
            QCOMPARE(printing::csvCell(risky), QStringLiteral("'") + risky);
        QCOMPARE(printing::csvCell("=HYPERLINK(\"x\",\"y\")"), QString("\"'=HYPERLINK(\"\"x\"\",\"\"y\"\")\""));
        QCOMPARE(printing::csvCell("a\rb"), QString("\"a\rb\""));
        QCOMPARE(printing::csvCell("Mary-Jane"), QString("Mary-Jane"));    // only a leading character matters
    }

    // Commander results are entered on the pod cards, without a window for routine entry.

    void commanderPodResultsAreEnteredOnTheCards()
    {
        QStringList names;
        for (int i = 1; i <= 16; ++i)
            names << QStringLiteral("Player %1").arg(i);
        names[5] = LONG_NAME;
        const qint64 tid = commander(names, 2);
        w_->resize(1180, 600);
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        QVERIFY(ev);
        const auto pod = [tid](int i) { return cdb::getRounds(tid)[0].toMap()["pods"].toList()[i].toMap(); };
        const auto nameAt = [&](int i, int seat) { return pod(i)["seats"].toList()[seat].toMap()["display_name"].toString(); };
        const auto idAt = [&](int i, int seat) { return pod(i)["seats"].toList()[seat].toMap()["player_id"].toLongLong(); };
        const auto winnerOf = [&](int i) {
            for (const QVariant &s : pod(i)["seats"].toList())
                if (s.toMap()["result"].toString() == "WIN")
                    return s.toMap()["player_id"].toLongLong();
            return qint64(0);
        };
        QCOMPARE(cdb::getRounds(tid)[0].toMap()["pods"].toList().size(), 4);

        // nothing is called saved before it is; the winner action says what it is worth
        QVERIFY(labelText(ev).contains(QStringLiteral("●  No result yet")));
        QVERIFY(!labelText(ev).contains("saved"));
        QPushButton *win = buttonNamed(ev, nameAt(2, 1) + " wins pod 3");
        QVERIFY(win);
        QCOMPARE(win->text(), QString("Winner  +5"));

        // the page stays where it is, and only that pod is redrawn
        QScrollBar *bar = ev->findChild<QScrollArea *>()->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum() / 2);
        settle();
        const int where = bar->value();
        const QPointer<QPushButton> elsewhere = buttonNamed(ev, nameAt(0, 0) + " wins pod 1");
        const QPointer<QPushButton> pressed = win;
        QVERIFY(elsewhere);
        win->setFocus();
        win->click();
        QTRY_VERIFY_WITH_TIMEOUT(!pressed, 10000);              // the old card is gone
        QVERIFY(elsewhere);                                     // the other pods were left alone
        QCOMPARE(bar->value(), where);
        QCOMPARE(winnerOf(2), idAt(2, 1));
        QCOMPARE(pod(2)["second_place_status"].toString(), QString("PENDING"));
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("RESULT_REPORTED"));

        // provisional, in words: second place is still open and the round cannot be finalized yet
        QVERIFY(labelText(ev).contains("2nd place still to choose, points provisional"));
        QVERIFY(labelText(ev).contains("Second place is not recorded yet in pod 3."));
        QVERIFY(labelText(ev).contains(QStringLiteral("1 / 4 pods reported")));
        QVERIFY(w_->focusWidget() && w_->focusWidget()->accessibleName().endsWith("second in pod 3"));
        QVERIFY(!buttonNamed(ev, nameAt(2, 1) + " second in pod 3"));          // never the winner
        QPushButton *second = buttonNamed(ev, nameAt(2, 2) + " second in pod 3");
        QVERIFY(second);
        QCOMPARE(second->text(), QString("2nd  +2.5"));
        QVERIFY(buttonNamed(ev, "No clear second place in pod 3"));

        // finishing the pending second place needs no reason and is not a correction
        second->click();
        settle();
        QCOMPARE(pod(2)["second_place_player_id"].toLongLong(), idAt(2, 2));
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("SECOND_PLACE_RECORDED"));
        QVERIFY(!cdb::getAudit(tid)[0].toMap()["detail"].toMap().contains("reason"));
        QVERIFY(labelText(ev).contains("saved, provisional until the round is finalized"));
        QCOMPARE(bar->value(), where);
        QVERIFY(elsewhere);
        QVERIFY(w_->focusWidget() && w_->focusWidget()->accessibleName() == "Correct the result of pod 3");

        // a draw is entered on the card too; a refused save keeps what was entered and says why
        buttonNamed(ev, "Report a draw in pod 4")->click();
        QVERIFY(labelText(ev).contains("Draw, not saved yet"));
        QList<QCheckBox *> outs;
        for (QCheckBox *cb : ev->findChildren<QCheckBox *>())
            if (cb->isVisible() && cb->accessibleName().endsWith("was knocked out before the draw"))
                outs << cb;
        QCOMPARE(outs.size(), 4);
        for (int i = 0; i < 3; ++i)
            outs[i]->setChecked(true);
        buttonNamed(ev, "Save the draw in pod 4")->click();
        settle();
        QVERIFY2(labelText(ev).contains("Not saved: A draw needs at least two players still in the game"), qPrintable(labelText(ev)));
        QVERIFY(outs[0]->isChecked() && outs[2]->isChecked() && outs[0]->isVisible());
        QVERIFY(!pod(3)["reported"].toBool());
        outs[2]->setChecked(false);
        buttonNamed(ev, "Save the draw in pod 4")->click();
        settle();
        QCOMPARE(pod(3)["outcome"].toString(), QString("DRAW"));
        QVERIFY(!labelText(ev).contains("Not saved"));

        // a result saved elsewhere meanwhile is not overwritten; the card shows what is saved and why
        QPushButton *stale = buttonNamed(ev, nameAt(0, 0) + " wins pod 1");
        cdb::reportPodResult(pod(0)["pod_id"].toLongLong(), "WIN", idAt(0, 1));
        stale->click();
        settle();
        QCOMPARE(winnerOf(0), idAt(0, 1));
        QVERIFY2(labelText(ev).contains("Not saved: This pod's result was changed elsewhere"), qPrintable(labelText(ev)));

        // changing a saved result is a correction: Save waits for a reason, which sits beside it
        bool beside = false;
        inNextDialog<ResultDialog>([&](ResultDialog *d) {
            QVERIFY(d->saveBtn && !d->saveBtn->isEnabled());
            auto *reason = d->findChild<QLineEdit *>();
            reason->setText("   ");
            QVERIFY(!d->saveBtn->isEnabled());
            beside = qAbs(reason->mapTo(d, QPoint(0, 0)).y() - d->saveBtn->mapTo(d, QPoint(0, 0)).y()) < d->saveBtn->height();
            reason->setText("slip was misread");
            d->outcomeCombo->setCurrentIndex(d->outcomeCombo->findData(idAt(0, 2)));
            d->secondCombo->setCurrentIndex(d->secondCombo->findData(idAt(0, 1)));
            d->saveBtn->click();
        });
        buttonNamed(ev, "Correct the result of pod 1")->click();
        settle();
        QVERIFY(beside);
        QCOMPARE(winnerOf(0), idAt(0, 2));
        QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["reason"].toString(), QString("slip was misread"));

        // a refused correction keeps the window open with everything as entered
        QString refused;
        bool kept = false;
        inNextDialog<ResultDialog>([&](ResultDialog *d) {
            d->findChild<QLineEdit *>()->setText("it was a draw");
            d->outcomeCombo->setCurrentIndex(d->outcomeCombo->count() - 1);
            int ticked = 0;
            for (QCheckBox *cb : d->findChildren<QCheckBox *>())
                cb->setChecked(ticked++ < 3);
            inNextDialog<ResultDialog>([&, d](ResultDialog *again) {
                refused = again->error->isVisible() ? again->error->text().remove(QChar(0x200B)) : QString("(no error shown)");
                kept = again == d && again->outcome() == "DRAW" && again->eliminated().size() == 3 && again->reason() == "it was a draw";
                again->reject();
            });
            d->saveBtn->click();
        });
        buttonNamed(ev, "Correct the result of pod 1")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!refused.isEmpty(), 10000);
        settle();
        QVERIFY2(refused.contains("A draw needs at least two players still in the game"), qPrintable(refused));
        QVERIFY(kept);
        QCOMPARE(winnerOf(0), idAt(0, 2));                                   // nothing saved

        // clearing asks for a reason (answered automatically in tests) and records it
        buttonNamed(ev, "Clear the result of pod 1")->click();
        settle();
        QVERIFY(!pod(0)["reported"].toBool());
        entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CLEARED"));
        QCOMPARE(entry["detail"].toMap()["reason"].toString(), QString("test correction"));

        // pending, provisional and decided cards fit in both themes, at both text sizes, in a narrow window
        cdb::reportPodResult(pod(1)["pod_id"].toLongLong(), "WIN", idAt(1, 0));
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(360, 480)}) {
                    w_->resize(size);
                    go<CommanderEventScreen>("round", {{"tournament_id", tid}});
                    const QByteArray at = QStringLiteral("%1 %2 %3").arg(textSize, mode).arg(size.width()).toUtf8();
                    QVERIFY2(layoutProblems().isEmpty(), at + " " + layoutProblems().join("; ").toUtf8());
                }
            }
        }
        // narrowing a page that is already open moves the actions under the names as well
        prefs::setPref("text_size", "large");
        w_->restyle();
        w_->resize(1180, 780);
        go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        w_->resize(360, 480);
        settle();
        QVERIFY2(layoutProblems().isEmpty(), qPrintable(layoutProblems().join("; ")));
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(1180, 780);
    }

    // The final's editor refuses endings that contradict each other before anything is saved.

    void finalEditorRefusesContradictoryEndings()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"}, 1, true, 80, "Editor Night", true);
        for (const QVariant &pv : cdb::getRounds(tid).last().toMap()["pods"].toList()) {
            const QVariantList seats = pv.toMap()["seats"].toList();
            cdb::reportPodResult(pv.toMap()["pod_id"].toLongLong(), "WIN", seats[0].toMap()["player_id"].toLongLong(), {},
                                 cdb::NONE, cdb::SecondPlace::noClearSecond());
        }
        cdb::finalizeRound(cdb::getRounds(tid).last().toMap()["round_id"].toLongLong());
        QList<qint64> seeds;
        for (const QVariant &c : cdb::finalCandidates(tid)["players"].toList())
            seeds << c.toMap()["player_id"].toLongLong();
        cdb::createFinal(tid, seeds);
        FinalEditor editor(tid, cdb::getFinal(tid), FinalEditor::Draft);
        editor.show();
        QVERIFY(!editor.ready());
        QVERIFY(editor.preview->text().contains("Choose the winner, or No winner"));
        for (int i = 1; i < 4; ++i) {
            editor.picks.value(seeds[i])->setChecked(true);
            editor.knockoutBtn->click();
        }
        QCOMPARE(editor.knockouts().size(), 3);
        QCOMPARE(editor.picks.size(), 1);                       // only the player still in is shown
        QVERIFY(!editor.picks.value(seeds[0])->isEnabled());    // and cannot be knocked out
        QCOMPARE(editor.winners.size(), 1);
        QVERIFY(editor.knockoutNote->text().contains("One player remains"));
        QVERIFY(!editor.noWinner->isEnabled());                 // one player left has won
        editor.noWinner->click();
        QVERIFY(!editor.ready() && editor.outcome().isEmpty());
        editor.winners.value(seeds[0])->click();
        QVERIFY(editor.ready());
        QCOMPARE(editor.groups().size(), 3);                   // nobody else was still in
        QCOMPARE(cdb::getFinal(tid)["groups"].toList().size(), 0);     // a draft saves nothing
    }

    // the final's controls: knockout pills on the left (several at once), winner pills on the right
    // (one only), stacked in a narrow window; the last player left cannot be knocked out, a failed
    // save keeps the selection, a second click saves nothing more, and a restart keeps it all

    void finalControlsArePillsInTwoSections()
    {
        QStringList names;
        for (int i = 1; i <= 8; ++i)
            names << QStringLiteral("%1 %2").arg(LONG_NAME).arg(i);      // every finalist has a long name
        const qint64 tid = commander(names, 1, true, 80, "Pill Final", true);
        const QList<qint64> s = playToFinal(tid);
        w_->resize(1180, 780);
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        FinalEditor *e = shownEditor(ev);
        QVERIFY(e && !e->stacked());
        const auto at = [&e](QWidget *w) { return w->mapTo(e, QPoint(0, 0)); };
        const auto labelOf = [](QPushButton *b) { return b->findChild<QLabel *>()->text().remove(QChar(0x200B)); };

        // side by side: the knockout pills on the left, level with the winner pills on the right
        QPushButton *firstPick = e->picks.value(s[0]), *firstWinner = e->winners.value(s[0]);
        QVERIFY(qobject_cast<T::WrapButton *>(firstPick) && firstPick->isCheckable() && firstWinner->isCheckable());
        QVERIFY(at(firstPick).x() + firstPick->width() < at(firstWinner).x());
        QVERIFY(qAbs(at(firstPick).y() - at(firstWinner).y()) < firstPick->height());
        QVERIFY(at(e->knockoutBtn).x() < at(firstWinner).x() && at(e->confirmBtn).x() >= at(firstWinner).x());
        QCOMPARE(e->knockoutBtn->text(), QString("Confirm knockout"));
        QCOMPARE(e->noWinner->accessibleName(), QString("No winner or draw in the final"));
        QVERIFY(!e->knockoutBtn->isEnabled() && !e->confirmBtn->isEnabled());      // nothing chosen yet

        // keyboard: the pills, then the knockout buttons, then the ending, in that order
        QList<QWidget *> chain;
        QWidget *w = firstPick;
        for (int i = 0; i < 400 && w; ++i, w = w->nextInFocusChain())
            if (!chain.contains(w) && e->isAncestorOf(w) && w->isVisible() && (w->focusPolicy() & Qt::TabFocus))
                chain << w;
        const QList<QWidget *> expected{firstPick, e->picks.value(s[3]), e->knockoutBtn, e->undoBtn, firstWinner,
                                        e->winners.value(s[3]), e->noWinner, e->confirmBtn, e->howBtn};
        for (int i = 1; i < expected.size(); ++i)
            QVERIFY2(chain.indexOf(expected[i - 1]) < chain.indexOf(expected[i]) && chain.indexOf(expected[i - 1]) >= 0,
                     qPrintable(expected[i]->accessibleName()));
        QTest::keyClick(e->picks.value(s[2]), Qt::Key_Space);
        QVERIFY(e->picks.value(s[2])->isChecked() && labelOf(e->picks.value(s[2])).startsWith("✓"));     // shown, not only coloured

        // two players out together; one winner at a time, or No winner / Draw instead
        e->picks.value(s[3])->click();
        QVERIFY(e->picks.value(s[2])->isChecked() && e->picks.value(s[3])->isChecked() && e->knockoutBtn->isEnabled());
        e->winners.value(s[1])->click();
        e->winners.value(s[0])->click();
        QVERIFY(e->winners.value(s[0])->isChecked() && !e->winners.value(s[1])->isChecked() && e->winner() == s[0]);
        e->noWinner->click();
        QVERIFY(e->noWinner->isChecked() && !e->winners.value(s[0])->isChecked() && e->outcome() == "NO_WINNER");

        // a save that fails keeps the selection and says why, beside the knockout
        db::exec("CREATE TRIGGER refuse_knockout BEFORE INSERT ON commander_audit "
                 "BEGIN SELECT RAISE(ABORT, 'knockouts cannot be saved'); END");
        e->knockoutBtn->click();
        settle();
        QVERIFY(cdb::getFinal(tid)["groups"].toList().isEmpty());
        QVERIFY(db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {tid}).isNull());
        QCOMPARE(shownEditor(ev), e);
        QVERIFY2(e->knockoutError->isVisible() && e->knockoutError->text().remove(QChar(0x200B)).startsWith("Not saved:")
                     && e->knockoutError->text().remove(QChar(0x200B)).contains("knockouts cannot be saved")
                     && e->knockoutError->text().remove(QChar(0x200B)).contains("(reference R-"),
                 qPrintable(e->knockoutError->text()));
        QVERIFY(e->picks.value(s[2])->isChecked() && e->picks.value(s[3])->isChecked() && e->noWinner->isChecked());
        db::exec("DROP TRIGGER refuse_knockout");

        // the same press works once the cause is gone; a quick second press saves nothing more
        e->knockoutBtn->click();
        e->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 1, 10000);
        settle();
        QCOMPARE(cdb::getFinal(tid)["groups"].toList()[0].toList(), (QVariantList{s[2], s[3]}));
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_audit WHERE tournament_id = ? AND action = 'FINAL_ELIMINATIONS_RECORDED'",
                           {tid}).toInt(), 1);
        QVERIFY(!db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {tid}).isNull());
        QVERIFY(!e->knockoutError->isVisible());
        // out: no longer offered to knock out or to win; the selection was cleared, the ending kept
        QCOMPARE(e->picks.size(), 2);
        QVERIFY(!e->winners.contains(s[2]) && !e->winners.contains(s[3]));
        QVERIFY(!e->picks.value(s[0])->isChecked() && !e->picks.value(s[1])->isChecked());
        QVERIFY(e->noWinner->isChecked());
        QVERIFY(labelText(ev).contains("Knockout 1: " + names.value(0).left(10)) && labelText(ev).contains("(together)"));

        // with two left, both together is not a knockout: the note points to No winner / Draw
        e->picks.value(s[0])->click();
        e->picks.value(s[1])->click();
        QVERIFY(!e->knockoutBtn->isEnabled());
        QVERIFY(e->knockoutNote->isVisible() && e->knockoutNote->text().remove(QChar(0x200B)).contains("choose No winner / Draw"));
        e->knockoutBtn->click();                                    // disabled: nothing is saved or reinterpreted
        settle();
        QCOMPARE(cdb::getFinal(tid)["groups"].toList().size(), 1);
        e->picks.value(s[0])->click();
        e->winners.value(s[0])->click();
        e->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 2, 10000);
        settle();

        // one player remains: no knockout for them, a short message, and the winner is still to confirm
        QCOMPARE(e->picks.keys(), QList<qint64>{s[0]});
        QVERIFY(!e->picks.value(s[0])->isEnabled() && !e->knockoutBtn->isEnabled());
        QCOMPARE(e->knockoutNote->text().remove(QChar(0x200B)), QString("One player remains. Confirm the winner to finish."));
        QCOMPARE(e->winners.keys(), QList<qint64>{s[0]});
        QVERIFY(e->winners.value(s[0])->isEnabled() && !e->noWinner->isEnabled() && !e->noWinner->isChecked());
        QCOMPARE(cdb::getFinal(tid)["status"].toString(), QString("PLAYING"));     // nothing saved or finished for them
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("SWISS"));
        // the core refuses the same knockout from anywhere, and nothing changes
        const int version = cdb::getFinal(tid)["result_version"].toInt();
        const int audits = int(cdb::getAudit(tid).size());
        QString refusal;
        try {
            cdb::saveFinalEliminations(tid, {{s[2], s[3]}, {s[1]}, {s[0]}}, version);
        } catch (const cdb::CommanderError &err) {
            refusal = err.message;
        }
        QVERIFY2(refusal.contains("cannot remove every player still in") && refusal.contains("No winner / Draw"), qPrintable(refusal));
        QCOMPARE(cdb::getFinal(tid)["result_version"].toInt(), version);
        QCOMPARE(int(cdb::getAudit(tid).size()), audits);

        // after a restart the knockouts are still there and the remaining player can be confirmed
        w_.reset();
        settle();
        db::closeThreadConnection();
        openWindow();
        w_->resize(1180, 780);
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        e = shownEditor(ev);
        QVERIFY(e);
        QCOMPARE(e->knockouts(), (QList<QList<qint64>>{{s[2], s[3]}, {s[1]}}));
        QCOMPARE(e->winners.keys(), QList<qint64>{s[0]});
        QVERIFY(!e->confirmBtn->isEnabled());
        e->winners.value(s[0])->click();
        const QStringList lines = e->preview->text().remove(QChar(0x200B)).split('\n');
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines[0].startsWith("1st") && lines[0].contains("won the final"));
        QVERIFY(lines[1].startsWith("2nd") && lines[1].contains("(seed 2)"));
        QVERIFY(lines[2].startsWith("3rd") && lines[2].contains("(seed 3)"));     // out together: seed order between them
        QVERIFY(lines[3].startsWith("4th") && lines[3].contains("(seed 4)"));
        QVERIFY(e->confirmBtn->isEnabled());

        // both themes, both text sizes, a wide and a narrow window: side by side or stacked, nothing cut off
        for (const QString &textSize : {QString("standard"), QString("large")}) {
            for (const QString &mode : {QString("dark"), QString("light")}) {
                prefs::setPref("theme", mode);
                prefs::setPref("text_size", textSize);
                w_->restyle();
                for (const QSize &size : {QSize(1180, 780), QSize(760, 600), QSize(360, 480)}) {
                    w_->resize(size);
                    ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
                    e = shownEditor(ev);
                    const QByteArray where = QStringLiteral("%1 %2 %3").arg(textSize, mode).arg(size.width()).toUtf8();
                    QVERIFY2(e, where);
                    e->winners.value(s[0])->click();
                    settle();
                    if (size.width() == 360)
                        QVERIFY2(e->stacked() && at(e->winners.value(s[0])).y() > at(e->knockoutBtn).y(), where);
                    if (size.width() == 1180)
                        QVERIFY2(!e->stacked(), where);
                    QVERIFY2(layoutProblems().isEmpty(), where + " " + layoutProblems().join("; ").toUtf8());
                }
            }
        }
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "standard");
        w_->restyle();
        w_->resize(1180, 780);
        ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        e = shownEditor(ev);
        e->winners.value(s[0])->click();
        e->confirmBtn->click();                                     // asks first (answered automatically here)
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"), 10000);
        QCOMPARE(cdb::getFinal(tid)["winner_player_id"].toLongLong(), s[0]);
    }

    // nobody winning is its own choice, never assumed: the players still in, including the last
    // ones going out together, are placed by seed

    void finalWithNoWinnerPlacesThePlayersStillInBySeed()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"}, 1, true, 80, "Drawn Final", true);
        const QList<qint64> s = playToFinal(tid);
        auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
        FinalEditor *e = shownEditor(ev);
        e->picks.value(s[3])->click();
        e->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 1, 10000);
        e->picks.value(s[2])->click();
        e->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 2, 10000);
        settle();
        // the last two went out at the same moment: nobody won, and that is chosen, never assumed
        QVERIFY(e->outcome().isEmpty() && !e->noWinner->isChecked() && !e->confirmBtn->isEnabled());
        QVERIFY(e->noWinner->isEnabled());
        e->noWinner->click();
        QStringList lines = e->preview->text().remove(QChar(0x200B)).split('\n');
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines[0].startsWith("1st") && lines[0].contains("placed by seed 1"));
        QVERIFY(lines[1].startsWith("2nd") && lines[1].contains("placed by seed 2"));
        QVERIFY(lines[2].startsWith("3rd") && lines[2].contains("knocked out (seed 3)"));
        QVERIFY(lines[3].startsWith("4th") && lines[3].contains("knocked out (seed 4)"));
        QCOMPARE(cdb::getFinal(tid)["status"].toString(), QString("PLAYING"));     // choosing saves nothing
        // a third knockout would leave one player, who then has won: No winner no longer fits
        e->picks.value(s[1])->click();
        e->knockoutBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 3, 10000);
        settle();
        QVERIFY(!e->noWinner->isChecked() && !e->noWinner->isEnabled() && e->outcome().isEmpty());
        e->undoBtn->click();                                        // a correction: the reason is answered automatically
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), 2, 10000);
        settle();
        const QVariantMap undo = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(undo["action"].toString(), QString("FINAL_ELIMINATIONS_CORRECTED"));
        QCOMPARE(undo["detail"].toMap()["reason"].toString(), QString("test correction"));
        QCOMPARE(e->picks.size(), 2);
        e->noWinner->click();
        e->confirmBtn->click();
        QTRY_COMPARE_WITH_TIMEOUT(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"), 10000);
        const QVariantMap fin = cdb::getFinal(tid);
        QCOMPARE(fin["outcome"].toString(), QString("NO_WINNER"));
        QHash<qint64, QPair<int, QString>> placed;
        for (const QVariant &p : fin["players"].toList())
            placed.insert(p.toMap()["player_id"].toLongLong(), {p.toMap()["placement"].toInt(), p.toMap()["placement_basis"].toString()});
        QCOMPARE(placed.value(s[0]), (QPair<int, QString>{1, "QUALIFYING_RANK"}));
        QCOMPARE(placed.value(s[1]), (QPair<int, QString>{2, "QUALIFYING_RANK"}));
        QCOMPARE(placed.value(s[2]), (QPair<int, QString>{3, "ELIMINATION"}));
        QCOMPARE(placed.value(s[3]), (QPair<int, QString>{4, "ELIMINATION"}));
    }

    // recording, repeating and undoing knockouts in the final leaves the page where it was and
    // never moves or resizes the window

    void finalKnockoutsLeaveThePageAndTheWindowWhereTheyAre()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"}, 1, true, 80, "Steady Final", true);
        const QList<qint64> seeds = playToFinal(tid);
        w_.reset();
        openWindow(true);               // focus only moves in a real, active window, and the jump came from focus
        for (const QSize &size : {QSize(1180, 520), QSize(640, 480), QSize(420, 600)}) {
            for (const QString &textSize : {QString("standard"), QString("large")}) {
                prefs::setPref("text_size", textSize);
                w_->restyle();
                w_->resize(size);
                settle();
                auto *ev = go<CommanderEventScreen>("round", {{"tournament_id", tid}});
                QVERIFY(ev && shownEditor(ev));
                auto *page = ev->findChild<QScrollArea *>();
                QScrollBar *bar = page->verticalScrollBar();
                const QByteArray where = QStringLiteral("%1 %2x%3").arg(textSize).arg(size.width()).arg(size.height()).toUtf8();
                QVERIFY2(bar->maximum() > 0, where + " the final page scrolls");

                // scrolls the button to the middle of the view, presses it with the mouse, and checks nothing moved
                const auto press = [&](const QString &step, const std::function<QAbstractButton *(FinalEditor *)> &choose,
                                       int groupsAfter) {
                    const QByteArray at = where + " " + step.toUtf8();
                    FinalEditor *e = shownEditor(ev);
                    QAbstractButton *b = choose(e);
                    QVERIFY2(b && b->isEnabled(), at);
                    page->ensureWidgetVisible(b, 0, page->viewport()->height() / 2 - b->height());
                    settle();
                    const int target = bar->value();
                    const QRect frame = w_->frameGeometry();
                    b->setFocus(Qt::MouseFocusReason);
                    QTest::mouseClick(b, Qt::LeftButton);
                    QTRY_COMPARE_WITH_TIMEOUT(cdb::getFinal(tid)["groups"].toList().size(), groupsAfter, 10000);
                    settle();
                    QVERIFY2(qAbs(bar->value() - qMin(target, bar->maximum())) <= 2,
                             at + QStringLiteral(": the page moved from %1 to %2").arg(target).arg(bar->value()).toUtf8());
                    QVERIFY2(w_->frameGeometry() == frame, at + ": the window moved or changed size");
                    // keyboard focus stays on something the user can see
                    QWidget *f = QApplication::focusWidget();
                    QVERIFY2(f && (!page->isAncestorOf(f) || !f->visibleRegion().isEmpty()),
                             at + ": focus went off screen to " + (f ? f->accessibleName().toUtf8() : QByteArray("nothing")));
                };
                const auto knockOut = [&](const QList<qint64> &who) {
                    return [who](FinalEditor *e) -> QAbstractButton * {
                        for (qint64 p : who)
                            e->picks.value(p)->setChecked(true);
                        return e->knockoutBtn;
                    };
                };
                press("first knockout", knockOut({seeds[3]}), 1);
                press("second knockout", knockOut({seeds[2]}), 2);
                press("undo", [](FinalEditor *e) { return e->undoBtn; }, 1);        // the reason is answered automatically
                press("two together", knockOut({seeds[1], seeds[2]}), 2);
                QCOMPARE(cdb::getFinal(tid)["groups"].toList()[1].toList().size(), 2);
                // back to the start for the next window size
                cdb::saveFinalEliminations(tid, {}, cdb::getFinal(tid)["result_version"].toInt(), "next pass of the test");
            }
        }
        prefs::setPref("text_size", "standard");
        w_->restyle();
    }

    // A wheel turn over a closed drop-down or number field scrolls the page instead of changing the value.

    void wheelOverClosedFieldsScrollsThePageInstead()
    {
        const auto turn = [](QWidget *target, bool trackpad) {
            const QPointF at = QPointF(target->rect().center());
            const QPointF global = QPointF(target->mapToGlobal(target->rect().center()));
            QWheelEvent wheel(at, global, trackpad ? QPoint(0, -40) : QPoint(), QPoint(0, trackpad ? -40 : -120), Qt::NoButton,
                              Qt::NoModifier, trackpad ? Qt::ScrollUpdate : Qt::NoScrollPhase, false,
                              trackpad ? Qt::MouseEventSynthesizedBySystem : Qt::MouseEventNotSynthesized);
            QApplication::sendEvent(target, &wheel);
            settle();
        };
        const qint64 tid = commander({"Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus"}, 3, false);
        w_->resize(1180, 480);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        QScrollBar *bar = reg->findChild<QScrollArea *>()->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        QWidget *spinText = reg->roundsSpin->findChild<QLineEdit *>();
        QVERIFY(spinText);
        for (bool trackpad : {false, true}) {
            for (QWidget *field : {static_cast<QWidget *>(reg->drawCombo), static_cast<QWidget *>(reg->shortCombo),
                                   static_cast<QWidget *>(reg->roundsSpin), spinText}) {
                const QByteArray what = QByteArray(field->metaObject()->className()) + (trackpad ? " trackpad" : " wheel");
                bar->setValue(0);
                field->setFocus();                             // focus does not make the wheel change it either
                const int draw = reg->drawCombo->currentIndex(), pods = reg->shortCombo->currentIndex();
                const int rounds = reg->roundsSpin->value();
                turn(field, trackpad);
                QVERIFY2(reg->drawCombo->currentIndex() == draw && reg->shortCombo->currentIndex() == pods
                         && reg->roundsSpin->value() == rounds, what);
                QVERIFY2(bar->value() > 0, what + ": the page scrolls instead");
            }
        }
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 3);

        // keys and the arrow buttons still change them
        reg->roundsSpin->setFocus();
        QTest::keyClick(reg->roundsSpin, Qt::Key_Up);
        QTRY_COMPARE(reg->roundsSpin->value(), 4);
        QStyleOptionSpinBox opt;
        opt.initFrom(reg->roundsSpin);
        opt.rect = reg->roundsSpin->rect();
        opt.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown | QStyle::SC_SpinBoxFrame | QStyle::SC_SpinBoxEditField;
        opt.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
        opt.buttonSymbols = reg->roundsSpin->buttonSymbols();
        opt.frame = true;
        const QRect up = reg->roundsSpin->style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxUp, reg->roundsSpin);
        QTest::mouseClick(reg->roundsSpin, Qt::LeftButton, {}, up.center());
        QTRY_COMPARE(reg->roundsSpin->value(), 5);
        reg->drawCombo->setFocus();
        QTest::keyClick(reg->drawCombo, Qt::Key_Down);
        QTRY_COMPARE(reg->drawCombo->currentData().toString(), QString("ALL_DRAW"));

        // an open list, and lists and text areas on a page, still take the wheel themselves
        QScrollArea page;
        auto *content = new QWidget;
        auto *v = new QVBoxLayout(content);
        QComboBox *many = T::combo();
        for (int i = 0; i < 60; ++i)
            many->addItem(QString::number(i));
        auto *list = new QListWidget;
        for (int i = 0; i < 60; ++i)
            list->addItem(QString::number(i));
        list->setFixedHeight(120);
        auto *text = new QPlainTextEdit(QString("line\n").repeated(200));
        text->setFixedHeight(120);
        v->addWidget(many);
        v->addWidget(list);
        v->addWidget(text);
        v->addSpacing(900);
        page.setWidget(content);
        page.setWidgetResizable(true);
        page.resize(400, 300);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        for (bool trackpad : {false, true}) {
            page.verticalScrollBar()->setValue(0);
            list->verticalScrollBar()->setValue(0);
            text->verticalScrollBar()->setValue(0);
            turn(many, trackpad);
            QCOMPARE(many->currentIndex(), 0);
            QVERIFY(page.verticalScrollBar()->value() > 0);
            const int pageAt = page.verticalScrollBar()->value();
            turn(list->viewport(), trackpad);
            QVERIFY(list->verticalScrollBar()->value() > 0);
            turn(text->viewport(), trackpad);
            QVERIFY(text->verticalScrollBar()->value() > 0);
            QCOMPARE(page.verticalScrollBar()->value(), pageAt);
        }
        many->showPopup();
        QTRY_VERIFY(many->view()->isVisible());
        const int top = many->view()->verticalScrollBar()->value();
        turn(many->view()->viewport(), false);
        QVERIFY(many->view()->verticalScrollBar()->value() > top);
        QCOMPARE(many->currentIndex(), 0);
        many->hidePopup();
    }

    // The suggested number of rounds is shown beside the rounds, with its basis, and only used on request.

    void suggestedRoundsAreShownAndOnlyUsedOnRequest()
    {
        const qint64 tid = commander({"Ana", "Bo", "Cy"}, 0, false);
        cdb::setCheckedIn(tid, cdb::getRegistrations(tid)[2]["player_id"].toLongLong(), false);
        auto *reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        QCOMPARE(reg->resetBtn->text(), QString("Use suggested rounds"));
        QCOMPARE(reg->roundsHint->text(), QString("A suggestion appears once three players check in."));
        QVERIFY(!reg->resetBtn->isEnabled());

        // not set by the organizer yet: the rounds follow the players checked in
        QStringList more;
        for (int i = 3; i <= 20; ++i)
            more << QStringLiteral("Guest %1").arg(i);
        for (qint64 p : players(more))
            tdb::enrollPlayer(tid, p);
        reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        const int for20 = cdb::recommendation(20)["rounds"].toInt();
        QCOMPARE(reg->roundsSpin->value(), for20);
        QCOMPARE(reg->roundsHint->text(), QStringLiteral("Suggested for 20 checked in: %1 rounds.\nFollows check-ins until you change it.").arg(for20));
        // another setting does not make the rounds a choice
        reg->drawCombo->setCurrentIndex(1);
        QVERIFY(!cdb::getEvent(tid)["rounds_chosen"].toBool());

        // rounds the organizer sets, even to the suggested number, stay as set (BUG-014)
        reg->roundsSpin->setValue(for20 + 1);
        reg->roundsSpin->setValue(for20);
        QVERIFY(cdb::getEvent(tid)["rounds_chosen"].toBool());
        more.clear();
        for (int i = 21; i <= 40; ++i)
            more << QStringLiteral("Guest %1").arg(i);
        for (qint64 p : players(more))
            tdb::enrollPlayer(tid, p);
        reg = go<CommanderRegistrationScreen>("registration", {{"tournament_id", tid}});
        const int for40 = cdb::recommendation(40)["rounds"].toInt();
        QVERIFY(for40 != for20);
        QCOMPARE(reg->roundsSpin->value(), for20);
        QVERIFY(reg->roundsHint->text().endsWith("Set by you: check-ins leave it alone."));

        // the button sets the suggestion and lets the rounds follow again; nothing else changes
        QVERIFY(reg->resetBtn->isEnabled());
        reg->resetBtn->click();
        settle();
        QCOMPARE(reg->roundsSpin->value(), for40);
        QVERIFY(!cdb::getEvent(tid)["rounds_chosen"].toBool());
        QVERIFY(reg->roundsHint->text().endsWith("Follows check-ins until you change it."));
        QCOMPARE(cdb::getEvent(tid)["settings"].toMap()["draw_policy"].toString(), QString("ALL_DRAW"));
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["rounds_before"].toInt(), for20);

        // on the setup page, rounds typed by the organizer are kept when the expected count changes
        auto *setup = go<TournamentSetupScreen>("tournament_setup", {{"game", "MTG"}});
        setup->formatCombo->setCurrentText("Commander");
        setup->playerCount->setValue(20);
        QCOMPARE(setup->roundsSpin->value(), for20);
        setup->roundsSpin->setValue(2);
        setup->playerCount->setValue(40);
        QCOMPARE(setup->roundsSpin->value(), 2);
        QVERIFY(setup->suggestionLabel->text().contains(QStringLiteral("Suggested for 40 expected players: %1 rounds.").arg(for40)));
        setup->nameInput->setText("Typed Rounds");
        setup->createTournament();
        settle();
        const qint64 typed = db::value("SELECT tournament_id FROM tournaments WHERE name = 'Typed Rounds'").toLongLong();
        QCOMPARE(cdb::getEvent(typed)["swiss_rounds"].toInt(), 2);
        QVERIFY(cdb::getEvent(typed)["rounds_chosen"].toBool());
    }
};

int main(int argc, char *argv[])
{
    QApplication::setStyle("Fusion");       // as main.cpp does
    QApplication app(argc, argv);
    db::requireChosenPath();
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_ui.moc"
