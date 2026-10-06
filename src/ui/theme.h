// Design tokens and shared widgets.  UCA purple and gray on a soft off-white
// light theme or a calm dark theme; pill-shaped controls with full outlines
// (2px normally, 3px when selected or for important panels).  Colours are
// variables that apply() swaps, so screens read them as T::NAME when they
// build (never cache them).
#pragma once

#include <QBoxLayout>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QString>
#include <QWidget>

namespace T {

extern const QString UCA_PURPLE, UCA_GRAY;

// palette (filled in by apply)
extern QString BG, SURFACE, SURFACE2, SURFACE3, SELECTED, BORDER, BORDER2, TEXT, MUTED, DIM;
extern QString PURPLE, PURPLE_HOVER, PURPLE_LT, PURPLE_DIM, PURPLE_BDR;
extern QString GREY, GREY_LT;
extern QString GREEN, RED, ORANGE, AMBER, LIVE, FOCUS;
extern QString OP, POKE, MTG, GOLD, SILVER, BRONZE;

extern QString MODE;                // "dark" | "light"
extern double SCALE;                // interface text size: 1.0 standard, 1.2 large

extern const QString FONT, MONO;
extern QString HEAD;                // display font for headings, tabs, buttons and timers (see chooseDisplayFont)
// Picks the display font from what is installed: Bahnschrift when present, else the first
// available fallback, else the body font.  Nothing is downloaded or bundled.
QString chooseDisplayFont();
const int CONTENT_MAX_W = 1400;       // the centred column, including its side padding
const int BUTTON_H = 40;              // minimum height of buttons (they grow with the text size)
const int RADIUS = 18;                // cards and panels
const int RADIUS_SM = 12;             // fields
const int LINE = 2;                   // outline of an ordinary control
const int LINE_STRONG = 3;            // outline of a selected control or an important panel
const int MIN_HIT = 40;             // smallest interactive target, in logical px

QString gameColor(const QString &game);
QString placeColor(int place);      // empty when the place has no colour
QString gameLabel(const QString &game);
QString gameShort(const QString &game);

// Switch palette and text scale.  Screens built afterwards pick the new values up.
void apply(const QString &mode = "dark", const QString &textSize = "standard");

int px(int n);                      // a pixel size that follows the text-size preference
QString P(int n);                   // the same, as text for stylesheets
QString alpha(const QString &hex, int a);       // CSS-style translucent colour, as rgba()
QString breakable(const QString &text, int run = 14);   // lets long unbroken strings wrap

QFont font(int size = 13, int weight = 400, bool mono = false, double spacing = 0.0, int head = -1);
QLabel *lbl(const QString &text = {}, const QString &color = {}, int size = 13, int weight = 400, bool wrap = false,
            bool mono = false, double spacing = 0.0);
QLabel *caps(const QString &text, const QString &color = {}, int size = 11);     // small uppercase section label
QLabel *pill(const QString &text, const QString &color, bool dot = true, const QString &bg = {});
// Small rounded tag naming a game, in that game's tag colour (the name is always written out).
QLabel *gameTag(const QString &game);
QString gameName(const QString &game);          // "One Piece", "Pokémon", "Magic: The Gathering"
QString gameTagName(const QString &game);       // the short form on a tag: "One Piece", "Pokémon", "Magic"
QFrame *hline(const QString &color = {});
// Pill-shaped button.  kind: primary | secondary | danger | ghost.
QPushButton *button(const QString &text, const QString &kind = "secondary");
QPushButton *roundButton(const QString &text, const QString &tooltip);            // circular icon control
// A checkable pill for "choose one" groups.  Checked: 3px purple outline, tint and a tick.
QPushButton *choicePill(const QString &text);
QString pillQss(bool selected, int height);     // shared by tabs and choice pills
// A dropdown whose opened list is opaque and themed (see comboStyle).
QComboBox *combo();
QString buttonQss(const QString &kind = "secondary", int height = MIN_HIT);
QFrame *panel(const QString &name = "pn", const QString &outline = {}, int width = 1, int radius = 16,
              const QString &bg = {});
QBoxLayout *row(int spacing = 10);
void clear(QLayout *layout);        // removes and deletes everything in a layout

QString inputStyle();
QString comboStyle();               // the closed control, its popup container, the list and its rows
QString scrollStyle();
QString appQss();                   // application-wide defaults, so dialogs follow the theme too

// An oval button whose label wraps, so long player names are never clipped.
class WrapButton : public QPushButton {
    Q_OBJECT
public:
    explicit WrapButton(const QString &text, const QString &kind = "ghost");
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(72, MIN_HIT); }

protected:
    void changeEvent(QEvent *e) override;

private:
    void paintLabel();
    QLabel *label_ = nullptr;
};

// A clickable, keyboard-reachable panel.
class ClickFrame : public QFrame {
    Q_OBJECT
public:
    explicit ClickFrame(const QString &name);
signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
};

// Lays children out left to right and wraps to new lines when space runs out.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 8, bool center = false);
    ~FlowLayout() override;
    void addItem(QLayoutItem *item) override { items_.append(item); }
    int count() const override { return int(items_.size()); }
    QLayoutItem *itemAt(int i) const override { return i >= 0 && i < items_.size() ? items_[i] : nullptr; }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < items_.size() ? items_.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), false); }
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;        // everything on one line: what it takes when there is room
    QSize minimumSize() const override;     // its widest item: below that it cannot wrap any further

private:
    int doLayout(const QRect &rect, bool apply) const;
    QList<QLayoutItem *> items_;
    int gap_;
    bool center_;
};

// Base for every screen: a centred, width-capped column that reflows from its
// own width (not the monitor's).  Layouts registered with flip() run
// left-to-right when there is room and stack when there is not.
class Screen : public QWidget {
    Q_OBJECT
public:
    Screen();
    QBoxLayout *flip(QBoxLayout *layout, bool atTiny = false);
    bool narrow = false, tiny = false;
    QVBoxLayout *root = nullptr;

protected:
    void resizeEvent(QResizeEvent *e) override;
    virtual void reflow() {}            // hook for screens that need more than flipped layouts

private:
    void setMargins();
    QList<QPair<QPointer<QBoxLayout>, bool>> flips_;
};

// Transparent vertical scroll area; `box` receives its content layout.  When the
// scrollbar appears, the content is inset by the same amount on the other side,
// so it stays centred in the column instead of shifting left.
QScrollArea *scrollArea(QVBoxLayout **box);

// Keeps the page where it is while part of it is rebuilt.
//
// Rebuilding a list hides the row that was clicked.  Qt then hands keyboard focus to the
// next control on the page, and a scroll area scrolls to whatever receives focus that way —
// which is what made the page jump after a click.  Create one of these before rebuilding:
// when it goes out of scope it puts every scroll position back and returns focus to the
// control that had it (matched by its accessible name), without scrolling.
class ScrollKeeper {
public:
    explicit ScrollKeeper(QWidget *scope);
    ~ScrollKeeper();
    ScrollKeeper(const ScrollKeeper &) = delete;
    ScrollKeeper &operator=(const ScrollKeeper &) = delete;

private:
    QPointer<QWidget> scope_;
    QList<QPair<QPointer<QScrollBar>, int>> bars_;
    QString focusName_;
    QByteArray focusClass_;
};

// A player's game tags side by side on one line.  They never wrap or stack: when there are
// more than fit, only this strip scrolls sideways (thin bar, arrow keys, Shift+wheel or a
// sideways wheel) and the name and actions beside it stay where they are.
class TagStrip : public QScrollArea {
    Q_OBJECT
public:
    explicit TagStrip(const QStringList &games);
    int contentWidth() const;
    QList<QLabel *> tags;

protected:
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
};

// One line of text that scrolls sideways when it does not fit (long player names): drag the
// thin bar, use Shift+wheel, or focus it and press the arrow keys.  The full text is
// also its tooltip and accessible description.  It never moves by itself.
class NameScroll : public QScrollArea {
    Q_OBJECT
public:
    explicit NameScroll(const QString &text, int size = 15, int weight = 700);
    QLabel *label = nullptr;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void wheelEvent(QWheelEvent *e) override;
};

// Sideways-only scrolling for a wide block (a standings table) inside a page
// that scrolls vertically: it is always exactly as tall as its content.
class HScroll : public QScrollArea {
    Q_OBJECT
public:
    explicit HScroll(QWidget *widget);
    void fit();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
};

} // namespace T
